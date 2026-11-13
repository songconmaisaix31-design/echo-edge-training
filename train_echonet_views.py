import argparse
import csv
import hashlib
import io
import json
import math
import re
import sqlite3
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from pathlib import Path, PurePosixPath
from zipfile import BadZipFile, ZipFile

import av
import torch
from av.video.reformatter import VideoReformatter
from torch import nn
from torchvision.models import mobilenet_v3_small

from benchmark_echo import resize_and_pad
from train_echo import WEIGHTS, Segment, fit_model, interval_bins, samples_in_intervals, video_frames


VIEWS = ("A4C", "PSAX")
SOURCE_SPLITS = tuple(map(str, range(10)))
TRAIN_SPLITS = SOURCE_SPLITS[2:]
VAL_SPLITS = ("1",)
TEST_SPLITS = ("0",)
ROTATION_COUNTS = {"train": 6354, "val": 798, "test": 658}
PUBLISHED_NAME = re.compile(r"(CR[0-9a-z]{7})-CR[0-9a-z]{7}-[0-9]{6}[.]avi", re.ASCII)


class ViewModel(nn.Module):
    def __init__(self, *, weights=WEIGHTS):
        super().__init__()
        backbone = mobilenet_v3_small(weights=weights)
        self.features = backbone.features
        self.pool = backbone.avgpool
        self.view_head = nn.Linear(backbone.classifier[0].in_features, len(VIEWS))

    def forward(self, images):
        return (self.view_head(self.pool(self.features(images)).flatten(1)),)


def read_sources(archive, errors):
    clips = []
    names = Counter(archive.namelist())
    for view in VIEWS:
        base = f"pediatric_echo_avi/{view}"
        table = f"{base}/FileList.csv"
        folder = f"{base}/Videos/"
        available = {name for name in names if name.startswith(folder) and name.lower().endswith(".avi")}
        seen = set()
        if names[table] != 1:
            errors.append({"csv": table, "error": "ZIP 内需要且只能包含一份 FileList.csv"})
            continue
        with archive.open(table) as entry, io.TextIOWrapper(entry, encoding="utf-8-sig", newline="") as text:
            reader = csv.DictReader(text)
            if not {"FileName", "Split"}.issubset(reader.fieldnames or ()):
                errors.append({"csv": table, "line": 1, "error": "缺少 FileName 或 Split 列"})
                continue
            for row in reader:
                name, split = row.get("FileName"), row.get("Split")
                line = reader.line_num
                if not name or PurePosixPath(name).name != name or "\\" in name or not name.lower().endswith(".avi"):
                    errors.append({"csv": table, "line": line, "error": "FileName 需要是 AVI 文件名"})
                    continue
                path = folder + name
                if split not in SOURCE_SPLITS or path in seen or path not in available or names[path] != 1:
                    errors.append({"csv": table, "line": line, "file": path,
                                   "error": "Split 无效、CSV 重复登记、AVI 缺失或 ZIP 路径不唯一"})
                    seen.add(path)
                    continue
                seen.add(path)
                clips.append((path, view, split, table, line))
        for path in sorted(available - seen):
            errors.append({"csv": table, "file": path, "error": "AVI 没有对应的 FileList.csv 行"})
    return clips


def open_avi(archive, path):
    return av.open(io.BytesIO(archive.read(path)))


def published_patient(path):
    matched = PUBLISHED_NAME.fullmatch(PurePosixPath(path).name)
    if matched is None:
        raise ValueError("发布文件名缺少符合格式的匿名患儿编号")
    return matched[1]


def verify_rotation(clips):
    patients = {}
    counts = Counter()
    for path, _, split, _, _ in clips:
        patient = published_patient(path)
        fold = str(int(hashlib.sha1(patient.encode("utf-8"), usedforsecurity=False).hexdigest(), 16) % 10)
        if split != fold or patients.setdefault(patient, split) != split:
            raise ValueError("原始 Split 与发布方患儿分组算法不一致")
        counts["test" if split == "0" else "val" if split == "1" else "train"] += 1
    if counts != ROTATION_COUNTS:
        raise ValueError(f"第 0 次轮转片段数量与本批归档不一致：{dict(counts)}")
    return {"clips": dict(counts), "patients": {role: sum(fold in folds for fold in patients.values())
            for role, folds in {"train": TRAIN_SPLITS, "val": VAL_SPLITS, "test": TEST_SPLITS}.items()}}


def inspect_avi(archive, clip, args):
    path, view, split, table, line = clip
    try:
        with open_avi(archive, path) as container:
            stream = container.streams.video[0]
            stream.codec_context.thread_count = 1
            offsets = [offset for offset, _ in video_frames(container, stream)]
        if not offsets:
            raise ValueError("AVI 没有可解码视频帧")
        segment = Segment(path, path, 0, math.floor(offsets[-1]) + 1, (VIEWS.index(view),))
        sampled = sum(1 for _ in samples_in_intervals(((offset, None) for offset in offsets),
                                                       [segment], [interval_bins(segment, args)]))
        return (clip, segment, len(offsets), sampled), None
    except (av.FFmpegError, OSError, BadZipFile, IndexError, ValueError) as error:
        return None, {"csv": table, "line": line, "file": path, "error": str(error)}


def preflight(archive, args):
    errors = []
    clips = read_sources(archive, errors)
    rotation = None
    if not errors:
        rotation = verify_rotation(clips)
    prepared = []
    with ThreadPoolExecutor(max_workers=4) as executor:
        for start in range(0, len(clips), 128):
            for item, error in executor.map(lambda clip: inspect_avi(archive, clip, args), clips[start:start + 128]):
                if error:
                    errors.append(error)
                else:
                    prepared.append(item)
            if start % 1024 == 0:
                print(f"已检查 {min(start + 128, len(clips))}/{len(clips)} 段 AVI", flush=True)

    counts = {view: Counter() for view in VIEWS}
    for (_, view, split, _, _), _, _, _ in prepared:
        counts[view][split] += 1
    for view in VIEWS:
        if not counts[view]:
            errors.append({"view": view, "error": "缺少可用的切面视频"})

    train_splits, val_splits = set(args.train_splits or ()), set(args.val_splits or ())
    roles = {"train": train_splits, "val": val_splits, "test": set(TEST_SPLITS)}
    if train_splits or val_splits:
        if train_splits != set(TRAIN_SPLITS) or val_splits != set(VAL_SPLITS):
            errors.append({"error": "只支持发布方第 0 次轮转：训练 2–9、验证 1、测试 0"})
        for role, selection in roles.items():
            for view in VIEWS:
                if not counts[view].keys() & selection:
                    errors.append({"role": role, "view": view, "error": "分组缺少该切面"})
    blockers = []
    if not args.stanford_authorization_confirmed:
        blockers.append("尚未确认斯坦福授权覆盖拟进行的训练及权重用途")
    if rotation is None or not args.patient_grouping_confirmed:
        blockers.append("尚未核实原始 Split 的患儿级隔离")
    if not train_splits or not val_splits:
        blockers.append("尚未指定哪些原始 Split 用于训练和验证")
    report = {
        "data_preflight": "blocked" if errors else "passed",
        "training_readiness": "blocked" if errors or blockers else "ready",
        "sample_fps": args.sample_fps, "max_frames_per_clip": args.max_frames_per_segment,
        "source_split_counts": {view: dict(sorted(counts[view].items())) for view in VIEWS},
        "verified_rotation_zero": rotation,
        "selected_split_roles": {role: sorted(selection) for role, selection in roles.items()},
        "decoded_clips": len(prepared), "decoded_frames": sum(item[2] for item in prepared),
        "frames_selected_for_preprocessing": sum(item[3] for item in prepared),
        "training_blockers": blockers, "errors": errors,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"数据预检 {report['data_preflight']}，开训状态 {report['training_readiness']}。报告 {args.report}", flush=True)
    return report, prepared


def train_views(archive, args, prepared):
    if not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    torch.manual_seed(args.seed)
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    args.run_dir.mkdir(parents=True, exist_ok=False)
    train_splits = set(args.train_splits)
    val_splits = set(args.val_splits)
    records = defaultdict(list)
    with tempfile.TemporaryDirectory(dir=args.run_dir) as scratch, closing(sqlite3.connect(
            Path(scratch) / "frames.sqlite")) as frame_cache:
        frame_cache.execute("CREATE TABLE frames (id INTEGER PRIMARY KEY, png BLOB NOT NULL)")
        print("开始按锁定的训练和验证分组抽帧", flush=True)
        for (path, _, source_split, _, _), segment, _, expected in prepared:
            if source_split not in train_splits | val_splits:
                continue
            role = "train" if source_split in train_splits else "val"
            reformatter = VideoReformatter()
            count = 0
            patient = published_patient(path)
            with open_avi(archive, path) as container:
                stream = container.streams.video[0]
                stream.codec_context.thread_count = 1
                samples = samples_in_intervals(video_frames(container, stream), [segment], [interval_bins(segment, args)])
                for _, frame in samples:
                    output = io.BytesIO()
                    resize_and_pad(frame, reformatter).save(output, format="PNG", compress_level=1)
                    key = frame_cache.execute("INSERT INTO frames (png) VALUES (?)", (output.getvalue(),)).lastrowid
                    records[role].append((key, segment.targets, patient))
                    count += 1
            if count != expected:
                raise ValueError(f"{path} 的实际抽帧数量与预检不一致")
        frame_cache.commit()
        labels = {"view": {view: {"name": view} for view in VIEWS}}
        print(f"抽帧完成，训练 {len(records['train'])} 帧，验证 {len(records['val'])} 帧。开始 CUDA 微调。", flush=True)
        fit_model(args, records, labels, ViewModel(), scratch,
                  {"source_splits": {"train": sorted(train_splits), "val": sorted(val_splits),
                                     "test": list(TEST_SPLITS)}, "initialization": "imagenet1k_v1"}, frame_cache)


def main():
    parser = argparse.ArgumentParser(description="EchoNet-Pediatric A4C 和 PSAX 切面分类")
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--train-splits", choices=SOURCE_SPLITS, nargs="+", default=TRAIN_SPLITS)
    parser.add_argument("--val-splits", choices=SOURCE_SPLITS, nargs="+", default=VAL_SPLITS)
    parser.add_argument("--stanford-authorization-confirmed", action="store_true")
    parser.add_argument("--patient-grouping-confirmed", action="store_true")
    parser.add_argument("--sample-fps", type=float, default=10)
    parser.add_argument("--max-frames-per-clip", dest="max_frames_per_segment", type=int, default=8)
    parser.add_argument("--run-dir", type=Path)
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--head-only-epochs", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--head-lr", type=float, default=3e-4)
    parser.add_argument("--backbone-lr", type=float, default=3e-5)
    args = parser.parse_args()
    if not args.preflight_only and not args.stanford_authorization_confirmed:
        parser.error("开训前需要确认斯坦福书面许可覆盖训练与权重用途")
    if args.report.suffix.lower() != ".json" or args.report.resolve() == args.archive.resolve():
        parser.error("--report 需要独立的 JSON 输出路径")
    if args.run_dir is not None and args.report.resolve().is_relative_to(args.run_dir.resolve()):
        parser.error("预检报告应放在训练目录之外")
    if min(args.batch_size, args.epochs, args.max_frames_per_segment) <= 0 or args.head_only_epochs < 0:
        parser.error("批大小、轮数和每段抽帧上限需要有效")
    if any(not math.isfinite(value) or value <= 0 for value in (args.sample_fps, args.head_lr, args.backbone_lr)):
        parser.error("抽帧率和学习率需要是有限正数")
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        report, prepared = preflight(archive, args)
        if report["errors"]:
            return 1
        if args.preflight_only:
            return 0
        if report["training_blockers"]:
            return 1
        if args.run_dir is None or args.cache_dir is None:
            parser.error("训练需要 --run-dir 和 --cache-dir")
        train_views(archive, args, prepared)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
