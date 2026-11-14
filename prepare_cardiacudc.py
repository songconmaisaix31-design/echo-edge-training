import argparse
import io
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
from PIL import Image

SIDE = 256


def assign_splits(groups, site_of, dense_of, seed):
    rng = np.random.default_rng(seed)
    strata = defaultdict(list)
    for group in groups:
        strata[(site_of[group], dense_of[group])].append(group)
    split_of = {}
    for key in sorted(strata):
        names = sorted(strata[key])
        rng.shuffle(names)
        total = len(names)
        if total >= 10:
            test = max(1, round(total * 0.15))
            val = max(1, round(total * 0.15))
        elif total >= 4:
            test, val = 1, 1
        elif total >= 2:
            test, val = 1, 0
        else:
            test, val = 0, 0
        for index, group in enumerate(names):
            split_of[group] = "test" if index < test else "validation" if index < test + val else "train"
    return split_of


def main():
    parser = argparse.ArgumentParser(description="把 HF MedOtter/CardiacUDC 的 annotated parquet 转成四腔训练数组")
    parser.add_argument("--parquet", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--side", type=int, default=SIDE)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    table = pq.read_table(args.parquet).to_pydict()
    rows = len(table["video_id"])
    groups = sorted(set(table["group_id"]))
    site_of = {}
    dense_of = {}
    for group, site, dense in zip(table["group_id"], table["site"], table["gt_dense"], strict=True):
        site_of[group] = site
        dense_of[group] = dense_of.get(group, False) or bool(dense)
    split_of = assign_splits(groups, site_of, dense_of, args.seed)
    images, masks, meta = [], [], []
    for index in range(rows):
        if not table["has_mask"][index]:
            raise ValueError("annotated 配置中出现无掩膜行")
        image = Image.open(io.BytesIO(table["image"][index]["bytes"])).convert("L").resize(
            (args.side, args.side), Image.BILINEAR)
        mask = Image.open(io.BytesIO(table["mask"][index]["bytes"])).resize(
            (args.side, args.side), Image.NEAREST)
        group = table["group_id"][index]
        images.append(np.asarray(image, dtype=np.uint8))
        masks.append(np.asarray(mask, dtype=np.uint8))
        meta.append({"split": split_of[group], "group": group, "video": table["video_id"][index],
                     "site": table["site"][index], "dense": bool(table["gt_dense"][index]),
                     "folder": table["folder"][index], "frame": int(table["frame_index"][index])})
    arrays = np.stack(images)
    mask_arrays = np.stack(masks)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    np.save(args.output_dir / "images.npy", arrays)
    np.save(args.output_dir / "masks.npy", mask_arrays)
    (args.output_dir / "meta.json").write_text(
        json.dumps({"side": args.side, "classes": ["background", "LV", "LA", "RA", "RV"],
                    "split_seed": args.seed, "frames": meta}, ensure_ascii=False) + "\n", encoding="utf-8")
    for split in ("train", "validation", "test"):
        chosen = [row for row in meta if row["split"] == split]
        group_count = len({row["group"] for row in chosen})
        print(split, "frames", len(chosen), "groups", group_count,
              "sites", {site: sum(1 for row in chosen if row["site"] == site) for site in ("G", "R", "unknown")})
    label_counts = {int(value): int((mask_arrays == value).sum()) for value in np.unique(mask_arrays)}
    print("labels", label_counts)


if __name__ == "__main__":
    main()
