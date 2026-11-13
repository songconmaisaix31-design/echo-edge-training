import argparse
import hashlib
import json
import math
import random
from collections import defaultdict
from pathlib import Path
from statistics import quantiles
from zipfile import ZipFile

import torch
from av.video.reformatter import VideoReformatter

from benchmark_echo import normalize_image, resize_and_pad
from train_echo import (IGNORE, TASKS, WEIGHTS, EchoModel, Segment, interval_bins, local_media, open_archive,
                        read_annotations, samples_in_intervals, selected_interval_frames, video_frames)
from train_echonet_views import (TEST_SPLITS, TRAIN_SPLITS, VAL_SPLITS, VIEWS, ViewModel, open_avi, published_patient,
                                 read_sources, verify_rotation)


PREPROCESSING = "ffmpeg_area_rgb_letterbox_224_imagenet"


def file_digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def checkpoint_model(path, device):
    with path.open("rb") as source:
        fingerprint = hashlib.file_digest(source, "sha256").hexdigest()
        source.seek(0)
        checkpoint = torch.load(source, weights_only=True, map_location="cpu")
    labels = {task: tuple(codes) for task, codes in checkpoint["labels"].items()}
    if tuple(labels) == TASKS and all(len(codes) >= 2 for codes in labels.values()):
        model = EchoModel(*(len(labels[task]) for task in TASKS), weights=None)
    elif labels == {"view": VIEWS}:
        model = ViewModel(weights=None)
    else:
        raise ValueError("checkpoint 模型类型或标签顺序无效")
    sampling = checkpoint.get("sampling", {})
    fps = sampling.get("sample_fps", 0)
    frames = sampling.get("max_frames_per_segment", 0)
    if (checkpoint.get("preprocessing") != PREPROCESSING or not isinstance(fps, (int, float))
            or not math.isfinite(fps) or fps <= 0 or type(frames) is not int or frames <= 0):
        raise ValueError("checkpoint 缺少已记录的训练预处理和抽样参数")
    model.load_state_dict(checkpoint["model"], strict=True)
    return model.eval().to(device), checkpoint, labels, fingerprint


def locked_policy(policy, labels):
    thresholds = policy["min_confidence"]
    if set(thresholds) != set(labels) or any(type(value) not in (float, int) or not 0 <= value <= 1
                                             for value in thresholds.values()):
        raise ValueError("每个输出头都需要一个已确定的 [0,1] 置信阈值")
    allowed = policy.get("usable_quality", [])
    low = policy.get("low_quality", [])
    reject = policy.get("reject_quality", [])
    if "quality" in labels:
        groups = (allowed, low, reject)
        if any(not isinstance(group, list) or any(not isinstance(code, str) for code in group) for group in groups):
            raise ValueError("质量决策代码需要填写为字符串列表")
        if (not allowed or not low or sum(map(len, groups)) != len(set(allowed + low + reject))
                or set(allowed + low + reject) != set(labels["quality"])):
            raise ValueError("临床确认的可用、低质量和拒判代码需互不重叠并覆盖质量字典")
    elif allowed or low or reject:
        raise ValueError("单切面模型没有质量代码")
    return {"min_confidence": thresholds, "usable_quality": allowed, "low_quality": low, "reject_quality": reject}


def decision(probabilities, cutoff, rejected=()):
    score, index = probabilities.max(dim=0)
    index = int(index)
    return index if float(score) >= cutoff and index not in rejected else len(probabilities)


class IntervalInference:
    def __init__(self, model, device, labels, policy):
        self.model = model
        self.device = device
        self.labels = labels
        self.policy = policy
        self.rejected = {task: [codes.index(code) for code in policy["reject_quality"]] if task == "quality" else []
                         for task, codes in labels.items()}
        self.normalization = WEIGHTS.transforms()

    @torch.inference_mode()
    def predict(self, frames, count):
        sums = [torch.zeros((count, len(codes)), device=self.device) for codes in self.labels.values()]
        support = [0] * count
        reformatter = VideoReformatter()
        for index, frame in frames:
            image = normalize_image(resize_and_pad(frame, reformatter), self.normalization)
            outputs = self.model(image.unsqueeze(0).to(self.device))
            for total, logits in zip(sums, outputs, strict=True):
                total[index] += logits[0].softmax(dim=0)
            support[index] += 1
        if not all(support) or any(not values.isfinite().all() for values in sums):
            raise ValueError("有评估区间没有有效模型输入或输出")
        return [tuple(decision(values[i] / support[i], self.policy["min_confidence"][task], self.rejected[task])
                      for task, values in zip(self.labels, sums, strict=True)) for i in range(count)]


def divide(numerator, denominator):
    return numerator / denominator if denominator else None


def quality_metrics(matrix, usable, low):
    count = sum(map(sum, matrix))
    rejected = sum(row[-1] for row in matrix)
    return {
        "low_quality_false_pass": divide(sum(matrix[i][j] for i in low for j in usable),
                                         sum(sum(matrix[i]) for i in low)),
        "reject_rate": divide(rejected, count),
        "coverage": divide(count - rejected, count),
    }


def confusion(rows, classes):
    matrix = [[0] * (classes + 1) for _ in range(classes)]
    for truth, predicted, _ in rows:
        matrix[truth][predicted] += 1
    return matrix


def summarize(rows, codes, usable=None, low=None):
    matrix = confusion(rows, len(codes))
    counts = list(map(sum, matrix))
    result = {"classes": list(codes), "prediction_columns": [*codes, None], "confusion": matrix,
              "support": counts, "patients_per_class": [len({row[2] for row in rows if row[0] == index})
                                                        for index in range(len(codes))],
              "recall": [divide(row[i], counts[i]) for i, row in enumerate(matrix)],
              "missing_classes": [code for code, count in zip(codes, counts, strict=True) if not count]}
    if usable is None:
        return result
    groups = defaultdict(list)
    for row in rows:
        groups[row[2]].append(row)
    patients = list(groups.values())
    rng = random.Random(42)
    samples = defaultdict(list)
    for _ in range(1000 if len(patients) >= 2 else 0):
        selected = [row for group in rng.choices(patients, k=len(patients)) for row in group]
        for name, value in quality_metrics(confusion(selected, len(codes)), usable, low).items():
            if value is not None:
                samples[name].append(value)
    result["quality"] = {name: {"value": value, "patient_bootstrap_95": (
        [quantiles(samples[name], n=40, method="inclusive")[i] for i in (0, 38)] if len(samples[name]) >= 2 else None
    )} for name, value in quality_metrics(matrix, usable, low).items()}
    return result


def team_rows(args, checkpoint, labels, inference):
    with args.labels.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != checkpoint.get("annotations_sha256"):
            raise ValueError("工作簿与训练时锁定的版本不一致")
    observed_labels, clips, segments = read_annotations(args.labels)
    if {task: tuple(codes) for task, codes in observed_labels.items()} != labels:
        raise ValueError("当前工作簿标签顺序与训练 checkpoint 不一致")
    assignments = checkpoint.get("patient_splits")
    if not assignments or any(role not in {"train", "val", "test"} for role in assignments.values()):
        raise ValueError("checkpoint 缺少冻结的患儿分组")
    if any(clip.split and assignments.get(clip.patient) != clip.split for clip in clips.values()):
        raise ValueError("工作簿患儿分组与训练 checkpoint 不一致")
    if any(clips[key].patient not in assignments for key in segments):
        raise ValueError("工作簿含未锁定分组的已确认患儿")
    with open_archive(args.media) as archive:
        for key, rows in segments.items():
            clip = clips[key]
            if assignments[clip.patient] != args.split:
                continue
            with local_media(args.media, archive, clip.path, args.scratch) as media:
                frames = selected_interval_frames(media, rows, [interval_bins(row, args) for row in rows])
                predictions = inference.predict(frames, len(rows))
            for row, prediction in zip(rows, predictions, strict=True):
                yield clip.patient, key, clip.device, row.mode or clip.mode, clip.site, row.targets, prediction


def stanford_samples(args, checkpoint, *, clips_per_class=None):
    required = {"train": list(TRAIN_SPLITS), "val": list(VAL_SPLITS), "test": list(TEST_SPLITS)}
    if checkpoint.get("source_splits") != required:
        raise ValueError("checkpoint 未记录 Stanford 第 0 次轮转的完整分组")
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        errors = []
        clips = read_sources(archive, errors)
        if errors:
            raise ValueError(f"Stanford 文件清单有 {len(errors)} 项错误")
        verify_rotation(clips)
        selected = [clip for clip in clips if clip[2] in required[args.split]]
        if clips_per_class is not None:
            if args.split != "val":
                raise ValueError("校准抽样仅使用验证折")
            rng = random.Random(42)
            selected = [clip for view in VIEWS
                        for clip in rng.sample([clip for clip in selected if clip[1] == view], clips_per_class)]
        for path, view, _, _, _ in selected:
            with open_avi(archive, path) as source:
                stream = source.streams.video[0]
                stream.codec_context.thread_count = 1
                offsets = [offset for offset, _ in video_frames(source, stream)]
            if not offsets:
                raise ValueError("Stanford 评估片段没有可解码帧")
            row = Segment(path, path, 0, math.floor(offsets[-1]) + 1, (VIEWS.index(view),))
            with open_avi(archive, path) as source:
                stream = source.streams.video[0]
                stream.codec_context.thread_count = 1
                frames = samples_in_intervals(video_frames(source, stream), [row], [interval_bins(row, args)])
                yield published_patient(path), path, row.targets, frames


def stanford_rows(args, checkpoint, inference):
    for patient, path, targets, frames in stanford_samples(args, checkpoint):
        prediction, = inference.predict(frames, 1)
        yield patient, path, "", "灰阶", "", targets, prediction


def evaluate(args):
    model, checkpoint, labels, digest = checkpoint_model(args.checkpoint, args.device)
    args.sample_fps = checkpoint["sampling"]["sample_fps"]
    args.max_frames_per_segment = checkpoint["sampling"]["max_frames_per_segment"]
    policy = json.loads(args.policy.read_text(encoding="utf-8-sig"))
    if args.split == "test" and (policy.get("checkpoint_sha256") != digest or policy.get("validated_on") != "val"):
        raise ValueError("独立测试需要同一 checkpoint 的验证集冻结文件")
    policy = locked_policy(policy, labels)
    inference = IntervalInference(model, args.device, labels, policy)
    args.scratch.mkdir(parents=True, exist_ok=True)
    team = args.archive is None
    if team != ("quality" in labels):
        raise ValueError("数据类型与 checkpoint 模型类型不一致")
    rows = list(team_rows(args, checkpoint, labels, inference) if team else stanford_rows(args, checkpoint, inference))
    if not rows:
        raise ValueError(f"{args.split} 没有实际评估区间")

    def metrics(selected):
        return {task: summarize([(row[5][i], row[6][i], row[0]) for row in selected if row[5][i] != IGNORE], codes,
                                [codes.index(code) for code in policy["usable_quality"]] if task == "quality" else None,
                                [codes.index(code) for code in policy["low_quality"]] if task == "quality" else None)
                for i, (task, codes) in enumerate(labels.items())}

    groups = {"device": defaultdict(list), "mode": defaultdict(list), "site": defaultdict(list)}
    for row in rows:
        for key, value in zip(groups, row[2:5], strict=True):
            groups[key][value or "未填写"].append(row)
    report = {"dataset": "team" if team else "EchoNet-Pediatric grey-scale", "split": args.split,
              "checkpoint_sha256": digest, "preprocessing": PREPROCESSING, "sampling": checkpoint["sampling"],
              "aggregation": "mean_frame_softmax", **policy, "patients": len({row[0] for row in rows}),
              "clips": len({row[1] for row in rows}), "intervals": len(rows), "metrics": metrics(rows),
              "groups": {key: {value: metrics(group) for value, group in observed.items()}
                         for key, observed in groups.items()} if team else {}}
    if args.lock:
        if any(not sum(task["support"]) for task in report["metrics"].values()):
            raise ValueError("每个输出头都需要实际验证区间才能冻结阈值")
        if team and report["metrics"]["quality"]["quality"]["low_quality_false_pass"]["value"] is None:
            raise ValueError("冻结质量阈值前需要已确认的低质量验证区间")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)
    if args.lock:
        with args.report.open("rb") as source:
            validation_digest = hashlib.file_digest(source, "sha256").hexdigest()
        with args.lock.open("x", encoding="utf-8") as output:
            json.dump({**policy, "checkpoint_sha256": digest, "validated_on": "val",
                       "validation_report_sha256": validation_digest}, output, indent=2)


def main():
    parser = argparse.ArgumentParser(description="按固定分组评估心超 checkpoint，不在测试集调整阈值")
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--media", type=Path)
    inputs.add_argument("--archive", type=Path)
    parser.add_argument("--labels", type=Path)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--split", choices=("val", "test"), required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--lock", type=Path)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    if bool(args.labels) != bool(args.media) or (args.lock and args.split != "val"):
        parser.error("团队影像需要 --labels，且只能在验证阶段写冻结文件")
    evaluate(args)


if __name__ == "__main__":
    main()
