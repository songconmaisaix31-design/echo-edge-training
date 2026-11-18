from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import BoundedSemaphore, Lock

from fastapi import HTTPException
from fastapi.responses import FileResponse, Response

import analysis_jobs
import model_inference


@dataclass
class Sample:
    video: Path
    frames: Path
    info: dict
    fingerprint: str
    view: dict | None = None
    latest_structure: int | None = None
    structures: dict[int, tuple[dict, bytes]] = field(default_factory=dict)


@contextmanager
def try_slot(slot):
    if not slot.acquire(blocking=False):
        raise HTTPException(503, "样例模型正在运行，请稍后重试")
    try:
        yield
    finally:
        slot.release()


def load_samples(root):
    samples = {}
    for number in range(1, 6):
        directory = root / "frames" / str(number)
        info = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
        frames = info["frames"]
        if not 1 <= len(frames) <= 219 or any(frame["index"] != index for index, frame in enumerate(frames)):
            raise ValueError("公开样例帧索引无效")
        if str(number) != info["id"] or len(frames) != info["frameCount"]:
            raise ValueError("公开样例的目录与元数据不一致")
        video = root / f"video-{number}.mp4"
        samples[number] = Sample(video, directory, info, analysis_jobs.sha256(video))
    if sum(sample.info["frameCount"] for sample in samples.values()) != 219:
        raise ValueError("公开样例需要 219 张影像帧")
    return samples


def result(id, model_type, version, fingerprint, output):
    return {"id": id, "model_type": model_type, "model_version": version, "input_sha256": fingerprint,
            "state": "succeeded", "output": output, "error": None, "created": datetime.now(timezone.utc).isoformat()}


def register(app):
    directory = os.environ.get("ECHO_PUBLIC_SAMPLE_ROOT")
    if not directory:
        return
    samples = load_samples(Path(directory).resolve(strict=True))
    scratch = Path(os.environ["ECHO_PUBLIC_SAMPLE_SCRATCH"]).resolve(strict=True)
    slot = BoundedSemaphore(1)
    cache = Lock()
    base = "/api/v1/public-samples/media/{number}"

    def get_sample(number):
        if number not in samples:
            raise HTTPException(404, "公开样例不存在")
        return samples[number]

    def get_frame(sample, index):
        if not 0 <= index < sample.info["frameCount"]:
            raise HTTPException(404, "公开样例帧不存在")
        return sample.info["frames"][index]

    @app.get(base)
    def metadata(number: int):
        sample = get_sample(number)
        detail, config = model_inference.configuration()
        return {**sample.info, "modelInputs": {"stanford_view": {
            "status": "eligible" if config else "model_unavailable", "detail": detail,
        }}}

    @app.get(base + "/frames/{index}")
    def frame(number: int, index: int):
        sample = get_sample(number)
        get_frame(sample, index)
        return FileResponse(sample.frames / f"{index}.jpg", media_type="image/jpeg")

    @app.get(base + "/analysis-jobs")
    def history(number: int):
        sample = get_sample(number)
        with cache:
            return ([sample.view] if sample.view else []) + (
                [sample.structures[sample.latest_structure][0]] if sample.latest_structure is not None else [])

    @app.post(base + "/analysis-jobs", status_code=201)
    def stanford(number: int):
        sample = get_sample(number)
        with try_slot(slot):
            with cache:
                if sample.view:
                    return {**sample.view, "cached": True}
            output = model_inference.predict_video(sample.video)
            job = result(0, "stanford_view", "Stanford " + output["precision"].upper(), sample.fingerprint, output)
            with cache:
                sample.view = job
        return {**job, "cached": False}

    @app.post(base + "/structure-jobs", status_code=201)
    def structure(number: int, body: analysis_jobs.StructureInput):
        sample = get_sample(number)
        index = body.frame_index
        evidence = get_frame(sample, index)
        with try_slot(slot):
            with cache:
                if index in sample.structures:
                    sample.latest_structure = index
                    return {**sample.structures[index][0], "cached": True}
            with try_slot(analysis_jobs.STRUCTURE_SLOT):
                config = analysis_jobs.checked_structure_configuration()
                with TemporaryDirectory(prefix="cardiacudc-", dir=scratch) as temporary:
                    image = Path(temporary) / "overlay.png"
                    output = analysis_jobs.segment_video(sample.video, index, image, config)
                    overlay = image.read_bytes()
            output["frame"] = {"media_id": str(number), "frame_index": index, "timestamp_ms": evidence["offsetMs"]}
            job = result(index + 1, analysis_jobs.STRUCTURE_MODEL, "cardiacudc:" + analysis_jobs.CARDIACUDC_SHA256,
                         sample.fingerprint, output)
            with cache:
                sample.structures[index] = (job, overlay)
                sample.latest_structure = index
        return {**job, "cached": False}

    @app.get(base + "/overlays/{index}")
    def overlay(number: int, index: int):
        sample = get_sample(number)
        get_frame(sample, index)
        with cache:
            if index not in sample.structures:
                raise HTTPException(404, "公开样例尚无分割叠图")
            return Response(sample.structures[index][1], media_type="image/png")
