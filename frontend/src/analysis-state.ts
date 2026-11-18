export type AnalysisJobState = 'pending' | 'running' | 'succeeded' | 'failed' | 'interrupted'

export const analysisStateLabels = {
  pending: '待处理',
  running: '运行中',
  succeeded: '成功',
  failed: '失败',
  interrupted: '已中断',
} satisfies Record<AnalysisJobState, string>
