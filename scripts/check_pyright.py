"""按基线检查 pyright 结果：不允许新增类型错误。

仓库里已有一批 SQLAlchemy 声明式模型带来的类型告警，短期无法清空。这个脚本把
当前计数登记成基线，之后 CI 只拦"新增的"（文件 + 规则 + 条数），既不让门禁形同
虚设，也不要求一次性重构。

    python -X utf8 scripts/check_pyright.py            # 与基线比较
    python -X utf8 scripts/check_pyright.py --update   # 重新登记基线（需在提交说明里写原因）
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "rules" / "pyright-baseline.json"
PYRIGHT = [sys.executable, "-m", "pyright", "backend", "--outputjson"]


def current_counts() -> Counter:
    result = subprocess.run(PYRIGHT, cwd=ROOT, capture_output=True, text=True)
    payload = result.stdout.strip()
    if not payload.startswith("{"):
        raise SystemExit("pyright 未返回 JSON：" + (result.stdout + result.stderr)[:400])
    data = json.loads(payload)
    counts: Counter = Counter()
    for item in data.get("generalDiagnostics", []):
        if item.get("severity") != "error":
            continue
        relative = os_rel(item["file"])
        rule = item.get("rule") or "unknown"
        counts[f"{relative}|{rule}"] += 1
    return counts


def os_rel(absolute: str) -> str:
    try:
        return Path(absolute).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return re.sub(r"^.*?/backend/", "backend/", absolute.replace("\\", "/"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()

    counts = current_counts()
    if args.update:
        BASELINE.write_text(json.dumps(dict(sorted(counts.items())), indent=2,
                                       ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"基线已更新：{sum(counts.values())} 条，覆盖 {len(counts)} 个文件-规则组合")
        return 0

    if not BASELINE.exists():
        raise SystemExit("缺少 rules/pyright-baseline.json，先运行 --update")
    baseline = Counter(json.loads(BASELINE.read_text(encoding="utf-8")))

    regressions = {key: counts[key] for key in counts if counts[key] > baseline.get(key, 0)}
    fixed = {key: baseline[key] - counts[key] for key in baseline if counts[key] < baseline[key]}

    print(f"当前 {sum(counts.values())} 条，基线 {sum(baseline.values())} 条")
    for key, count in sorted(regressions.items()):
        print(f"  新增 {key}: {baseline.get(key, 0)} -> {count}")
    for key, count in sorted(fixed.items()):
        print(f"  减少 {key}: -{count}（可用 --update 收紧基线）")

    if regressions:
        print("类型检查出现新增错误，请修复或说明后更新基线。")
        return 1
    print("没有新增类型错误。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
