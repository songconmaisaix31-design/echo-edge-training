"""Run the released Stanford checkpoint using the repository's preprocessing."""
import argparse
import json
from pathlib import Path
import sys
from types import SimpleNamespace


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--engine-report", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.runtime.resolve()))

    import av
    from av.video.reformatter import VideoReformatter
    import torch
    from benchmark_echo import normalize_image, resize_and_pad
    from evaluate_echo import checkpoint_model, decision, locked_policy
    from train_echo import WEIGHTS, whole_video_samples
    from train_echonet_views import VIEWS

    torch.set_num_threads(2)
    report = json.loads(args.engine_report.read_text(encoding="utf-8-sig"))
    model, checkpoint, labels, digest = checkpoint_model(args.checkpoint, torch.device("cpu"))
    if (
        digest != report["checkpoint_sha256"]
        or labels != {"view": VIEWS}
        or report["labels"] != {"view": list(VIEWS)}
        or checkpoint["preprocessing"] != report["preprocessing"]
        or checkpoint["sampling"] != report["sampling"]
        or report["aggregation"] != "mean_frame_softmax"
    ):
        raise ValueError("Checkpoint and release report do not match")
    threshold = locked_policy(report["policy"], labels)["min_confidence"]["view"]
    normalizer = WEIGHTS.transforms()
    reformatter = VideoReformatter()
    scores = torch.zeros(len(VIEWS))
    count = 0
    with torch.inference_mode():
        for _, frame in whole_video_samples(
            lambda: av.open(str(args.video)), SimpleNamespace(**checkpoint["sampling"])
        ):
            image = normalize_image(resize_and_pad(frame, reformatter), normalizer)
            (logits,) = model(image.unsqueeze(0))
            scores += logits[0].softmax(dim=0)
            count += 1
    if not count or not scores.isfinite().all():
        raise ValueError("No valid predictions")
    mean = scores / count
    print(json.dumps({
        "precision": "fp32",
        "view": (*VIEWS, None)[decision(mean, threshold)],
        "sampled_frames": count,
        "probabilities": dict(zip(VIEWS, mean.tolist())),
    }, allow_nan=False))


if __name__ == "__main__":
    main()
