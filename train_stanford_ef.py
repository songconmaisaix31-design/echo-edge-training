import argparse
import csv
import io
import json
import math
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from zipfile import ZipFile

import av
import numpy as np
import torch
from av.video.reformatter import VideoReformatter
from sklearn.metrics import roc_auc_score
from torch import nn
from torch.nn import functional as F
from torchvision.models import MobileNet_V3_Small_Weights, mobilenet_v3_small

from benchmark_echo import resize_and_pad
from train_echo import Segment, interval_bins, samples_in_intervals, video_frames
from train_echonet_views import TRAIN_SPLITS, VAL_SPLITS, avi_frames, published_patient, read_sources, verify_rotation

SIDE = 224
FRAMES = 8
SAMPLE_FPS = 10.0
MEAN = np.array((0.485, 0.456, 0.406), dtype=np.float32)
STD = np.array((0.229, 0.224, 0.225), dtype=np.float32)
TARGET = "left_ventricular_ejection_fraction"


def read_labels(archive, clips):
    labels = {}
    for view in ("A4C",):
        with archive.open(f"pediatric_echo_avi/{view}/FileList.csv") as entry, \
                io.TextIOWrapper(entry, encoding="utf-8-sig", newline="") as text:
            for row in csv.DictReader(text):
                try:
                    value = float(row["EF"])
                except (KeyError, TypeError, ValueError):
                    raise ValueError(f"{view} FileList.csv 的 EF 列无效") from None
                if not 0 < value <= 100:
                    raise ValueError(f"{view} 的 EF 超出范围：{value}")
                labels[row["FileName"]] = value
    selected = [clip for clip in clips if clip[1] == "A4C"]
    return selected, {clip[0].rsplit("/", 1)[-1]: labels[clip[0].rsplit("/", 1)[-1]] for clip in selected}


def decode_clip(archive, clip, bins):
    path, *_ = clip
    reformatter = VideoReformatter()
    with avi_frames(archive, path) as frames:
        decoded = list(frames)
    if not decoded:
        raise ValueError(f"{path} 没有可解码帧")
    interval = Segment(path, path, 0, math.floor(decoded[-1][0]) + 1, (0,))
    selected = [frame for _, frame in samples_in_intervals(decoded, [interval], [bins])]
    if not selected:
        raise ValueError(f"{path} 没有抽到可用帧")
    pixels = []
    for frame in selected:
        image = resize_and_pad(frame, reformatter)
        pixels.append(np.asarray(image, dtype=np.uint8).transpose(2, 0, 1))
    while len(pixels) < FRAMES:
        pixels.append(pixels[-1].copy())
    return np.stack(pixels[:FRAMES])


def prepare_cache(archive, clips, labels, cache_path, meta_path, workers):
    if cache_path.exists() and meta_path.exists():
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        if meta["frames"] == FRAMES and meta["side"] == SIDE and len(meta["clips"]) == len(clips):
            print(f"复用抽帧缓存 {cache_path}", flush=True)
            return np.load(cache_path, mmap_mode="r"), meta["clips"]
    cache = np.lib.format.open_memmap(cache_path, mode="w+", dtype=np.uint8,
                                      shape=(len(clips), FRAMES, 3, SIDE, SIDE))
    entries = []

    def work(batch):
        result = []
        for clip in batch:
            path, view, split, _, _ = clip
            frames = decode_clip(archive, clip, FRAMES)
            result.append((clip, frames, published_patient(path)))
        return result

    done = 0
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for prepared in pool.map(work, [clips[i:i + workers * 4] for i in range(0, len(clips), workers * 4)]):
            for clip, frames, patient in prepared:
                cache[done] = frames
                entries.append({"path": clip[0], "view": clip[1], "split": clip[2], "patient": patient,
                                "ef": labels[clip[0].rsplit("/", 1)[-1]]})
                done += 1
            print(f"抽帧 {done}/{len(clips)}", flush=True)
    cache.flush()
    del cache
    meta_path.write_text(json.dumps({"frames": FRAMES, "side": SIDE, "clips": entries}, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    return np.load(cache_path, mmap_mode="r"), entries


class EfModel(nn.Module):
    def __init__(self, weights=MobileNet_V3_Small_Weights.IMAGENET1K_V1):
        super().__init__()
        backbone = mobilenet_v3_small(weights=weights)
        self.features = backbone.features
        self.pool = backbone.avgpool
        self.head = nn.Linear(576, 1)

    def forward(self, images):
        batch, frames = images.shape[:2]
        features = self.features(images.flatten(0, 1))
        features = self.pool(features).flatten(1).view(batch, frames, -1).mean(1)
        return self.head(features).squeeze(1)


def batches(indices, size):
    for start in range(0, len(indices), size):
        yield indices[start:start + size]


def normalize(batch):
    pixels = torch.from_numpy(batch).float().div_(255)
    mean = torch.as_tensor(MEAN, device=pixels.device).view(1, 1, 3, 1, 1)
    std = torch.as_tensor(STD, device=pixels.device).view(1, 1, 3, 1, 1)
    return (pixels - mean) / std


@torch.inference_mode()
def predict_clips(model, cache, indices, device, batch_size):
    model.eval()
    values = []
    for chunk in batches(indices, batch_size):
        pixels = normalize(np.stack([cache[i] for i in chunk])).to(device, non_blocking=True)
        values.append(model(pixels).float().cpu())
    return torch.cat(values).numpy()


def aggregate(entries, predictions):
    clips = []
    patients = defaultdict(list)
    for entry, value in zip(entries, predictions, strict=True):
        clips.append((float(entry["ef"]), float(value)))
        patients[(entry["patient"])].append((float(entry["ef"]), float(value)))
    clip_target = np.array([row[0] for row in clips])
    clip_pred = np.array([row[1] for row in clips])
    patient_target = np.array([np.mean([row[0] for row in rows]) for rows in patients.values()])
    patient_pred = np.array([np.mean([row[1] for row in rows]) for rows in patients.values()])
    return {"clips": len(clips), "patients": len(patients),
            "clip_target": clip_target, "clip_pred": clip_pred,
            "patient_target": patient_target, "patient_pred": patient_pred}


def regression_metrics(target, prediction):
    error = prediction - target
    mae = float(np.abs(error).mean())
    rmse = float(np.sqrt((error ** 2).mean()))
    correlation = float(np.corrcoef(target, prediction)[0, 1])
    denominator = float(((target - target.mean()) ** 2).sum())
    r2 = float(1 - ((error ** 2).sum() / denominator)) if denominator else None
    return {"mae": mae, "rmse": rmse, "pearson": correlation, "r2": r2}


def abnormal_metrics(target, prediction):
    report = {}
    for threshold in (45.0, 50.0, 55.0):
        positive = target < threshold
        if positive.sum() < 2 or (~positive).sum() < 2:
            report[f"ef_lt_{int(threshold)}"] = {"positives": int(positive.sum()), "auroc": None}
            continue
        prediction_positive = prediction < threshold
        true_positive = int((prediction_positive & positive).sum())
        false_positive = int((prediction_positive & ~positive).sum())
        sensitivity = true_positive / int(positive.sum())
        specificity = int((~prediction_positive & ~positive).sum()) / int((~positive).sum())
        report[f"ef_lt_{int(threshold)}"] = {
            "positives": int(positive.sum()), "negatives": int((~positive).sum()),
            "auroc": float(roc_auc_score(positive, -prediction)),
            "sensitivity": sensitivity, "specificity": specificity,
        }
    return report


def evaluate(model, cache, entries, indices, device, batch_size):
    predictions = predict_clips(model, cache, indices, device, batch_size)
    data = aggregate([entries[i] for i in indices], predictions)
    return {
        "clips": data["clips"], "patients": data["patients"],
        "clip": regression_metrics(data["clip_target"], data["clip_pred"]),
        "patient": regression_metrics(data["patient_target"], data["patient_pred"]),
        "abnormal_patient": abnormal_metrics(data["patient_target"], data["patient_pred"]),
        "abnormal_clip": abnormal_metrics(data["clip_target"], data["clip_pred"]),
    }


def role_indices(entries, splits):
    return [i for i, entry in enumerate(entries) if entry["split"] in splits]


def save_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def train(args, cache, entries):
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    model = EfModel().to(device)
    train_indices = role_indices(entries, TRAIN_SPLITS)
    val_indices = role_indices(entries, VAL_SPLITS)
    if not train_indices or not val_indices:
        raise ValueError("训练或验证折没有可用片段")
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr)
    schedule = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs, eta_min=args.lr / 10)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    target = np.array([entries[i]["ef"] for i in train_indices], dtype=np.float32)
    args.run_dir.mkdir(parents=True, exist_ok=False)
    best = math.inf
    history = []
    for epoch in range(1, args.epochs + 1):
        model.train()
        order = np.random.default_rng(args.seed + epoch).permutation(len(train_indices))
        loss_sum = torch.zeros((), device=device)
        for chunk in batches(order, args.batch_size):
            indices = [train_indices[i] for i in chunk]
            pixels = normalize(np.stack([cache[i] for i in indices])).to(device, non_blocking=True)
            labels = torch.as_tensor(target[chunk], dtype=torch.float32, device=device)
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device.type, enabled=device.type == "cuda"):
                output = model(pixels)
                loss = F.smooth_l1_loss(output, labels)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            loss_sum += loss.detach() * len(chunk)
        schedule.step()
        val = evaluate(model, cache, entries, val_indices, device, args.batch_size)
        patient_mae = val["patient"]["mae"]
        row = {"epoch": epoch, "train_loss": float(loss_sum.item() / len(train_indices)),
               "val_clip_mae": val["clip"]["mae"], "val_patient_mae": patient_mae}
        history.append(row)
        print(json.dumps(row), flush=True)
        if patient_mae < best:
            best = patient_mae
            temporary = args.run_dir / "best.partial"
            torch.save({"model": model.state_dict(), "epoch": epoch, "frames": FRAMES, "side": SIDE,
                        "target": TARGET, "source_splits": {"train": list(TRAIN_SPLITS), "val": list(VAL_SPLITS),
                                                            "test": ["0"]},
                        "val_patient_mae": patient_mae, "seed": args.seed, "lr": args.lr},
                       temporary)
            temporary.replace(args.run_dir / "best.pt")
    save_json(args.run_dir / "validation.json", {"best_patient_mae": best, "epochs": history,
                                                 "clips": {"train": len(train_indices), "val": len(val_indices)}})


def load_model(checkpoint, device):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["frames"] != FRAMES or saved["side"] != SIDE or saved["target"] != TARGET:
        raise ValueError("checkpoint 与当前 EF 协议不一致")
    model = EfModel(weights=None).to(device)
    model.load_state_dict(saved["model"])
    return model.eval(), saved


def main():
    parser = argparse.ArgumentParser(description="EchoNet-Pediatric 左室射血分数（EF）回归与异常功能判定")
    commands = parser.add_subparsers(dest="action", required=True)
    training = commands.add_parser("train")
    testing = commands.add_parser("test")
    predicting = commands.add_parser("predict")
    for command in (training, testing, predicting):
        command.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    for command in (training, testing):
        command.add_argument("--archive", type=Path, required=True)
        command.add_argument("--cache-dir", type=Path, required=True)
        command.add_argument("--batch-size", type=int, default=8)
        command.add_argument("--workers", type=int, default=6)
    training.add_argument("--run-dir", type=Path, required=True)
    training.add_argument("--epochs", type=int, default=12)
    training.add_argument("--lr", type=float, default=3e-4)
    training.add_argument("--seed", type=int, default=42)
    for command in (testing, predicting):
        command.add_argument("--checkpoint", type=Path, required=True)
    testing.add_argument("--report", type=Path, required=True)
    predicting.add_argument("--video", type=Path, required=True)
    args = parser.parse_args()
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    if args.action == "predict":
        model, _ = load_model(args.checkpoint, args.device)
        with av.open(str(args.video)) as container:
            stream = container.streams.video[0]
            decoded = list(video_frames(container, stream))
        if not decoded:
            raise ValueError("视频没有可解码帧")
        interval = Segment(str(args.video), str(args.video), 0, math.floor(decoded[-1][0]) + 1, (0,))
        selected = [frame for _, frame in samples_in_intervals(decoded, [interval], [FRAMES])]
        reformatter = VideoReformatter()
        pixels = [np.asarray(resize_and_pad(frame, reformatter), dtype=np.uint8) for frame in selected]
        while len(pixels) < FRAMES:
            pixels.append(pixels[-1].copy())
        with torch.inference_mode():
            batch = normalize(np.stack([np.stack(pixels[:FRAMES])])).to(torch.device(args.device))
            value = float(model(batch)[0].item())
        print(json.dumps({"target": TARGET, "ef": round(value, 2), "sampled_frames": len(selected)}, ensure_ascii=False))
        return
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        errors = []
        clips = read_sources(archive, errors)
        if errors:
            raise ValueError(f"Stanford 文件清单有 {len(errors)} 项错误")
        verify_rotation(clips)
        selected, labels = read_labels(archive, clips)
        if args.action == "train" and args.run_dir.exists():
            raise FileExistsError(args.run_dir)
        cache, entries = prepare_cache(archive, selected, labels, args.cache_dir / "a4c-ef-frames.npy",
                                       args.cache_dir / "a4c-ef-frames.json", args.workers)
        if args.action == "train":
            train(args, cache, entries)
            return
        if args.report.exists():
            raise FileExistsError(args.report)
        model, saved = load_model(args.checkpoint, torch.device(args.device))
        indices = [i for i, entry in enumerate(entries) if entry["split"] == "0"]
        if cache.shape[0] != len(entries):
            raise ValueError("抽帧缓存与清单不一致")
        report = {"target": saved["target"], "epoch": saved["epoch"], "frames": FRAMES, "side": SIDE,
                  "source_splits": saved["source_splits"], "checkpoint_sha256": __import__("hashlib").sha256(
                      args.checkpoint.read_bytes()).hexdigest(),
                  "test": evaluate(model, cache, entries, indices, torch.device(args.device), args.batch_size),
                  "test_clips": len(indices)}
        save_json(args.report, report)
        print(json.dumps({"clip": report["test"]["clip"], "patient": report["test"]["patient"],
                          "abnormal_patient": report["test"]["abnormal_patient"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()

