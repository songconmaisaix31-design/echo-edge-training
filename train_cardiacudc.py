import argparse
import json
import math
from pathlib import Path

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

SIDE = 256
CLASS_NAMES = ("background", "LV", "LA", "RA", "RV")
FOREGROUND = ("LV", "LA", "RA", "RV")


def load_dataset(data_dir):
    images = np.load(Path(data_dir) / "images.npy")
    masks = np.load(Path(data_dir) / "masks.npy")
    meta = json.loads((Path(data_dir) / "meta.json").read_text(encoding="utf-8"))
    if meta["side"] != SIDE or tuple(meta["classes"]) != CLASS_NAMES:
        raise ValueError("标记文件的尺寸或类别与脚本不一致")
    if images.shape[0] != len(meta["frames"]):
        raise ValueError("影像与元数据数量不一致")
    return images, masks, meta["frames"]


def indices_of(meta, split):
    return [index for index, row in enumerate(meta) if row["split"] == split]


def batches(order, size):
    for start in range(0, len(order), size):
        yield np.asarray(order[start:start + size])


def block(inputs, outputs):
    return nn.Sequential(nn.Conv2d(inputs, outputs, 3, padding=1, bias=False), nn.BatchNorm2d(outputs),
                         nn.ReLU(inplace=True), nn.Conv2d(outputs, outputs, 3, padding=1, bias=False),
                         nn.BatchNorm2d(outputs), nn.ReLU(inplace=True))


class FourChamberUNet(nn.Module):
    def __init__(self):
        super().__init__()
        channels = (16, 32, 64, 96)
        self.encoder = nn.ModuleList(block(left, right) for left, right in zip((1, *channels[:-1]), channels))
        self.decoder = nn.ModuleList(block(deep + skip, skip)
                                     for deep, skip in zip(channels[:0:-1], channels[-2::-1], strict=True))
        self.mask = nn.Conv2d(channels[0], len(CLASS_NAMES), 1)

    def forward(self, images):
        skips = [self.encoder[0](images)]
        for layer in self.encoder[1:]:
            skips.append(layer(F.max_pool2d(skips[-1], 2)))
        result = skips.pop()
        for layer, skip in zip(self.decoder, reversed(skips), strict=True):
            up = F.interpolate(result, size=skip.shape[-2:], mode="nearest")
            result = layer(torch.cat((up, skip), dim=1))
        return self.mask(result)


@torch.inference_mode()
def predict_seg(model, images, indices, device, batch_size):
    model.eval()
    result = []
    for chunk in batches(indices, batch_size):
        pixels = torch.from_numpy(images[chunk]).float().div_(255).unsqueeze(1).to(device)
        result.append(model(pixels).argmax(1).cpu().numpy().astype(np.uint8))
    return np.concatenate(result)


def frame_metrics(prediction, target):
    report = {}
    for value, name in enumerate(CLASS_NAMES):
        if value == 0:
            continue
        predicted = prediction == value
        truth = target == value
        intersection = (predicted & truth).sum(axis=(1, 2))
        denominator = predicted.sum(axis=(1, 2)) + truth.sum(axis=(1, 2))
        union = (predicted | truth).sum(axis=(1, 2))
        dice = np.where(denominator > 0, 2 * intersection / np.maximum(denominator, 1), np.nan)
        iou = np.where(union > 0, intersection / np.maximum(union, 1), np.nan)
        report[name] = {"dice": dice, "iou": iou, "truth_pixels": truth.sum(axis=(1, 2))}
    return report


def summarize_seg(prediction, target, meta):
    per_frame = frame_metrics(prediction, target)
    report = {}
    for name in FOREGROUND:
        present = per_frame[name]["truth_pixels"] > 0
        report[name] = {"frames_present": int(present.sum()),
                        "frame_dice": float(np.nanmean(per_frame[name]["dice"][present])),
                        "frame_iou": float(np.nanmean(per_frame[name]["iou"][present]))}
    groups = sorted({row["group"] for row in meta})
    group_rows = {}
    for group in groups:
        rows = [index for index, row in enumerate(meta) if row["group"] == group]
        group_rows[group] = {name: (float(np.nanmean(per_frame[name]["dice"][rows])),
                                    float(np.nanmean(per_frame[name]["iou"][rows]))) for name in FOREGROUND}
    for name in FOREGROUND:
        report[name]["group_macro_dice"] = float(np.mean([row[name][0] for row in group_rows.values()]))
        report[name]["group_macro_iou"] = float(np.mean([row[name][1] for row in group_rows.values()]))
    report["groups"] = len(groups)
    report["frames"] = int(len(prediction))
    report["foreground_frame_dice"] = float(np.mean([report[name]["frame_dice"] for name in FOREGROUND]))
    report["foreground_frame_iou"] = float(np.mean([report[name]["frame_iou"] for name in FOREGROUND]))
    report["foreground_group_macro_dice"] = float(np.mean([report[name]["group_macro_dice"] for name in FOREGROUND]))
    report["foreground_group_macro_iou"] = float(np.mean([report[name]["group_macro_iou"] for name in FOREGROUND]))
    report["sites"] = {site: sum(1 for row in meta if row["site"] == site) for site in ("G", "R", "unknown")}
    return report


def save_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def train(args, images, masks, meta):
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    train_indices = indices_of(meta, "train")
    val_indices = indices_of(meta, "validation")
    if not train_indices or not val_indices:
        raise ValueError("训练或验证折为空")
    model = FourChamberUNet().to(device, memory_format=torch.channels_last)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr)
    schedule = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs, eta_min=args.lr / 10)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    args.run_dir.mkdir(parents=True, exist_ok=False)
    best = -math.inf
    history = []
    for epoch in range(1, args.epochs + 1):
        model.train()
        order = np.random.default_rng(args.seed + epoch).permutation(len(train_indices))
        loss_sum = torch.zeros((), device=device)
        for chunk in batches(order, args.batch_size):
            rows = [train_indices[i] for i in chunk]
            pixels = torch.from_numpy(images[rows]).float().div_(255).unsqueeze(1).to(
                device, non_blocking=True).contiguous(memory_format=torch.channels_last)
            target = torch.from_numpy(masks[rows].astype(np.int64)).to(device, non_blocking=True)
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device.type, enabled=device.type == "cuda"):
                logits = model(pixels)
                ce = F.cross_entropy(logits, target)
                probability = logits.float().softmax(1)
                dice_loss = 0.0
                for value in range(1, len(CLASS_NAMES)):
                    truth = (target == value).float()
                    predicted = probability[:, value]
                    common = (predicted * truth).sum()
                    total = predicted.sum() + truth.sum()
                    dice_loss = dice_loss + (1 - (2 * common + 1) / (total + 1))
                loss = ce + dice_loss / (len(CLASS_NAMES) - 1)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            loss_sum += loss.detach() * len(chunk)
        schedule.step()
        prediction = predict_seg(model, images, val_indices, device, args.batch_size)
        report = summarize_seg(prediction, masks[val_indices], [meta[i] for i in val_indices])
        score = report["foreground_group_macro_dice"]
        row = {"epoch": epoch, "train_loss": float(loss_sum.item() / len(train_indices)),
               "val_foreground_group_macro_dice": score,
               "val_by_structure": {name: report[name]["group_macro_dice"] for name in FOREGROUND}}
        history.append(row)
        print(json.dumps(row), flush=True)
        if score > best:
            best = score
            temporary = args.run_dir / "best.partial"
            torch.save({"model": model.state_dict(), "epoch": epoch, "side": SIDE, "classes": CLASS_NAMES,
                        "val_foreground_group_macro_dice": score}, temporary)
            temporary.replace(args.run_dir / "best.pt")
    save_json(args.run_dir / "validation.json", {"best": best, "epochs": history,
                                                 "frames": {"train": len(train_indices),
                                                            "validation": len(val_indices)}})


def load_model(checkpoint, device):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["side"] != SIDE or saved["classes"] != CLASS_NAMES:
        raise ValueError("checkpoint 与四腔分割协议不一致")
    model = FourChamberUNet().to(device, memory_format=torch.channels_last)
    model.load_state_dict(saved["model"])
    return model.eval(), saved


def main():
    parser = argparse.ArgumentParser(description="CardiacUDC 四腔分割（LV/LA/RA/RV）")
    commands = parser.add_subparsers(dest="action", required=True)
    training = commands.add_parser("train")
    training.add_argument("--data-dir", type=Path, required=True)
    training.add_argument("--run-dir", type=Path, required=True)
    training.add_argument("--epochs", type=int, default=20)
    training.add_argument("--batch-size", type=int, default=8)
    training.add_argument("--lr", type=float, default=1e-3)
    training.add_argument("--seed", type=int, default=42)
    training.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    testing = commands.add_parser("test")
    testing.add_argument("--data-dir", type=Path, required=True)
    testing.add_argument("--checkpoint", type=Path, required=True)
    testing.add_argument("--report", type=Path, required=True)
    testing.add_argument("--batch-size", type=int, default=8)
    testing.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    args = parser.parse_args()
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    images, masks, meta = load_dataset(args.data_dir)
    device = torch.device(args.device)
    if args.action == "train":
        train(args, images, masks, meta)
        return
    if args.report.exists():
        raise FileExistsError(args.report)
    model, saved = load_model(args.checkpoint, device)
    indices = indices_of(meta, "test")
    prediction = predict_seg(model, images, indices, device, args.batch_size)
    report = {"task": "four_chamber_segmentation", "dataset": "CardiacUDC (annotated)",
              "classes": CLASS_NAMES, "split": "test", "epoch": saved["epoch"],
              "checkpoint_sha256": __import__("hashlib").sha256(args.checkpoint.read_bytes()).hexdigest(),
              "test": summarize_seg(prediction, masks[indices], [meta[i] for i in indices])}
    save_json(args.report, report)
    print(json.dumps({"epoch": saved["epoch"], "test": report["test"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
