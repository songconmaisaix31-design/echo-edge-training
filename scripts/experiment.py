r"""固定种子重复与单变量消融的实验登记器。

一个基线、一次只动一个变量、每个配置跑多个种子、只报均值与方差，并把划分、
超参数和代码版本一起写进 ledger.json。

    # 基线：同一配置跑 3 个种子
    python -X utf8 scripts/experiment.py baseline --emb '.\work\emb' --out '.\runs\exp\base' --seeds 0,1,2 --epochs 80

    # 消融：只改 --vary 指定的那一个变量
    python -X utf8 scripts/experiment.py ablate --emb '.\work\emb' --out '.\runs\exp\layers' --seeds 0,1,2 --epochs 80 --vary layers=1,2

两个命令都会在 out 下写 ``ledger.json``，并在终端打印可直接粘进文档的均值±方差表。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRAINER = ROOT / "train_emb_head.py"
METRIC_KEYS = ("mae", "bias", "rmse", "pearson_r", "auroc_ef_lt_50")
DEFAULTS = {"epochs": 80, "batch": 32, "lr": 1e-3, "dim-model": 256, "layers": 2,
            "val-frac": 0.15, "folds": 0, "bootstrap": 500}


def git_state():
    def run(*args):
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    commit = run("rev-parse", "HEAD").stdout.strip()
    branch = run("rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    dirty = bool(run("status", "--porcelain").stdout.strip())
    return {"commit": commit, "branch": branch, "dirty": dirty}


def data_state(emb: Path):
    manifest = json.loads((emb / "index.json").read_text(encoding="utf-8"))
    split_pairs = sorted((info.get("split", "train"), info["subject"])
                         for info in manifest.values() if "subject" in info)
    digest = hashlib.sha256(json.dumps(split_pairs, ensure_ascii=False)
                            .encode("utf-8")).hexdigest()
    sizes = {}
    for info in manifest.values():
        sizes[info.get("split", "train")] = sizes.get(info.get("split", "train"), 0) + 1
    return {"path": str(emb), "entries": len(manifest), "split_sizes": sizes,
            "split_sha256": digest,
            "index_sha256": hashlib.sha256((emb / "index.json").read_bytes()).hexdigest()}


def env_state():
    import torch
    return {"python": platform.python_version(), "torch": torch.__version__,
            "device": "cuda" if torch.cuda.is_available() else "cpu",
            "platform": platform.platform()}


def run_once(emb: Path, run_dir: Path, args: dict, seed: int, variant: dict):
    run_dir.mkdir(parents=True, exist_ok=True)
    report = run_dir / "test.json"
    if report.exists():
        return json.loads(report.read_text(encoding="utf-8")), "cached"
    command = [sys.executable, "-X", "utf8", str(TRAINER), "--emb", str(emb),
               "--run-dir", str(run_dir), "--report", str(report), "--seed", str(seed)]
    for key, value in {**args, **variant}.items():
        command += [f"--{key}", str(value)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0 or not report.exists():
        tail = (result.stdout + result.stderr).strip().splitlines()[-3:]
        raise SystemExit(f"训练失败 seed={seed} variant={variant}: {' | '.join(tail)}")
    return json.loads(report.read_text(encoding="utf-8")), "ran"


def summarise(reports):
    """把同组多次运行的指标压成均值与总体标准差。"""
    rows = {}
    for name, getter in (("test", lambda r: r["test"]),
                         ("calibrated", lambda r: r.get("test_calibrated", {}))):
        values = [getter(report) for report in reports]
        rows[name] = {}
        for key in METRIC_KEYS:
            series = [value[key] for value in values if key in value]
            if series:
                rows[name][key] = {"mean": round(statistics.fmean(series), 3),
                                   "sd": round(statistics.pstdev(series), 3),
                                   "n": len(series)}
    return rows


def evaluate_variant(emb, out, name, args, seeds, variant):
    reports, modes = [], []
    for seed in seeds:
        run_dir = out / name / f"seed{seed}"
        report, mode = run_once(emb, run_dir, args, seed, variant)
        reports.append(report)
        modes.append(mode)
        print(f"  [{name}] seed={seed} {mode} ---> "
              f"test MAE {report['test']['mae']:.3f}, "
              f"calibrated {report.get('test_calibrated', {}).get('mae', float('nan')):.3f}",
              flush=True)
    return {"variant": name, "overrides": variant, "seeds": seeds,
            "runs": modes, "summary": summarise(reports)}


def print_table(results):
    print()
    print("| 配置 | MAE（未校准） | MAE（校准后） | Pearson r | EF<50 AUROC |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for result in results:
        raw = result["summary"]["test"]
        cal = result["summary"]["calibrated"]
        def cell(block, key, digits=2):
            if key not in block:
                return "—"
            return f"{block[key]['mean']:.{digits}f} ± {block[key]['sd']:.{digits}f}"
        print(f"| {result['variant']} | {cell(raw, 'mae')} | {cell(cal, 'mae')} | "
              f"{cell(raw, 'pearson_r', 3)} | {cell(raw, 'auroc_ef_lt_50', 3)} |")
    print()


def write_ledger(out, results, args, seeds, manifest):
    ledger = {"created": datetime.now(timezone.utc).isoformat(), "manifest": manifest,
              "args": args, "seeds": seeds, "results": results}
    out.mkdir(parents=True, exist_ok=True)
    (out / "ledger.json").write_text(json.dumps(ledger, ensure_ascii=False, indent=2),
                                     encoding="utf-8")
    print("ledger →", out / "ledger.json")


def parse_seeds(text):
    return [int(part) for part in text.split(",") if part.strip()]


def attr(key):
    """--dim-model 在 argparse 里叫 dim_model。"""
    return key.replace("-", "_")


def parse_vary(text):
    key, _, values = text.partition("=")
    return key.strip(), [value.strip() for value in values.split(",") if value.strip()]


def main():
    parser = argparse.ArgumentParser(description="固定种子重复与单变量消融")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("baseline", "ablate"):
        item = sub.add_parser(name)
        item.add_argument("--emb", type=Path, required=True)
        item.add_argument("--out", type=Path, required=True)
        item.add_argument("--seeds", default="0,1,2")
        for key, value in DEFAULTS.items():
            kind = int if isinstance(value, int) else float
            item.add_argument(f"--{key}", type=kind, default=value)
        if name == "ablate":
            item.add_argument("--vary", required=True, help="例如 layers=1,2 或 dim-model=128,256")
    args = parser.parse_args()

    seeds = parse_seeds(args.seeds)
    if len(seeds) < 2:
        raise SystemExit("至少两个种子：单次结果不能作为结论")
    settings = {key: getattr(args, attr(key)) for key in DEFAULTS}
    manifest = {"git": git_state(), "data": data_state(args.emb), "env": env_state()}
    args.out.mkdir(parents=True, exist_ok=True)

    if args.command == "baseline":
        results = [evaluate_variant(args.emb, args.out, "baseline", settings, seeds, {})]
    else:
        key, values = parse_vary(args.vary)
        if key not in DEFAULTS:
            raise SystemExit(f"--vary 只允许改动基线里的一个键：{sorted(DEFAULTS)}")
        results = [evaluate_variant(args.emb, args.out, f"{key}={value}", settings, seeds,
                                    {key: value}) for value in values]
        print(f"\n单变量消融：只改动 {key}，其余保持基线 {DEFAULTS}")

    print_table(results)
    write_ledger(args.out, results, settings, seeds, manifest)


if __name__ == "__main__":
    main()
