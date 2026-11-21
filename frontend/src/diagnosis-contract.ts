import type {AnalysisJobResponse, InsufficientEvidence} from './analysis-api.generated'

type DisplayFields = 'id' | 'model_type' | 'model_version' | 'created' | 'state' | 'output' | 'error'
export type AnalysisJob<T> =
  | (Pick<Extract<AnalysisJobResponse, {state: 'succeeded'}>, Exclude<DisplayFields, 'output'>> & {output: T})
  | Pick<Extract<AnalysisJobResponse, {state: 'insufficient_evidence'}>, DisplayFields | 'insufficient_evidence'>
  | Pick<Extract<AnalysisJobResponse, {output: null}>, DisplayFields>

export function analysisOutcome<T>(job: AnalysisJob<T> | null | undefined) {
  if (!job) return null
  switch (job.state) {
    case 'succeeded':
      return {kind: 'result', output: job.output} as const
    case 'insufficient_evidence':
      return {kind: 'insufficient_evidence', evidence: job.insufficient_evidence, output: job.output} as const
    case 'pending':
    case 'running':
    case 'failed':
    case 'interrupted':
      return null
    default: {
      const unreachable: never = job
      throw new Error('无法识别的分析状态 ' + unreachable)
    }
  }
}

export const diagnosisHref = (examId: number | string) => '#diagnosis/' + encodeURIComponent(String(examId))
export const structureEvidenceHref = (examId: number | string, mediaId?: string) =>
  diagnosisHref(examId) + '/structure' + (mediaId ? '/' + mediaId : '')

export type ModelVersion = {
  modelId: string
  version: string
  artifactSha256: string
  preprocessingVersion: string
  intendedScope: string
}
export type EvidenceFrame = {
  mediaId: string
  frameIndex: number // zero-based source frame index
  offsetMs?: number
}
export type DiagnosticEvidence = {
  id: string
  finding: string
  frame?: EvidenceFrame
  view?: string
  stance: 'supporting' | 'contradicting' | 'missing'
}
type Output =
  | { kind: 'structure'; structures: { label: string; frame: EvidenceFrame; maskRef?: string }[] }
  | { kind: 'finding'; evidence: DiagnosticEvidence[] }
  | { kind: 'multi_view'; evidence: DiagnosticEvidence[] }

export type DiagnosisModelResult = {
  schemaVersion: '1.0'
  patientId: number
  examId: number
  mediaIds: string[]
  jobId: string
  model: ModelVersion
} & (
  | { state: 'completed'; output: Output; uncertainty?: string }
  | { state: 'insufficient_evidence'; insufficient_evidence: InsufficientEvidence }
  | { state: 'failed'; reason: string }
)
