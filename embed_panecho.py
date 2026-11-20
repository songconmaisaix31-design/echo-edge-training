r"""阶段一：用冻结的 PanEcho 把视频变成向量，缓存到 work/emb/。

这一步不使用任何标签，也不训练任何参数，只做编码。产出是每个视频一个
``(n_clips, 768)`` 的 float32 数组，供 ``train_emb_head.py`` 直接读取。

设计边界：

- PanEcho 权重与代码不进入仓库（CC BY-NC-SA 4.0 / AGPLv3），通过参数指向授权位置。
- 患者号、视频清单和缓存目录写在 ``work/`` 下，已被 .gitignore 排除。
- clip 采样是确定的（等间距起点），同一视频重复运行结果一致，便于断点续跑。

用法::

    python -X utf8 embed_panecho.py `
      --weights '.\work\models\panecho.pt' --panecho-repo '.\work\PanEcho' `
      --video-dir '.\data\videos' --index '.\work\echo-index-private.json' `
      --out '.\work\emb' --clips 16
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
import types
from pathlib import Path

import av
import numpy as np
import torch
import torch.nn.functional as functional
from av.video.reformatter import VideoReformatter

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


def to_input(frame, reformatter):
    size = scaled_size(frame)
    resized = reformatter.reformat(frame, width=size[0], height=size[1], format="rgb24",
                                   interpolation="AREA", threads=1)
    image = resized.to_ndarray(format="rgb24")
    padded = np.zeros((IMAGE_SIZE, IMAGE_SIZE, 3), np.uint8)
    top = (IMAGE_SIZE - size[1]) // 2
    left = (IMAGE_SIZE - size[0]) // 2
    padded[top:top + size[1], left:left + size[0]] = image
    tensor = torch.from_numpy(padded).permute(2, 0, 1).float().div_(255)
    mean = torch.tensor(IMAGENET_MEAN).view(3, 1, 1)
    std = torch.tensor(IMAGENET_STD).view(3, 1, 1)
    return (tensor - mean) / std


def clip_starts(total_frames, n_clips, clip_len=CLIP_LEN):
    """等间距起点；不足一个 clip 时全部从 0 开始并由首帧补齐。"""
    if total_frames <= clip_len:
        return [0] * n_clips
    span = total_frames - clip_len
    return [int(round(value)) for value in np.linspace(0, span, n_clips)]


def read_clip(path, start, clip_len=CLIP_LEN):
    """从 start 帧起读 clip_len 帧，返回 (clip_len, 3, 224, 224)；不足时补末帧。

    帧率缺失的容器（部分 .ogv）无法按时间戳定位，退回顺序解码并丢弃前缀帧。
    """
    with av.open(str(path)) as container:
        stream = configure(container)
        reformatter = VideoReformatter()
        rate = stream.average_rate or stream.guessed_rate
        if start and rate:
            container.seek(int(start * av.time_base / float(rate)), stream=stream)
        frames = []
        position = 0
        for frame in container.decode(stream):
            if rate and frame.pts is not None:
                index = int(round(float(frame.pts * frame.time_base) * float(rate)))
            else:
                index = position
            position += 1
            if index < start:
                continue
            frames.append(to_input(frame, reformatter))
            if len(frames) == clip_len:
                break
        if not frames:
            raise ValueError(f"无法解码：{path}")
        while len(frames) < clip_len:
            frames.append(frames[-1])
        return torch.stack(frames, 0)                      # (T, 3, H, W)


def count_frames(path):
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        if stream.frames:
            return int(stream.frames)
        return sum(1 for _ in container.decode(stream))


def encode_video(model, path, n_clips, device):
    total = count_frames(path)
    starts = clip_starts(total, n_clips)
    vectors = []
    for start in starts:
        clip = read_clip(path, start).permute(1, 0, 2, 3).unsqueeze(0).to(device)
        with torch.no_grad():
            vectors.append(model(clip).squeeze(0).float().cpu().numpy())
    return np.stack(vectors, 0).astype(np.float32), total


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


def key_for(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix() if root in path.parents else path.name
    return hashlib.sha1(relative.encode("utf-8")).hexdigest()[:16]


def main():
    parser = argparse.ArgumentParser(description="用 PanEcho 抽取视频嵌入")
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--panecho-repo", type=Path, required=True)
    parser.add_argument("--video-dir", type=Path, required=True)
    parser.add_argument("--index", type=Path, help="可选：{相对路径或文件名: {subject, split}}")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clips", type=int, default=16)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    index = json.loads(args.index.read_text(encoding="utf-8")) if args.index else {}
    args.out.mkdir(parents=True, exist_ok=True)

    videos = sorted(p for p in args.video_dir.rglob("*") if p.suffix.lower() in VIDEO_SUFFIXES)
    if args.limit:
        videos = videos[: args.limit]
    model = build_encoder(args.weights, args.panecho_repo).to(args.device)
    print(f"{len(videos)} 个视频，设备 {args.device}", flush=True)

    manifest_path = args.out / "index.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    started = time.time()
    for position, video in enumerate(videos, start=1):
        key = key_for(video, args.video_dir)
        target = args.out / f"{key}.npy"
        if target.exists() and key in manifest:
            continue
        embedding, total = encode_video(model, video, args.clips, args.device)
        np.save(target, embedding)
        entry = {"source": video.relative_to(args.video_dir).as_posix(),
                 "n_frames": total, "n_clips": int(embedding.shape[0]),
                 "dim": int(embedding.shape[1])}
        entry.update(index.get(video.name, index.get(entry["source"], {})))
        manifest[key] = entry
        if position % 5 == 0 or position == len(videos):
            rate = (time.time() - started) / max(1, position)
            print(f"[{position}/{len(videos)}] {entry['source']} {embedding.shape} "
                  f"{rate:.1f}s/视频", flush=True)
        if position % 25 == 0:
            manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2),
                                     encoding="utf-8")

    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    summary = {"videos": len(manifest), "clips": args.clips,
               "seconds": round(time.time() - started, 1)}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"完成：{len(manifest)} 个嵌入 → {args.out}", flush=True)


if __name__ == "__main__":
    main()

