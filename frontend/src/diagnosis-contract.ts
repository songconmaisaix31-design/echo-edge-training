/** Future adapter payload. No endpoint or clinical capability is implied. */
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
export type DiseaseCandidate = {
  label: string
  scope: string
  evidenceIds: string[]
}
type Output =
  | { kind: 'structure'; structures: { label: string; frame: EvidenceFrame; maskRef?: string }[] }
  | { kind: 'finding'; evidence: DiagnosticEvidence[] }
  | { kind: 'multi_view'; evidence: DiagnosticEvidence[] }
  | { kind: 'disease_candidate'; candidates: DiseaseCandidate[]; evidence: DiagnosticEvidence[] }

export type DiagnosisModelResult = {
  schemaVersion: '1.0'
  patientId: number
  examId: number
  mediaIds: string[]
  jobId: string
  model: ModelVersion
} & (
  | { state: 'completed'; output: Output; uncertainty?: string }
  | { state: 'abstained'; reason: string; uncertainty?: string }
  | { state: 'failed'; reason: string }
)
