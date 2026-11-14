import {useEffect, useState} from 'react'
import {api} from './Modules'
import {Button} from './button'

type Frame = {index:number; offsetMs:number}
type Job = {
 id:number
 model_type:string
 state:string
 error:string|null
 output:{epoch:number;foreground_pixels:number;frame:{frame_index:number;timestamp_ms:number}}|null
}
type Receipt = {hint:string;sequence:number}

export default function StructureEvidence({id, configured, terminal}:{id:string;configured:boolean;terminal:boolean}){
 const [frames,setFrames]=useState<Frame[]>([])
 const [index,setIndex]=useState(0)
 const [job,setJob]=useState<Job|null>(null)
 const [receipt,setReceipt]=useState<Receipt|null>(null)
 const [busy,setBusy]=useState('')
 const [error,setError]=useState('')

 useEffect(()=>{
  let current=true
  Promise.all([api<{frames:Frame[]}>('/media/'+id),api<Job[]>('/media/'+id+'/analysis-jobs')])
   .then(([media,jobs])=>{
    if(!current)return
    setFrames(media.frames)
    setJob(jobs.find(item=>item.model_type==='cardiacudc_fourchamber')??null)
   }).catch((failure:Error)=>{if(current)setError(failure.message)})
  return ()=>{current=false}
 },[id])

 async function segment(){
  setBusy('正在分割…');setError('');setReceipt(null)
  try{
   setJob(await api<Job>('/media/'+id+'/structure-jobs',{frame_index:index}))
  }catch(failure){setError((failure as Error).message)}
  finally{setBusy('')}
 }

 async function send(jobId:number){
  setBusy('等待终端回执…');setError('');setReceipt(null)
  try{
   setReceipt(await api<Receipt>('/analysis-jobs/'+jobId+'/terminal',{}))
  }catch(failure){setError((failure as Error).message)}
  finally{setBusy('')}
 }

 return <article className="diagnosis-card">
  <h2>四腔结构分割</h2>
  <p>CardiacUDC 研究版。请选择已确认属于心尖四腔心切面的影像帧。</p>
  <div className="context-bar"><label>选择证据帧<select value={index} disabled={!frames.length||!!busy} onChange={event=>setIndex(Number(event.target.value))}>
   {frames.map(frame=><option key={frame.index} value={frame.index}>第 {frame.index+1} 帧 · {frame.offsetMs.toFixed(0)} ms</option>)}
  </select></label></div>
  <Button disabled={!configured||!frames.length||!!busy} onClick={()=>void segment()}>运行四腔分割</Button>
  {!configured&&<p>四腔分割权重未配置。</p>}
  {busy&&<p role="status">{busy}</p>}
  {(error||job?.error)&&<p role="alert">{error||job?.error}</p>}
  {job?.state==='succeeded'&&job.output&&<div className="record">
   <h3>四腔结构叠图</h3>
   <p>第 {job.output.frame.frame_index+1} 帧 · {job.output.frame.timestamp_ms} ms · 模型第 {job.output.epoch} 轮</p>
   <div className="cine-image"><img src={'/api/v1/analysis-jobs/'+job.id+'/overlay'} alt="LV 蓝色、LA 绿色、RA 橙色、RV 紫色的四腔结构叠图"/></div>
   <p>LV 蓝色 · LA 绿色 · RA 橙色 · RV 紫色</p>
   {terminal&&<Button variant="outline" disabled={!!busy} onClick={()=>void send(job.id)}>发送分割完成到终端</Button>}
   {receipt&&<p role="status">终端已接收 {receipt.hint} · 回执 #{receipt.sequence}</p>}
  </div>}
 </article>
}
