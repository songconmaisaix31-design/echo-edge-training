r"""阶段二：在冻结的 PanEcho 嵌入上训练轻量注意力头。

输入是 ``embed_panecho.py`` 产出的 ``work/emb/<key>.npy``（每个视频 ``(n_clips, 768)``），
编码器不再前向，所以这一步在 CPU 上以秒计。

与 EchoFocus 的关系：结构上同源（不切面、Transformer 编码 + 池化 + 线性头），
但池化换成显式注意力、编码层数可配、并且用本仓库自己的数据与标签重训。
权重不共享，也不需要共享。

用法::

    python -X utf8 train_emb_head.py `
      --emb '.\work\emb' --run-dir '.\runs\emb-head-v1' --epochs 200 `
      --report '.\runs\emb-head-v1\test.json'
"""

from __future__ import annotations

import argparse
import json
import random
from collections import defaultdict
from pathlib import Path

import numpy as np
import torch
from torch import nn


class EmbeddingHead(nn.Module):
    """(B, N, 768) → 标量：投影、Transformer 编码、注意力池化、回归。"""

    def __init__(self, dim_in=768, dim_model=256, layers=2, heads=4, dropout=0.1):
        super().__init__()
        self.project = nn.Linear(dim_in, dim_model)
        block = nn.TransformerEncoderLayer(
            dim_model, heads, dim_feedforward=dim_model * 2, dropout=dropout,
            batch_first=True, norm_first=True, activation="gelu")
        self.encoder = nn.TransformerEncoder(block, layers, enable_nested_tensor=False)
        self.norm = nn.LayerNorm(dim_model)
        self.attention = nn.Linear(dim_model, 1)
        self.dropout = nn.Dropout(dropout)
        self.head = nn.Linear(dim_model, 1)

    def forward(self, clips):
        encoded = self.encoder(self.project(clips))
        weights = torch.softmax(self.attention(encoded).squeeze(-1), dim=1)
        pooled = (weights.unsqueeze(-1) * encoded).sum(1)
        return self.head(self.dropout(self.norm(pooled)))


def load_manifest(emb_dir: Path):
    manifest_path = emb_dir / "index.json"
    if not manifest_path.exists():
        raise SystemExit(f"缺少 {manifest_path}，先运行 embed_panecho.py")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = []
    for key, info in manifest.items():
        path = emb_dir / f"{key}.npy"
        if not path.exists() or "ef" not in info:
            continue
        entries.append({"key": key, "path": path, **info})
    if not entries:
        raise SystemExit("没有可用样本：index.json 里的条目需要 ef 字段")
    missing_subject = [e["key"] for e in entries if "subject" not in e]
    if missing_subject:
        raise SystemExit(
            f"{len(missing_subject)} 条嵌入缺少 subject，无法按患儿划分：{missing_subject[:3]}")
    return entries


def split_entries(entries, val_frac, seed):
    train_pool = [e for e in entries if e.get("split", "train") == "train"]
    test = [e for e in entries if e.get("split") == "test"]
    subjects = sorted({e["subject"] for e in train_pool})
    random.Random(seed).shuffle(subjects)
    n_val = max(1, round(len(subjects) * val_frac))
    val_subjects = set(subjects[:n_val])
    train = [e for e in train_pool if e["subject"] not in val_subjects]
    val = [e for e in train_pool if e["subject"] in val_subjects]
    if not val or not test:
        raise SystemExit(f"划分失败：train {len(train)} val {len(val)} test {len(test)}")
    return train, val, test


def to_tensors(entries):
    arrays = [np.load(e["path"]) for e in entries]
    n_clips = min(a.shape[0] for a in arrays)
    x = torch.stack([torch.from_numpy(np.ascontiguousarray(a[:n_clips])) for a in arrays])
    y = torch.tensor([e["ef"] for e in entries], dtype=torch.float32)
    return x, y


def score(y_true, y_pred):
    y_true = np.asarray(y_true, float)
    y_pred = np.asarray(y_pred, float)
    error = y_pred - y_true
    out = {"n": int(len(y_true)), "unit": "EF percentage points",
           "mae": round(float(np.mean(np.abs(error))), 3),
           "bias": round(float(np.mean(error)), 3),
           "rmse": round(float(np.sqrt(np.mean(error ** 2))), 3)}
    if len(y_true) > 2 and np.std(y_pred) > 0:
        out["pearson_r"] = round(float(np.corrcoef(y_true, y_pred)[0, 1]), 4)
    positive = y_pred[y_true < 50]
    negative = y_pred[y_true >= 50]
    if len(positive) and len(negative):
        wins = sum(1.0 if p < q else 0.5 if p == q else 0.0 for p in positive for q in negative)
        out["auroc_ef_lt_50"] = round(wins / (len(positive) * len(negative)), 4)
        out["n_positive"] = int(len(positive))
    return out


def predict(model, x):
    model.eval()
    with torch.no_grad():
        return model(x).squeeze(-1).cpu().numpy() * 100.0


def main():
    parser = argparse.ArgumentParser(description="在冻结嵌入上训练注意力头")
    parser.add_argument("--emb", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--dim-model", type=int, default=256)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--val-frac", type=float, default=0.15)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    entries = load_manifest(args.emb)
    train, val, test = split_entries(entries, args.val_frac, args.seed)
    x_train, y_train = to_tensors(train)
    x_val, y_val = to_tensors(val)
    x_test, y_test = to_tensors(test)
    print(f"样本：train {len(train)} / val {len(val)} / test {len(test)}；"
          f"每视频 {x_train.shape[1]} clip × {x_train.shape[2]} 维", flush=True)

    constant = score(y_test, np.full(len(y_test), y_train.mean()))
    print(f"常数基线（训练均值 {y_train.mean():.1f}）：{constant}", flush=True)

    model = EmbeddingHead(x_train.shape[2], args.dim_model, args.layers)
    with torch.no_grad():
        model.head.bias.fill_(float(y_train.mean()) / 100.0)
        model.head.weight.mul_(0.1)
    parameters = sum(p.numel() for p in model.parameters())

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)
    loss_fn = nn.SmoothL1Loss(beta=0.1)

    best = float("inf")
    history = []
    for epoch in range(1, args.epochs + 1):
        model.train()
        order = torch.randperm(len(x_train))
        total, count = 0.0, 0
        for start in range(0, len(order), args.batch):
            index = order[start:start + args.batch]
            if len(index) < 2:
                continue
            loss = loss_fn(model(x_train[index]).squeeze(-1), y_train[index] / 100.0)
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            total += loss.item() * len(index)
            count += len(index)
        scheduler.step()
        if epoch % 20 == 0 or epoch == 1:
            metrics = score(y_val, predict(model, x_val))
            history.append({"epoch": epoch, "loss": round(total / max(count, 1), 4), **metrics})
            print(f"epoch {epoch:3d} loss {total / max(count, 1):.4f} "
                  f"val MAE {metrics['mae']:6.2f} r {metrics.get('pearson_r', float('nan')):6.3f}",
                  flush=True)
            if metrics["mae"] < best:
                best = metrics["mae"]
                torch.save({"model": model.state_dict(), "args": vars(args),
                            "val_mae": best}, args.run_dir / "best.pt")

    checkpoint = torch.load(args.run_dir / "best.pt", map_location="cpu", weights_only=False)
    model.load_state_dict(checkpoint["model"])
    report = {
        "model": "embedding-attention-head",
        "encoder": "PanEcho convnext_tiny (frozen)",
        "parameters": parameters,
        "clips_per_video": int(x_train.shape[1]),
        "split": {"train": len(train), "val": len(val), "test": len(test),
                  "unit": "video"},
        "val": score(y_val, predict(model, x_val)),
        "test": score(y_test, predict(model, x_test)),
        "constant_baseline_test": constant,
        "history": history,
        "args": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
    }
    report_path = args.report or (args.run_dir / "test.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\nTEST  ", report["test"], flush=True)
    print("常量   ", constant, flush=True)
    print("报告 →", report_path, flush=True)


if __name__ == "__main__":
    main()

