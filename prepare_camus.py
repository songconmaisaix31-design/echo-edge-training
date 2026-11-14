import argparse
import io
import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
from PIL import Image

SIDE = 256
FILES = (("ed_es__train-00000-of-00001.parquet", "train"),
         ("ed_es__validation-00000-of-00001.parquet", "validation"),
         ("ed_es__test-00000-of-00001.parquet", "test"))


def main():
    parser = argparse.ArgumentParser(description="把 HF MedOtter/CAMUS 的 ed_es parquet 转成训练用 npy 数组")
    parser.add_argument("--parquet-dir", type=Path, required=True, help="ed_es__*.parquet 所在目录")
    parser.add_argument("--output-dir", type=Path, required=True, help="输出 images.npy、masks.npy、meta.json")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    frames = []
    for name, split in FILES:
        path = args.parquet_dir / name
        if not path.exists():
            raise FileNotFoundError(path)
        table = pq.read_table(path).to_pydict()
        for index in range(len(table["patient_id"])):
            image = Image.open(io.BytesIO(table["image"][index]["bytes"])).convert("L").resize(
                (SIDE, SIDE), Image.BILINEAR)
            mask = Image.open(io.BytesIO(table["mask"][index]["bytes"])).resize((SIDE, SIDE), Image.NEAREST)
            frames.append({"split": split, "patient": table["patient_id"][index],
                           "view": table["view"][index], "phase": table["phase"][index],
                           "quality": table["image_quality"][index], "ef": float(table["ef"][index]),
                           "image": np.asarray(image, dtype=np.uint8),
                           "mask": np.asarray(mask, dtype=np.uint8)})
        print(split, "ready", len(frames), flush=True)
    images = np.stack([frame["image"] for frame in frames])
    masks = np.stack([frame["mask"] for frame in frames])
    meta = [{key: frame[key] for key in ("split", "patient", "view", "phase", "quality", "ef")} for frame in frames]
    np.save(args.output_dir / "images.npy", images)
    np.save(args.output_dir / "masks.npy", masks)
    (args.output_dir / "meta.json").write_text(
        json.dumps({"side": SIDE, "frames": meta}, ensure_ascii=False) + "\n", encoding="utf-8")
    print("saved", images.shape, masks.shape, flush=True)


if __name__ == "__main__":
    main()
