from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import sys
import time
import types
from collections import Counter
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import av
import numpy as np
import torch
import torch.nn.functional as functional
from av.video.reformatter import VideoReformatter

from embedding_labels import validate_labels

IMAGE_SIZE = 224
CLIP_LEN = 16
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)
VIDEO_SUFFIXES = {".avi", ".mp4", ".mov", ".mkv", ".ogv", ".webm"}


def configure(container):
    stream = container.streams.video[0]
    stream.thread_type = "AUTO"
    stream.codec_context.thread_count = 4
    return stream


def scaled_size(frame):
    scale = IMAGE_SIZE / max(frame.width, frame.height)
    return tuple(max(1, round(side * scale)) for side in (frame.width, frame.height))


def to_image(frame, reformatter):
    size = scaled_size(frame)
    resized = reformatter.reformat(frame, width=size[0], height=size[1], format="rgb24",
                                   interpolation="AREA", threads=1)
    image = resized.to_ndarray(format="rgb24")
    padded = np.zeros((IMAGE_SIZE, IMAGE_SIZE, 3), np.uint8)
    top = (IMAGE_SIZE - size[1]) // 2
    left = (IMAGE_SIZE - size[0]) // 2
    padded[top:top + size[1], left:left + size[0]] = image
    return padded


def clip_starts(total_frames, n_clips, clip_len=CLIP_LEN):
    """等间距起点；不足一个 clip 时全部从 0 开始。"""
    if total_frames <= clip_len:
        return [0] * n_clips
    span = total_frames - clip_len
    return [int(round(value)) for value in np.linspace(0, span, n_clips)]


def video_clips(video, n_clips):
    total = count_frames(video)
    if not total:
        raise ValueError("视频没有可解码帧")
    indices = np.minimum(np.asarray(clip_starts(total, n_clips))[:, None] + np.arange(CLIP_LEN),
                         total - 1)
    unique, inverse = np.unique(indices, return_inverse=True)
    frames = []
    with av.open(video) as container:
        stream = configure(container)
        reformatter = VideoReformatter()
        for index, frame in enumerate(container.decode(stream)):
            if index == unique[len(frames)]:
                frames.append(to_image(frame, reformatter))
                if len(frames) == len(unique):
                    break
    if len(frames) != len(unique):
        raise ValueError("视频帧数少于容器声明的帧数")
    return np.stack(frames), inverse.reshape(n_clips, CLIP_LEN), total


def count_frames(video):
    with av.open(video) as container:
        stream = container.streams.video[0]
        total = stream.frames or sum(1 for _ in container.decode(stream))
    if isinstance(video, io.BytesIO):
        video.seek(0)
    return total


@torch.inference_mode()
def encode_video(model, video, n_clips, device, dtype):
    images, indices, total = video_clips(video, n_clips)
    frames = torch.from_numpy(images).to(device).float().div_(255)
    mean = torch.tensor(IMAGENET_MEAN, device=device)
    std = torch.tensor(IMAGENET_STD, device=device)
    frames = ((frames - mean) / std).to(dtype)
    clips = frames[torch.from_numpy(indices).to(device)].permute(0, 4, 1, 2, 3).contiguous()
    vectors = torch.cat([model(chunk) for chunk in clips.split(4)]).float().cpu().numpy()
    if not np.isfinite(vectors).all():
        raise ValueError("PanEcho 输出含非有限值")
    return vectors, total


def build_encoder(weights: Path, panecho_repo: Path):
    sys.path.insert(0, str(panecho_repo))
    module = types.ModuleType("src")
    module.__path__ = [str(panecho_repo / "src")]
    sys.modules.setdefault("src", module)
    from src.models import FrameTransformer

    payload = torch.load(weights, map_location="cpu", weights_only=False)
    state = payload["weights"]
    state.pop("encoder.time_encoder.pe", None)             # 允许可变 clip 长度
    encoder = FrameTransformer("convnext_tiny", 8, 4, 0.0, "mean", CLIP_LEN)
    encoder.load_state_dict(
        {key[len("encoder."):]: value for key, value in state.items()
         if key.startswith("encoder.")}, strict=False)
    encoder.eval()
    return encoder


def key_for(relative: str) -> str:
    return hashlib.sha1(relative.encode("utf-8")).hexdigest()[:16]


def pediatric_sources(archive):
    from train_echonet_views import (TRAIN_SPLITS, VAL_SPLITS, VIEWS, published_patient,
                                    read_sources, verify_rotation)

    errors = []
    clips = read_sources(archive, errors)
    if errors:
        raise ValueError(f"EchoNet-Pediatric 归档未通过检查，共 {len(errors)} 项：{errors[0]['error']}")
    verify_rotation(clips)
    labels = {}
    for view in VIEWS:
        root = f"pediatric_echo_avi/{view}"
        with archive.open(f"{root}/FileList.csv") as entry, \
                io.TextIOWrapper(entry, encoding="utf-8-sig", newline="") as table:
            for row in csv.DictReader(table):
                labels[f"{root}/Videos/{row['FileName']}"] = float(row["EF"])
    return {
        path: {"subject": published_patient(path), "ef": labels[path],
               "split": "train" if fold in TRAIN_SPLITS else "val" if fold in VAL_SPLITS else "test"}
        for path, _, fold, _, _ in clips
    }


def directory_sources(root, index):
    videos = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in VIDEO_SUFFIXES)
    names = Counter(video.name for video in videos)
    sources = {}
    for video in videos:
        relative = video.relative_to(root).as_posix()
        metadata = index.get(relative)
        if relative not in index and video.name in index:
            if names[video.name] != 1:
                raise ValueError(f"索引中的文件名对应多个视频，请使用相对路径：{video.name}")
            metadata = index[video.name]
        sources[relative] = metadata
    return sources


def save_index(path, manifest):
    pending = path.with_suffix(".json.pending")
    pending.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    pending.replace(path)


def embed_sources(args, sources, open_video):
    if not sources:
        raise ValueError("没有找到视频")
    for source, info in sources.items():
        if args.index or args.pediatric_archive:
            validate_labels(info, source)

    selected = list(sources)[:args.limit or None]
    device = torch.device(args.device)
    if device.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA 不可用")
    if args.fp16 and device.type != "cuda":
        raise ValueError("--fp16 需要 CUDA")
    dtype = torch.float16 if args.fp16 else torch.float32
    precision = "fp16" if args.fp16 else "fp32"
    args.out.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out / "index.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    pending = []
    for source in selected:
        key = key_for(source)
        existing = manifest.get(key)
        target = args.out / f"{key}.npy"
        if existing is None or not target.is_file():
            pending.append((source, key, target))
            continue
        if (existing["source"] != source or existing["n_clips"] != args.clips
                or existing.get("precision", "fp32") != precision):
            raise ValueError(f"缓存设置与本次输入不一致：{key}，请使用新的 --out 目录")
        if sources[source] is not None:
            existing.update({field: sources[source][field] for field in ("subject", "ef", "split")
                             if field in sources[source]})
    if set(manifest).difference(key_for(source) for source in sources):
        raise ValueError("缓存包含当前输入以外的嵌入，请使用新的 --out 目录")
    if manifest:
        save_index(manifest_path, manifest)

    print(f"{len(selected)} 个视频，待编码 {len(pending)}，设备 {device}，精度 {precision}", flush=True)
    started = time.monotonic()
    if pending:
        model = build_encoder(args.weights, args.panecho_repo).to(device=device, dtype=dtype)
        model.encoder.to(memory_format=torch.channels_last)
    for position, (source, key, target) in enumerate(pending, start=1):
        embedding, total = encode_video(model, open_video(source), args.clips, device, dtype)
        temporary = target.with_suffix(".npy.pending")
        with temporary.open("wb") as file:
            np.save(file, embedding)
        temporary.replace(target)
        entry = {"source": source, "n_frames": total, "n_clips": int(embedding.shape[0]),
                 "dim": int(embedding.shape[1]), "precision": precision}
        if sources[source] is not None:
            entry.update({field: sources[source][field] for field in ("subject", "ef", "split")
                          if field in sources[source]})
        manifest[key] = entry
        if position % 25 == 0 or position == len(pending):
            rate = (time.monotonic() - started) / position
            save_index(manifest_path, manifest)
            print(f"[{position}/{len(pending)}] {rate:.2f}s/视频", flush=True)

    summary = {"videos": len(manifest), "clips": args.clips, "device": str(device), "precision": precision,
               "encoded": len(pending), "seconds": round(time.monotonic() - started, 1)}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"完成：{len(manifest)} 个嵌入 → {args.out}", flush=True)


def main():
    parser = argparse.ArgumentParser(description="用 PanEcho 抽取视频嵌入")
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--panecho-repo", type=Path, required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--video-dir", type=Path)
    inputs.add_argument("--pediatric-archive", type=Path, help="EchoNet-Pediatric 原始 ZIP，直接读取视频和标签")
    parser.add_argument("--index", type=Path, help="可选：{相对路径或文件名: {subject, ef, split}}")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clips", type=int, default=16)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--fp16", action="store_true", help="CUDA 编码使用 FP16，输出仍为 float32")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    if args.clips < 1 or args.limit < 0:
        parser.error("--clips 至少为 1，--limit 至少为 0")
    if args.pediatric_archive:
        if args.index:
            parser.error("--pediatric-archive 直接使用归档标签，无需 --index")
        with ZipFile(args.pediatric_archive) as archive:
            embed_sources(args, pediatric_sources(archive), lambda source: io.BytesIO(archive.read(source)))
    else:
        index = json.loads(args.index.read_text(encoding="utf-8")) if args.index else {}
        embed_sources(args, directory_sources(args.video_dir, index),
                      lambda source: str(args.video_dir / PurePosixPath(source)))


if __name__ == "__main__":
    main()

