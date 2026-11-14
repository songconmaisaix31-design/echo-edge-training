import json
import logging
import math
import os
from pathlib import Path
import subprocess
from threading import BoundedSemaphore

from fastapi import HTTPException


SLOTS = BoundedSemaphore(1)
LABELS = {"A4C", "PSAX"}
VIDEO_SUFFIXES = (".avi", ".mp4", ".mov")


def configuration():
    names = ("ECHO_INFERENCE_PYTHON", "ECHO_INFERENCE_SCRIPT", "ECHO_ENGINE_REPORT")
    values = [os.environ.get(name) for name in names]
    if not all(values):
        return "缺少推理环境或 TensorRT 引擎报告", None
    python, script, report = [Path(value) for value in values]
    if not all(path.is_absolute() and path.is_file() for path in (python, script, report)):
        return "找不到配置的解释器、推理脚本或引擎报告", None
    checkpoint_value = os.environ.get("ECHO_STANFORD_CHECKPOINT")
    if checkpoint_value:
        checkpoint = Path(checkpoint_value)
        if not checkpoint.is_absolute() or not checkpoint.is_file():
            return "找不到 Stanford PyTorch 权重", None
        return "Stanford 研究模型 · PyTorch CPU FP32", (python, script, report, checkpoint)
    try:
        metadata = json.loads(report.read_text(encoding="utf-8-sig"))
        engine = Path(metadata["models"]["fp16"]["file"])
        if engine.name != str(engine) or engine.suffix != ".engine":
            raise ValueError
        if not (report.parent / engine).is_file():
            return "引擎报告对应的 FP16 文件未找到", None
    except (OSError, ValueError, KeyError, TypeError):
        return "无法读取有效的 TensorRT 引擎报告", None
    return "模型文件已配置，运行时由推理入口校验", (python, script, report, None)


def predict(folder):
    reason, config = configuration()
    if config is None:
        raise HTTPException(503, reason)
    video = next((folder / ("source" + suffix) for suffix in VIDEO_SUFFIXES
                  if (folder / ("source" + suffix)).is_file()), None)
    if video is None:
        raise HTTPException(422, "需要重新导入原始 AVI、MP4 或 MOV 视频")
    if not SLOTS.acquire(blocking=False):
        raise HTTPException(503, "另一段影像正在推理，请稍后再试")
    python, script, report, checkpoint = config
    precision = "fp32" if checkpoint else "fp16"
    if checkpoint:
        command = [str(python), "-X", "utf8", str(Path(__file__).with_name("stanford_cpu.py").resolve()),
                   "--runtime", str(script.parent), "--checkpoint", str(checkpoint)]
    else:
        command = [str(python), "-X", "utf8", str(script), "predict"]
    command += ["--engine-report", str(report), "--video", str(video.resolve())]
    if not checkpoint:
        command += ["--precision", precision]
    try:
        result = subprocess.run(
            command,
            cwd=script.parent, capture_output=True, text=True, encoding="utf-8", timeout=180,
            check=True,
        )
        payload = json.loads(result.stdout)
        scores = payload["probabilities"]
        count = payload["sampled_frames"]
        if (payload["precision"] != precision or payload["view"] not in LABELS | {None}
                or type(count) is not int or count <= 0 or set(scores) != LABELS
                or any(type(v) not in (int, float) or not math.isfinite(v) or not 0 <= v <= 1
                       for v in scores.values()) or abs(sum(scores.values()) - 1) > 0.001):
            raise ValueError("invalid inference output")
        return {key: payload[key] for key in ("view", "sampled_frames", "probabilities", "precision")}
    except subprocess.TimeoutExpired:
        raise HTTPException(504, "模型推理超时") from None
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        logging.getLogger(__name__).warning("Stanford inference failed: %s", type(exc).__name__)
        raise HTTPException(502, "模型运行失败，请检查权重、运行环境和视频格式") from None
    finally:
        SLOTS.release()
