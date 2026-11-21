// 由 FastAPI OpenAPI 生成。

export type AssessmentResponse = AssessmentSucceeded | AssessmentInsufficient
export type ExamId = number
export type Revision = number
export type Editable = boolean
export type RequiredViews = ('A4C' | 'A2C' | 'A3C' | 'A5C' | 'PLAX' | 'PSAX' | 'subcostal' | 'suprasternal')[]
export type View = 'A4C' | 'A2C' | 'A3C' | 'A5C' | 'PLAX' | 'PSAX' | 'subcostal' | 'suprasternal'
export type MediaId = string
export type FrameIndex = number
export type TimestampMs = number
export type Frames = FrameEvidence[]
export type View1 = 'A4C' | 'A2C' | 'A3C' | 'A5C' | 'PLAX' | 'PSAX' | 'subcostal' | 'suprasternal'
export type Status = 'covered' | 'missing'
export type Coverage = Coverage1[]
export type State = 'succeeded'
export type AnalysisJobId = number
export type ObservationNumber = number
/**
 * @minItems 1
 */
export type EvidenceFrames = [FrameEvidence, ...FrameEvidence[]]
export type MissingViews = ('A4C' | 'A2C' | 'A3C' | 'A5C' | 'PLAX' | 'PSAX' | 'subcostal' | 'suprasternal')[]
export type Findings = Finding[]
export type ExamId1 = number
export type Revision1 = number
export type Editable1 = boolean
export type RequiredViews1 = ('A4C' | 'A2C' | 'A3C' | 'A5C' | 'PLAX' | 'PSAX' | 'subcostal' | 'suprasternal')[]
export type Frames1 = FrameEvidence[]
export type Coverage2 = Coverage1[]
export type State1 = 'insufficient_evidence'
export type Reason = 'quality' | 'coverage' | 'out_of_scope'
export type Message = string
export type NextStep = string

export interface AssessmentSucceeded {
  exam_id: ExamId
  revision: Revision
  editable: Editable
  required_views: RequiredViews
  frames: Frames
  coverage: Coverage
  state: State
  findings: Findings
}
export interface FrameEvidence {
  view: View
  media_id: MediaId
  frame_index: FrameIndex
  timestamp_ms: TimestampMs
}
export interface Coverage1 {
  view: View1
  status: Status
}
export interface Finding {
  analysis_job_id: AnalysisJobId
  observation_number: ObservationNumber
  evidence_frames: EvidenceFrames
  missing_views: MissingViews
}
export interface AssessmentInsufficient {
  exam_id: ExamId1
  revision: Revision1
  editable: Editable1
  required_views: RequiredViews1
  frames: Frames1
  coverage: Coverage2
  state: State1
  insufficient_evidence: InsufficientEvidence
}
export interface InsufficientEvidence {
  reason: Reason
  message: Message
  next_step: NextStep
  [k: string]: unknown
}
