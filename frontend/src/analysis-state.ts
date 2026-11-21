import type {AnalysisJobResponse} from './analysis-api.generated'

export type AnalysisJobState = AnalysisJobResponse['state']

export const analysisStateLabels = {
  pending: '待处理',
  running: '运行中',
  succeeded: '成功',
  insufficient_evidence: '证据不足',
  failed: '失败',
  interrupted: '已中断',
} satisfies Record<AnalysisJobState, string>
