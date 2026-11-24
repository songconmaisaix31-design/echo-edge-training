r"""把同一患儿的 2CH 与 4CH 嵌入拼成一个检查级样本。

阶段二目前按视频训练：一个患儿贡献两段样本，标签相同。这个脚本把它们拼成
一条序列，用来比较"按视频"与"按患儿（双切面）"两种聚合方式的差别。

合并方式：沿 clip 维拼接，`(n_clips_2CH + n_clips_4CH, 768)`。
标签取两切面的均值（CAMUS 的 EF 是患儿级，两者相同，取均值只是为了标明口径）。

    python -X utf8 scripts/build_dualview_embeddings.py `
      --emb-dir '.\work\emb' --out-dir '.\work\emb_dual'
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description="合并双切面嵌入为检查级样本")
    parser.add_argument("--emb-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--require-both", action="store_true", default=True,
                        help="只保留 2CH 与 4CH 都具备的患儿")
    args = parser.parse_args()

    index = json.loads((args.emb_dir / "index.json").read_text(encoding="utf-8"))
    grouped = defaultdict(list)
    for key, info in index.items():
        grouped[info["subject"]].append((key, info))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    merged_index, kept, dropped = {}, 0, 0
    for subject, items in sorted(grouped.items()):
        views = {info.get("view") for _key, info in items}
        if args.require_both and not {"2CH", "4CH"} <= views:
            dropped += 1
            continue
        arrays = [np.load(args.emb_dir / f"{key}.npy") for key, _info in items]
        total_clips = sum(array.shape[0] for array in arrays)
        if len({array.shape[1] for array in arrays}) != 1:
            raise SystemExit(f"{subject} 的切面嵌入维度不一致")
        merged = np.concatenate(arrays, axis=0).astype(np.float32)
        np.save(args.out_dir / f"{subject}.npy", merged)
        splits = {info.get("split", "train") for _key, info in items}
        if len(splits) != 1:
            raise SystemExit(f"{subject} 的两个切面被分到了不同集合：{splits}")
        merged_index[subject] = {
            "source": f"{subject}:" + "+".join(sorted(views)),
            "ef": float(np.mean([info["ef"] for _key, info in items])),
            "subject": subject,
            "split": splits.pop(),
            "n_clips": int(total_clips),
            "dim": int(merged.shape[1]),
            "views": sorted(views),
        }
        kept += 1

    (args.out_dir / "index.json").write_text(
        json.dumps(merged_index, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"合并 {kept} 名患儿，跳过 {dropped} 名缺切面的患儿 → {args.out_dir}")


if __name__ == "__main__":
    main()
