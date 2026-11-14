import argparse
import json
import math
import shutil
import statistics
import tempfile
import time
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import av
import torch
import torchvision
from PIL import Image
from av.video.reformatter import VideoReformatter
from torchvision.models import MobileNet_V3_Small_Weights, mobilenet_v3_small
from torchvision.transforms import functional as transforms


IMAGE_SIZE = 224
DECODER_THREADS = 4
WARMUP_FRAMES = 40


def configure_video(container):
    stream = container.streams.video[0]
    stream.thread_type = "AUTO"
    stream.codec_context.thread_count = DECODER_THREADS
    return stream


def scaled_size(frame):
    scale = IMAGE_SIZE / max(frame.width, frame.height)
    return tuple(max(1, round(side * scale)) for side in (frame.width, frame.height))


def resize_rgb_frame(frame, reformatter, size):
    return reformatter.reformat(frame, width=size[0], height=size[1], format="rgb24", interpolation="AREA", threads=1)


def pad_rgb_image(resized):
    padded = Image.new("RGB", (IMAGE_SIZE, IMAGE_SIZE))
    padded.paste(resized, tuple((IMAGE_SIZE - side) // 2 for side in resized.size))
    return padded


def resize_and_pad(frame, reformatter=None):
    size = scaled_size(frame)
    if reformatter is None:
        resized = frame.to_image().resize(size, Image.Resampling.BILINEAR)
    else:
        resized = resize_rgb_frame(frame, reformatter, size).to_image()
    return pad_rgb_image(resized)


def normalize_pixels(tensor, normalization):
    tensor = tensor.float().div_(255)
    return transforms.normalize(tensor, normalization.mean, normalization.std)


def image_pixels(image):
    return transforms.pil_to_tensor(image)


def normalize_image(image, normalization):
    return normalize_pixels(image_pixels(image), normalization)


def preprocess(frame, normalization, reformatter=None):
    return normalize_image(resize_and_pad(frame, reformatter), normalization).unsqueeze(0)


class CudaForward:
    def __init__(self, model, use_graph):
        self.model = model
        self.use_graph = use_graph
        if use_graph:
            self.host_input = torch.empty((1, 3, IMAGE_SIZE, IMAGE_SIZE), pin_memory=True)
            self.device_input = torch.empty_like(self.host_input, device="cuda")
            self.graph = torch.cuda.CUDAGraph()

    def prepare(self, tensor):
        if self.use_graph:
            self.host_input.copy_(tensor)
            return self.host_input
        return tensor

    def transfer(self, tensor):
        if self.use_graph:
            self.device_input.copy_(tensor, non_blocking=True)
            return self.device_input
        return tensor.to("cuda")

    def warmup(self, tensor):
        sample = self.transfer(self.prepare(tensor))
        if self.use_graph:
            current_stream = torch.cuda.current_stream()
            warmup_stream = torch.cuda.Stream()
            warmup_stream.wait_stream(current_stream)
            with torch.cuda.stream(warmup_stream):
                for _ in range(WARMUP_FRAMES):
                    self.model(sample)
            current_stream.wait_stream(warmup_stream)
            torch.cuda.synchronize()
            with torch.cuda.graph(self.graph):
                self.graph_output = self.model(sample)
            self.graph.replay()
        else:
            for _ in range(WARMUP_FRAMES):
                self.model(sample)
        torch.cuda.synchronize()

    def __call__(self, tensor):
        if self.use_graph:
            self.graph.replay()
            return self.graph_output
        return self.model(tensor)


def latency_summary(seconds):
    percentiles = statistics.quantiles(seconds, n=100, method="inclusive")
    return {
        "mean_ms": statistics.mean(seconds) * 1000,
        "p50_ms": statistics.median(seconds) * 1000,
        "p95_ms": percentiles[94] * 1000,
        "p99_ms": percentiles[98] * 1000,
    }


def benchmark_video(path, forward, normalization, sample_fps, warmup, mode):
    reformatter = None if mode == "baseline" else VideoReformatter()
    if warmup:
        with av.open(str(path)) as container:
            stream = configure_video(container)
            frame = next(container.decode(stream))
        forward.warmup(preprocess(frame, normalization, reformatter))
        torch.cuda.reset_peak_memory_stats()

    decode_start = time.perf_counter()
    with av.open(str(path)) as container:
        stream = configure_video(container)
        metadata = {
            "codec": stream.codec_context.name,
            "width": stream.codec_context.width,
            "height": stream.codec_context.height,
            "source_fps": float(stream.average_rate),
        }
        decode_only_wait = []
        decode_tick = time.perf_counter()
        for _ in container.decode(stream):
            decode_only_wait.append(time.perf_counter() - decode_tick)
            decode_tick = time.perf_counter()
        decode_count = len(decode_only_wait)
    decode_seconds = time.perf_counter() - decode_start

    timings = {stage: [] for stage in (
        "decode_only_frame_wait", "pipeline_frame_wait", "decode_wait_per_sample",
        "preprocess", "preprocess_thread_cpu", "h2d_host", "h2d_cuda", "h2d_and_forward",
        "cuda_forward", "forward_host", "sample_to_logits", "decode_plus_sample_to_logits",
    )}
    timings["decode_only_frame_wait"] = decode_only_wait
    copy_start = torch.cuda.Event(enable_timing=True)
    gpu_start = torch.cuda.Event(enable_timing=True)
    gpu_end = torch.cuda.Event(enable_timing=True)
    first_pts = None
    last_pts = None
    last_slot = -1
    processed = 0
    decode_since_sample = 0.0
    pipeline_start = time.perf_counter()
    with av.open(str(path)) as container:
        stream = configure_video(container)
        decode_tick = time.perf_counter()
        for frame in container.decode(stream):
            decode_wait = time.perf_counter() - decode_tick
            timings["pipeline_frame_wait"].append(decode_wait)
            decode_since_sample += decode_wait
            processed += 1
            timestamp = frame.time
            if timestamp is None or (last_pts is not None and timestamp < last_pts):
                raise ValueError(f"视频帧缺少时间戳或时间戳回退：{path.name} 第 {processed} 帧")
            last_pts = timestamp
            if first_pts is None:
                first_pts = timestamp
            slot = math.floor((timestamp - first_pts) * sample_fps + 1e-6)
            if slot == last_slot:
                decode_tick = time.perf_counter()
                continue
            last_slot = slot

            sample_start = time.perf_counter()
            cpu_start = time.thread_time()
            tensor = forward.prepare(preprocess(frame, normalization, reformatter))
            preprocess_cpu = time.thread_time() - cpu_start
            prep_end = time.perf_counter()
            copy_start.record()
            tensor = forward.transfer(tensor)
            copy_end_host = time.perf_counter()
            gpu_start.record()
            logits = forward(tensor)
            gpu_end.record()
            gpu_end.synchronize()
            sample_end = time.perf_counter()
            timings["preprocess"].append(prep_end - sample_start)
            timings["preprocess_thread_cpu"].append(preprocess_cpu)
            timings["h2d_host"].append(copy_end_host - prep_end)
            timings["h2d_cuda"].append(copy_start.elapsed_time(gpu_start) / 1000)
            timings["h2d_and_forward"].append(sample_end - prep_end)
            timings["cuda_forward"].append(gpu_start.elapsed_time(gpu_end) / 1000)
            timings["forward_host"].append(sample_end - copy_end_host)
            timings["sample_to_logits"].append(sample_end - sample_start)
            timings["decode_wait_per_sample"].append(decode_since_sample)
            timings["decode_plus_sample_to_logits"].append(decode_since_sample + sample_end - sample_start)
            decode_since_sample = 0.0
            decode_tick = time.perf_counter()
    pipeline_seconds = time.perf_counter() - pipeline_start

    if processed != decode_count or not timings["cuda_forward"]:
        raise RuntimeError(f"解码帧数不一致或未产生推理样本：{path.name}")
    if tuple(logits.shape) != (1, 1000) or not torch.isfinite(logits).all().item():
        raise RuntimeError(f"模型输出形状异常或包含非有限值：{path.name}")
    return {
        **metadata,
        "decoded_frames": decode_count,
        "inferred_frames": len(timings["cuda_forward"]),
        "decode_only_s": decode_seconds,
        "decode_only_fps": decode_count / decode_seconds,
        "pipeline_s": pipeline_seconds,
        "pipeline_inferences_per_s": len(timings["cuda_forward"]) / pipeline_seconds,
        "latency": {stage: latency_summary(values) for stage, values in timings.items()},
        "logits_dtype": str(logits.dtype),
    }, timings


def benchmark(args):
    if not torch.cuda.is_available():
        raise RuntimeError("PyTorch 无法使用 CUDA")
    torch.set_num_threads(1)
    device = torch.cuda.get_device_properties(0)
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    torch.hub.set_dir(str(args.cache_dir / "torch-hub"))
    weights = MobileNet_V3_Small_Weights.IMAGENET1K_V1
    model = mobilenet_v3_small(weights=weights).eval().to("cuda")
    forward = CudaForward(model, use_graph=args.mode == "optimized")
    normalization = weights.transforms()
    clips = []
    all_timings = {}

    with (
        ZipFile(args.archive, metadata_encoding="utf-8") as archive,
        tempfile.TemporaryDirectory(dir=args.cache_dir) as scratch,
    ):
        entries = [
            entry for entry in archive.infolist()
            if not entry.is_dir()
            and "__MACOSX" not in PurePosixPath(entry.filename).parts
            and PurePosixPath(entry.filename).suffix.lower() in {".mp4", ".mov"}
        ]
        if args.clip:
            requested = set(args.clip)
            entries = [entry for entry in entries if PurePosixPath(entry.filename).name in requested]
            missing = requested - {PurePosixPath(entry.filename).name for entry in entries}
            if missing:
                raise ValueError(f"压缩包内找不到指定视频：{', '.join(sorted(missing))}")
        if not entries:
            raise ValueError("压缩包内没有 MP4 或 MOV 视频")
        with torch.inference_mode(), torch.autocast("cuda", dtype=torch.float16, enabled=args.precision == "fp16"):
            for index, entry in enumerate(entries):
                name = PurePosixPath(entry.filename)
                path = Path(scratch) / f"{index:02d}{name.suffix.lower()}"
                with archive.open(entry) as source, path.open("wb") as target:
                    shutil.copyfileobj(source, target, 1024 * 1024)
                result, timings = benchmark_video(path, forward, normalization, args.sample_fps, index == 0, args.mode)
                path.unlink()
                clips.append({"file": name.name, **result})
                for stage, values in timings.items():
                    all_timings.setdefault(stage, []).extend(values)
                print(f"{name.name}: {result['decoded_frames']} decoded, {result['inferred_frames']} inferred", flush=True)

    decode_count = sum(clip["decoded_frames"] for clip in clips)
    inferred_count = sum(clip["inferred_frames"] for clip in clips)
    decode_seconds = sum(clip["decode_only_s"] for clip in clips)
    pipeline_seconds = sum(clip["pipeline_s"] for clip in clips)
    return {
        "device": device.name,
        "compute_capability": [device.major, device.minor],
        "vram_mib": device.total_memory / 1024**2,
        "torch": torch.__version__,
        "torchvision": torchvision.__version__,
        "cuda_runtime": torch.version.cuda,
        "pyav": av.__version__,
        "weights": weights.name,
        "model": "MobileNetV3-Small",
        "mode": args.mode,
        "model_parameters": sum(param.numel() for param in model.parameters()),
        "precision": "CUDA autocast FP16" if args.precision == "fp16" else "FP32",
        "input_nchw": [1, 3, IMAGE_SIZE, IMAGE_SIZE],
        "preprocessing": (
            "Full-resolution RGB then Pillow bilinear resize" if args.mode == "baseline"
            else "Fused FFmpeg area resize and RGB conversion, one scaler thread"
        ) + "; aspect-preserving resize, black padding and ImageNet normalization",
        "forward_backend": "CUDA Graph and reusable pinned input" if args.mode == "optimized" else "PyTorch eager",
        "source_sample_fps": args.sample_fps,
        "zip_metadata_encoding": "utf-8",
        "decoder_threads": DECODER_THREADS,
        "torch_cpu_threads": torch.get_num_threads(),
        "warmup_forward_passes": WARMUP_FRAMES,
        "timing_scope": {
            "decode_only": "Separate local-file pass with CPU decoding of every source frame",
            "pipeline": "Local-file decoding of every source frame and inference on timestamp-selected frames",
            "sample_to_logits": "Decoded frame to completed GPU logits, including CPU preprocessing and device transfer",
            "decode_wait_per_sample": "Sum of host waits for decoded frames since the preceding selected frame",
            "h2d_cuda": "CUDA event interval spanning the input transfer; blocking submission can add a host scheduling gap",
            "cuda_forward": "CUDA events around the model forward after device transfer",
            "excluded": "ZIP extraction, weight loading, warm-up, rendering and networking",
        },
        "decoded_frames": decode_count,
        "inferred_frames": inferred_count,
        "decode_only_s": decode_seconds,
        "decode_only_fps": decode_count / decode_seconds,
        "pipeline_s": pipeline_seconds,
        "pipeline_inferences_per_s": inferred_count / pipeline_seconds,
        "torch_peak_allocated_mib": torch.cuda.max_memory_allocated() / 1024**2,
        "torch_peak_reserved_mib": torch.cuda.max_memory_reserved() / 1024**2,
        "latency": {stage: latency_summary(values) for stage, values in all_timings.items()},
        "clips": clips,
    }


def main():
    parser = argparse.ArgumentParser(description="用真实超声视频测量 MobileNetV3-Small 的 CUDA 推理耗时")
    parser.add_argument("archive", type=Path)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--sample-fps", type=float, default=10)
    parser.add_argument("--precision", choices=("fp32", "fp16"), default="fp16")
    parser.add_argument("--mode", choices=("baseline", "optimized"), default="optimized")
    parser.add_argument("--clip", action="append", help="按文件名选择视频，可重复指定")
    args = parser.parse_args()
    if not math.isfinite(args.sample_fps) or args.sample_fps <= 0:
        parser.error("--sample-fps 必须是有限正数")
    result = benchmark(args)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "clips"}, indent=2))


if __name__ == "__main__":
    main()
