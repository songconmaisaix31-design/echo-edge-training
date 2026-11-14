import argparse
import io
import json
import math
from itertools import zip_longest
from pathlib import Path
from time import perf_counter
from types import SimpleNamespace
from zipfile import ZipFile

import av
import torch
import tensorrt as trt
from av.video.reformatter import VideoReformatter

from benchmark_echo import latency_summary, preprocess
from evaluate_echo import PREPROCESSING, decision, locked_policy, stanford_members, summarize
from stanford_tensorrt import TensorRTForward, TensorRTGraph
from train_echo import VIDEO_SUFFIXES, WEIGHTS, whole_video_samples
from train_echonet_views import VIEWS


WARMUP_CLIPS = 5
PROFILE_STAGES = ("decode_and_sample", "preprocess", "upload", "inference", "download")
SUBMISSION_STAGES = ("host_enqueue", "blocking_readback", "gpu_interval")


def timed_samples(frames, timings):
    iterator = iter(frames)
    while True:
        started = perf_counter()
        item = next(iterator, None)
        timings["decode_and_sample"] += perf_counter() - started
        if item is None:
            return
        yield item


class VideoInference:
    def __init__(self, forward, metadata, *, use_graph=False):
        self.forward = forward
        self.normalization = WEIGHTS.transforms()
        self.cutoff = locked_policy(metadata["policy"], {"view": VIEWS})["min_confidence"]["view"]
        self.sampling = SimpleNamespace(**metadata["sampling"])
        self.graph = TensorRTGraph(forward, self.sampling.max_frames_per_segment) if use_graph else None

    def images(self, open_video, decode_passes, timings):
        reformatter = VideoReformatter()
        frames = whole_video_samples(open_video, self.sampling, decode_passes=decode_passes)
        if timings is not None:
            frames = timed_samples(frames, timings)
        for _, frame in frames:
            started = perf_counter() if timings is not None else None
            image = preprocess(frame, self.normalization, reformatter)
            if timings is not None:
                timings["preprocess"] += perf_counter() - started
            yield image

    def eager_logits(self, images, timings, submission_profile):
        for image in images:
            if submission_profile is not None:
                scores, measured = self.forward.profile_submission(image)
                for stage, seconds in measured.items():
                    submission_profile[stage].append(seconds)
            elif timings is None:
                scores = self.forward.predict(image)
            else:
                scores, upload, inference, download = self.forward.profile_predict(image)
                timings["upload"] += upload
                timings["inference"] += inference
                timings["download"] += download
            yield scores

    def logits(self, open_video, decode_passes=1, timings=None, submission_profile=None):
        images = self.images(open_video, decode_passes, timings)
        if self.graph is not None:
            return self.graph.predict_clip(images)
        return self.eager_logits(images, timings, submission_profile)

    @torch.inference_mode()
    def classify(self, open_video, *, decode_passes=1, timings=None, submission_profile=None):
        probabilities = torch.zeros(len(VIEWS))
        count = 0
        for scores in self.logits(open_video, decode_passes, timings, submission_profile):
            if not torch.isfinite(scores).all():
                raise ValueError("TensorRT 输出含非有限值")
            probabilities += scores.softmax(dim=0)
            count += 1
        if not count:
            raise ValueError("视频没有抽到可用帧")
        mean_probabilities = probabilities / count
        codes = (*VIEWS, None)
        return {"view": codes[decision(mean_probabilities, self.cutoff)], "sampled_frames": count,
                "probabilities": dict(zip(VIEWS, mean_probabilities.tolist(), strict=True))}


def load_metadata(path):
    metadata = json.loads(path.read_text(encoding="utf-8-sig"))
    sampling = metadata.get("sampling", {})
    fps = sampling.get("sample_fps", 0)
    limit = sampling.get("max_frames_per_segment")
    runtime = metadata["runtime"]
    if (metadata["labels"] != {"view": list(VIEWS)} or metadata["preprocessing"] != PREPROCESSING
            or metadata["aggregation"] != "mean_frame_softmax" or metadata["split"] != "test"
            or runtime["version"] != trt.__version__ or runtime["gpu"] != torch.cuda.get_device_name()
            or type(fps) not in (float, int) or not math.isfinite(fps) or fps <= 0
            or type(limit) is not int or limit <= 0):
        raise ValueError("验证报告的模型配置或 TensorRT 环境与当前配置不一致")
    return metadata


def load_inference(directory, metadata, precision, runtime, stream, *, use_graph=False):
    model = metadata["models"][precision]
    name = Path(model["file"])
    if name.name != model["file"] or name.suffix != ".engine":
        raise ValueError("验证报告需要包含引擎文件名")
    engine_path = directory / name
    forward = TensorRTForward(engine_path, runtime, stream, fp16=precision == "fp16",
                             expected_digest=model["sha256"])
    return VideoInference(forward, metadata, use_graph=use_graph)


def write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)


def profile_pipeline(args, metadata, model):
    measurements = {name: [] for name in (*PROFILE_STAGES, "transfer", "instrumented_wall")}
    rows = []
    frames = 0
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        val = stanford_members(archive, "val", metadata)
        test = stanford_members(archive, "test", metadata)
        for _, path, _ in val[:WARMUP_CLIPS]:
            video = archive.read(path)
            model.classify(lambda: av.open(io.BytesIO(video)), decode_passes=args.decode_passes,
                           timings=dict.fromkeys(PROFILE_STAGES, 0.0))
        for patient, path, truth in test:
            video = archive.read(path)
            timings = dict.fromkeys(PROFILE_STAGES, 0.0)
            started = perf_counter()
            result = model.classify(lambda: av.open(io.BytesIO(video)), decode_passes=args.decode_passes,
                                    timings=timings)
            timings["instrumented_wall"] = perf_counter() - started
            timings["transfer"] = timings["upload"] + timings["download"]
            for name, elapsed in timings.items():
                measurements[name].append(elapsed)
            rows.append((truth, (*VIEWS, None).index(result["view"]), patient))
            frames += result["sampled_frames"]
            if len(rows) % 100 == 0:
                print(f"已测量 {len(rows)} 段视频的阶段耗时", flush=True)
    if (len(rows) != metadata["clips"] or len({row[2] for row in rows}) != metadata["patients"]
            or frames != metadata["frames"] or summarize(rows, VIEWS) != metadata["models"]["fp16"]["view"]):
        raise ValueError("阶段测量未复现已锁定的测试折")
    write_report(args.report, {
        "dataset": metadata["dataset"], "original_fold": "0", "decode_passes": args.decode_passes,
        "engine_sha256": model.forward.digest, "clips": len(rows), "sampled_frames": frames,
        "runtime": {"gpu": torch.cuda.get_device_name(), "tensorrt": trt.__version__,
                    "decoder_threads": 1, "warmup_validation_clips": WARMUP_CLIPS, "batch": 1},
        "timing_scope": "Per-clip totals. CPU wall time for decode/sampling, preprocess and synchronized transfers; "
                        "CUDA events for inference. ZIP reads and warmup excluded. "
                        "Synchronization and instrumentation differ from the uninstrumented end-to-end benchmark.",
        "stage_latency": {name: latency_summary(values) for name, values in measurements.items()},
    })
    print(f"已测量 {len(rows)} 段视频、{frames} 个抽样帧", flush=True)


def profile_submission(args, metadata, model):
    samples = {stage: [] for stage in SUBMISSION_STAGES}
    clips = {stage: [] for stage in SUBMISSION_STAGES}
    rows = []
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        for _, path, _ in stanford_members(archive, "val", metadata)[:WARMUP_CLIPS]:
            video = archive.read(path)
            model.classify(lambda: av.open(io.BytesIO(video)), submission_profile={key: [] for key in samples})
        for patient, path, truth in stanford_members(archive, "test", metadata):
            video = archive.read(path)
            measured = {stage: [] for stage in SUBMISSION_STAGES}
            result = model.classify(lambda: av.open(io.BytesIO(video)), submission_profile=measured)
            rows.append((truth, (*VIEWS, None).index(result["view"]), patient))
            for stage, elapsed in measured.items():
                samples[stage].extend(elapsed)
                clips[stage].append(sum(elapsed))
            if len(rows) % 100 == 0:
                print(f"已测量 {len(rows)} 段视频的提交和同步开销", flush=True)
    if (len(rows) != metadata["clips"] or len({row[2] for row in rows}) != metadata["patients"]
            or any(len(values) != metadata["frames"] for values in samples.values())
            or summarize(rows, VIEWS) != metadata["models"]["fp16"]["view"]):
        raise ValueError("逐帧提交测量未复现已锁定的测试折")
    write_report(args.report, {
        "dataset": metadata["dataset"], "original_fold": "0", "clips": len(rows),
        "sampled_frames": len(samples["host_enqueue"]), "fp16_engine_sha256": model.forward.digest,
        "runtime": {"gpu": torch.cuda.get_device_name(), "tensorrt": trt.__version__,
                    "decode_passes": 1, "batch": 1, "warmup_validation_clips": WARMUP_CLIPS},
        "timing_scope": {
            "host_enqueue": "CPU wall time inside execute_async_v3, excluding event recording",
            "blocking_readback": "CPU wall time of blocking .cpu(), including outstanding GPU wait and output copy",
            "gpu_interval": "CUDA events bracketing TensorRT enqueue on its stream",
            "overlap": "GPU interval can overlap host enqueue and blocking readback; do not add all three",
        },
        "per_frame": {stage: latency_summary(values) for stage, values in samples.items()},
        "per_clip": {stage: latency_summary(values) for stage, values in clips.items()},
    })
    print(f"完成 {len(rows)} 段视频的逐帧提交测量", flush=True)


def benchmark(args, metadata, models):
    latencies = {name: [] for name in models}
    rows = {name: [] for name in models}
    samples = {name: 0 for name in models}
    patients = set()
    disagreements = 0
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        val = stanford_members(archive, "val", metadata)
        test = stanford_members(archive, "test", metadata)
        for _, path, _ in val[:WARMUP_CLIPS]:
            video = archive.read(path)
            for model in models.values():
                model.classify(lambda: av.open(io.BytesIO(video)))
        print("预热完成，开始原始测试折 0", flush=True)
        for index, (patient, path, truth) in enumerate(test):
            patients.add(patient)
            video = archive.read(path)
            results = {}
            order = ("fp32", "fp16") if index % 2 == 0 else ("fp16", "fp32")
            for name in order:
                started = perf_counter()
                result = models[name].classify(lambda: av.open(io.BytesIO(video)))
                latencies[name].append(perf_counter() - started)
                samples[name] += result["sampled_frames"]
                predicted = (*VIEWS, None).index(result["view"])
                rows[name].append((truth, predicted, patient))
                results[name] = (predicted, result["sampled_frames"])
            if results["fp32"][1] != results["fp16"][1]:
                raise ValueError("两份引擎抽帧数不一致")
            disagreements += int(results["fp32"][0] != results["fp16"][0])
            if (index + 1) % 100 == 0:
                print(f"已对照 {index + 1} 段视频", flush=True)

    if len(test) != metadata["clips"] or len(patients) != metadata["patients"] or any(
            count != metadata["frames"] for count in samples.values()):
        raise ValueError("复测样本数量与锁定引擎验证报告不一致")
    report = {
        "dataset": metadata["dataset"], "split": "test", "original_fold": "0",
        "checkpoint_sha256": metadata["checkpoint_sha256"], "sampling": metadata["sampling"],
        "policy": metadata["policy"], "aggregation": metadata["aggregation"],
        "clips": len(test), "patients": len(patients), "sampled_frames": samples["fp32"],
        "clip_class_disagreements": disagreements,
        "runtime": {"version": trt.__version__, "gpu": torch.cuda.get_device_name(), "batch": 1,
                    "decoder_threads": 1, "warmup_validation_clips_per_engine": WARMUP_CLIPS,
                    "timing_scope": "read-ready encoded clip, one PyAV decode pass through final clip decision",
                    "throughput_scope": "clips or sampled frames divided by total measured clip latency",
                    "excluded": ["ZIP extraction", "engine loading", "warmup", "report writing"]},
        "models": {},
    }
    for name in models:
        elapsed = sum(latencies[name])
        view = summarize(rows[name], VIEWS)
        if view != metadata["models"][name]["view"]:
            raise ValueError(f"全链路分类指标与先前引擎复测不一致 {name}")
        report["models"][name] = {
            "engine": metadata["models"][name]["file"],
            "engine_sha256": models[name].forward.digest, "view": view,
            "clip_latency": latency_summary(latencies[name]),
            "total_measured_seconds": elapsed,
            "clips_per_second": len(test) / elapsed,
            "sampled_frames_per_second": samples[name] / elapsed,
        }
    write_report(args.report, report)
    print(f"完成 {len(test)} 段视频、每个引擎 {samples['fp32']} 个抽样帧", flush=True)


def validate_sampled_inputs(archive, test, model):
    verified = 0
    for _, path, _ in test:
        video = archive.read(path)

        def open_video():
            return av.open(io.BytesIO(video))

        before = whole_video_samples(open_video, model.sampling, decode_passes=2)
        after = whole_video_samples(open_video, model.sampling, decode_passes=1)
        old_scaler, new_scaler = VideoReformatter(), VideoReformatter()
        for left, right in zip_longest(before, after):
            if left is None or right is None or left[1].pts != right[1].pts or left[1].time_base != right[1].time_base:
                raise ValueError("单遍解码改变了抽样帧数量或呈现时间戳")
            old_image = preprocess(left[1], model.normalization, old_scaler)
            new_image = preprocess(right[1], model.normalization, new_scaler)
            if not torch.equal(old_image, new_image):
                raise ValueError("单遍解码改变了抽样帧的归一化输入")
            verified += 1
    return verified


def benchmark_decode(args, metadata, model):
    passes = (2, 1)
    latencies = {count: [] for count in passes}
    rows = []
    frames = 0
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        val = stanford_members(archive, "val", metadata)
        test = stanford_members(archive, "test", metadata)
        for _, path, _ in val[:WARMUP_CLIPS]:
            video = archive.read(path)
            for count in passes:
                model.classify(lambda: av.open(io.BytesIO(video)), decode_passes=count)
        for index, (patient, path, truth) in enumerate(test):
            video = archive.read(path)
            results = {}
            for count in passes if index % 2 == 0 else reversed(passes):
                started = perf_counter()
                results[count] = model.classify(lambda: av.open(io.BytesIO(video)), decode_passes=count)
                latencies[count].append(perf_counter() - started)
            if results[2] != results[1]:
                raise ValueError("单遍解码改变了片段判定、概率或抽样帧数")
            rows.append((truth, (*VIEWS, None).index(results[1]["view"]), patient))
            frames += results[1]["sampled_frames"]
            if len(rows) % 100 == 0:
                print(f"已完成 {len(rows)} 段视频的全链路对照", flush=True)
        print("开始逐帧核对呈现时间戳和归一化输入", flush=True)
        verified = validate_sampled_inputs(archive, test, model)
    patients = len({row[2] for row in rows})
    view = summarize(rows, VIEWS)
    if (len(rows) != metadata["clips"] or patients != metadata["patients"] or frames != metadata["frames"]
            or verified != frames or view != metadata["models"]["fp16"]["view"]):
        raise ValueError("全链路优化对照未复现已锁定的测试折")
    write_report(args.report, {
        "dataset": metadata["dataset"], "original_fold": "0", "checkpoint_sha256": metadata["checkpoint_sha256"],
        "fp16_engine_sha256": model.forward.digest, "clips": len(rows), "patients": patients, "sampled_frames": frames,
        "identical_sampled_frames": verified, "identical_clip_predictions_and_probabilities": len(rows), "view": view,
        "runtime": {"gpu": torch.cuda.get_device_name(), "tensorrt": trt.__version__, "decoder_threads": 1,
                    "warmup_validation_clips_per_path": WARMUP_CLIPS, "batch": 1,
                    "timing_scope": "Wall time from opening the encoded video in memory to final clip decision.",
                    "excluded": ["ZIP reads", "engine loading", "warmup", "frame equivalence checks", "report writing"]},
        "pipelines": {
            f"{count}_decode_passes": {
                "clip_latency": latency_summary(values), "total_measured_seconds": sum(values),
                "clips_per_second": len(rows) / sum(values), "sampled_frames_per_second": frames / sum(values),
            } for count, values in latencies.items()
        },
    })
    print(f"全部 {len(rows)} 段判定、{verified} 帧输入一致", flush=True)


@torch.inference_mode()
def benchmark_graph(args, metadata, models):
    names = ("per_frame", "cuda_graph")
    latencies = {name: [] for name in names}
    rows = []
    frames = 0
    identical_logits = 0
    digests = {model.forward.digest for model in models.values()}
    if len(digests) != 1:
        raise ValueError("CUDA Graph 对照需要同一份 FP16 引擎")
    with ZipFile(args.archive, metadata_encoding="utf-8") as archive:
        val = stanford_members(archive, "val", metadata)
        test = stanford_members(archive, "test", metadata)
        for _, path, _ in val[:WARMUP_CLIPS]:
            video = archive.read(path)
            for model in models.values():
                model.classify(lambda: av.open(io.BytesIO(video)))
        for index, (patient, path, truth) in enumerate(test):
            video = archive.read(path)
            results = {}
            for name in names if index % 2 == 0 else reversed(names):
                started = perf_counter()
                results[name] = models[name].classify(lambda: av.open(io.BytesIO(video)))
                latencies[name].append(perf_counter() - started)
            before, after = (results[name] for name in names)
            if before != after:
                raise ValueError("CUDA Graph 改变了片段判定、概率或抽样帧数")
            rows.append((truth, (*VIEWS, None).index(after["view"]), patient))
            frames += after["sampled_frames"]
            if len(rows) % 100 == 0:
                print(f"已完成 {len(rows)} 段视频的全链路对照", flush=True)
        print("开始逐帧核对原始模型输出", flush=True)
        for _, path, _ in test:
            video = archive.read(path)
            expected = models["per_frame"].logits(lambda: av.open(io.BytesIO(video)))
            observed = models["cuda_graph"].logits(lambda: av.open(io.BytesIO(video)))
            for left, right in zip(expected, observed, strict=True):
                if not torch.equal(left, right):
                    raise ValueError("CUDA Graph 改变了逐帧原始模型输出")
                identical_logits += 1
    patients = len({row[2] for row in rows})
    view = summarize(rows, VIEWS)
    if (len(rows) != metadata["clips"] or patients != metadata["patients"] or frames != metadata["frames"]
            or identical_logits != frames or view != metadata["models"]["fp16"]["view"]):
        raise ValueError("CUDA Graph 对照未复现已锁定的测试折")
    write_report(args.report, {
        "dataset": metadata["dataset"], "original_fold": "0", "checkpoint_sha256": metadata["checkpoint_sha256"],
        "fp16_engine_sha256": next(iter(digests)), "clips": len(rows), "patients": patients, "sampled_frames": frames,
        "identical_frame_logits": identical_logits, "identical_clip_predictions_and_probabilities": len(rows),
        "view": view,
        "runtime": {"gpu": torch.cuda.get_device_name(), "tensorrt": trt.__version__, "decode_passes": 1,
                    "decoder_threads": 1, "warmup_validation_clips_per_path": WARMUP_CLIPS, "batch": 1,
                    "timing_scope": "Wall time from opening the encoded video in memory to final clip decision.",
                    "excluded": ["ZIP reads", "engine loading", "graph capture", "warmup", "logit equivalence checks",
                                 "report writing"]},
        "pipelines": {
            name: {"clip_latency": latency_summary(values), "total_measured_seconds": sum(values),
                   "clips_per_second": len(rows) / sum(values), "sampled_frames_per_second": frames / sum(values)}
            for name, values in latencies.items()
        },
    })
    print(f"全部 {len(rows)} 段判定、{identical_logits} 帧原始输出一致", flush=True)


def main():
    parser = argparse.ArgumentParser(description="用 Stanford TensorRT 引擎对本地视频判定切面")
    actions = parser.add_subparsers(dest="action", required=True)
    predict = actions.add_parser("predict")
    compare = actions.add_parser("benchmark")
    profile = actions.add_parser("profile")
    submission = actions.add_parser("profile-submission")
    compare_decode = actions.add_parser("benchmark-decode")
    compare_graph = actions.add_parser("benchmark-graph")
    for action in (predict, compare, profile, compare_decode, submission, compare_graph):
        action.add_argument("--engine-report", type=Path, required=True)
    predict.add_argument("--video", type=Path, required=True)
    predict.add_argument("--precision", choices=("fp16", "fp32"), default="fp16")
    for action in (compare, profile, compare_decode, submission, compare_graph):
        action.add_argument("--archive", type=Path, required=True)
        action.add_argument("--report", type=Path, required=True)
    profile.add_argument("--decode-passes", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    if not torch.cuda.is_available():
        raise RuntimeError("TensorRT 视频推理需要 CUDA")
    if args.action == "predict" and (not args.video.is_file() or args.video.suffix.lower() not in VIDEO_SUFFIXES):
        parser.error("--video 需要指向 AVI、MP4 或 MOV 文件")
    if args.action != "predict" and args.report.exists():
        raise FileExistsError(args.report)
    torch.set_num_threads(1)
    metadata = load_metadata(args.engine_report)
    logger = trt.Logger(trt.Logger.WARNING)
    runtime = trt.Runtime(logger)
    stream = torch.cuda.Stream()
    directory = args.engine_report.parent
    if args.action == "predict":
        model = load_inference(directory, metadata, args.precision, runtime, stream, use_graph=args.precision == "fp16")
        result = model.classify(lambda: av.open(str(args.video)))
        print(json.dumps({"precision": args.precision, **result}, ensure_ascii=False, allow_nan=False))
    elif args.action == "profile":
        model = load_inference(directory, metadata, "fp16", runtime, stream)
        profile_pipeline(args, metadata, model)
    elif args.action == "profile-submission":
        model = load_inference(directory, metadata, "fp16", runtime, stream)
        profile_submission(args, metadata, model)
    elif args.action == "benchmark-decode":
        model = load_inference(directory, metadata, "fp16", runtime, stream)
        benchmark_decode(args, metadata, model)
    elif args.action == "benchmark-graph":
        models = {name: load_inference(directory, metadata, "fp16", runtime, stream, use_graph=name == "cuda_graph")
                  for name in ("per_frame", "cuda_graph")}
        benchmark_graph(args, metadata, models)
    else:
        models = {name: load_inference(directory, metadata, name, runtime, stream, use_graph=name == "fp16")
                  for name in ("fp32", "fp16")}
        benchmark(args, metadata, models)


if __name__ == "__main__":
    main()
