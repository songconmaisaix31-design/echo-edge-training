// 由 FastAPI OpenAPI 生成。

export type AnalysisJobResponse = SucceededAnalysisJob | InsufficientEvidenceAnalysisJob | OtherAnalysisJob
export type Id = number
export type ExamId = number | null
export type MediaId = string | null
export type ModelType = string
export type ModelVersion = string
export type InputSha256 = string
export type EvidenceFrames = {
  [k: string]: unknown
}[]
export type Created = string
export type Completed = string | null
export type Actor = number | null
export type Cached = boolean | null
export type State = 'succeeded'
export type Error = null
export type Id1 = number
export type ExamId1 = number | null
export type MediaId1 = string | null
export type ModelType1 = string
export type ModelVersion1 = string
export type InputSha2561 = string
export type EvidenceFrames1 = {
  [k: string]: unknown
}[]
export type Created1 = string
export type Completed1 = string | null
export type Actor1 = number | null
export type Cached1 = boolean | null
export type State1 = 'insufficient_evidence'
export type Error1 = null
export type Reason = 'quality' | 'coverage' | 'out_of_scope'
export type Message = string
export type NextStep = string
export type Id2 = number
export type ExamId2 = number | null
export type MediaId2 = string | null
export type ModelType2 = string
export type ModelVersion2 = string
export type InputSha2562 = string
export type EvidenceFrames2 = {
  [k: string]: unknown
}[]
export type Created2 = string
export type Completed2 = string | null
export type Actor2 = number | null
export type Cached2 = boolean | null
export type State2 = 'pending' | 'running' | 'failed' | 'interrupted'
export type Error2 = string | null
export type Output2 = null

export interface SucceededAnalysisJob {
  id: Id
  exam_id?: ExamId
  media_id?: MediaId
  model_type: ModelType
  model_version: ModelVersion
  input_sha256: InputSha256
  evidence_frames?: EvidenceFrames
  created: Created
  completed?: Completed
  actor?: Actor
  cached?: Cached
  state: State
  error: Error
  output: Output
  [k: string]: unknown
}
export interface Output {
  [k: string]: unknown
}
export interface InsufficientEvidenceAnalysisJob {
  id: Id1
  exam_id?: ExamId1
  media_id?: MediaId1
  model_type: ModelType1
  model_version: ModelVersion1
  input_sha256: InputSha2561
  evidence_frames?: EvidenceFrames1
  created: Created1
  completed?: Completed1
  actor?: Actor1
  cached?: Cached1
  state: State1
  error: Error1
  output: Output1
  insufficient_evidence: InsufficientEvidence
  [k: string]: unknown
}
export interface Output1 {
  [k: string]: unknown
}
export interface InsufficientEvidence {
  reason: Reason
  message: Message
  next_step: NextStep
  [k: string]: unknown
}
export interface OtherAnalysisJob {
  id: Id2
  exam_id?: ExamId2
  media_id?: MediaId2
  model_type: ModelType2
  model_version: ModelVersion2
  input_sha256: InputSha2562
  evidence_frames?: EvidenceFrames2
  created: Created2
  completed?: Completed2
  actor?: Actor2
  cached?: Cached2
  state: State2
  error: Error2
  output: Output2
  [k: string]: unknown
}

export type FrameIndex = number
export type FrameView = 'A4C' | 'non_a4c' | 'unconfirmed'

export interface StructureInput {
  frame_index: FrameIndex
  frame_view?: FrameView
  [k: string]: unknown
}
