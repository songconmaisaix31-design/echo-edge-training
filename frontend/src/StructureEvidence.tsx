import {useEffect, useState} from 'react'
import {analysisHistory,api,apiUrl,errorMessage,isRecord,mediaApiPath,submitAnalysis,type MediaScope} from './api'
import type {FrameView, StructureInput} from './analysis-api.generated'
import {analysisOutcome,type AnalysisJob} from './diagnosis-contract'
import {InsufficientEvidenceNotice} from './InsufficientEvidenceNotice'
import {Button} from './button'

type Frame = {index:number; offsetMs:number}
type ResultFrame={frame_index:number;timestamp_ms:number}
type Segmentation={epoch:number;frame:ResultFrame}
const modelType='cardiacudc_fourchamber'

function isFrame(value:unknown):value is ResultFrame{
 return isRecord(value)&&typeof value.frame_index==='number'&&typeof value.timestamp_ms==='number'
}

function isSegmentation(value:unknown):value is Segmentation{
 return isRecord(value)&&typeof value.epoch==='number'&&isFrame(value.frame)
}

export default function StructureEvidence({id, configured, scope='private',examId}:{id:string;configured:boolean|null;scope?:MediaScope;examId?:number|string}){
 const path=mediaApiPath(id,scope)
 const [frames,setFrames]=useState<Frame[]>([])
 const [index,setIndex]=useState(0)
 const [frameView,setFrameView]=useState<FrameView>('unconfirmed')
 const [job,setJob]=useState<AnalysisJob<Segmentation>|null>(null)
 const [loading,setLoading]=useState(true)
 const [busy,setBusy]=useState('')
 const [error,setError]=useState('')
 const outcome=analysisOutcome(job)
 const output=outcome?.kind==='result'?outcome.output:null

 useEffect(()=>{
  let current=true
  Promise.all([api<{frames:Frame[]}>(path),analysisHistory(path+'/analysis-jobs',modelType,isSegmentation)])
   .then(([media,jobs])=>{
    if(!current)return
    setFrames(media.frames)
    const latest=jobs[0]??null
    const frame=latest?.output?.frame
    if(isFrame(frame)&&media.frames.some(item=>item.index===frame.frame_index)){
     setIndex(frame.frame_index)
     if(latest?.state==='succeeded')setFrameView('A4C')
     if(latest?.state==='insufficient_evidence'&&latest.output.frame_view==='non_a4c')setFrameView('non_a4c')
    }
    setJob(latest)
   }).catch((failure:Error)=>{if(current)setError(errorMessage(failure))})
   .finally(()=>{if(current)setLoading(false)})
  return ()=>{current=false}
 },[path])

 async function segment(){
  setBusy('正在处理所选帧…');setError('')
  try{
   const request:StructureInput={frame_index:index,frame_view:frameView}
   setJob(await submitAnalysis(path+'/structure-jobs',request,isSegmentation))
  }catch(failure){setError(errorMessage(failure))}
  finally{setBusy('')}
 }

 return <article className="diagnosis-card">
  <h2>四腔结构分割</h2>
  <p>CardiacUDC 研究版。请选择已确认属于心尖四腔心切面的影像帧。</p>
  <div className="context-bar"><label>选择证据帧<select value={index} disabled={!frames.length||!!busy} onChange={event=>{
   setIndex(Number(event.target.value));setFrameView('unconfirmed');setJob(null);setError('')
  }}>
   {frames.map(frame=><option key={frame.index} value={frame.index}>第 {frame.index+1} 帧 · {frame.offsetMs.toFixed(0)} ms</option>)}
  </select></label>
   <label>所选帧切面<select value={frameView} disabled={!frames.length||!!busy} onChange={event=>{
    const view=event.target.value
    if(view==='A4C'||view==='non_a4c'||view==='unconfirmed'){
     setFrameView(view);setJob(null);setError('')
    }
   }}>
    <option value="unconfirmed">尚未确认</option>
    <option value="A4C">已确认 A4C</option>
    <option value="non_a4c">已确认非 A4C</option>
   </select></label>
  </div>
  {frames.length>0&&<div className="cine-image"><img src={apiUrl(path+'/frames/'+index)} alt={'所选证据帧 '+(index+1)}/></div>}
  <Button disabled={loading||!frames.length||!!busy||(frameView==='A4C'&&!configured)} onClick={()=>void segment()}>运行四腔分割</Button>
  {configured===false&&<p>四腔分割权重未配置。</p>}
  {busy&&<p role="status">{busy}</p>}
  {(error||job?.error)&&<p role="alert">{error||job?.error}</p>}
  {outcome?.kind==='insufficient_evidence'&&<InsufficientEvidenceNotice evidence={outcome.evidence} examId={examId}/>}
  {job&&output&&<div className="record">
   <h3>四腔结构叠图</h3>
   <p>第 {output.frame.frame_index+1} 帧 · {output.frame.timestamp_ms} ms · 模型第 {output.epoch} 轮</p>
   <div className="cine-image"><img src={apiUrl(scope==='samples'?path+'/overlays/'+output.frame.frame_index:'/analysis-jobs/'+job.id+'/overlay')} alt="LV 蓝色、LA 绿色、RA 橙色、RV 紫色的四腔结构叠图"/></div>
   <p>LV 蓝色 · LA 绿色 · RA 橙色 · RV 紫色</p>
  </div>}
 </article>
}
