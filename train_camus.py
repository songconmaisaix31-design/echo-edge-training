import argparse
import json
import math
from pathlib import Path

import numpy as np
import torch
from sklearn.metrics import confusion_matrix, f1_score
from torch import nn
from torch.nn import functional as F
from torchvision.models import MobileNet_V3_Small_Weights, mobilenet_v3_small

SIDE = 256
QUALITY_SIDE = 224
NUM_CLASSES = 4
CLASS_NAMES = ("background", "LV_endocardium", "LV_myocardium", "LA")
QUALITY_NAMES = ("Good", "Medium", "Poor")


def load_dataset(data_dir):
    images = np.load(Path(data_dir) / "images.npy")
    masks = np.load(Path(data_dir) / "masks.npy")
    meta = json.loads((Path(data_dir) / "meta.json").read_text(encoding="utf-8"))
    if images.shape[0] != len(meta["frames"]):
        raise ValueError("CAMUS 影像与元数据数量不一致")
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


class MultiStructureUNet(nn.Module):
    def __init__(self):
        super().__init__()
        channels = (16, 32, 64, 96)
        self.encoder = nn.ModuleList(block(left, right) for left, right in zip((1, *channels[:-1]), channels))
        self.decoder = nn.ModuleList(block(deep + skip, skip)
                                     for deep, skip in zip(channels[:0:-1], channels[-2::-1], strict=True))
        self.mask = nn.Conv2d(channels[0], NUM_CLASSES, 1)

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
    for value in range(1, NUM_CLASSES):
        predicted = prediction == value
        truth = target == value
        intersection = (predicted & truth).sum(axis=(1, 2))
        denominator = predicted.sum(axis=(1, 2)) + truth.sum(axis=(1, 2))
        union = (predicted | truth).sum(axis=(1, 2))
        dice = np.where(denominator > 0, 2 * intersection / np.maximum(denominator, 1), np.nan)
        iou = np.where(union > 0, intersection / np.maximum(union, 1), np.nan)
        report[CLASS_NAMES[value]] = {"dice": dice, "iou": iou, "truth_pixels": truth.sum(axis=(1, 2))}
    return report


def summarize_seg(prediction, target, meta):
    per_frame = frame_metrics(prediction, target)
    report = {}
    for name, values in per_frame.items():
        present = values["truth_pixels"] > 0
        dice = values["dice"][present]
        iou = values["iou"][present]
        report[name] = {"frames_present": int(present.sum()), "frame_dice": float(np.nanmean(dice)),
                        "frame_iou": float(np.nanmean(iou))}
    patients = sorted({row["patient"] for row in meta})
    per_patient = {}
    for patient in patients:
        rows = [index for index, row in enumerate(meta) if row["patient"] == patient]
        row_report = {}
        for name, values in per_frame.items():
            dice = values["dice"][rows]
            iou = values["iou"][rows]
            row_report[name] = (float(np.nanmean(dice)), float(np.nanmean(iou)))
        per_patient[patient] = row_report
    for name in ("LV_endocardium", "LV_myocardium", "LA"):
        report[name]["patient_macro_dice"] = float(np.mean([row[name][0] for row in per_patient.values()]))
        report[name]["patient_macro_iou"] = float(np.mean([row[name][1] for row in per_patient.values()]))
    report["patients"] = len(patients)
    report["frames"] = int(len(prediction))
    report["foreground_frame_dice"] = float(np.mean([report[name]["frame_dice"] for name in
                                                     ("LV_endocardium", "LV_myocardium", "LA")]))
    report["foreground_patient_macro_dice"] = float(np.mean([report[name]["patient_macro_dice"] for name in
                                                             ("LV_endocardium", "LV_myocardium", "LA")]))
    report["foreground_frame_iou"] = float(np.mean([report[name]["frame_iou"] for name in
                                                    ("LV_endocardium", "LV_myocardium", "LA")]))
    report["foreground_patient_macro_iou"] = float(np.mean([report[name]["patient_macro_iou"] for name in
                                                            ("LV_endocardium", "LV_myocardium", "LA")]))
    return report


def save_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def train_seg(args, images, masks, meta):
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    train_indices = indices_of(meta, "train")
    val_indices = indices_of(meta, "validation")
    if not train_indices or not val_indices:
        raise ValueError("CAMUS 训练或验证折为空")
    model = MultiStructureUNet().to(device, memory_format=torch.channels_last)
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
                for value in range(1, NUM_CLASSES):
                    truth = (target == value).float()
                    predicted = probability[:, value]
                    common = (predicted * truth).sum()
                    total = predicted.sum() + truth.sum()
                    dice_loss = dice_loss + (1 - (2 * common + 1) / (total + 1))
                loss = ce + dice_loss / (NUM_CLASSES - 1)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            loss_sum += loss.detach() * len(chunk)
        schedule.step()
        prediction = predict_seg(model, images, val_indices, device, args.batch_size)
        report = summarize_seg(prediction, masks[val_indices], [meta[i] for i in val_indices])
        score = report["foreground_patient_macro_dice"]
        row = {"epoch": epoch, "train_loss": float(loss_sum.item() / len(train_indices)),
               "val_foreground_patient_macro_dice": score,
               "val_by_structure": {name: report[name]["patient_macro_dice"] for name in
                                    ("LV_endocardium", "LV_myocardium", "LA")}}
        history.append(row)
        print(json.dumps(row), flush=True)
        if score > best:
            best = score
            temporary = args.run_dir / "best.partial"
            torch.save({"model": model.state_dict(), "epoch": epoch, "side": SIDE, "classes": CLASS_NAMES,
                        "splits": {"train": "train", "validation": "validation", "test": "test"},
                        "val_foreground_patient_macro_dice": score}, temporary)
            temporary.replace(args.run_dir / "best.pt")
    save_json(args.run_dir / "validation.json", {"best": best, "epochs": history,
                                                 "frames": {"train": len(train_indices),
                                                            "validation": len(val_indices)}})


def load_seg(checkpoint, device):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["side"] != SIDE or saved["classes"] != CLASS_NAMES:
        raise ValueError("checkpoint 与 CAMUS 分割协议不一致")
    model = MultiStructureUNet().to(device, memory_format=torch.channels_last)
    model.load_state_dict(saved["model"])
    return model.eval(), saved


class QualityModel(nn.Module):
    def __init__(self, weights=None):
        super().__init__()
        backbone = mobilenet_v3_small(weights=weights)
        self.features = backbone.features
        self.pool = backbone.avgpool
        self.head = nn.Linear(576, len(QUALITY_NAMES))

    def forward(self, images):
        return self.head(self.pool(self.features(images)).flatten(1))


def quality_tensors(images):
    return torch.from_numpy(images).float().div_(255).unsqueeze(1).expand(-1, 3, -1, -1)


@torch.inference_mode()
def predict_quality(model, images, indices, device, batch_size):
    model.eval()
    probabilities = []
    for chunk in batches(indices, batch_size):
        pixels = quality_tensors(images[chunk]).to(device)
        probabilities.append(model(pixels).float().softmax(1).cpu().numpy())
    return np.concatenate(probabilities)


def quality_labels(meta):
    mapping = {name: index for index, name in enumerate(QUALITY_NAMES)}
    unknown = sorted({row["quality"] for row in meta} - set(mapping))
    if unknown:
        raise ValueError(f"未知质量标签：{unknown}")
    return np.array([mapping[row["quality"]] for row in meta], dtype=np.int64)


def quality_report(probabilities, labels, meta):
    prediction = probabilities.argmax(1)
    frame = {
        "samples": int(len(prediction)),
        "accuracy": float((prediction == labels).mean()),
        "macro_f1": float(f1_score(labels, prediction, average="macro")),
        "confusion": confusion_matrix(labels, prediction, labels=list(range(len(QUALITY_NAMES)))).tolist(),
        "per_class": {},
    }
    for value, name in enumerate(QUALITY_NAMES):
        selected = labels == value
        frame["per_class"][name] = {"support": int(selected.sum()),
                                    "recall": float((prediction[selected] == value).mean()) if selected.any() else None}
    groups = {}
    for index, row in enumerate(meta):
        groups.setdefault((row["patient"], row["view"]), []).append(index)
    group_labels, group_probabilities = [], []
    for (_, _), rows in sorted(groups.items()):
        values = labels[rows]
        if len(set(values.tolist())) != 1:
            raise ValueError("同一患者同一视位的两个相位质量标签不一致")
        group_labels.append(int(values[0]))
        group_probabilities.append(probabilities[rows].mean(axis=0))
    group_labels = np.array(group_labels)
    group_probabilities = np.array(group_probabilities)
    group_prediction = group_probabilities.argmax(1)
    group = {
        "samples": int(len(group_labels)),
        "accuracy": float((group_prediction == group_labels).mean()),
        "macro_f1": float(f1_score(group_labels, group_prediction, average="macro")),
        "confusion": confusion_matrix(group_labels, group_prediction,
                                      labels=list(range(len(QUALITY_NAMES)))).tolist(),
        "per_class": {},
    }
    for value, name in enumerate(QUALITY_NAMES):
        selected = group_labels == value
        group["per_class"][name] = {"support": int(selected.sum()),
                                    "recall": float((group_prediction[selected] == value).mean()) if selected.any() else None}
    rng = np.random.default_rng(42)
    scores = []
    for _ in range(1000):
        picked = rng.integers(0, len(group_labels), len(group_labels))
        scores.append(f1_score(group_labels[picked], group_prediction[picked], average="macro"))
    group["macro_f1_ci95"] = [float(np.percentile(scores, 2.5)), float(np.percentile(scores, 97.5))]
    return {"frame": frame, "patient_view": group}


def augment_quality(pixels, device):
    flip = torch.rand(len(pixels), device=device) < 0.5
    if flip.any():
        pixels[flip] = pixels[flip].flip(-1)
    gain = 0.85 + 0.3 * torch.rand(len(pixels), 1, 1, 1, device=device)
    pixels = torch.clamp(pixels * gain, 0, 1)
    mean = pixels.mean(dim=(1, 2, 3), keepdim=True)
    contrast = 0.85 + 0.3 * torch.rand(len(pixels), 1, 1, 1, device=device)
    return torch.clamp((pixels - mean) * contrast + mean, 0, 1)


def train_quality(args, images, meta):
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    labels = quality_labels(meta)
    resized = torch.nn.functional.interpolate(torch.from_numpy(images).float().unsqueeze(1),
                                              size=(QUALITY_SIDE, QUALITY_SIDE), mode="bilinear",
                                              align_corners=False).squeeze(1).numpy().astype(np.uint8)
    train_indices = indices_of(meta, "train")
    val_indices = indices_of(meta, "validation")
    model = QualityModel(weights=MobileNet_V3_Small_Weights.IMAGENET1K_V1).to(device)
    counts = np.bincount(labels[train_indices], minlength=len(QUALITY_NAMES)).astype(np.float32)
    weights = torch.as_tensor(counts.sum() / (counts * len(QUALITY_NAMES)), dtype=torch.float32, device=device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    schedule = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, args.epochs, eta_min=args.lr / 20)
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
            pixels = augment_quality(quality_tensors(resized[rows]).to(device, non_blocking=True), device)
            target = torch.as_tensor(labels[rows], dtype=torch.long, device=device)
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device.type, enabled=device.type == "cuda"):
                loss = F.cross_entropy(model(pixels), target, weight=weights, label_smoothing=0.05)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            loss_sum += loss.detach() * len(chunk)
        schedule.step()
        probabilities = predict_quality(model, resized, val_indices, device, args.batch_size)
        report = quality_report(probabilities, labels[val_indices], [meta[i] for i in val_indices])
        score = report["patient_view"]["macro_f1"]
        row = {"epoch": epoch, "train_loss": float(loss_sum.item() / len(train_indices)),
               "val_patient_view_macro_f1": score, "val_frame_macro_f1": report["frame"]["macro_f1"]}
        history.append(row)
        print(json.dumps(row), flush=True)
        if score > best:
            best = score
            temporary = args.run_dir / "best.partial"
            torch.save({"model": model.state_dict(), "epoch": epoch, "classes": QUALITY_NAMES,
                        "side": QUALITY_SIDE, "val_patient_view_macro_f1": score, "class_weights": weights.cpu()},
                       temporary)
            temporary.replace(args.run_dir / "best.pt")
    save_json(args.run_dir / "validation.json", {"best": best, "epochs": history,
                                                 "frames": {"train": len(train_indices),
                                                            "validation": len(val_indices)}})


def load_quality(checkpoint, device):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["side"] != QUALITY_SIDE or saved["classes"] != QUALITY_NAMES:
        raise ValueError("checkpoint 与 CAMUS 质量协议不一致")
    model = QualityModel().to(device)
    model.load_state_dict(saved["model"])
    return model.eval(), saved


def main():
    parser = argparse.ArgumentParser(description="CAMUS 多结构分割与影像质量分级")
    commands = parser.add_subparsers(dest="action", required=True)
    training = commands.add_parser("train")
    training.add_argument("--task", choices=("seg", "quality"), required=True)
    training.add_argument("--run-dir", type=Path, required=True)
    training.add_argument("--data-dir", type=Path, default=Path(r"work\camus"))
    training.add_argument("--cache-dir", type=Path, default=Path(r"work\cache"))
    training.add_argument("--epochs", type=int, default=None)
    training.add_argument("--batch-size", type=int, default=None)
    training.add_argument("--lr", type=float, default=None)
    training.add_argument("--seed", type=int, default=42)
    training.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    testing = commands.add_parser("test")
    testing.add_argument("--task", choices=("seg", "quality"), required=True)
    testing.add_argument("--checkpoint", type=Path, required=True)
    testing.add_argument("--report", type=Path, required=True)
    testing.add_argument("--data-dir", type=Path, default=Path(r"work\camus"))
    testing.add_argument("--batch-size", type=int, default=8)
    testing.add_argument("--device", choices=("cuda", "cpu"), default="cuda")
    args = parser.parse_args()
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("当前 PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    images, masks, meta = load_dataset(args.data_dir)
    if args.action == "train":
        if args.task == "seg":
            args.epochs = args.epochs or 20
            args.batch_size = args.batch_size or 8
            args.lr = args.lr or 1e-3
            train_seg(args, images, masks, meta)
        else:
            args.epochs = args.epochs or 15
            args.batch_size = args.batch_size or 16
            args.lr = args.lr or 3e-4
            torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
            train_quality(args, images, meta)
        return
    device = torch.device(args.device)
    if args.task == "seg":
        model, saved = load_seg(args.checkpoint, device)
        indices = indices_of(meta, "test")
        prediction = predict_seg(model, images, indices, device, args.batch_size)
        report = {"task": "multi_structure_segmentation", "epoch": saved["epoch"], "classes": CLASS_NAMES,
                  "split": "test", "checkpoint_sha256": __import__("hashlib").sha256(
                      args.checkpoint.read_bytes()).hexdigest(),
                  "test": summarize_seg(prediction, masks[indices], [meta[i] for i in indices])}
        save_json(args.report, report)
        print(json.dumps({"epoch": saved["epoch"], "test": report["test"]}, ensure_ascii=False))
    else:
        model, saved = load_quality(args.checkpoint, device)
        indices = indices_of(meta, "test")
        labels = quality_labels(meta)
        resized = torch.nn.functional.interpolate(torch.from_numpy(images).float().unsqueeze(1),
                                                  size=(QUALITY_SIDE, QUALITY_SIDE), mode="bilinear",
                                                  align_corners=False).squeeze(1).numpy().astype(np.uint8)
        probabilities = predict_quality(model, resized, indices, device, args.batch_size)
        report = {"task": "image_quality_grading", "epoch": saved["epoch"], "classes": QUALITY_NAMES,
                  "split": "test", "checkpoint_sha256": __import__("hashlib").sha256(
                      args.checkpoint.read_bytes()).hexdigest(),
                  "test": quality_report(probabilities, labels[indices], [meta[i] for i in indices])}
        save_json(args.report, report)
        print(json.dumps(report["test"]["patient_view"], ensure_ascii=False))


if __name__ == "__main__":
    main()
