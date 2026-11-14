import argparse
import csv
import io
import json
import math
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from itertools import batched, chain
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import av
import numpy as np
import torch
from PIL import Image
from skimage.draw import polygon
from torch import nn
from torch.nn import functional as F

from evaluate_echo import file_digest
from train_echonet_views import (TEST_SPLITS, TRAIN_SPLITS, VAL_SPLITS, VIEWS, open_avi, published_patient,
                                 read_sources, verify_rotation)


SIDE = 112
FOLDS = {"train": list(TRAIN_SPLITS), "val": list(VAL_SPLITS), "test": list(TEST_SPLITS)}
TARGET = "left_ventricular_endocardial_cavity"


def block(inputs, outputs):
    return nn.Sequential(nn.Conv2d(inputs, outputs, 3, padding=1, bias=False), nn.BatchNorm2d(outputs),
                         nn.ReLU(inplace=True), nn.Conv2d(outputs, outputs, 3, padding=1, bias=False),
                         nn.BatchNorm2d(outputs), nn.ReLU(inplace=True))


class LeftVentricleUNet(nn.Module):
    def __init__(self):
        super().__init__()
        channels = (16, 32, 64, 96)
        self.encoder = nn.ModuleList(block(left, right) for left, right in zip((1, *channels[:-1]), channels))
        self.decoder = nn.ModuleList(block(deep + skip, skip)
                                     for deep, skip in zip(channels[:0:-1], channels[-2::-1], strict=True))
        self.mask = nn.Conv2d(channels[0], 1, 1)

    def forward(self, images):
        skips = [self.encoder[0](images)]
        for layer in self.encoder[1:]:
            skips.append(layer(F.max_pool2d(skips[-1], 2)))
        result = skips.pop()
        for layer, skip in zip(self.decoder, reversed(skips), strict=True):
            up = F.interpolate(result, size=skip.shape[-2:], mode="nearest")
            result = layer(torch.cat((up, skip), dim=1))
        return self.mask(result)


def read_traces(archive, clips):
    traces = {path: defaultdict(list) for path, *_ in clips}
    missing = Counter()
    for view in VIEWS:
        with archive.open(f"pediatric_echo_avi/{view}/VolumeTracings.csv") as source:
            reader = csv.DictReader(io.TextIOWrapper(source, encoding="utf-8-sig", newline=""))
            if reader.fieldnames != ["FileName", "X", "Y", "Frame"]:
                raise ValueError(f"{view} 的描迹表列名不匹配")
            for row in reader:
                path = f"pediatric_echo_avi/{view}/Videos/{row['FileName']}"
                if path not in traces:
                    raise ValueError(f"{view} 的描迹第 {reader.line_num} 行无法对应 AVI")
                index = row["Frame"]
                if index in {"No Systolic", "No Diastolic"} and not (row["X"] or row["Y"]):
                    missing[path, index] += 1
                    continue
                if not index.isdecimal():
                    raise ValueError(f"{view} 的描迹第 {reader.line_num} 行帧号无效")
                point = (float(row["X"]), float(row["Y"]))
                if not all(map(math.isfinite, point)):
                    raise ValueError(f"{view} 的描迹第 {reader.line_num} 行坐标无效")
                traces[path][int(index)].append(point)
    return traces, missing


def pixel_data(frame):
    if (frame.width, frame.height) != (SIDE, SIDE):
        raise ValueError(f"模型输入需要 {SIDE}×{SIDE} 像素")
    return frame.to_ndarray(format="gray")


def decode_traces(archive, clip, traces):
    path, view, *_ = clip
    contours = traces[path]
    frames = []
    seen = set()
    if contours:
        with open_avi(archive, path) as container:
            stream = container.streams.video[0]
            stream.codec_context.thread_count = 1
            for index, frame in enumerate(container.decode(stream)):
                if index in contours:
                    points = np.rint(contours[index]).astype(np.intp)
                    if len(np.unique(points, axis=0)) < 3:
                        raise ValueError(f"{view} 的已标注帧轮廓顶点不足")
                    rows, columns = polygon(points[:, 1], points[:, 0], (SIDE, SIDE))
                    if not len(rows):
                        raise ValueError(f"{view} 的已标注帧轮廓无有效像素")
                    mask = np.zeros((SIDE, SIDE), dtype=np.uint8)
                    mask[rows, columns] = 1
                    frames.append((pixel_data(frame), mask))
                    seen.add(index)
                if len(seen) == len(contours):
                    break
    return frames, len(contours) - len(seen)


def load_split(archive, clips, traces, missing, role, workers, device):
    selected = [clip for clip in clips if clip[2] in FOLDS[role]]
    images, masks, annotations = [], [], []
    by_view = defaultdict(Counter)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        prepared = chain.from_iterable(pool.map(lambda clip: decode_traces(archive, clip, traces), batch)
                                       for batch in batched(selected, workers * 8))
        for index, (clip, (frames, out_of_range)) in enumerate(zip(selected, prepared, strict=True), 1):
            path, view, *_ = clip
            counts = by_view[view]
            counts["source_clips"] += 1
            counts["labeled_clips"] += bool(frames)
            counts["labeled_frames"] += len(frames)
            counts["no_systolic"] += missing[path, "No Systolic"]
            counts["no_diastolic"] += missing[path, "No Diastolic"]
            counts["out_of_range_frames"] += out_of_range
            for image, mask in frames:
                images.append(image)
                masks.append(mask)
                annotations.append((published_patient(PurePosixPath(path)), view))
            if index % 1000 == 0:
                print(f"{role} 已读 {index}/{len(selected)} 段", flush=True)
    if not images:
        raise ValueError(f"{role} 没有可验证的描迹帧")
    tensors = tuple(torch.from_numpy(np.stack(rows))[:, None].to(device) for rows in (images, masks))
    print(f"{role} 可用描迹帧 {len(images)}", flush=True)
    return (*tensors, annotations, {view: dict(counts) for view, counts in by_view.items()})


def metrics(values, annotations):
    def summarize(indices):
        scores = values[indices]
        patients = defaultdict(list)
        for index, score in zip(indices, scores, strict=True):
            patients[annotations[index][0]].append(score)
        dice, iou = scores.mean(axis=0)
        patient_dice, patient_iou = np.stack([np.mean(rows, axis=0) for rows in patients.values()]).mean(axis=0)
        return {"frames": len(indices), "patients": len(patients), "dice": float(dice), "iou": float(iou),
                "patient_macro_dice": float(patient_dice), "patient_macro_iou": float(patient_iou)}

    return {"overall": summarize(list(range(len(annotations)))),
            "by_view": {view: summarize([i for i, (_, label) in enumerate(annotations) if label == view])
                        for view in VIEWS}}


@torch.inference_mode()
def evaluate(model, dataset, batch_size):
    images, masks, annotations, coverage = dataset
    model.eval()
    values = []
    for image, mask in zip(images.split(batch_size), masks.split(batch_size), strict=True):
        logits = model(image.float().div_(255))
        if not torch.isfinite(logits).all():
            raise FloatingPointError("分割模型输出非有限值")
        prediction = logits >= 0
        target = mask.bool()
        overlap = (prediction & target).sum((1, 2, 3))
        union = (prediction | target).sum((1, 2, 3))
        values.append(torch.stack((2 * overlap / (union + overlap), overlap / union), dim=1).cpu())
    return {"metrics": metrics(torch.cat(values).numpy(), annotations), "coverage": coverage}


def save_json(path, report):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)


def train(args, train_set, val_set):
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    model = LeftVentricleUNet().to(device, memory_format=torch.channels_last)
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.001)
    schedule = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs, eta_min=0.0001)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    images, masks, _, _ = train_set
    best = -math.inf
    best_epoch = 0
    history = []
    args.run_dir.mkdir(parents=True, exist_ok=False)
    for epoch in range(1, args.epochs + 1):
        model.train()
        loss_sum = torch.zeros((), device=device)
        for indices in torch.randperm(len(images), device=device).split(args.batch_size):
            image = images[indices].float().div_(255).contiguous(memory_format=torch.channels_last)
            target = masks[indices].float()
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device.type, enabled=device.type == "cuda"):
                logits = model(image)
                loss = F.binary_cross_entropy_with_logits(logits, target)
            probability = logits.float().sigmoid()
            common = (probability * target).sum((1, 2, 3))
            total = (probability + target).sum((1, 2, 3))
            loss = loss + 1 - ((2 * common + 1) / (total + 1)).mean()
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            loss_sum += loss.detach() * len(indices)
        schedule.step()
        result = evaluate(model, val_set, args.batch_size)["metrics"]["overall"]
        score = result["patient_macro_dice"]
        row = {"epoch": epoch, "train_loss": float(loss_sum.item() / len(images)), "val": result}
        if not math.isfinite(row["train_loss"]):
            raise FloatingPointError("训练损失非有限值")
        history.append(row)
        if score > best:
            best, best_epoch = score, epoch
            temporary = args.run_dir / "best.partial"
            torch.save({"model": model.state_dict(), "epoch": epoch, "source_folds": FOLDS, "side": SIDE,
                        "target": TARGET, "threshold": 0.5,
                        "val_patient_macro_dice": score}, temporary)
            temporary.replace(args.run_dir / "best.pt")
        print(json.dumps(row), flush=True)
    save_json(args.run_dir / "validation.json", {"best_epoch": best_epoch, "source_folds": FOLDS,
                                                "seed": args.seed, "epochs": history,
                                                "train_coverage": train_set[3], "val_coverage": val_set[3]})


def load_model(checkpoint, device):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if (saved["source_folds"] != FOLDS or saved["side"] != SIDE or saved["target"] != TARGET
            or saved["threshold"] != 0.5):
        raise ValueError("模型设置和独立测试协议不一致")
    model = LeftVentricleUNet().to(device, memory_format=torch.channels_last)
    model.load_state_dict(saved["model"])
    return model.eval(), saved


@torch.inference_mode()
def predict_frame(args):
    if args.mask.suffix.lower() != ".png" or args.mask.exists():
        raise ValueError("模型输出需要一个新的 PNG 路径")
    model, _ = load_model(args.checkpoint, args.device)
    with av.open(str(args.video)) as container:
        stream = container.streams.video[0]
        frame = next((frame for index, frame in enumerate(container.decode(stream)) if index == args.frame), None)
        if frame is None:
            raise ValueError("指定的源视频帧不存在")
        pixels = torch.as_tensor(pixel_data(frame), device=args.device)[None, None].float().div_(255)
        logits = model(pixels)
        if not torch.isfinite(logits).all():
            raise FloatingPointError("分割模型输出非有限值")
    mask = (logits[0, 0] >= 0).to(torch.uint8).cpu().numpy() * 255
    args.mask.parent.mkdir(parents=True, exist_ok=True)
    with args.mask.open("xb") as stream:
        Image.fromarray(mask).save(stream, format="PNG")


def main():
    parser = argparse.ArgumentParser(description="使用 Stanford 左室内膜描迹训练并独立评估分割模型")
    commands = parser.add_subparsers(dest="action", required=True)
    training = commands.add_parser("train")
    testing = commands.add_parser("test")
    predicting = commands.add_parser("predict")
    for command in (training, testing, predicting):
        command.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    for command in (training, testing):
        command.add_argument("--archive", type=Path, required=True)
        command.add_argument("--batch-size", type=int, default=32)
        command.add_argument("--workers", type=int, default=6)
    training.add_argument("--epochs", type=int, default=12)
    training.add_argument("--seed", type=int, default=42)
    training.add_argument("--run-dir", type=Path, required=True)
    for command in (testing, predicting):
        command.add_argument("--checkpoint", type=Path, required=True)
    testing.add_argument("--report", type=Path, required=True)
    predicting.add_argument("--video", type=Path, required=True)
    predicting.add_argument("--frame", type=int, required=True)
    predicting.add_argument("--mask", type=Path, required=True)
    args = parser.parse_args()
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    if args.action == "predict":
        if args.frame < 0:
            parser.error("源视频帧编号必须大于等于 0")
        predict_frame(args)
        return
    if min(args.workers, args.batch_size) < 1 or getattr(args, "epochs", 1) < 1:
        parser.error("线程数、批大小和轮数需要为正数")
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        errors = []
        clips = read_sources(archive, errors)
        if errors:
            raise ValueError(f"Stanford 文件清单有 {len(errors)} 项错误")
        verify_rotation(clips)
        traces, missing = read_traces(archive, clips)
        if args.action == "train":
            if args.run_dir.exists():
                raise FileExistsError(args.run_dir)
            train(args, *(load_split(archive, clips, traces, missing, role, args.workers, args.device)
                          for role in ("train", "val")))
        else:
            if args.report.exists():
                raise FileExistsError(args.report)
            model, saved = load_model(args.checkpoint, args.device)
            dataset = load_split(archive, clips, traces, missing, "test", args.workers, args.device)
            args.report.parent.mkdir(parents=True, exist_ok=True)
            report = {"target": saved["target"], "epoch": saved["epoch"], "source_folds": FOLDS,
                      "checkpoint_sha256": file_digest(args.checkpoint), "threshold": saved["threshold"],
                      **evaluate(model, dataset, args.batch_size)}
            save_json(args.report, report)
            print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
