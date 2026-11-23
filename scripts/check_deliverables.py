"""按 docs/DELIVERABLES.md 检查可机器验证的交付判据。

    python -X utf8 scripts/check_deliverables.py
    python -X utf8 scripts/check_deliverables.py --strict   # 有 FAIL 时退出码 1

指标是否"可追溯"以 rules/metrics.json 的登记为准：每个对外的数字必须同时有
报告（Release 资产）和 claims 条目。无法自动验证的条目（授权环境、判读、外部
签核）报告为 MANUAL，不计入失败。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WEIGHT_SUFFIXES = (".pt", ".pth", ".onnx", ".engine", ".safetensors")
MEDIA_SUFFIXES = (".npy", ".hdf5", ".dcm", ".avi", ".mp4", ".mov", ".mkv",
                  ".zip", ".7z", ".tar", ".db", ".sqlite", ".sqlite3")
ALLOWED_PREFIXES = ("docs/", "templates/", "fixtures/")

RESULT_TOKENS = {
    "MAE": r"\bMAE\b",
    "单位": r"EF\s*百分点",
    "相关系数": r"Pearson|相关系数",
    "AUROC": r"AUROC",
    "常数基线": r"常数基线",
    "区间": r"区间|置信|CI",
    "校准": r"校准",
    "种子或折": r"种子|折|seed",
    "样本量": r"视频数|患儿数|段视频|名患儿",
}


class Check:
    def __init__(self, code, title):
        self.code, self.title = code, title
        self.status, self.detail = "PASS", ""

    def fail(self, detail):
        self.status, self.detail = "FAIL", detail
        return self

    def manual(self, detail):
        self.status, self.detail = "MANUAL", detail
        return self

    def ok(self, detail=""):
        self.detail = detail
        return self


def read(relative):
    path = ROOT / relative
    return path.read_text(encoding="utf-8") if path.exists() else ""


def tracked():
    try:
        out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return [line for line in out.splitlines() if line.strip()]


def load_metrics():
    path = ROOT / "rules" / "metrics.json"
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def check_d0():
    check = Check("D0", "数据与许可台账")
    files = tracked()
    if files is None:
        return check.manual("不是 git 仓库，无法检查跟踪文件")
    bad = []
    for name in files:
        lower = name.lower()
        if lower.startswith(ALLOWED_PREFIXES):
            continue
        if lower.endswith(WEIGHT_SUFFIXES) or lower.endswith(MEDIA_SUFFIXES):
            bad.append(name)
        elif lower.endswith((".csv", ".xlsx")) and not lower.startswith(ALLOWED_PREFIXES):
            bad.append(name)
    if bad:
        return check.fail(f"跟踪了不应入库的文件：{bad[:3]}")
    if not re.search(r"许可|licen[cs]e", read("docs/PUBLIC_DATASETS.md"), re.I):
        return check.fail("docs/PUBLIC_DATASETS.md 没有记录许可证")
    return check.ok(f"{len(files)} 个跟踪文件，无受限资产")


def check_d1():
    check = Check("D1", "编码产物")
    embed = read("embed_panecho.py")
    if not embed:
        return check.fail("缺少 embed_panecho.py")
    for token in ("--pediatric-archive", "--index", "--clips"):
        if token not in embed:
            return check.fail(f"embed_panecho.py 缺少 {token}")
    if not read("embedding_labels.py"):
        return check.fail("缺少 embedding_labels.py 标签校验")
    doc = read("docs/EMBEDDING_PIPELINE.md")
    if "交集" not in doc:
        return check.fail("文档未记录训练/验证/测试的患儿交集检查")
    return check.ok("脚本、标签校验与交集记录齐备")


def check_d2():
    check = Check("D2", "阶段二性能报告")
    doc = read("docs/EMBEDDING_PIPELINE.md")
    if not doc:
        return check.fail("缺少 docs/EMBEDDING_PIPELINE.md")
    missing = [name for name, pattern in RESULT_TOKENS.items() if not re.search(pattern, doc)]
    if missing:
        return check.fail("结果段缺少：" + "、".join(missing))

    metrics = load_metrics()
    if metrics is None:
        return check.fail("rules/metrics.json 缺失或不是合法 JSON")
    reports = metrics.get("reports", {})
    report_keys = [key for key, value in reports.items()
                   if isinstance(value, dict)
                   and re.search(r"emb|embed|head", f"{key} {value.get('report', '')}", re.I)]
    if not report_keys:
        return check.fail("rules/metrics.json 没有登记嵌入头相关报告（reports 条目）")
    claims = [c for c in metrics.get("claims", [])
              if c.get("path") == "docs/EMBEDDING_PIPELINE.md"]
    if not claims:
        return check.fail("rules/metrics.json 的 claims 未登记该文档中的数字")
    return check.ok(f"报告 {report_keys[:2]}，claims {len(claims)} 条")


def check_d3():
    check = Check("D3", "标签可信度（EF 复测）")
    if not read("docs/EF_REVIEW_PROTOCOL.md"):
        return check.fail("缺少 EF 复测协议")
    if not (ROOT / "scripts" / "ef_review.py").exists():
        return check.fail("缺少 scripts/ef_review.py")
    return check.manual("协议与工具就位；判读与 Q1/Q2/Q3 待授权环境完成")


def check_d4():
    check = Check("D4", "外部基线对照")
    doc = read("docs/ECHOFOCUS.md")
    pipeline = read("docs/EMBEDDING_PIPELINE.md")
    if not doc:
        return check.fail("缺少 docs/ECHOFOCUS.md")
    metrics = load_metrics() or {}
    keys = [key for key in metrics.get("reports", {}) if re.search(r"echofocus", key, re.I)]
    if not keys:
        return check.fail("rules/metrics.json 未登记 EchoFocus 同批对照报告")
    claims = [c for c in metrics.get("claims", [])
              if "EchoFocus" in c.get("text", "") and keys[0] in c.get("reports", [])]
    if not claims:
        return check.fail("同批对照的表格行未登记 claim")
    if "零样本" not in pipeline or "退化" not in pipeline:
        return check.fail("文档未写明零样本口径或失效说明")
    return check.ok(f"同批嵌入对照已登记：{keys[0]}，并记录了退化判定")


def check_d5():
    check = Check("D5", "多切面独立评价")
    for name in ("docs/MULTIVIEW_EVALUATION.md", "docs/MULTIVIEW_LABELS.md"):
        if not read(name):
            return check.fail(f"缺少 {name}")
    if not (ROOT / "evaluate_multiview.py").exists():
        return check.fail("缺少 evaluate_multiview.py")
    return check.manual("工具与协议就位；需签核与冻结输入后执行")


def check_d6():
    check = Check("D6", "对外宣称门禁")
    if not (ROOT / "rules" / "banned-claims.txt").exists():
        return check.fail("缺少 rules/banned-claims.txt")
    script = ROOT / "scripts" / "check_redlines.py"
    if not script.exists():
        return check.fail("缺少 scripts/check_redlines.py")
    card = [p for p in ("docs/MODEL_CARD.md", "MODEL_CARD.md") if (ROOT / p).exists()]
    if not card:
        return check.fail("缺少模型卡（适用人群 / 已知失效 / 非器械声明）")
    if "MODEL_CARD.md" not in read("README.md"):
        return check.fail("README 未引用模型卡")
    result = subprocess.run([sys.executable, "-X", "utf8", str(script)],
                            cwd=ROOT, capture_output=True, text=True)
    if "ModuleNotFoundError" in result.stderr:
        return check.manual("红线脚本依赖未安装（markdown_it），需在验收环境执行")
    if result.returncode != 0:
        tail = (result.stdout + result.stderr).strip().splitlines()
        return check.fail("红线检查未通过：" + (tail[-1] if tail else "退出码非零"))
    return check.ok("红线检查退出码 0")


def check_d7():
    check = Check("D7", "工程验收")
    commands = read("AGENTS.md")
    for token in ("typecheck", "pytest backend", "verify.mjs"):
        if token not in commands:
            return check.fail(f"AGENTS.md 缺少验收命令 {token}")
    return check.manual("命令已定义；需在装有前后端依赖的环境执行")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    checks = [check_d0(), check_d1(), check_d2(), check_d3(),
              check_d4(), check_d5(), check_d6(), check_d7()]
    width = max(len(c.title) for c in checks) + 2
    for check in checks:
        print(f"{check.code}  {check.title:<{width}} {check.status:<6} {check.detail}")

    failed = [c.code for c in checks if c.status == "FAIL"]
    manual = [c.code for c in checks if c.status == "MANUAL"]
    print()
    print(f"通过 {sum(c.status == 'PASS' for c in checks)} / 待人工 {len(manual)} / 未达标 {len(failed)}")
    if failed:
        print("未达标：" + "、".join(failed))
    return 1 if (args.strict and failed) else 0


if __name__ == "__main__":
    sys.exit(main())

