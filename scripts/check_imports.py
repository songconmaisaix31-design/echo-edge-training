"""导入约束检查：后端不得依赖训练、推理与评测脚本。

用 AST 直接读 `backend/` 下的 import 语句，不依赖包安装或命名空间语义——后端是
一组扁平模块（`import main`、`import clinical`），所以不用 import-linter 的包模型。

    python -X utf8 scripts/check_imports.py
"""

from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "backend"

FORBIDDEN = {
    "train_echo", "train_camus", "train_cardiacudc", "train_echonet_views",
    "train_emb_head", "train_stanford_ef", "train_stanford_lv",
    "infer_stanford", "evaluate_echo", "evaluate_patient_heldout",
    "evaluate_multiview", "evaluate_echofocus", "benchmark_echo",
    "embed_panecho", "stanford_edge", "stanford_tensorrt", "edge_onnx",
}

# 允许的例外：两个独立 CLI 复用训练侧预处理以保持数值一致；一个测试直接测评估器。
ALLOWED = {
    ("stanford_cpu.py", "benchmark_echo"),
    ("stanford_cpu.py", "evaluate_echo"),
    ("stanford_cpu.py", "train_echo"),
    ("stanford_cpu.py", "train_echonet_views"),
    ("cardiacudc_frame.py", "train_cardiacudc"),
    ("test_multiview_independent.py", "evaluate_multiview"),
}

# 契约层不得反向依赖接口层
LAYERED = {"analysis_contract.py", "model_protocol.py"}
LAYER_FORBIDDEN = {"main", "clinical", "platform_db"}


def imported_names(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            names.add(node.module.split(".")[0])
    return names


def check() -> list[str]:
    problems: list[str] = []
    for path in sorted(BACKEND.glob("*.py")):
        names = imported_names(path)
        for name in sorted(names & FORBIDDEN):
            if (path.name, name) not in ALLOWED:
                problems.append(f"{path.relative_to(ROOT).as_posix()} 不允许导入 {name}")
        if path.name in LAYERED:
            for name in sorted(names & LAYER_FORBIDDEN):
                problems.append(f"{path.relative_to(ROOT).as_posix()} 是契约层，不允许反向导入 {name}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list-exceptions", action="store_true")
    args = parser.parse_args()
    if args.list_exceptions:
        for path, module in sorted(ALLOWED):
            print(f"{path} -> {module}")
        return 0
    problems = check()
    if problems:
        for problem in problems:
            print("  " + problem)
        print(f"导入约束未通过：{len(problems)} 处")
        return 1
    print(f"导入约束通过：{len(ALLOWED)} 处例外已登记")
    return 0


if __name__ == "__main__":
    sys.exit(main())
