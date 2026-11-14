import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

    import av
    import numpy as np
    from PIL import Image
    import torch
    from train_cardiacudc import CLASS_NAMES, SIDE, load_model

    with av.open(str(args.video)) as video:
        frame = next(image for index, image in enumerate(video.decode(video=0)) if index == args.frame)
        source = frame.to_image().convert("RGB")
    gray = source.convert("L").resize((SIDE, SIDE), Image.Resampling.BILINEAR)
    torch.set_num_threads(2)
    model, saved = load_model(args.checkpoint, torch.device("cpu"))
    with torch.inference_mode():
        logits = model(torch.from_numpy(np.array(gray)).float().div_(255)[None, None])
        if logits.shape != (1, len(CLASS_NAMES), SIDE, SIDE) or not logits.isfinite().all():
            raise ValueError("四腔模型输出无效")
        mask = logits.argmax(1)[0].byte().numpy()

    labels = Image.fromarray(mask).resize(source.size, Image.Resampling.NEAREST)
    colors = np.array([(0, 0, 0, 0), (66, 165, 245, 115), (102, 187, 106, 115),
                       (255, 167, 38, 115), (171, 71, 188, 115)], dtype=np.uint8)
    overlay = Image.fromarray(colors[np.asarray(labels)])
    Image.alpha_composite(source.convert("RGBA"), overlay).save(args.output, format="PNG")
    print(json.dumps({"epoch": saved["epoch"], "classes": list(CLASS_NAMES),
                      "foreground_pixels": int(np.count_nonzero(mask))}, allow_nan=False))


if __name__ == "__main__":
    main()
