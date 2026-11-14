import json
import math
import os
import secrets
import tempfile
from pathlib import Path
from threading import BoundedSemaphore

import av
from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse
from PIL import Image
from platform_db import Media, Session, current, get_case, log, protect_media, register

app = FastAPI(title="Echo Edge Media API")
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
    return {"model": "unavailable", "formats": list(FORMATS), "maxBytes": LIMIT, "maxFrames": MAX_FRAMES}

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
            result = decode_upload(media, suffix)
            db.add(Media(id=result["id"], case_id=case_id))
            log(db, user, "media.import", result["id"])
            db.commit()
            return result
    finally:
        SLOTS.release()

def decode_upload(media, suffix):
    size = 0
    identifier = secrets.token_hex(16)
    with tempfile.TemporaryDirectory(dir=ROOT) as scratch:
        folder = Path(scratch)
        source = folder / "source"
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
        source.unlink()
        (folder / "metadata.json").write_text(json.dumps(result), encoding="utf-8")
        folder.rename(ROOT / identifier)
        return result

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
    return FileResponse(file, media_type="application/json", headers={"Cache-Control": "no-store"})

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
