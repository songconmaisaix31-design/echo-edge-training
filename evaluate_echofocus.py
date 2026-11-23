r"""在冻结嵌入上运行外部基线头（EchoFocus_Measure），用于同批对照。

输入与 ``train_emb_head.py`` 完全相同：``work/emb/<key>.npy`` 的 ``(n_clips, 768)``。
不重新编码，因此这是"同一输入、不同头"的对照，而不是两个管线的对比。

EchoFocus 权重与代码不进入本仓库（CC BY-NC-SA 4.0 / AGPLv3），用参数指向授权位置：

    python -X utf8 evaluate_echofocus.py `
      --emb '.\work\emb' --echofocus-repo '.\work\echofocus' `
      --checkpoint '.\work\models\EchoFocus_Measure\best_checkpoint.pt' `
      --report '.\runs\echofocus\camus.json'

报告先给预测分布（均值和标准差）。标准差接近 0 时说明它在该数据上退化为常数，
此时只报告失效，不参与按误差排序。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

LABELS = ["AA01", "AR01", "EF05", "LD05", "LE05", "LE07", "LM12", "LS04",
          "MA02", "MP01", "PA02", "TA01", "LA34", "RA06", "RV19", "ST39",
          "ST49", "RV32"]


def load_head(checkpoint: Path, echofocus_repo: Path):
    sys.path.insert(0, str(echofocus_repo))
    from models import CustomTransformer

    payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    state = payload["model_state_dict"]
    layers = sum(1 for key in state if key.endswith("self_attn.in_proj_weight"))
    outputs = state["ff.weight"].shape[0]
    model = CustomTransformer(input_size=768, encoder_dim=768, n_encoder_layers=layers,
                              output_size=outputs, clip_dropout=0.0, tf_combine="avg")
    model.load_state_dict(state)
    model.eval()
    return model, payload.get("input_norm_dict"), layers, outputs


def un_normalize(values, norm, index):
    params = norm[LABELS[index]]
    out = values * params["stdev"] + params["mean"]
    if params.get("apply_boxcox"):
        from scipy.special import inv_boxcox
        out = inv_boxcox(out, params["lmbda"])
    return out - params["shift"]


def score(y_true, y_pred):
    y_true = np.asarray(y_true, float)
    y_pred = np.asarray(y_pred, float)
    error = y_pred - y_true
    result = {"n": int(len(y_true)), "unit": "EF percentage points",
              "mae": round(float(np.mean(np.abs(error))), 3),
              "bias": round(float(np.mean(error)) , 3),
              "rmse": round(float(np.sqrt(np.mean(error ** 2))), 3),
              "pred_mean": round(float(np.mean(y_pred)), 3),
              "pred_sd": round(float(np.std(y_pred)), 3),
              "true_mean": round(float(np.mean(y_true)), 3),
              "true_sd": round(float(np.std(y_true)), 3)}
    result["pred_sd_ratio"] = (round(result["pred_sd"] / result["true_sd"], 3)
                               if result["true_sd"] else None)
    if len(y_true) > 2 and np.std(y_pred) > 0:
        result["pearson_r"] = round(float(np.corrcoef(y_true, y_pred)[0, 1]), 4)
    positive = y_pred[y_true < 50]
    negative = y_pred[y_true >= 50]
    if len(positive) and len(negative):
        wins = sum(1.0 if p < q else 0.5 if p == q else 0.0 for p in positive for q in negative)
        result["auroc_ef_lt_50"] = round(wins / (len(positive) * len(negative)), 4)
        result["n_positive"] = int(len(positive))
    return result


def main():
    parser = argparse.ArgumentParser(description="在冻结嵌入上运行 EchoFocus 的测量头")
    parser.add_argument("--emb", type=Path, required=True)
    parser.add_argument("--echofocus-repo", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--split", default="test")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--predictions", type=Path)
    args = parser.parse_args()

    manifest = json.loads((args.emb / "index.json").read_text(encoding="utf-8"))
    entries = [{"key": key, **info} for key, info in manifest.items()
               if info.get("split", "train") == args.split and (args.emb / f"{key}.npy").exists()]
    if not entries:
        raise SystemExit(f"没有 {args.split} 条目")
    for entry in entries:
        if "ef" not in entry:
            raise SystemExit(f"{entry['key']} 缺少 ef 标签")

    model, norm, layers, outputs = load_head(args.checkpoint, args.echofocus_repo)
    if norm is None:
        raise SystemExit("checkpoint 不含 input_norm_dict，无法反归一化")
    print(f"EchoFocus 头：{layers} 层、{outputs} 个输出；{len(entries)} 个 {args.split} 视频",
          flush=True)

    n_clips = min(np.load(entry["path"] if "path" in entry else
                          args.emb / f"{entry['key']}.npy").shape[0] for entry in entries)
    predictions, truths, keys = [], [], []
    ef_index = LABELS.index("EF05")
    for entry in entries:
        clips = np.load(args.emb / f"{entry['key']}.npy")[:n_clips]
        with torch.no_grad():
            raw = model(torch.from_numpy(clips).float()).numpy()
        predictions.append(float(un_normalize(raw[ef_index], norm, ef_index)))
        truths.append(float(entry["ef"]))
        keys.append(entry["key"])

    report = {
        "model": "EchoFocus_Measure",
        "source": "https://github.com/cavalab/echofocus",
        "licence": "weights CC BY-NC-SA 4.0, code AGPLv3",
        "relation": "zero-shot external head on the same frozen embeddings",
        "train_set_of_head": "Boston Children's Hospital pediatric data (not this dataset)",
        "clips_per_video": int(n_clips),
        "split": args.split,
        "metrics": score(truths, predictions),
    }
    report["note"] = ("pred_sd_ratio 接近 0 表示该头在当前数据上退化为常数，"
                      "此时只报告失效，不参与按误差排序。")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.predictions:
        args.predictions.write_text(json.dumps(
            {key: round(value, 3) for key, value in zip(keys, predictions)},
            ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report["metrics"], ensure_ascii=False, indent=2), flush=True)
    print("报告 →", args.report, flush=True)


if __name__ == "__main__":
    main()
