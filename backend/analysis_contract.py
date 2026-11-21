from typing import Annotated, Literal

from pydantic import BaseModel, Field


INSUFFICIENT = "insufficient_evidence"
FINISHED = ("succeeded", INSUFFICIENT)
FrameView = Literal["A4C", "non_a4c", "unconfirmed"]


class InsufficientEvidence(BaseModel):
    reason: Literal["quality", "coverage", "out_of_scope"]
    message: str
    next_step: str


class AnalysisJobFields(BaseModel):
    id: int
    exam_id: int | None = None
    media_id: str | None = None
    model_type: str
    model_version: str
    input_sha256: str
    evidence_frames: list[dict] = Field(default_factory=list)
    created: str
    completed: str | None = None
    actor: int | None = None
    cached: bool | None = None


class SucceededAnalysisJob(AnalysisJobFields):
    state: Literal["succeeded"]
    error: None
    output: dict


class InsufficientEvidenceAnalysisJob(AnalysisJobFields):
    state: Literal["insufficient_evidence"]
    error: None
    output: dict
    insufficient_evidence: InsufficientEvidence


class OtherAnalysisJob(AnalysisJobFields):
    state: Literal["pending", "running", "failed", "interrupted"]
    error: str | None
    output: None


AnalysisJobResponse = Annotated[
    SucceededAnalysisJob | InsufficientEvidenceAnalysisJob | OtherAnalysisJob, Field(discriminator="state")
]


def evidence_shortfall(model_type, output):
    if model_type == "stanford_view" and "view" in output and output["view"] is None:
        return InsufficientEvidence(
            reason="quality", message="模型未达到判定阈值，暂无法确认这段视频的切面。",
            next_step="请复核当前影像，或补采所需切面后重试。",
        )
    if model_type == "cardiacudc_fourchamber":
        if output.get("frame_view") == "non_a4c":
            return InsufficientEvidence(
                reason="out_of_scope", message="所选帧不属于心尖四腔心切面，无法生成四腔分割结果。",
                next_step="请选择已确认属于心尖四腔心切面的帧，或补采后重试。",
            )
        if output.get("frame_view") == "unconfirmed":
            return InsufficientEvidence(
                reason="coverage", message="所选帧的心尖四腔心切面尚未确认。",
                next_step="请先复核并确认所选帧的切面，或补采所需切面。",
            )
    if model_type == "llm_findings" and output.get("assessment") == "indeterminate":
        return InsufficientEvidence(
            reason="coverage", message="现有影像证据不足，暂无法判断异常征象。",
            next_step="请由医生复核原始视频，并按需补采相关切面。",
        )
    if model_type == "llm_quality" and output.get("assessment") == "uninterpretable":
        return InsufficientEvidence(
            reason="quality", message="现有影像质量不足以进行复核。",
            next_step="请核对影像质量并补采清晰影像后重试。",
        )
    return None


def job_result(model_type, state, output):
    if state not in FINISHED:
        return {"state": state, "output": None}
    # 旧记录在读回时按同一规则解释，无需改写已有结果。
    shortfall = evidence_shortfall(model_type, output) if output is not None else None
    if shortfall is not None:
        if model_type in ("llm_quality", "llm_findings"):
            fields = ("kind", "assessment", "sampled_frames", "source_frame_count", "duration_ms",
                      "model", "prompt_version", "requires_clinician_review")
            output = {field: output[field] for field in fields if field in output}
        return {"state": INSUFFICIENT, "insufficient_evidence": shortfall.model_dump(), "output": output}
    return {"state": state, "output": output}
