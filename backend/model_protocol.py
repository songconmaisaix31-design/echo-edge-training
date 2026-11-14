"""Contract for task runners. Only the released Stanford adapter is enabled."""
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Literal, TypedDict

ModelType = Literal["view_classification", "cardiac_structure", "abnormality_detection"]
TaskState = Literal["pending", "running", "succeeded", "failed", "interrupted"]


class EvidenceFrame(TypedDict):
    media_id: str
    frame_index: int
    timestamp_ms: float


@dataclass(frozen=True)
class ModelInput:
    exam_id: int
    media_id: str
    source: Path
    sha256: str


@dataclass(frozen=True)
class ModelOutput:
    payload: dict[str, Any]
    evidence_frames: tuple[EvidenceFrame, ...] = ()


@dataclass(frozen=True)
class ModelAdapter:
    type: ModelType
    version: str
    execute: Callable[[ModelInput], ModelOutput]
