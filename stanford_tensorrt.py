import argparse
import hashlib
import json
import subprocess
from collections import Counter
from pathlib import Path
from time import perf_counter

import onnx
import torch
import tensorrt as trt
from av.video.reformatter import VideoReformatter

from benchmark_echo import WARMUP_FRAMES, latency_summary, preprocess
from edge_onnx import INPUT_SHAPE, load_locked
from evaluate_echo import decision, file_digest, stanford_samples, summarize
from train_echo import WEIGHTS
from train_echonet_views import VIEWS


def build_engine(onnx_path, target, logger, *, fp16):
    builder = trt.Builder(logger)
    network = builder.create_network(0)
    parser = trt.OnnxParser(network, logger)
    if not parser.parse_from_file(str(onnx_path)):
        errors = "\n".join(str(parser.get_error(i)) for i in range(parser.num_errors))
        raise RuntimeError(f"TensorRT 无法解析 ONNX\n{errors}")
    config = builder.create_builder_config()
    config.clear_flag(trt.BuilderFlag.TF32)
    if fp16:
        config.set_flag(trt.BuilderFlag.FP16)
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 1024**3)
    config.profiling_verbosity = trt.ProfilingVerbosity.DETAILED
    config.max_aux_streams = 0
    serialized = builder.build_serialized_network(network, config)
    if serialized is None:
        raise RuntimeError("TensorRT 未能构建引擎")
    with target.open("xb") as output:
        output.write(bytes(serialized))


class TensorRTForward:
    def __init__(self, path, runtime, stream, *, fp16, expected_digest=None):
        serialized = path.read_bytes()
        self.digest = hashlib.sha256(serialized).hexdigest()
        if expected_digest is not None and self.digest != expected_digest:
            raise ValueError(f"引擎摘要与验证报告不一致 {path.name}")
        self.engine = runtime.deserialize_cuda_engine(serialized)
        if self.engine is None:
            raise RuntimeError(f"TensorRT 无法加载引擎 {path.name}")
        io = {self.engine.get_tensor_name(i) for i in range(self.engine.num_io_tensors)}
        if io != {"images", "view_logits"}:
            raise ValueError("TensorRT 引擎签名与 Stanford ONNX 不一致")
        self.context = self.engine.create_execution_context()
        if self.context is None:
            raise RuntimeError("TensorRT 无法创建执行上下文")
        self.stream = stream
        self.start = torch.cuda.Event(enable_timing=True)
        self.end = torch.cuda.Event(enable_timing=True)
        self.input = self.bind("images", INPUT_SHAPE, trt.TensorIOMode.INPUT)
        self.output = self.bind("view_logits", (1, len(VIEWS)), trt.TensorIOMode.OUTPUT)
        inspector = self.engine.create_engine_inspector()
        inspector.execution_context = self.context
        details = json.loads(inspector.get_engine_information(trt.LayerInformationFormat.JSON))
        formats = Counter(tensor["Format/Datatype"] for layer in details["Layers"] if isinstance(layer, dict)
                          for io_name in ("Inputs", "Outputs") for tensor in layer.get(io_name, [])
                          if "Format/Datatype" in tensor)
        self.formats = dict(sorted(formats.items()))
        uses_half = any("half" in name.lower() or "fp16" in name.lower() for name in formats)
        if not formats or uses_half != fp16:
            raise RuntimeError(f"引擎内部张量精度与构建配置不符 {self.formats}")
        self.context.nvtx_verbosity = trt.ProfilingVerbosity.NONE
        self.profile_start = torch.cuda.Event(enable_timing=True)
        self.profile_end = torch.cuda.Event(enable_timing=True)

    def bind(self, name, shape, mode):
        if (tuple(self.engine.get_tensor_shape(name)) != shape
                or self.engine.get_tensor_dtype(name) != trt.float32
                or self.engine.get_tensor_mode(name) != mode
                or self.engine.get_tensor_location(name) != trt.TensorLocation.DEVICE
                or self.engine.get_tensor_format(name) != trt.TensorFormat.LINEAR):
            raise ValueError(f"TensorRT 张量签名异常 {name}")
        tensor = torch.empty(shape, dtype=torch.float32, device="cuda")
        if not self.context.set_tensor_address(name, tensor.data_ptr()):
            raise RuntimeError(f"TensorRT 无法绑定张量 {name}")
        return tensor

    def enqueue(self):
        if not self.context.execute_async_v3(self.stream.cuda_stream):
            raise RuntimeError("TensorRT CUDA 推理失败")

    def predict(self, image):
        with torch.cuda.stream(self.stream):
            self.input.copy_(image)
            self.enqueue()
            return self.output[0].cpu()

    def profile_predict(self, image):
        with torch.cuda.stream(self.stream):
            started = perf_counter()
            self.input.copy_(image)
            self.stream.synchronize()
            uploaded = perf_counter()
            self.start.record(self.stream)
            self.enqueue()
            self.end.record(self.stream)
            self.end.synchronize()
            inference = self.start.elapsed_time(self.end) / 1000
            download_started = perf_counter()
            scores = self.output[0].cpu()
            download = perf_counter() - download_started
        return scores, uploaded - started, inference, download

    def profile_submission(self, image):
        with torch.cuda.stream(self.stream):
            self.input.copy_(image)
            self.profile_start.record(self.stream)
            started = perf_counter()
            self.enqueue()
            submitted = perf_counter()
            self.profile_end.record(self.stream)
            waiting = perf_counter()
            scores = self.output[0].cpu()
            completed = perf_counter()
            gpu = self.profile_start.elapsed_time(self.profile_end) / 1000
        return scores, {
            "host_enqueue": submitted - started,
            "blocking_readback": completed - waiting,
            "gpu_interval": gpu,
        }

    def run(self, image):
        self.input.copy_(image)
        self.start.record(self.stream)
        self.enqueue()
        self.end.record(self.stream)
        self.end.synchronize()
        elapsed = self.start.elapsed_time(self.end) / 1000
        return self.output[0].cpu(), elapsed


class TensorRTGraph:
    def __init__(self, forward, max_frames, *, double_buffer=False):
        self.forward = forward
        input_slots = 2 if double_buffer else max_frames
        self.host_inputs = torch.empty((input_slots, *forward.input.shape),
                                       dtype=forward.input.dtype, pin_memory=True)
        self.host_outputs = torch.empty((max_frames, forward.output.shape[1]), dtype=forward.output.dtype,
                                        pin_memory=True)
        self.graph = None
        self.uploaded = [torch.cuda.Event() for _ in range(2)] if double_buffer else None

    def capture(self):
        forward = self.forward
        forward.input.copy_(self.host_inputs[0], non_blocking=True)
        forward.enqueue()
        forward.stream.synchronize()
        graph = torch.cuda.CUDAGraph()
        with torch.cuda.graph(graph, stream=forward.stream):
            forward.enqueue()
        self.graph = graph

    def predict_clip(self, images):
        count = 0
        for image in images:
            self.host_inputs[count].copy_(image)
            count += 1
        return self.replay_clip(count)

    def replay_clip(self, count, timings=None):
        if not count:
            raise ValueError("视频没有抽到可用帧")
        forward = self.forward
        with torch.cuda.stream(forward.stream):
            if self.graph is None:
                self.capture()
            for index in range(count):
                forward.input.copy_(self.host_inputs[index], non_blocking=True)
                self.graph.replay()
                self.host_outputs[index].copy_(forward.output[0], non_blocking=True)
            waiting = perf_counter() if timings is not None else None
            forward.stream.synchronize()
            if timings is not None:
                timings["cpu_wait_clip"] = perf_counter() - waiting
        return self.host_outputs[:count]

    def replay_overlapped(self, frames, write, timings=None, check_input=None):
        forward = self.forward
        count = 0
        previous_complete = torch.cuda.Event() if timings is not None else None
        with torch.cuda.stream(forward.stream):
            try:
                for index, (_, frame) in enumerate(frames):
                    slot = index % 2
                    if index >= 2 and not self.uploaded[slot].query():
                        waiting = perf_counter() if timings is not None else None
                        self.uploaded[slot].synchronize()
                        if timings is not None:
                            timings["cpu_wait_input_slot"] += perf_counter() - waiting
                    if timings is not None:
                        pending = index > 0 and not previous_complete.query()
                        started = perf_counter()
                    write(frame, slot)
                    if timings is not None:
                        timings["cpu_preprocessing"] += perf_counter() - started
                        timings["preprocess_frames_with_pending_gpu"] += int(pending)
                    if check_input is not None:
                        check_input(index, self.host_inputs[slot])
                    if self.graph is None:
                        self.capture()
                    forward.input.copy_(self.host_inputs[slot], non_blocking=True)
                    self.uploaded[slot].record(forward.stream)
                    self.graph.replay()
                    self.host_outputs[index].copy_(forward.output[0], non_blocking=True)
                    if previous_complete is not None:
                        previous_complete.record(forward.stream)
                    count += 1
            finally:
                # 即使预处理报错，也要等未完成的复制结束后再释放或复用锁页内存。
                waiting = perf_counter() if timings is not None else None
                forward.stream.synchronize()
                if timings is not None:
                    timings["cpu_wait_clip"] = perf_counter() - waiting
        if not count:
            raise ValueError("视频没有抽到可用帧")
        return self.host_outputs[:count]


@torch.inference_mode()
def evaluate(args, metadata, forwards):
    for key, setting in metadata["sampling"].items():
        setattr(args, key, setting)
    args.split = "test"
    normalization = WEIGHTS.transforms()
    cutoff = metadata["policy"]["min_confidence"]["view"]
    patients = set()
    clips = set()
    metrics = [[], []]
    times = [[], []]
    differences = Counter(frame_class_disagreements=0, clip_class_disagreements=0)
    max_error = 0.0
    frame_count = 0
    for patient, clip, (target,), frames in stanford_samples(args, metadata):
        reformatter = VideoReformatter()
        totals = [torch.zeros(len(VIEWS)), torch.zeros(len(VIEWS))]
        count = 0
        for _, frame in frames:
            image = preprocess(frame, normalization, reformatter)
            if frame_count == 0:
                for forward in forwards:
                    for _ in range(WARMUP_FRAMES):
                        forward.run(image)
            logits = [None, None]
            for index in (0, 1) if frame_count % 2 == 0 else (1, 0):
                scores, elapsed = forwards[index].run(image)
                if not torch.isfinite(scores).all() or elapsed <= 0:
                    raise ValueError("TensorRT 输出或 CUDA 计时无效")
                logits[index] = scores
                times[index].append(elapsed)
                totals[index] += scores.softmax(dim=0)
            max_error = max(max_error, float((logits[0] - logits[1]).abs().max()))
            differences["frame_class_disagreements"] += int(logits[0].argmax() != logits[1].argmax())
            count += 1
            frame_count += 1
        if not count or clip in clips:
            raise ValueError("测试片段重复或没有可用帧")
        clips.add(clip)
        patients.add(patient)
        predictions = [decision(total / count, cutoff) for total in totals]
        differences["clip_class_disagreements"] += int(predictions[0] != predictions[1])
        for rows, predicted in zip(metrics, predictions, strict=True):
            rows.append((target, predicted, patient))
        if len(clips) % 100 == 0:
            print(f"已评估 {len(clips)} 段", flush=True)
    if len(clips) != 658 or len(patients) != 186:
        raise ValueError("独立测试需覆盖 186 名患儿的 658 段视频")
    return ({"clips": len(clips), "patients": len(patients), "frames": frame_count,
             "agreement": {**differences, "max_abs_logit_difference": max_error}}, metrics, times)


def run(args):
    _, metadata = load_locked(args, "cpu")
    graph = onnx.load(args.onnx)
    props = {item.key: item.value for item in graph.metadata_props}
    if (metadata["labels"] != {"view": list(VIEWS)} or json.loads(props["echo_edge"]) != metadata
            or any(node.op_type in {"QuantizeLinear", "DequantizeLinear"} for node in graph.graph.node)):
        raise ValueError("ONNX 必须是对应 Stanford 锁定 checkpoint 的 FP32 导出文件")
    source_digest = file_digest(args.onnx)
    driver = subprocess.check_output(
        ["nvidia-smi", "-i", "0", "--query-gpu=driver_version", "--format=csv,noheader"], text=True,
    ).strip()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    logger = trt.Logger(trt.Logger.WARNING)
    names = ("fp32", "fp16")
    paths = [args.output_dir / f"Stanford-A4C-PSAX-TensorRT-{name.upper()}.engine" for name in names]
    for name, path in zip(names, paths, strict=True):
        print(f"正在构建 TensorRT {name.upper()} 引擎", flush=True)
        build_engine(args.onnx, path, logger, fp16=name == "fp16")
    if file_digest(args.onnx) != source_digest:
        raise ValueError("构建期间源 ONNX 发生变化")
    digests = [file_digest(path) for path in paths]
    runtime = trt.Runtime(logger)
    stream = torch.cuda.Stream()
    with torch.cuda.stream(stream):
        forwards = [TensorRTForward(path, runtime, stream, fp16=name == "fp16")
                    for name, path in zip(names, paths, strict=True)]
        print("两份引擎已构建并加载，开始原始测试折 0", flush=True)
        results, metrics, times = evaluate(args, metadata, forwards)
    if [file_digest(path) for path in paths] != digests:
        raise ValueError("测试期间引擎文件发生变化")
    report = {
        **metadata, **results, "dataset": "EchoNet-Pediatric grey-scale", "split": "test", "original_fold": "0",
        "source_onnx_sha256": source_digest, "runtime": {
            "version": trt.__version__, "pytorch_cuda": torch.version.cuda, "driver": driver,
            "gpu": torch.cuda.get_device_name(), "compute_capability": list(torch.cuda.get_device_capability()),
            "batch": 1, "warmup_per_engine": WARMUP_FRAMES, "tf32": False, "aux_streams": 0,
            "timing_scope": "CUDA events around execute_async_v3, excluding host/device transfers and preprocessing",
        },
        "models": {
            name: {"file": path.name, "bytes": path.stat().st_size, "sha256": digest,
                   "precision": "FP16 enabled (mixed)" if name == "fp16" else "FP32",
                   "engine_tensor_formats": forward.formats, "inference_per_frame": latency_summary(timing),
                   "view": summarize(rows, VIEWS)}
            for name, path, digest, forward, timing, rows in zip(names, paths, digests, forwards,
                                                                times, metrics, strict=True)
        },
    }
    with (args.output_dir / "Stanford-TensorRT-FP32-FP16-test.json").open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)
    print(f"已评估 {results['clips']} 段、{results['frames']} 帧", flush=True)


def main():
    parser = argparse.ArgumentParser(description="构建 Stanford TensorRT 引擎并在独立测试折对照 FP32 和 FP16")
    for name in ("archive", "checkpoint", "policy", "onnx", "output-dir"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args()
    if not torch.cuda.is_available():
        raise RuntimeError("TensorRT 对照需要 CUDA")
    torch.set_num_threads(1)
    run(args)


if __name__ == "__main__":
    main()
