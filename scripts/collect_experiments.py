"""把 experiment.py 产出的 ledger 汇总成结果表与结论草稿。

    python -X utf8 scripts/collect_experiments.py --ledger runs/exp/dim/ledger.json [--ledger ...]

输出一段 markdown：每个配置的均值±方差、相对基线的差值与判定（是否超过两倍
种子标准差）。只读 ledger，不改动任何实验。
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

METRICS = (("mae", "MAE（未校准）", 2), ("pearson_r", "Pearson r", 3),
           ("auroc_ef_lt_50", "AUROC（EF<50）", 3))


def cell(block, key, digits):
    if key not in block:
        return "—"
    return f"{block[key]['mean']:.{digits}f} ± {block[key]['sd']:.{digits}f}"


def judge(results, baseline=None):
    """以第一个配置为基线，判断其余配置是否超出两倍种子标准差。"""
    if not results:
        return []
    order = [r for r in results if r["variant"] == baseline] if baseline else []
    base_result = order[0] if order else results[0]
    base = base_result["summary"]["test"].get("mae")
    if not base:
        return []
    verdicts = []
    for result in results:
        if result is base_result:
            continue
        current = result["summary"]["test"].get("mae")
        if not current:
            continue
        delta = current["mean"] - base["mean"]
        threshold = 2 * max(current["sd"], base["sd"])
        if abs(delta) <= threshold:
            text = f"无显著差异（Δ {delta:+.2f}，阈值 ±{threshold:.2f}）"
        elif delta < 0:
            text = f"改善（Δ {delta:+.2f}，超过 ±{threshold:.2f}）"
        else:
            text = f"变差（Δ {delta:+.2f}，超过 ±{threshold:.2f}）"
        verdicts.append((result["variant"], text))
    return verdicts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ledger", action="append", required=True)
    parser.add_argument("--title", default="消融结果")
    parser.add_argument("--baseline", help="作为基线的配置名，例如 dim-model=256")
    args = parser.parse_args()

    print(f"### {args.title}\n")
    print("| 配置 | MAE（未校准） | MAE（校准后） | Pearson r | AUROC（EF<50） | 判定 |")
    print("| --- | ---: | ---: | ---: | ---: | --- |")
    for path in args.ledger:
        ledger = json.loads(Path(path).read_text(encoding="utf-8"))
        results = ledger["results"]
        verdicts = dict(judge(results, args.baseline))
        for result in results:
            raw = result["summary"]["test"]
            cal = result["summary"]["calibrated"]
            name = result["variant"]
            label = name if name != (args.baseline or results[0]["variant"]) else name + "（基线）"
            print(f"| {label} | {cell(raw, 'mae', 2)} | {cell(cal, 'mae', 2)} | "
                  f"{cell(raw, 'pearson_r', 3)} | {cell(raw, 'auroc_ef_lt_50', 3)} | "
                  f"{verdicts.get(name, '基线')} |")
        manifest = ledger["manifest"]
        print(f"\n> {Path(path).name}：commit `{manifest['git']['commit'][:8]}`，"
              f"dirty={manifest['git']['dirty']}，种子 {ledger['seeds']}，"
              f"切分 `{manifest['data']['split_sha256'][:12]}`\n")


if __name__ == "__main__":
    main()
