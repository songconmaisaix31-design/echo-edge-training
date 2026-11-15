from contextlib import contextmanager
from datetime import timedelta
import hashlib
import logging
import os
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from threading import BoundedSemaphore

from fastapi import Depends, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field
from sqlalchemy import Column, DateTime, ForeignKey, Integer, String, Text, select, update

import model_inference
import medical_review
import platform_db as db
import terminal_link
from clinical import Examination
from model_protocol import ModelAdapter, ModelInput, ModelOutput


class AnalysisJob(db.Base):
    __tablename__ = "analysis_jobs"

    id = Column(Integer, primary_key=True)
    exam_id = Column(ForeignKey("examinations.id"), nullable=False, index=True)
    media_id = Column(ForeignKey("media.id"), nullable=False, index=True)
    model_type = Column(String(40), nullable=False)
    model_version = Column(String(80), nullable=False)
    input_sha256 = Column(String(64), nullable=False)
    state = Column(String(16), nullable=False)
    error = Column(Text)
    output = Column(Text)
    created = Column(DateTime, nullable=False)
    completed = Column(DateTime)
    actor = Column(ForeignKey("users.id"), nullable=False)


RUNNER_MARKER = "[active local runner]"
CARDIACUDC_SHA256 = "6e821f6768a57214d61e25602d024316b032d1f4511a7886d7f133ec2cf95f4f"
STRUCTURE_MODEL = "cardiacudc_fourchamber"
STRUCTURE_SLOT = BoundedSemaphore(1)
REVIEW_SLOT = BoundedSemaphore(1)


class StructureInput(BaseModel):
    frame_index: int = Field(ge=0, strict=True)


class ReviewInput(BaseModel):
    kind: medical_review.ReviewKind


def structure_configuration():
    paths = [Path(os.environ.get(name, "")) for name in ("ECHO_STRUCTURE_PYTHON", "ECHO_CARDIACUDC_CHECKPOINT")]
    return paths if all(path.is_absolute() and path.is_file() for path in paths) else None


def serialize(job):
    output = json.loads(job.output) if job.output else None
    evidence = []
    if output and job.model_type == STRUCTURE_MODEL:
        evidence = [output["frame"]]
    elif output and job.model_type in ("llm_quality", "llm_findings"):
        evidence = list({frame["frame_index"]: frame for item in output["observations"] for frame in item["evidence_frames"]}.values())
    return {
        "id": job.id, "exam_id": job.exam_id, "media_id": job.media_id,
        "model_type": job.model_type, "model_version": job.model_version,
        "input_sha256": job.input_sha256, "state": job.state, "error": None if job.error == RUNNER_MARKER else job.error,
        "output": output,
        "evidence_frames": evidence,
        "created": job.created.isoformat() + "Z",
        "completed": job.completed.isoformat() + "Z" if job.completed else None,
        "actor": job.actor,
    }


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(identifier, folder, user, legacy=False):
    with db.Session() as session:
        media = session.get(db.Media, identifier)
        if media is None:
            raise HTTPException(404, "影像不存在")
        case = db.get_case(session, media.case_id, user)
        exam = session.get(Examination, case.id)
        if exam is None and not legacy:
            raise HTTPException(409, "影像尚未关联正式检查")
    if exam is None:
        return model_inference.predict(folder)

    video = next((folder / ("source" + ext) for ext in model_inference.VIDEO_SUFFIXES
                  if (folder / ("source" + ext)).is_file()), None)
    if video is None:
        raise HTTPException(422, "缺少原始 AVI、MP4 或 MOV 视频")
    reason, config = model_inference.configuration()
    if config is None:
        raise HTTPException(503, reason)

    _, _, report, checkpoint = config
    try:
        artifact = checkpoint or report.parent / json.loads(
            report.read_text(encoding="utf-8-sig"))["models"]["fp16"]["file"]
        version = ("fp32:" if checkpoint else "fp16:") + sha256(artifact)
        fingerprint = sha256(video)
    except (OSError, ValueError, KeyError, TypeError):
        raise HTTPException(503, "无法读取模型版本或影像原件") from None

    adapter = ModelAdapter("view_classification", version,
                           lambda item: ModelOutput(model_inference.predict(item.source.parent)))
    model_input = ModelInput(case.id, identifier, video, fingerprint)
    result, failure = execute_job(model_input, user, "stanford_view", adapter.version,
                                  lambda _job_id: adapter.execute(model_input).payload)
    if legacy:
        if failure:
            raise failure
        if result["state"] != "succeeded":
            raise HTTPException(409, "分析任务已中断")
        return result["output"]
    return result


def execute_job(model_input, user, model_type, version, execute):
    # 同步执行并持久化成功或失败状态，供页面刷新读回。
    with db.Session() as session:
        job = AnalysisJob(
            exam_id=model_input.exam_id, media_id=model_input.media_id, model_type=model_type,
            model_version=version, input_sha256=model_input.sha256, state="running",
            created=db.now(), actor=user.id, error=RUNNER_MARKER,
        )
        session.add(job)
        session.flush()
        db.log(session, user, "analysis.start", job.id)
        session.commit()
        job_id = job.id

    output, failure = None, None
    try:
        output = json.dumps(execute(job_id), allow_nan=False)
    except HTTPException as exc:
        failure = exc
    except Exception:
        logging.getLogger(__name__).exception("Analysis task %s failed", job_id)
        failure = HTTPException(502, "推理执行失败")

    with db.Session.begin() as session:
        session.execute(update(AnalysisJob).where(
            AnalysisJob.id == job_id, AnalysisJob.state == "running",
        ).values(state="failed" if failure else "succeeded", output=output,
                 error=str(failure.detail) if failure else None, completed=db.now()))
        job = session.get(AnalysisJob, job_id)
        result = serialize(job)
    return result, failure


def run_review(identifier, folder, kind, user):
    with db.Session() as session:
        media = session.get(db.Media, identifier)
        if media is None:
            raise HTTPException(404, "影像不存在")
        case = db.get_case(session, media.case_id, user)
        if session.get(Examination, case.id) is None:
            raise HTTPException(409, "影像尚未关联正式检查")
    detail, config = medical_review.configuration()
    if config is None:
        raise HTTPException(503, detail)
    video = next((path for path in folder.glob("source.*") if path.is_file()), None)
    if video is None:
        raise HTTPException(422, "缺少视频原件，请重新导入影像。")
    metadata, frames = medical_review.read_frames(folder)
    model_input = ModelInput(case.id, identifier, video, sha256(video))
    version = "review:" + hashlib.sha256((config.endpoint + config.model + medical_review.PROMPT_VERSION).encode()).hexdigest()
    return execute_job(model_input, user, "llm_" + kind, version,
                       lambda _job_id: medical_review.analyze(config, folder, identifier, kind, metadata, frames))[0]


def run_structure(identifier, folder, index, user):
    with db.Session() as session:
        media = session.get(db.Media, identifier)
        if media is None:
            raise HTTPException(404, "影像不存在")
        case = db.get_case(session, media.case_id, user)
        if session.get(Examination, case.id) is None:
            raise HTTPException(409, "影像尚未关联正式检查")
    config = structure_configuration()
    if config is None:
        raise HTTPException(503, "四腔分割模型未配置")
    python, checkpoint = config
    if sha256(checkpoint) != CARDIACUDC_SHA256:
        raise HTTPException(503, "四腔分割权重与 CardiacUDC 发布版不一致")
    video = next((path for path in folder.glob("source.*") if path.is_file()), None)
    metadata = folder / "metadata.json"
    if video is None or not metadata.is_file():
        raise HTTPException(422, "缺少已导入的视频原件或帧索引")
    frames = json.loads(metadata.read_text(encoding="utf-8"))["frames"]
    if index >= len(frames):
        raise HTTPException(422, "所选帧超出视频范围")
    model_input = ModelInput(case.id, identifier, video, sha256(video))

    def segment(job_id):
        runner = Path(__file__).with_name("cardiacudc_frame.py")
        with TemporaryDirectory(prefix=".structure-", dir=folder) as scratch:
            image = Path(scratch) / "overlay.png"
            command = [str(python), "-X", "utf8", str(runner), "--checkpoint", str(checkpoint),
                       "--video", str(video.resolve()), "--frame", str(index), "--output", str(image)]
            try:
                process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                         check=True, timeout=60)
            except subprocess.TimeoutExpired as error:
                raise HTTPException(504, "四腔分割超时") from error
            output = json.loads(process.stdout)
            image.replace(folder / f"structure-{job_id}.png")
        return {**output, "frame": {"media_id": identifier, "frame_index": index,
                                     "timestamp_ms": frames[index]["offsetMs"]}}

    version = "cardiacudc:" + CARDIACUDC_SHA256
    return execute_job(model_input, user, STRUCTURE_MODEL, version, segment)[0]


@contextmanager
def runner_lock():
    """One task-writing service per local SQLite database."""
    database = db.engine.url.database
    if db.engine.url.get_backend_name() != "sqlite" or not database or database == ":memory:":
        raise RuntimeError("Analysis recovery requires a local SQLite file")
    lock_path = Path(database).resolve().with_suffix(".analysis.lock")
    with lock_path.open("a+b") as lock:
        lock.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError("Another analysis service holds the SQLite runner lock") from exc
        yield  # Closing the handle also releases the lock after a process crash.


def recover_jobs():
    with db.Session.begin() as session:
        now = db.now()
        session.execute(update(AnalysisJob).where(
            AnalysisJob.state.in_(("pending", "running")),
            # Old 0005 workers do not acquire the runner lock. Give them time
            # to finish their 180-second subprocess timeout.
            AnalysisJob.output.is_(None),
            ((AnalysisJob.error == RUNNER_MARKER) |
             (AnalysisJob.created < now - timedelta(minutes=5))),
        ).values(state="interrupted", error="服务退出，任务未完成，请重新提交。",
                 completed=now))


def register(app, media_folder):
    @app.post("/api/v1/media/{identifier}/review-jobs", status_code=201)
    def submit_review(identifier: str, body: ReviewInput, user=Depends(db.current)):
        if not REVIEW_SLOT.acquire(blocking=False):
            raise HTTPException(503, "另一个大模型影像复核任务正在运行，请稍后重试。")
        try:
            return run_review(identifier, media_folder(identifier), body.kind, user)
        finally:
            REVIEW_SLOT.release()

    @app.post("/api/v1/media/{identifier}/analysis-jobs", status_code=201)
    def submit(identifier: str, user=Depends(db.current)):
        return run(identifier, media_folder(identifier), user)

    @app.post("/api/v1/media/{identifier}/structure-jobs", status_code=201)
    def submit_structure(identifier: str, body: StructureInput, user=Depends(db.current)):
        if not STRUCTURE_SLOT.acquire(blocking=False):
            raise HTTPException(503, "另一个四腔分割任务正在运行")
        try:
            return run_structure(identifier, media_folder(identifier), body.frame_index, user)
        finally:
            STRUCTURE_SLOT.release()

    @app.get("/api/v1/analysis-jobs/{key}")
    def status(key: int, user=Depends(db.current)):
        with db.Session() as session:
            job = session.get(AnalysisJob, key)
            if job is None:
                raise HTTPException(404, "分析任务不存在")
            db.get_case(session, job.exam_id, user)
            return serialize(job)

    @app.get("/api/v1/analysis-jobs/{key}/overlay")
    def structure_overlay(key: int, user=Depends(db.current)):
        with db.Session() as session:
            job = session.get(AnalysisJob, key)
            if job is None or job.model_type != STRUCTURE_MODEL or job.state != "succeeded":
                raise HTTPException(404, "结构叠图不存在")
            db.get_case(session, job.exam_id, user)
            image = media_folder(job.media_id) / f"structure-{key}.png"
        if not image.is_file():
            raise HTTPException(404, "结构叠图文件不存在")
        return FileResponse(image, media_type="image/png", headers={"Cache-Control": "no-store"})

    @app.post("/api/v1/analysis-jobs/{key}/terminal")
    def send_terminal(key: int, user=Depends(db.current)):
        with db.Session() as session:
            job = session.get(AnalysisJob, key)
            if job is None:
                raise HTTPException(404, "分析任务不存在")
            case = db.get_case(session, job.exam_id, user)
            if user.role != "admin" and case.owner != user.id:
                raise HTTPException(403, "只有检查创建者或管理员可发送终端提示")
            if job.state != "succeeded" or not job.output:
                raise HTTPException(409, "只有成功的分析可发送到终端")
            if job.model_type == STRUCTURE_MODEL:
                if not (media_folder(job.media_id) / f"structure-{key}.png").is_file():
                    raise HTTPException(409, "四腔结构叠图文件不存在")
                hint = "MASK READY"
            elif job.model_type == "stanford_view":
                output = json.loads(job.output)
                if "view" not in output or output["view"] not in ("A4C", "PSAX", None):
                    raise HTTPException(409, "该任务没有有效的切面结果")
                hint = terminal_link.HINTS[output["view"]]
            else:
                raise HTTPException(409, "当前分析类型不可发送到终端")
        return terminal_link.send_hint(hint)

    @app.get("/api/v1/examinations/{key}/analysis-jobs")
    def exam_history(key: int, user=Depends(db.current)):
        with db.Session() as session:
            db.get_case(session, key, user)
            if session.get(Examination, key) is None:
                raise HTTPException(404, "检查不存在")
            query = select(AnalysisJob).where(AnalysisJob.exam_id == key)
            return [serialize(job) for job in session.scalars(query.order_by(AnalysisJob.id.desc()))]

    @app.get("/api/v1/media/{identifier}/analysis-jobs")
    def media_history(identifier: str, user=Depends(db.current)):
        db.protect_media(identifier, user)
        with db.Session() as session:
            query = select(AnalysisJob).where(AnalysisJob.media_id == identifier)
            return [serialize(job) for job in session.scalars(query.order_by(AnalysisJob.id.desc()))]
