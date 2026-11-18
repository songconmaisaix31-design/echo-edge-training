export const vitalFields = [
  ['height_cm', '身高/身长（cm）', 300],
  ['weight_kg', '体重（kg）', 500],
  ['heart_rate', '心率（次/分）', 400],
  ['systolic', '收缩压（mmHg）', 400],
  ['diastolic', '舒张压（mmHg）', 300],
  ['spo2', 'SpO₂（%）', 100],
] as const

export type VitalField = typeof vitalFields[number][0]

export type ExamParameters = Partial<Record<VitalField, number | null>> & {
  exam_date?: string
  age_days?: number
  bsa_m2?: number | null
  doctor?: string
  device?: string
  indication?: string
  clinical_notes?: string
}

export function readExamParameters(form: FormData): ExamParameters {
  const parameters: ExamParameters = {
    exam_date: String(form.get('exam_date') ?? ''),
    doctor: String(form.get('doctor') ?? ''),
    device: String(form.get('device') ?? ''),
    indication: String(form.get('indication') ?? ''),
  }
  const notes = form.get('clinical_notes')
  if (typeof notes === 'string') parameters.clinical_notes = notes
  for (const [key] of vitalFields) {
    const value = form.get(key)
    parameters[key] = typeof value === 'string' && value !== '' ? Number(value) : null
  }
  return parameters
}
