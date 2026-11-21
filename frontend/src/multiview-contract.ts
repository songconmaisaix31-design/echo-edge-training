import type {FrameEvidence, View} from './multiview-api.generated'

export const viewLabels = {
 A4C:'心尖四腔心切面（A4C）',
 A2C:'心尖两腔心切面（A2C）',
 A3C:'心尖三腔心切面（A3C）',
 A5C:'心尖五腔心切面（A5C）',
 PLAX:'胸骨旁长轴切面（PLAX）',
 PSAX:'胸骨旁短轴切面（PSAX）',
 subcostal:'剑突下切面',
 suprasternal:'胸骨上窝切面',
} satisfies Record<View,string>

export function isView(value:unknown):value is View{
 return typeof value==='string'&&Object.hasOwn(viewLabels,value)
}

export const availableViews=Object.keys(viewLabels).filter(isView)

export type SelectedFrame=Pick<FrameEvidence,'view'|'media_id'|'frame_index'>
export type AssessmentInput={revision:number;required_views:View[];frames:SelectedFrame[]}
