import argparse
import json
import platform
import tempfile
from collections import Counter
from pathlib import Path
from time import perf_counter

import onnx
import onnxruntime as ort
import torch
from av.video.reformatter import VideoReformatter
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static
from onnxruntime.quantization.shape_inference import quant_pre_process

from benchmark_echo import WARMUP_FRAMES, latency_summary, preprocess
from edge_onnx import OnnxForward, load_locked
from evaluate_echo import decision, file_digest, stanford_samples, summarize
from train_echo import WEIGHTS
from train_echonet_views import VIEWS


CPU_THREADS = 1
CALIBRATION_CLIPS_PER_CLASS = 80


def stanford_metadata(args):
    model, metadata = load_locked(args, "cpu")
    if metadata["labels"] != {"view": list(VIEWS)}:
        raise ValueError("需要 Stanford A4C／PSAX 单头 checkpoint")
    for key, setting in metadata["sampling"].items():
        setattr(args, key, setting)
    return model, metadata


class ValidationReader(CalibrationDataReader):
    def __init__(self, args, metadata):
        self.args = args
        self.metadata = metadata
        self.frames = 0
        self.counts = Counter()
        self.items = self.inputs()

    def inputs(self):
        normalization = WEIGHTS.transforms()
        for _, _, (target,), frames in stanford_samples(self.args, self.metadata,
                                                        clips_per_class=CALIBRATION_CLIPS_PER_CLASS):
            self.counts[VIEWS[target]] += 1
            reformatter = VideoReformatter()
            for _, frame in frames:
                self.frames += 1
                yield {"images": preprocess(frame, normalization, reformatter).numpy()}

    def get_next(self):
        return next(self.items, None)


def quantize(args):
    _, metadata = stanford_metadata(args)
    if args.int8.exists():
        raise FileExistsError(args.int8)
    OnnxForward(args.fp32, "cpu", metadata, threads=CPU_THREADS)
    args.split = "val"
    reader = ValidationReader(args, metadata)
    args.int8.parent.mkdir(parents=True, exist_ok=True)
    source_digest = file_digest(args.fp32)
    with tempfile.TemporaryDirectory(dir=args.int8.parent) as directory:
        scratch = Path(directory)
        prepared = scratch / "prepared.onnx"
        target = scratch / "int8.onnx"
        previous_tempdir = tempfile.tempdir
        try:
            tempfile.tempdir = scratch
            quant_pre_process(args.fp32, prepared, skip_optimization=True)
            quantize_static(prepared, target, reader, quant_format=QuantFormat.QDQ, per_channel=True,
                            activation_type=QuantType.QInt8, weight_type=QuantType.QInt8,
                            op_types_to_quantize=["Conv", "Gemm", "MatMul"],
                            calibration_providers=["CPUExecutionProvider"],
                            extra_options={"CalibMaxIntermediateOutputs": 32})
        finally:
            tempfile.tempdir = previous_tempdir
        if reader.counts != dict.fromkeys(VIEWS, CALIBRATION_CLIPS_PER_CLASS) or not reader.frames:
            raise ValueError("验证折校准样本不完整")
        graph = onnx.load(target)
        props = {item.key: item.value for item in graph.metadata_props}
        props.update(echo_edge=json.dumps(metadata, ensure_ascii=False), echo_edge_quantization=json.dumps({
            "method": "static S8S8 QDQ MinMax",
            "per_channel_weights": True,
            "op_types": ["Conv", "Gemm", "MatMul"],
            "calibration_original_fold": "1",
            "calibration_seed": 42,
            "calibration_clips": dict(reader.counts),
            "calibration_frames": reader.frames,
            "source_onnx_sha256": source_digest,
        }))
        onnx.helper.set_model_props(graph, props)
        onnx.checker.check_model(graph)
        onnx.save(graph, target)
        OnnxForward(target, "cpu", metadata, threads=CPU_THREADS)
        target.rename(args.int8)
    print(f"验证折完成校准，共 {sum(reader.counts.values())} 段、{reader.frames} 帧", flush=True)


def profile_int8(path, metadata, image):
    forward = OnnxForward(path, "cpu", metadata, threads=CPU_THREADS, profiling=True)
    forward.run(image)
    profile = Path(forward.session.end_profiling())
    rows = json.loads(profile.read_text(encoding="utf-8"))
    profile.unlink()
    operations = Counter(row["args"]["op_name"] for row in rows
                         if row.get("args", {}).get("provider") == "CPUExecutionProvider")
    if not any(op.startswith(("QLinear", "QGemm", "MatMulInteger", "ConvInteger")) for op in operations):
        raise RuntimeError("CPU 分析未发现执行的整数模型算子")
    return dict(sorted(operations.items()))


@torch.inference_mode()
def compare(args):
    if args.report.exists():
        raise FileExistsError(args.report)
    reference, metadata = stanford_metadata(args)
    paths = (args.fp32, args.int8)
    forwards = [OnnxForward(path, "cpu", metadata, threads=CPU_THREADS) for path in paths]
    digests = [file_digest(path) for path in paths]
    quantization = json.loads(forwards[1].session.get_modelmeta().custom_metadata_map["echo_edge_quantization"])
    if quantization["source_onnx_sha256"] != digests[0] or quantization["calibration_original_fold"] != "1":
        raise ValueError("INT8 模型需要由当前 FP32 模型和验证折生成")
    graph = onnx.load(args.int8)
    ops = Counter(node.op_type for node in graph.graph.node)
    int8_weights = sum(weight.data_type == onnx.TensorProto.INT8 and len(weight.dims) >= 2
                       for weight in graph.graph.initializer)
    if not ops["QuantizeLinear"] or not ops["DequantizeLinear"] or not int8_weights:
        raise ValueError("INT8 模型缺少 QDQ 节点或整数权重")
    print("FP32 和 INT8 模型摘要已核对，开始原始测试折 0", flush=True)
    args.split = "test"
    normalized = WEIGHTS.transforms()
    metrics = [[], []]
    timings = [[], []]
    preprocess_seconds = []
    patients = set()
    clips = set()
    changed = Counter()
    cutoff = metadata["policy"]["min_confidence"]["view"]
    max_fp32_error = 0.0
    frame_count = 0
    for patient, clip, (target,), frames in stanford_samples(args, metadata):
        reformatter = VideoReformatter()
        totals = [torch.zeros(len(VIEWS)) for _ in range(3)]
        count = 0
        for _, frame in frames:
            started = perf_counter()
            image = preprocess(frame, normalized, reformatter)
            preprocessed = perf_counter() - started
            if frame_count == 0:
                executed_int8_ops = profile_int8(args.int8, metadata, image)
                for forward in forwards:
                    for _ in range(WARMUP_FRAMES):
                        forward.run(image)
            preprocess_seconds.append(preprocessed)
            expected, = reference(image)
            if not torch.isfinite(expected).all():
                raise ValueError("PyTorch 对照输出含非有限值")
            for index in (0, 1) if frame_count % 2 == 0 else (1, 0):
                (output,), _, elapsed = forwards[index].run(image)
                if not torch.isfinite(output).all():
                    raise ValueError("ONNX 输出含非有限值")
                timings[index].append(elapsed)
                totals[index] += output[0].softmax(dim=0)
            fp32, = forwards[0].outputs
            int8, = forwards[1].outputs
            max_fp32_error = max(max_fp32_error, float((fp32 - expected).abs().max()))
            changed["fp32_vs_pytorch_frames"] += int(fp32.argmax() != expected.argmax())
            changed["int8_vs_fp32_frames"] += int(int8.argmax() != fp32.argmax())
            totals[2] += expected[0].softmax(dim=0)
            count += 1
            frame_count += 1
        if not count or clip in clips:
            raise ValueError("测试片段重复或没有可用帧")
        patients.add(patient)
        clips.add(clip)
        predictions = [decision(total / count, cutoff) for total in totals]
        for rows, predicted in zip(metrics, predictions[:2], strict=True):
            rows.append((target, predicted, patient))
        changed["fp32_vs_pytorch_clips"] += int(predictions[0] != predictions[2])
        changed["int8_vs_fp32_clips"] += int(predictions[1] != predictions[0])
        if len(clips) % 100 == 0:
            print(f"已评估 {len(clips)} 段", flush=True)
    if len(clips) != 658 or len(patients) != 186:
        raise ValueError("原始测试折需要包含 186 名患儿的 658 段视频")
    if [file_digest(path) for path in paths] != digests:
        raise ValueError("测试期间模型文件发生变化")
    report = {
        **metadata, "dataset": "EchoNet-Pediatric grey-scale", "split": "test", "original_fold": "0",
        "clips": len(clips), "patients": len(patients), "frames": frame_count,
        "runtime": {"version": ort.__version__, "provider": forwards[0].provider, "cpu": platform.processor(),
                    "intra_op_threads": CPU_THREADS, "inter_op_threads": 1, "batch": 1,
                    "warmup_per_model": WARMUP_FRAMES, "timing_scope": "prebound ONNX session run and output sync"},
        "shared_preprocessing_per_frame": latency_summary(preprocess_seconds),
        "quantization": {**quantization, "int8_weight_tensors": int8_weights,
                         "qdq_nodes": {name: ops[name] for name in ("QuantizeLinear", "DequantizeLinear")},
                         "runtime_ops": executed_int8_ops},
        "agreement": {**changed, "fp32_vs_pytorch_max_abs_logit_error": max_fp32_error},
        "models": {name: {"file": path.name, "bytes": path.stat().st_size, "sha256": digest,
                          "inference_per_frame": latency_summary(timing), "view": summarize(rows, VIEWS)}
                   for name, path, digest, timing, rows in zip(("fp32", "int8"), paths, digests, timings,
                                                               metrics, strict=True)},
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)
    print(f"已评估 {len(clips)} 段、{frame_count} 帧", flush=True)


def main():
    parser = argparse.ArgumentParser(description="用验证折量化 Stanford ONNX 并在独立测试折对照")
    commands = parser.add_subparsers(dest="action", required=True)
    quantizing = commands.add_parser("quantize")
    comparing = commands.add_parser("compare")
    for command in (quantizing, comparing):
        for parameter in ("archive", "checkpoint", "policy", "fp32", "int8"):
            command.add_argument(f"--{parameter}", type=Path, required=True)
    comparing.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    torch.set_num_threads(CPU_THREADS)
    if args.action == "quantize":
        quantize(args)
    else:
        compare(args)


if __name__ == "__main__":
    main()
