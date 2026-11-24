r"""把 CAMUS 嵌入目录的 meta.json 转成 train_emb_head.py 需要的 index.json。

`scripts/embed_camus_panecho.py`（或任何逐文件抽取的脚本）写出 meta.json 与
`<stem>.npy`；训练与评估入口读的是 index.json。这个转换只搬运标签与切分，
不改动数组本身。

    python -X utf8 scripts/index_camus_embeddings.py --emb-dir '.\work\emb'
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description="由 meta.json 生成 index.json")
    parser.add_argument("--emb-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, help="默认写到 <emb-dir>/index.json")
    args = parser.parse_args()

    meta_path = args.emb_dir / "meta.json"
    if not meta_path.exists():
        raise SystemExit(f"缺少 {meta_path}")
    meta = json.loads(meta_path.read_text(encoding="utf-8"))

    index, skipped = {}, 0
    for stem, info in meta.items():
        array_path = args.emb_dir / f"{stem}.npy"
        if not array_path.exists():
            skipped += 1
            continue
        shape = np.load(array_path, mmap_mode="r").shape
        index[stem] = {
            "source": stem,
            "ef": info["ef"],
            "subject": info["subject"],
            "split": info["split"],
            "n_frames": info.get("frames"),
            "n_clips": int(shape[0]),
            "dim": int(shape[1]),
            "view": info.get("view"),
            "quality": info.get("quality"),
        }

    target = args.out or (args.emb_dir / "index.json")
    target.write_text(json.dumps(index, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(index)} 条写入 {target}（跳过 {skipped} 条缺数组的记录）")


if __name__ == "__main__":
    main()
