r"""阶段二：在冻结的 PanEcho 嵌入上训练轻量注意力头。

输入是 ``embed_panecho.py`` 产出的 ``work/emb/<key>.npy``（每个视频 ``(n_clips, 768)``），
编码器不再前向，所以这一步在 CPU 上以分钟计。

与 EchoFocus 的关系：结构同源（不切面、Transformer 编码 + 池化 + 线性头），
但池化换成显式注意力、层数可配，并且用本仓库自己的数据与标签重训。

功能：

- 按患儿划分，训练/验证/测试不共享病人
- ``--calibrate`` 在验证集上拟合仿射校准，同时报告校准前后
- ``--bootstrap`` 以患儿为单位重采样，给出 MAE / AUROC 的 95% 区间
- ``--folds`` 对训练池做患儿级 K 折，报告均值±标准差（选型稳定性，不是最终模型）
- ``--sweep`` 小网格搜索，按折均值排序
- ``--predictions`` 导出 ``{source: ef_pred}``，供 ``scripts/ef_review.py analyze`` 使用
- ``--dump-attention`` 导出每个测试视频的片段注意力权重

用法::

    python -X utf8 train_emb_head.py `
      --emb '.\work\emb' --run-dir '.\runs\emb-head-v1' --epochs 200 `
      --report '.\runs\emb-head-v1\test.json' `
      --predictions '.\runs\emb-head-v1\predictions.json' `
      --dump-attention '.\runs\emb-head-v1\attention.json'
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

DEFAULT_GRID = [
    {"dim_model": 128, "layers": 1},
    {"dim_model": 256, "layers": 2},
    {"dim_model": 256, "layers": 1},
    {"dim_model": 384, "layers": 2},
]


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
        return self.pooled(clips)[0]

    def pooled(self, clips):
        encoded = self.encoder(self.project(clips))
        weights = torch.softmax(self.attention(encoded).squeeze(-1), dim=1)
        pooled = (weights.unsqueeze(-1) * encoded).sum(1)
        return self.head(self.dropout(self.norm(pooled))), weights


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
    missing = [e["key"] for e in entries if "subject" not in e]
    if missing:
        raise SystemExit(f"{len(missing)} 条嵌入缺少 subject，无法按患儿划分：{missing[:3]}")
    return entries


def split_by_subject(entries, val_frac, seed):
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


def fit_calibration(y_true, y_pred):
    """最小二乘仿射校准 y ≈ a·p + b，只用验证集。"""
    p = np.asarray(y_pred, float)
    y = np.asarray(y_true, float)
    if np.std(p) < 1e-8:
        return 1.0, float(y.mean() - p.mean())
    a = float(np.cov(p, y, bias=True)[0, 1] / np.var(p))
    b = float(y.mean() - a * p.mean())
    return a, b


def bootstrap_ci(y_true, y_pred, subjects, rounds=1000, seed=0):
    """按患儿为单位重采样，返回 MAE / AUROC 的 95% 区间。"""
    if rounds <= 0:
        return {"skipped": "bootstrap rounds = 0"}
    groups = defaultdict(list)
    for value_true, value_pred, subject in zip(y_true, y_pred, subjects):
        groups[subject].append((value_true, value_pred))
    keys = list(groups)
    if len(keys) < 3:
        return {}
    rng = random.Random(seed)
    maes, aucs = [], []
    for _ in range(rounds):
        sample = [groups[rng.choice(keys)] for _ in range(len(keys))]
        pairs = [pair for group in sample for pair in group]
        true = np.array([p[0] for p in pairs])
        pred = np.array([p[1] for p in pairs])
        maes.append(float(np.mean(np.abs(pred - true))))
        auc = score(true, pred).get("auroc_ef_lt_50")
        if auc is not None:
            aucs.append(auc)
    result = {"mae_ci95": [round(float(np.percentile(maes, 2.5)), 3),
                           round(float(np.percentile(maes, 97.5)), 3)],
              "bootstrap_rounds": rounds, "unit": "patient"}
    if aucs:
        result["auroc_ci95"] = [round(float(np.percentile(aucs, 2.5)), 4),
                                round(float(np.percentile(aucs, 97.5)), 4)]
    return result


def train_model(x_train, y_train, x_val, y_val, args, config=None, seed=None):
    config = config or {"dim_model": args.dim_model, "layers": args.layers}
    if seed is not None:
        torch.manual_seed(seed)
        random.seed(seed)
    model = EmbeddingHead(x_train.shape[2], config["dim_model"], config["layers"])
    with torch.no_grad():
        model.head.bias.fill_(float(y_train.mean()) / 100.0)
        model.head.weight.mul_(0.1)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)
    loss_fn = nn.SmoothL1Loss(beta=0.1)
    best, best_state, history = float("inf"), None, []
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
            metrics = score(y_val.numpy(), predict(model, x_val))
            history.append({"epoch": epoch, "loss": round(total / max(count, 1), 4), **metrics})
            if metrics["mae"] < best:
                best = metrics["mae"]
                best_state = {k: v.clone() for k, v in model.state_dict().items()}
    if best_state is not None:
        model.load_state_dict(best_state)
    return model, best, history


def cross_validate(entries, args, config):
    """患儿级 K 折，返回每折验证 MAE。"""
    subjects = sorted({e["subject"] for e in entries})
    random.Random(args.seed).shuffle(subjects)
    folds = [[] for _ in range(args.folds)]
    for position, subject in enumerate(subjects):
        folds[position % args.folds].append(subject)
    scores = []
    for number, held in enumerate(folds, start=1):
        held_set = set(held)
        train = [e for e in entries if e["subject"] not in held_set]
        val = [e for e in entries if e["subject"] in held_set]
        if not train or not val:
            continue
        x_train, y_train = to_tensors(train)
        x_val, y_val = to_tensors(val)
        _, best, _ = train_model(x_train, y_train, x_val, y_val, args, config)
        scores.append(round(float(best), 3))
    return scores


def main():
    parser = argparse.ArgumentParser(description="在冻结嵌入上训练注意力头")
    parser.add_argument("--emb", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--predictions", type=Path)
    parser.add_argument("--dump-attention", type=Path)
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--dim-model", type=int, default=256)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--val-frac", type=float, default=0.15)
    parser.add_argument("--folds", type=int, default=0, help="患儿级 K 折，0 表示跳过")
    parser.add_argument("--sweep", action="store_true", help="小网格搜索")
    parser.add_argument("--calibrate", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--bootstrap", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    entries = load_manifest(args.emb)
    train, val, test = split_by_subject(entries, args.val_frac, args.seed)
    x_train, y_train = to_tensors(train)
    x_val, y_val = to_tensors(val)
    x_test, y_test = to_tensors(test)
    print(f"样本：train {len(train)} / val {len(val)} / test {len(test)}；"
          f"每视频 {x_train.shape[1]} clip × {x_train.shape[2]} 维", flush=True)

    constant = score(y_test.numpy(), np.full(len(y_test), float(y_train.mean())))
    print(f"常数基线（训练均值 {float(y_train.mean()):.1f}）：{constant}", flush=True)

    sweep_results = None
    if args.sweep:
        pool = train + val
        sweep_results = []
        for config in DEFAULT_GRID:
            scores = cross_validate(pool, args, config)
            if scores:
                sweep_results.append({"config": config, "folds": scores,
                                      "mean_mae": round(float(np.mean(scores)), 3),
                                      "sd_mae": round(float(np.std(scores)), 3)})
                print(f"  sweep {config} -> MAE {np.mean(scores):.2f} ± {np.std(scores):.2f}",
                      flush=True)
        sweep_results.sort(key=lambda r: r["mean_mae"])

    folds = cross_validate(train + val, args, {"dim_model": args.dim_model,
                                               "layers": args.layers}) if args.folds else None

    model, best, history = train_model(x_train, y_train, x_val, y_val, args)
    parameters = sum(p.numel() for p in model.parameters())
    val_pred = predict(model, x_val)
    test_pred = predict(model, x_test)
    test_metrics = score(y_test.numpy(), test_pred)

    report = {
        "model": "embedding-attention-head",
        "encoder": "PanEcho convnext_tiny (frozen)",
        "parameters": parameters,
        "clips_per_video": int(x_train.shape[1]),
        "split": {"train": len(train), "val": len(val), "test": len(test), "unit": "video"},
        "val": score(y_val.numpy(), val_pred),
        "test": test_metrics,
        "constant_baseline_test": constant,
        "bootstrap": bootstrap_ci(y_test.numpy(), test_pred,
                                  [e["subject"] for e in test], args.bootstrap, args.seed),
        "history": history,
        "args": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
    }
    if folds:
        report["subject_kfold"] = {"folds": folds, "mean_mae": round(float(np.mean(folds)), 3),
                                   "sd_mae": round(float(np.std(folds)), 3)}
    if sweep_results:
        report["sweep"] = sweep_results

    if args.calibrate:
        slope, intercept = fit_calibration(y_val.numpy(), val_pred)
        calibrated = slope * test_pred + intercept
        report["calibration"] = {
            "slope": round(slope, 4), "intercept": round(intercept, 4),
            "fitted_on": "validation", "train_mean_baseline": round(float(y_train.mean()), 3)}
        report["test_calibrated"] = score(y_test.numpy(), calibrated)
        report["bootstrap_calibrated"] = bootstrap_ci(
            y_test.numpy(), calibrated, [e["subject"] for e in test], args.bootstrap, args.seed)

    report_path = args.report or (args.run_dir / "test.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    if args.predictions:
        payload = {"model": "embedding-attention-head", "unit": "EF percentage points"}
        for entry, value in zip(test, test_pred):
            payload[entry["key"]] = round(float(value), 3)
            payload[entry.get("source", entry["key"])] = round(float(value), 3)
        if args.calibrate and "test_calibrated" in report:
            slope, intercept = report["calibration"]["slope"], report["calibration"]["intercept"]
            payload["_calibrated"] = {entry["key"]: round(float(slope * v + intercept), 3)
                                      for entry, v in zip(test, test_pred)}
        args.predictions.parent.mkdir(parents=True, exist_ok=True)
        args.predictions.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                                    encoding="utf-8")
        print("预测 →", args.predictions, flush=True)

    if args.dump_attention:
        model.eval()
        rows = []
        with torch.no_grad():
            for offset in range(0, len(x_test), 16):
                chunk = x_test[offset:offset + 16]
                _, weights = model.pooled(chunk)
                weights = weights.cpu().numpy()
                for index, entry in enumerate(test[offset:offset + 16]):
                    vector = weights[index]
                    rows.append({"key": entry["key"], "source": entry.get("source", entry["key"]),
                                 "ef": entry["ef"], "n_clips": int(len(vector)),
                                 "weights": [round(float(w), 4) for w in vector],
                                 "peak_clip": int(np.argmax(vector))})
        args.dump_attention.parent.mkdir(parents=True, exist_ok=True)
        args.dump_attention.write_text(json.dumps(rows, ensure_ascii=False, indent=2),
                                       encoding="utf-8")
        print("注意力 →", args.dump_attention, flush=True)

    print("\n验证 ", report["val"], flush=True)
    print("测试 ", test_metrics, flush=True)
    if "test_calibrated" in report:
        print("校准后", report["test_calibrated"], flush=True)
    print("区间 ", report["bootstrap"], flush=True)
    print("报告 →", report_path, flush=True)


if __name__ == "__main__":
    main()
