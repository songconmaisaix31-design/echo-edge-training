import argparse
import hashlib
import json
import math
import re
import tempfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import numpy as np
import torch
from av.video.reformatter import VideoReformatter

from benchmark_echo import normalize_image, resize_and_pad
from train_echo import (
    IGNORE, PREPROCESSING, TASKS, WEIGHTS, EchoModel, explicit_patient_splits, interval_bins,
    local_media, open_archive, read_annotations, samples_in_intervals, selected_interval_frames,
)
from train_echonet_views import SOURCE_SPLITS, VIEWS, ViewModel, avi_frames, inspect_avi, read_sources


BOOTSTRAP_SEED = 42
BOOTSTRAP_REPEATS = 1000
STANFORD_FILENAME = re.compile(r"(CR[0-9a-z]{7})-CR[0-9a-z]{7}-[0-9]{6}\.avi")


@dataclass(frozen=True)
class Prediction:
    patient: str
    targets: tuple[int, ...]
    decisions: tuple[int, ...]
    device: str = ""
    mode: str = ""
    site: str = ""


def sampling_config(args):
    return {"sample_fps": args.sample_fps, "max_frames_per_segment": args.max_frames_per_segment}


def load_checkpoint(args, labels, split_metadata, constructor, thresholds):
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    saved_labels = checkpoint.get("labels")
    if saved_labels is None:
        raise ValueError("checkpoint 缺少心超任务类别，需使用本仓库训练入口生成的 best.pt")
    if tuple(saved_labels) != tuple(labels) or any(tuple(saved_labels[task]) != tuple(labels[task]) for task in labels):
        raise ValueError("checkpoint 的任务和类别顺序与评估数据不一致")
    if checkpoint["preprocessing"] != PREPROCESSING or checkpoint.get("sampling") != sampling_config(args):
        raise ValueError("checkpoint 的预处理或抽帧参数与评估配置不一致")
    for key, value in split_metadata.items():
        saved = checkpoint.get(key)
        if (key == "source_splits" and isinstance(saved, dict) and set(saved) == {"train", "val"}):
            value = {role: value[role] for role in saved}
        if saved != value:
            raise ValueError(f"checkpoint 的 {key} 与本次分组不一致")
    model = constructor()
    model.load_state_dict(checkpoint["model"], strict=True)
    model.eval().to(args.device)
    with args.checkpoint.open("rb") as file:
        identity = hashlib.file_digest(file, "sha256").hexdigest()
    return model, {
        "name": args.checkpoint.name, "sha256": identity, "epoch": checkpoint["epoch"],
        "device": args.device, "precision": "float32",
        "class_order": {task: list(entries) for task, entries in labels.items()},
        "preprocessing": PREPROCESSING, "confidence_thresholds": dict(zip(labels, thresholds, strict=True)),
        "aggregation": "mean_logits_then_softmax", "threshold_rule": "confidence_below_threshold_is_rejected",
    }


@torch.inference_mode()
def predict_intervals(model, samples, rows, device, thresholds):
    normalization = WEIGHTS.transforms()
    scaler = VideoReformatter()
    frames = [[] for _ in rows]
    for index, frame in samples:
        frames[index].append(normalize_image(resize_and_pad(frame, scaler), normalization))
    results = []
    for row, images in zip(rows, frames, strict=True):
        if not images:
            raise ValueError(f"区间 {row.key} 没有抽到真实帧")
        outputs = model(torch.stack(images).to(device))
        predictions = []
        for logits, threshold in zip(outputs, thresholds, strict=True):
            if not torch.isfinite(logits).all().item():
                raise ValueError(f"区间 {row.key} 的模型输出包含非有限值")
            confidence, index = logits.float().mean(0).softmax(0).max(0)
            predictions.append(int(index) if float(confidence) >= threshold else IGNORE)
        results.append((tuple(predictions), len(images)))
    return results


def ratio_with_ci(eligible_patients, numerators, denominators):
    numerator, denominator = int(numerators[0]), int(denominators[0])
    available = denominators[1:] > 0
    ci = None
    if eligible_patients >= 2 and available.any():
        ci = np.quantile(numerators[1:][available] / denominators[1:][available], (0.025, 0.975)).tolist()
    return {"value": numerator / denominator if denominator else None, "numerator": numerator,
            "denominator": denominator, "eligible_patients": eligible_patients, "ci95": ci,
            "bootstrap_valid_repeats": int(available.sum())}


def task_report(predictions, task_index, codes, quality_classes=None):
    size = len(codes)
    patients = defaultdict(lambda: np.zeros((size, size + 1), dtype=np.int64))
    for entry in predictions:
        target, decision = entry.targets[task_index], entry.decisions[task_index]
        if target != IGNORE:
            patients[entry.patient][target, size if decision == IGNORE else decision] += 1

    matrices = np.array(list(patients.values()), dtype=np.int64).reshape(-1, size, size + 1)
    totals = np.zeros((BOOTSTRAP_REPEATS + 1, size, size + 1), dtype=np.int64)
    totals[0] = matrices.sum(0)
    if len(matrices) >= 2:
        rng = np.random.default_rng(BOOTSTRAP_SEED)
        draws = rng.multinomial(len(matrices), np.full(len(matrices), 1 / len(matrices)), size=BOOTSTRAP_REPEATS)
        totals[1:] = (draws @ matrices.reshape(len(matrices), -1)).reshape(-1, size, size + 1)
    supports = totals.sum(2)
    segments = supports.sum(1)
    rejected = totals[:, :, -1].sum(1)
    result = {
        "patients": len(matrices), "labeled_segments": int(segments[0]),
        "confusion": {"true_labels": codes, "predicted_labels": codes + [None], "counts": totals[0].tolist()},
        "missing_classes": [code for i, code in enumerate(codes) if not supports[0, i]],
        "per_class": {code: {"support": int(supports[0, i]),
                             "recall": ratio_with_ci(int(np.count_nonzero(matrices[:, i].sum(1))),
                                                     totals[:, i, i], supports[:, i])}
                      for i, code in enumerate(codes)},
        "coverage": ratio_with_ci(len(matrices), segments - rejected, segments),
        "rejection_rate": ratio_with_ci(len(matrices), rejected, segments),
    }
    if quality_classes is not None:
        acceptable, low_codes = quality_classes
        allowed = codes.index(acceptable)
        low = [codes.index(code) for code in low_codes]
        result["false_pass"] = {
            "acceptable_class": acceptable, "low_quality_classes": low_codes,
            **ratio_with_ci(int(np.count_nonzero(matrices[:, low, :].sum((1, 2)))),
                            totals[:, low, allowed].sum(1), supports[:, low].sum(1)),
        }
    return result


def scoring_report(predictions, labels, quality_classes=None):
    return {task: task_report(predictions, i, list(entries), quality_classes if task == "quality" else None)
            for i, (task, entries) in enumerate(labels.items())}


def strata_report(predictions, labels, quality_classes, training_sites):
    report = {}
    for field in ("device", "mode", "site"):
        groups = defaultdict(list)
        for item in predictions:
            groups[getattr(item, field) or None].append(item)
        report[field] = [
            {"value": key, "metrics": scoring_report(rows, labels, quality_classes)}
            for key, rows in groups.items()
        ]
    report["sites_absent_from_train_and_val"] = sorted(
        {entry.site for entry in predictions if entry.site} - training_sites,
    )
    return report


def team_evaluation(args):
    labels, clips, segments = read_annotations(args.labels)
    assignments = explicit_patient_splits(clips)
    if set(assignments.values()) != {"train", "val", "test"}:
        raise ValueError("视频清单需要显式指定 train、val 和 test")
    if any(clips[key].patient not in assignments for key in segments):
        raise ValueError("视频清单存在有已确认区间但未分组的患儿")
    selected = [(clips[key], rows) for key, rows in segments.items() if assignments[clips[key].patient] == "test"]
    if not selected:
        raise ValueError("测试集缺少已确认区间")
    if args.acceptable_quality not in labels["quality"]:
        raise ValueError("--acceptable-quality 必须为标签字典中的 quality 代码")
    low = sorted(set(args.low_quality))
    if args.acceptable_quality in low or set(low) - labels["quality"].keys():
        raise ValueError("--low-quality 需使用标签字典中的质量代码，且不能包含可用类别")
    quality_classes = (args.acceptable_quality, low)
    thresholds = (args.view_threshold, args.quality_threshold)
    model, model_info = load_checkpoint(
        args, labels, {"patient_splits": assignments},
        lambda: EchoModel(*(len(labels[task]) for task in TASKS), weights=None), thresholds,
    )
    predictions, sampled = [], 0
    with open_archive(args.media) as archive, tempfile.TemporaryDirectory(dir=args.report.parent) as scratch:
        for clip, rows in selected:
            with local_media(args.media, archive, clip.path, scratch) as path:
                samples = selected_interval_frames(path, rows, [interval_bins(row, args) for row in rows])
                results = predict_intervals(model, samples, rows, args.device, thresholds)
            for row, (decisions, count) in zip(rows, results, strict=True):
                sampled += count
                predictions.append(Prediction(clip.patient, row.targets, decisions, clip.device,
                                              row.mode or clip.mode, clip.site))
    sites = {clip.site for clip in clips.values() if assignments.get(clip.patient) in {"train", "val"} and clip.site}
    return {
        "status": "scored", "source": "team", "cohort": args.labels.name, "model": model_info,
        "split": {"patients": dict(sorted(Counter(assignments.values()).items())),
                  "cross_split_patients": 0, "test_patients_evaluated": len({row.patient for row in predictions}),
                  "test_clips_evaluated": len(selected), "test_segments_evaluated": len(predictions)},
        "sampled_frames": sampled, "metrics": scoring_report(predictions, labels, quality_classes),
        "strata": strata_report(predictions, labels, quality_classes, sites),
    }


def stanford_partition(archive, args):
    roles = {role: set(getattr(args, f"{role}_splits")) for role in ("train", "val", "test")}
    role_by_fold = {fold: role for role, folds in roles.items() for fold in folds}
    if len(role_by_fold) != sum(map(len, roles.values())):
        raise ValueError("train、val 和 test 的原始 Split 不能重叠")
    errors = []
    clips = read_sources(archive, errors)
    if errors:
        raise ValueError(json.dumps(errors, ensure_ascii=False))
    patient_folds = {}
    patients = defaultdict(set)
    counts = defaultdict(Counter)
    selected = []
    for clip in clips:
        path, view, fold, table, line = clip
        match = STANFORD_FILENAME.fullmatch(PurePosixPath(path).name)
        if match is None:
            raise ValueError(f"{table} 第 {line} 行不符合发布方的 AVI 文件名格式")
        patient = match[1]
        expected = str(int(hashlib.sha1(patient.encode("utf-8")).hexdigest(), 16) % 10)
        if fold != expected or patient_folds.get(patient, fold) != fold:
            raise ValueError(f"{table} 第 {line} 行的原始 Split 与患儿分组不一致")
        patient_folds[patient] = fold
        role = role_by_fold.get(fold, "unused")
        patients[role].add(patient)
        counts[role][view] += 1
        if role == "test":
            selected.append((clip, patient))
    if any(not counts[role] for role in roles):
        raise ValueError("指定的 train、val 和 test 均须包含视频")
    return selected, {
        "source_folds": {role: sorted(folds) for role, folds in roles.items()},
        "patients": {role: len(values) for role, values in sorted(patients.items())},
        "clips_per_view": {role: dict(sorted(values.items())) for role, values in sorted(counts.items())},
        "cross_split_patients": 0,
    }


def stanford_evaluation(args):
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        selected, partition = stanford_partition(archive, args)
        report = {"status": "audit", "source": "stanford", "split": partition}
        if not args.audit_only:
            labels = {"view": {code: {"name": code} for code in VIEWS}}
            roles = partition["source_folds"]
            thresholds = (args.view_threshold,)
            model, model_info = load_checkpoint(
                args, labels, {"source_splits": roles},
                lambda: ViewModel(weights=None), thresholds,
            )
            report.update(status="scored", model=model_info)
        predicted, sampled, decoded = [], 0, 0
        support, sampled_by_view = Counter(), Counter()
        for clip, patient in selected:
            item, error = inspect_avi(archive, clip, args)
            if error:
                raise ValueError(json.dumps(error, ensure_ascii=False))
            _, row, frame_count, expected = item
            path, view, _, _, _ = clip
            if expected == 0:
                raise ValueError(f"{path} 没有抽到真实帧")
            decoded += frame_count
            sampled += expected
            support[view] += 1
            sampled_by_view[view] += expected
            if not args.audit_only:
                with avi_frames(archive, path) as frames:
                    timed = samples_in_intervals(frames, [row], [interval_bins(row, args)])
                    (decisions, actual), = predict_intervals(model, timed, [row], args.device, thresholds)
                if actual != expected:
                    raise ValueError(f"{path} 的抽帧数量与预检不一致")
                predicted.append(Prediction(patient, row.targets, decisions))
        report.update(decoded_frames=decoded, sampled_frames=sampled, sampled_frames_per_view=dict(sampled_by_view),
                      test_clips_checked=len(selected), reference_support=dict(support))
        if not args.audit_only:
            report["metrics"] = scoring_report(predicted, labels)
        return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="使用固定分组独立评估心超 checkpoint")
    sub = parser.add_subparsers(dest="source", required=True)
    team = sub.add_parser("team")
    team.add_argument("--media", type=Path, required=True)
    team.add_argument("--labels", type=Path, required=True)
    team.add_argument("--quality-threshold", type=float, required=True)
    team.add_argument("--acceptable-quality", required=True)
    team.add_argument("--low-quality", nargs="+", required=True)
    stanford = sub.add_parser("stanford")
    stanford.add_argument("--archive", type=Path, required=True)
    for role in ("train", "val", "test"):
        stanford.add_argument(f"--{role}-splits", choices=SOURCE_SPLITS, nargs="+", required=True)
    stanford.add_argument("--audit-only", action="store_true")
    stanford.add_argument("--stanford-authorization-confirmed", action="store_true")
    for mode in (team, stanford):
        mode.add_argument("--report", type=Path, required=True)
        mode.add_argument("--checkpoint", type=Path, required=mode is team)
        mode.add_argument("--view-threshold", type=float, required=mode is team)
        mode.add_argument("--device", choices=["cpu", "cuda"], default="cpu")
        mode.add_argument("--sample-fps", type=float, default=10)
        mode.add_argument("--max-frames-per-segment", type=int, default=8)
    args = parser.parse_args(argv)
    if args.source == "stanford" and not args.audit_only:
        if not args.stanford_authorization_confirmed or args.checkpoint is None or args.view_threshold is None:
            parser.error("Stanford 正式评估需要已确认授权、--checkpoint 和 --view-threshold")
    for value in (args.view_threshold, getattr(args, "quality_threshold", None)):
        if value is not None and not 0 <= value <= 1:
            parser.error("置信阈值必须在 [0, 1] 内")
    if not math.isfinite(args.sample_fps) or args.sample_fps <= 0 or args.max_frames_per_segment <= 0:
        parser.error("抽帧率和帧数上限必须为正")
    if args.report.suffix.lower() != ".json" or args.report.exists():
        parser.error("--report 需要指向新的 JSON 文件")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    if args.device == "cpu":
        torch.set_num_threads(1)
    report = team_evaluation(args) if args.source == "team" else stanford_evaluation(args)
    report["sampling"] = sampling_config(args)
    if report["status"] == "scored":
        report["confidence_intervals"] = {"method": "patient_cluster_percentile_bootstrap",
                                          "repeats": BOOTSTRAP_REPEATS, "seed": BOOTSTRAP_SEED}
    with args.report.open("x", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2, allow_nan=False)
        file.write("\n")
    print(f"{report['status']}：{args.report}")


if __name__ == "__main__":
    main()
