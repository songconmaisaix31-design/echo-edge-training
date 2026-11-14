from contextlib import contextmanager
from datetime import timedelta
import hashlib
import logging
import os
import json
from pathlib import Path

from fastapi import Depends, HTTPException
from sqlalchemy import Column, DateTime, ForeignKey, Integer, String, Text, select, update

import model_inference
import platform_db as db
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


def serialize(job):
    return {
        "id": job.id, "exam_id": job.exam_id, "media_id": job.media_id,
        "model_type": job.model_type, "model_version": job.model_version,
        "input_sha256": job.input_sha256, "state": job.state, "error": None if job.error == RUNNER_MARKER else job.error,
        "output": json.loads(job.output) if job.output else None,
        "evidence_frames": [],
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

    # Synchronous execution keeps the existing request contract.
    with db.Session() as session:
        job = AnalysisJob(
            exam_id=case.id, media_id=identifier, model_type="stanford_view",
            model_version=adapter.version, input_sha256=model_input.sha256, state="running",
            created=db.now(), actor=user.id, error=RUNNER_MARKER,
        )
        session.add(job)
        session.flush()
        db.log(session, user, "analysis.start", job.id)
        session.commit()
        job_id = job.id

    output, failure = None, None
    try:
        output = json.dumps(adapter.execute(model_input).payload, allow_nan=False)
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
    if legacy:
        if failure:
            raise failure
        if result["state"] != "succeeded":
            raise HTTPException(409, "分析任务已中断")
        return result["output"]
    return result


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
    @app.post("/api/v1/media/{identifier}/analysis-jobs", status_code=201)
    def submit(identifier: str, user=Depends(db.current)):
        return run(identifier, media_folder(identifier), user)

    @app.get("/api/v1/analysis-jobs/{key}")
    def status(key: int, user=Depends(db.current)):
        with db.Session() as session:
            job = session.get(AnalysisJob, key)
            if job is None:
                raise HTTPException(404, "分析任务不存在")
            db.get_case(session, job.exam_id, user)
            return serialize(job)

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
