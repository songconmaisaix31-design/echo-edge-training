from contextlib import asynccontextmanager
import json
import logging
import math
import os
import secrets
import shutil
import tempfile
from pathlib import Path
from threading import BoundedSemaphore

import av
import model_inference
from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse
from PIL import Image
from sqlalchemy.exc import SQLAlchemyError
from platform_db import Media, Session, current, get_case, log, protect_media, register

@asynccontextmanager
async def lifespan(app):
    with analysis_jobs.runner_lock():
        analysis_jobs.recover_jobs()
        yield


app = FastAPI(title="Echo Edge Media API", lifespan=lifespan)
register(app)
ROOT = Path(os.environ.get("ECHO_MEDIA_ROOT", str(Path(tempfile.gettempdir()) / "echo-edge-media")))
LIMIT = 64 * 1024 * 1024
MAX_FRAMES = 3600
SLOTS = BoundedSemaphore(1)
FORMATS = {".avi": "avi", ".mp4": "mp4", ".mov": "mov", ".webm": "webm"}

def fail(status, code, message):
    raise HTTPException(status, detail={"code": code, "message": message})

@app.get("/api/v1/capabilities")
def capabilities():
    reason, config = model_inference.configuration()
    return {"model": "Stanford 切面模型" if config else "unavailable", "modelDetail": reason, "formats": list(FORMATS), "maxBytes": LIMIT, "maxFrames": MAX_FRAMES,
            "terminalConfigured": bool(os.environ.get("ECHO_TERMINAL_PORT")),
            "structureConfigured": analysis_jobs.structure_configuration() is not None}

@app.post("/api/v1/media", status_code=201)
def import_media(media: UploadFile = File(...), case_id: int = Form(...), user=Depends(current)):
    suffix = Path(media.filename or "").suffix.lower()
    if suffix not in FORMATS:
        fail(415, "unsupported_format", "支持 AVI、MP4、MOV、WebM。此版本不接收 DICOM。")
    if not SLOTS.acquire(blocking=False):
        fail(503, "busy", "正在解析另一段影像，请稍后重试。")
    try:
        ROOT.mkdir(parents=True, exist_ok=True)
        with Session() as db:
            case = get_case(db, case_id, user)
            if case.status != "draft" or (case.owner != user.id and user.role != "admin"):
                fail(403, "case_locked", "仅可向有编辑权限的草稿病例添加影像。")
            identifier = secrets.token_hex(16)
            destination = ROOT / identifier
            with tempfile.TemporaryDirectory(prefix=".upload-", dir=ROOT) as scratch:
                folder = Path(scratch) / identifier
                folder.mkdir()
                result = decode_upload(media, suffix, folder, identifier)
                db.add(Media(id=identifier, case_id=case_id))
                log(db, user, "media.import", identifier)
                db.flush()
                response = {**result, **media_input_status(folder)}
                if destination.exists():
                    raise FileExistsError(identifier)
                folder.rename(destination)
                try:
                    db.commit()
                except Exception:
                    try:
                        shutil.rmtree(destination)
                    except OSError:
                        logging.getLogger(__name__).exception("Upload cleanup failed for %s", identifier)
                    raise
                return response
    except OSError:
        fail(500, "media_storage_failed", "影像文件无法保存，请检查存储空间和写入权限。")
    except SQLAlchemyError:
        fail(503, "media_database_failed", "影像关联未能保存，请稍后重试。")
    finally:
        SLOTS.release()


def decode_upload(media, suffix, folder, identifier):
    size = 0
    source = folder / ("source" + suffix)
    with source.open("wb") as target:
        while chunk := media.file.read(1024 * 1024):
            size += len(chunk)
            if size > LIMIT:
                fail(413, "too_large", "视频不得超过 64 MiB。")
            target.write(chunk)
    frames = []
    try:
        with av.open(str(source)) as container:
            if FORMATS[suffix] not in container.format.name.split(",") or not container.streams.video:
                fail(415, "format_mismatch", "实际容器与扩展名不符，或没有视频流。")
            stream = container.streams.video[0]
            stream.codec_context.thread_count = 1
            width, height = stream.width, stream.height
            if not 0 < width * height <= 4096 * 2160:
                fail(422, "dimensions", "视频尺寸超出支持范围。")
            origin = None
            for index, frame in enumerate(container.decode(stream)):
                if index >= MAX_FRAMES:
                    fail(422, "frame_limit", "视频超过 3600 帧。")
                timestamp = frame.time
                if timestamp is None or not math.isfinite(timestamp):
                    fail(422, "missing_timestamp", "视频缺少有效帧时间戳。")
                if origin is None:
                    origin = timestamp
                offset = round((timestamp - origin) * 1000, 3)
                if frames and offset < frames[-1]["offsetMs"]:
                    fail(422, "invalid_timestamps", "视频帧时间戳倒退。")
                image = frame.to_image()
                image.thumbnail((960, 960), Image.Resampling.LANCZOS)
                image.save(folder / f"{index}.jpg", quality=88)
                frames.append({"index": index, "offsetMs": offset})
            if not frames:
                fail(422, "no_frames", "未解码出有效视频帧。")
            result = {"id": identifier, "sizeBytes": size, "width": width, "height": height,
                      "originalName": Path(media.filename or "video").name, "frameCount": len(frames), "codec": stream.codec_context.name,
                      "container": container.format.name, "durationMs": frames[-1]["offsetMs"],
                      "frames": frames}
    except (av.FFmpegError, ValueError):
        fail(422, "decode_failed", "无法解码视频，请检查文件完整性与编码。")
    (folder / "metadata.json").write_text(json.dumps(result), encoding="utf-8")
    return result


def media_input_status(folder):
    suffix = next((ext for ext in FORMATS if (folder / ("source" + ext)).is_file()), None)
    supported = suffix in model_inference.VIDEO_SUFFIXES
    reason, configuration = model_inference.configuration()
    if suffix is None:
        status, detail = "original_missing", "原件缺失，仅可阅片。切面识别需重新导入原始 AVI、MP4 或 MOV。"
    elif not supported:
        status, detail = "unsupported_format", "原件已保存，但 Stanford 切面模型不支持此格式。"
    elif configuration is None:
        status, detail = "model_unavailable", reason
    else:
        status, detail = "eligible", "原件符合 Stanford 切面模型的格式要求，实际运行由推理接口验证。"
    return {
        "original": {"exists": suffix is not None, "format": suffix},
        "modelInputs": {"stanford_view": {
            "inputCompatible": supported, "status": status, "detail": detail,
        }},
    }


def media_folder(identifier):
    if len(identifier) != 32 or any(c not in "0123456789abcdef" for c in identifier):
        fail(404, "not_found", "找不到影像。")
    return ROOT / identifier

@app.get("/api/v1/media/{identifier}")
def metadata(identifier: str, user=Depends(current)):
    protect_media(identifier, user)
    file = media_folder(identifier) / "metadata.json"
    if not file.is_file():
        fail(404, "not_found", "找不到影像。")
    try:
        data = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        fail(500, "media_metadata_unavailable", "影像元数据无法读取。")
    return JSONResponse({**data, **media_input_status(file.parent)}, headers={"Cache-Control": "no-store"})

@app.post("/api/v1/media/{identifier}/predict")
def predict_view(identifier: str, user=Depends(current)):
    protect_media(identifier, user)
    return analysis_jobs.run(identifier, media_folder(identifier), user, legacy=True)


@app.get("/api/v1/media/{identifier}/frames/{index}")
def frame_preview(identifier: str, index: int, user=Depends(current)):
    protect_media(identifier, user)
    file = media_folder(identifier) / f"{index}.jpg"
    if not file.is_file():
        fail(404, "not_found", "找不到该帧。")
    return FileResponse(file, media_type="image/jpeg", headers={"Cache-Control": "no-store"})



import clinical
clinical.register(app)


import exam_parameters
exam_parameters.register(app)

import analysis_jobs
analysis_jobs.register(app, media_folder)

import research_reports
research_reports.register(app)
