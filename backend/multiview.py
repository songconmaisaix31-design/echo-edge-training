import json
from typing import Annotated, Literal

from fastapi import Depends, HTTPException
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator
from sqlalchemy import Column, ForeignKey, Integer, Text, select, update
from sqlalchemy.dialects.sqlite import insert

import platform_db as db
from analysis_contract import FINISHED, INSUFFICIENT, InsufficientEvidence, job_result
from analysis_jobs import AnalysisJob
from exam_parameters import checked_exam
from medical_review import MediaIndex


View = Literal["A4C", "A2C", "A3C", "A5C", "PLAX", "PSAX", "subcostal", "suprasternal"]


class MultiviewSetup(db.Base):
    __tablename__ = "examination_multiview"

    exam_id = Column(ForeignKey("examinations.id"), primary_key=True)
    revision = Column(Integer, nullable=False)
    data = Column(Text, nullable=False)


class FrameSelection(BaseModel):
    model_config = ConfigDict(extra="forbid")
    view: View
    media_id: str = Field(pattern=r"^[a-f0-9]{32}$")
    frame_index: int = Field(ge=0, strict=True)


class FrameEvidence(FrameSelection):
    timestamp_ms: float = Field(ge=0, allow_inf_nan=False)


class AssessmentInput(BaseModel):
    model_config = ConfigDict(extra="forbid")
    revision: int = Field(ge=0, strict=True)
    required_views: list[View] = Field(max_length=8)
    frames: list[FrameSelection] = Field(max_length=128)

    @model_validator(mode="after")
    def check_duplicates(self):
        if len(set(self.required_views)) != len(self.required_views):
            raise ValueError("必要切面不能重复")
        if len({(frame.media_id, frame.frame_index) for frame in self.frames}) != len(self.frames):
            raise ValueError("同一原始帧只能确认一个切面")
        return self


class Coverage(BaseModel):
    model_config = ConfigDict(extra="forbid")
    view: View
    status: Literal["covered", "missing"]


class Finding(BaseModel):
    model_config = ConfigDict(extra="forbid")
    analysis_job_id: int
    observation_number: int = Field(ge=1)
    evidence_frames: list[FrameEvidence] = Field(min_length=1)
    missing_views: list[View]


class AssessmentFields(BaseModel):
    model_config = ConfigDict(extra="forbid")
    exam_id: int
    revision: int
    editable: bool
    required_views: list[View]
    frames: list[FrameEvidence]
    coverage: list[Coverage]


class AssessmentSucceeded(AssessmentFields):
    state: Literal["succeeded"]
    findings: list[Finding]


class AssessmentInsufficient(AssessmentFields):
    state: Literal["insufficient_evidence"]
    insufficient_evidence: InsufficientEvidence


AssessmentResponse = Annotated[AssessmentSucceeded | AssessmentInsufficient, Field(discriminator="state")]


def checked_frames(session, exam_id, frames, media_folder):
    video_ids = {frame.media_id for frame in frames}
    owned = set(session.scalars(select(db.Media.id).where(db.Media.case_id == exam_id, db.Media.id.in_(video_ids))))
    if owned != video_ids:
        raise HTTPException(422, "确认的原始帧须来自本次检查的影像。")
    indexes = {}
    for media_id in video_ids:
        folder = media_folder(media_id)
        try:
            index = MediaIndex.model_validate_json((folder / "metadata.json").read_bytes())
        except (OSError, ValidationError):
            raise HTTPException(422, "影像帧索引无法读取，请检查原始影像。") from None
        indexes[media_id] = (folder, index)
    evidence = []
    for frame in frames:
        folder, index = indexes[frame.media_id]
        if (frame.frame_index >= index.frameCount
                or not (folder / f"{frame.frame_index}.jpg").is_file()):
            raise HTTPException(422, "确认的原始帧超出影像范围。")
        evidence.append(FrameEvidence(**frame.model_dump(), timestamp_ms=index.frames[frame.frame_index].offsetMs))
    return evidence


def saved_findings(session, exam_id, required, frames):
    confirmed = {(frame.media_id, frame.frame_index): frame for frame in frames if frame.view in required}
    jobs = session.scalars(select(AnalysisJob).where(
        AnalysisJob.exam_id == exam_id,
        AnalysisJob.media_id.in_({media_id for media_id, _index in confirmed}),
        AnalysisJob.model_type == "llm_findings", AnalysisJob.state.in_(FINISHED),
    ).order_by(AnalysisJob.id.desc()))
    findings = []
    processed = set()
    for job in jobs:
        if job.media_id in processed:
            continue
        processed.add(job.media_id)
        output = json.loads(job.output)
        if job_result(job.model_type, job.state, output)["state"] != "succeeded" or output["assessment"] != "suspected_findings":
            continue
        for number, item in enumerate(output["observations"], start=1):
            if item["confidence"] not in ("moderate", "high"):
                continue
            keys = dict.fromkeys((frame["media_id"], frame["frame_index"]) for frame in item["evidence_frames"])
            evidence = [confirmed[key] for key in keys if key[0] == job.media_id and key in confirmed]
            if evidence:
                views = {frame.view for frame in evidence}
                findings.append(Finding(analysis_job_id=job.id, observation_number=number,
                                        evidence_frames=evidence, missing_views=[view for view in required if view not in views]))
    return findings


def assessment(session, exam_id, editable):
    row = session.get(MultiviewSetup, exam_id)
    stored = json.loads(row.data) if row else {"required_views": [], "frames": []}
    required = stored["required_views"]
    frames = [FrameEvidence.model_validate(frame) for frame in stored["frames"]]
    covered = {frame.view for frame in frames}
    coverage = [Coverage(view=view, status="covered" if view in covered else "missing") for view in required]
    fields = dict(exam_id=exam_id, revision=row.revision if row else 0, editable=editable,
                  required_views=required, frames=frames, coverage=coverage)
    if not required:
        return AssessmentInsufficient(**fields, state=INSUFFICIENT, insufficient_evidence=InsufficientEvidence(
            reason="coverage", message="本次检查尚未指定多切面评估所需的切面。",
            next_step="请先由操作者确认本次评估所需的切面。",
        ))
    if any(item.status == "missing" for item in coverage):
        return AssessmentInsufficient(**fields, state=INSUFFICIENT, insufficient_evidence=InsufficientEvidence(
            reason="coverage", message="本次评估缺少必要切面的已确认帧。",
            next_step="请复核已有影像并补充确认。缺少相应影像时可补充导入。",
        ))
    return AssessmentSucceeded(**fields, state="succeeded", findings=saved_findings(session, exam_id, required, frames))


def register(app, media_folder):
    path = "/api/v1/examinations/{key}/multiview"

    @app.get(path, response_model=AssessmentResponse)
    def read_assessment(key: int, user=Depends(db.current)):
        with db.Session() as session:
            checked_exam(session, key, user)
            case = session.get(db.Case, key)
            return assessment(session, key, case.owner == user.id or user.role == "admin")

    @app.put(path, response_model=AssessmentResponse)
    def save_assessment(key: int, body: AssessmentInput, user=Depends(db.current)):
        with db.Session.begin() as session:
            checked_exam(session, key, user, write=True)
            frames = checked_frames(session, key, body.frames, media_folder)
            data = json.dumps({"required_views": body.required_views, "frames": [frame.model_dump() for frame in frames]})
            if body.revision == 0:
                statement = insert(MultiviewSetup).values(exam_id=key, revision=1, data=data).on_conflict_do_nothing()
            else:
                statement = update(MultiviewSetup).where(
                    MultiviewSetup.exam_id == key, MultiviewSetup.revision == body.revision,
                ).values(revision=body.revision + 1, data=data)
            if session.execute(statement).rowcount != 1:
                raise HTTPException(409, "本次评估已被更新，请刷新后重试。")
            db.log(session, user, "multiview.save", key)
            return assessment(session, key, True)
