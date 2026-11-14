import argparse
import json
import math
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import numpy as np
import onnx
import torch
import onnxruntime as ort
from av.video.reformatter import VideoReformatter
from pydicom.misc import is_dicom

from benchmark_echo import latency_summary, normalize_image, resize_and_pad
from evaluate_echo import PREPROCESSING, checkpoint_model, decision, locked_policy
from train_echo import (IGNORE, MEDIA_SUFFIXES, TASKS, WEIGHTS, Segment, interval_bins, local_media, open_archive,
                        selected_interval_frames)


INPUT_SHAPE = (1, 3, 224, 224)


def output_names(labels):
    return tuple(f"{task}_logits" for task in labels)


class WindowsGpuMemory:
    def __init__(self):
        self.stop = threading.Event()
        self.values = []
        self.errors = []
        pid = os.getpid()
        self.command = (f"$ErrorActionPreference='Stop'; ($samples = (Get-Counter "
                        f"'\\GPU Process Memory(*)\\Dedicated Usage').CounterSamples) | Out-Null; "
                        f"$samples | Where-Object InstanceName -Like 'pid_{pid}_*' | "
                        "ForEach-Object { [long]$_.CookedValue }")
        self.thread = threading.Thread(target=self.measure, daemon=True)

    def measure(self):
        while not self.stop.is_set():
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", self.command],
                                    capture_output=True, text=True, timeout=12, check=False)
            if result.returncode:
                self.errors.append(result.stderr.strip())
                return
            values = [int(line) for line in result.stdout.splitlines() if line.strip()]
            if values:
                self.values.append(sum(values))
            self.stop.wait(0.5)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *ignore):
        self.stop.set()
        self.thread.join()

    def report(self):
        return {"method": "Windows GPU Process Memory Dedicated Usage", "requested_poll_pause_seconds": 0.5,
                "samples": len(self.values), "peak_observed_bytes": max(self.values, default=None),
                "counter_error": self.errors[0] if self.errors else None}


def load_locked(args, device):
    model, checkpoint, labels, digest = checkpoint_model(args.checkpoint, device)
    policy = json.loads(args.policy.read_text(encoding="utf-8-sig"))
    if policy.get("checkpoint_sha256") != digest or policy.get("validated_on") != "val":
        raise ValueError("需要来自同一 checkpoint 的验证集冻结文件")
    metadata = {"checkpoint_sha256": digest, "labels": {task: list(codes) for task, codes in labels.items()},
                "preprocessing": PREPROCESSING, "sampling": checkpoint["sampling"],
                "aggregation": "mean_frame_softmax", "policy": locked_policy(policy, labels)}
    if "quality" not in labels:
        metadata["source_splits"] = checkpoint["source_splits"]
        metadata["validation_report_sha256"] = policy["validation_report_sha256"]
    return model, metadata


def export(args):
    model, metadata = load_locked(args, "cpu")
    if args.onnx.exists():
        raise FileExistsError(args.onnx)
    args.onnx.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.onnx.parent) as scratch:
        target = Path(scratch) / "model.onnx"
        torch.onnx.export(model, (torch.zeros(INPUT_SHAPE),), target, input_names=["images"],
                          output_names=list(output_names(metadata["labels"])), dynamo=True, external_data=False)
        graph = onnx.load(target)
        onnx.helper.set_model_props(graph, {"echo_edge": json.dumps(metadata, ensure_ascii=False)})
        onnx.checker.check_model(graph)
        onnx.save(graph, target)
        target.rename(args.onnx)


class OnnxForward:
    def __init__(self, path, device, metadata, *, threads=None, profiling=False):
        if device == "cuda":
            if not torch.cuda.is_available() or "CUDAExecutionProvider" not in ort.get_available_providers():
                raise RuntimeError("CUDA 需要同环境中的 CUDA PyTorch 和 onnxruntime-gpu")
            ort.preload_dlls()
            torch.backends.cuda.matmul.fp32_precision = "ieee"
            torch.backends.cudnn.conv.fp32_precision = "ieee"
        options = ort.SessionOptions()
        if threads is not None:
            options.intra_op_num_threads = threads
            options.inter_op_num_threads = 1
        if profiling:
            options.enable_profiling = True
            options.profile_file_prefix = str(path.parent / "onnx-profile")
        if device == "cuda":
            options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
        provider = "CUDAExecutionProvider" if device == "cuda" else "CPUExecutionProvider"
        items = [(provider, {"use_tf32": "0"})] if device == "cuda" else [provider]
        self.session = ort.InferenceSession(str(path), sess_options=options, providers=items)
        if self.session.get_providers()[0] != provider:
            raise RuntimeError(f"ONNX Runtime 没有使用 {provider}")
        stored = json.loads(self.session.get_modelmeta().custom_metadata_map["echo_edge"])
        inputs = self.session.get_inputs()
        outputs = self.session.get_outputs()
        labels = metadata["labels"]
        names = output_names(labels)
        if (stored != metadata or len(inputs) != 1 or inputs[0].name != "images" or tuple(inputs[0].shape) != INPUT_SHAPE
                or inputs[0].type != "tensor(float)" or tuple(row.name for row in outputs) != names
                or any(row.shape != [1, len(codes)] or row.type != "tensor(float)"
                       for codes, row in zip(labels.values(), outputs, strict=True))):
            raise ValueError("ONNX 签名或固定模型元数据与锁定的 checkpoint 不一致")
        self.device = device
        self.provider = provider
        self.binding = self.session.io_binding()
        self.input = torch.empty(INPUT_SHAPE, device=device)
        self.outputs = [torch.empty((1, len(codes)), device=device) for codes in labels.values()]
        self.binding.bind_input("images", device, 0, np.float32, INPUT_SHAPE, self.input.data_ptr())
        for name, tensor in zip(names, self.outputs, strict=True):
            self.binding.bind_output(name, device, 0, np.float32, tuple(tensor.shape), tensor.data_ptr())

    def sync(self):
        if self.device == "cuda":
            torch.cuda.synchronize()

    def run(self, image):
        started = time.perf_counter()
        self.input.copy_(image)
        self.sync()
        transferred = time.perf_counter()
        self.session.run_with_iobinding(self.binding)
        self.binding.synchronize_outputs()
        finished = time.perf_counter()
        return self.outputs, transferred - started, finished - transferred


def compare_clip(args, path, expected, actual, metadata):
    args.sample_fps = metadata["sampling"]["sample_fps"]
    args.max_frames_per_segment = metadata["sampling"]["max_frames_per_segment"]
    intervals = [Segment("input", "input", args.start_ms, args.end_ms, (IGNORE, IGNORE))]
    frames = iter(selected_interval_frames(path, intervals, [interval_bins(intervals[0], args)]))
    reformatter = VideoReformatter()
    normalization = WEIGHTS.transforms()
    reference = [torch.zeros(len(metadata["labels"][task])) for task in TASKS]
    observed = [torch.zeros_like(tensor) for tensor in reference]
    maximum = [0.0, 0.0]
    changes = [0, 0]
    timing = {name: [] for name in ("decode_and_sample", "preprocess", "transfer", "onnx", "end_to_end")}
    while True:
        started = time.perf_counter()
        item = next(frames, None)
        if item is None:
            break
        decoded = time.perf_counter()
        image = normalize_image(resize_and_pad(item[1], reformatter), normalization).unsqueeze(0)
        preprocessed = time.perf_counter()
        output, transfer, execution = actual.run(image)
        timing["decode_and_sample"].append(decoded - started)
        timing["preprocess"].append(preprocessed - decoded)
        timing["transfer"].append(transfer)
        timing["onnx"].append(execution)
        timing["end_to_end"].append(time.perf_counter() - started)
        reference_output = expected(image.to(args.device))
        for i, (left, right) in enumerate(zip(reference_output, output, strict=True)):
            left, right = left[0].cpu(), right[0].cpu()
            if not left.isfinite().all() or not right.isfinite().all():
                raise ValueError("模型输出含非有限值")
            maximum[i] = max(maximum[i], float((left - right).abs().max()))
            changes[i] += int(left.argmax() != right.argmax())
            reference[i] += left.softmax(dim=0)
            observed[i] += right.softmax(dim=0)
    count = len(timing["onnx"])
    if not count:
        raise ValueError("指定区间没有可解码帧")
    thresholds = metadata["policy"]["min_confidence"]
    result = {}
    for i, task in enumerate(TASKS):
        codes = [*metadata["labels"][task], None]
        rejected = [codes.index(code) for code in metadata["policy"]["reject_quality"]] if task == "quality" else []
        pytorch = codes[decision(reference[i] / count, thresholds[task], rejected)]
        onnx_code = codes[decision(observed[i] / count, thresholds[task], rejected)]
        result[task] = {"max_abs_logit_error": maximum[i], "frame_class_disagreements": changes[i],
                        "pytorch_interval_decision": pytorch, "onnx_interval_decision": onnx_code}
        if task == "quality":
            usable = metadata["policy"]["usable_quality"]
            result[task]["pytorch_allows_use"] = pytorch in usable
            result[task]["onnx_allows_use"] = onnx_code in usable
    return {"frames": count, "heads": result, "timing": {key: latency_summary(values) for key, values in timing.items()},
            "sampled_frames_per_second": count / sum(timing["end_to_end"])}


@torch.inference_mode()
def verify(args):
    if args.report.exists():
        raise FileExistsError(args.report)
    expected, metadata = load_locked(args, args.device)
    if tuple(metadata["labels"]) != TASKS:
        raise ValueError("verify --media 需要团队双头模型")
    actual = OnnxForward(args.onnx, args.device, metadata)
    args.scratch.mkdir(parents=True, exist_ok=True)
    files = []
    with open_archive(args.media) as archive:
        if archive is not None:
            names = [member.filename for member in archive.infolist() if not member.is_dir()]
        elif args.media.is_dir():
            names = [item.relative_to(args.media).as_posix() for item in args.media.rglob("*") if item.is_file()]
        else:
            names = [args.media.name]
        selected = [name for name in names if Path(name).suffix.lower() in MEDIA_SUFFIXES
                    and "__MACOSX" not in Path(name).parts and Path(name).name.upper() != "DICOMDIR"]
        if not selected:
            raise ValueError("没有找到影像")
        for index, name in enumerate(sorted(selected), start=1):
            with local_media(args.media, archive, name, args.scratch) as source:
                if not source.suffix and (archive is not None or args.media.is_dir()) and not is_dicom(source):
                    continue
                if args.device == "cuda" and os.name == "nt":
                    with WindowsGpuMemory() as memory:
                        video = compare_clip(args, source, expected, actual, metadata)
                    video["gpu_process_memory"] = memory.report()
                else:
                    video = compare_clip(args, source, expected, actual, metadata)
                files.append({"video": index, **video})
    if not files:
        raise ValueError("没有找到可核对的影像")
    failed = any(head["max_abs_logit_error"] > args.max_abs_error
                 or head["frame_class_disagreements"] or head["pytorch_interval_decision"] != head["onnx_interval_decision"]
                 for video in files for head in video["heads"].values())
    report = {**metadata, "verification_passed": not failed, "max_abs_error_tolerance": args.max_abs_error,
              "provider": actual.provider, "runtime_version": ort.__version__, "torch_version": torch.__version__,
              "gpu": torch.cuda.get_device_name() if args.device == "cuda" else None,
              "memory_scope": "PyTorch reference and ONNX comparison process together" if args.device == "cuda" else None,
              "interval_ms": [args.start_ms, args.end_ms], "videos": files}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description="导出冻结的 ONNX 模型并核对团队双头模型")
    commands = parser.add_subparsers(dest="action", required=True)
    exporting = commands.add_parser("export")
    checking = commands.add_parser("verify")
    for command in (exporting, checking):
        command.add_argument("--checkpoint", type=Path, required=True)
        command.add_argument("--policy", type=Path, required=True)
        command.add_argument("--onnx", type=Path, required=True)
    checking.add_argument("--media", type=Path, required=True)
    checking.add_argument("--scratch", type=Path, required=True)
    checking.add_argument("--report", type=Path, required=True)
    checking.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    checking.add_argument("--start-ms", type=int, default=0)
    checking.add_argument("--end-ms", type=int, required=True)
    checking.add_argument("--max-abs-error", type=float, required=True)
    args = parser.parse_args()
    if args.action == "export":
        export(args)
        return 0
    if not 0 <= args.start_ms < args.end_ms or not math.isfinite(args.max_abs_error) or args.max_abs_error < 0:
        parser.error("区间或对照误差上限无效")
    return verify(args)


if __name__ == "__main__":
    raise SystemExit(main())
