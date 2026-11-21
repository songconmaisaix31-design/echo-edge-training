import {useEffect,useState} from 'react'
import {Button} from './button'
import {api,apiUrl,errorMessage,mediaApiPath,type MediaScope} from './api'
import {analysisStateLabels,type AnalysisJobState} from './analysis-state'

type InputStatus={status:string;detail:string}
type Media={original?:{exists:boolean;format:string|null};modelInputs?:{stanford_view?:InputStatus};id:string;width:number;height:number;codec:string;frameCount:number;frames:{index:number;offsetMs:number}[]}

export function Player({id,scope='private'}:{id:string;scope?:MediaScope}){
 const path=mediaApiPath(id,scope)
 const [media,setMedia]=useState<Media|null>(null),[index,setIndex]=useState(0),[play,setPlay]=useState(false),[error,setError]=useState('')
 useEffect(()=>{let active=true;setMedia(null);setIndex(0);setPlay(false);setError('');api<Media>(path).then(v=>{if(active)setMedia(v)}).catch(e=>{if(active)setError(errorMessage(e))});return()=>{active=false}},[path])
 useEffect(()=>{
  if(!play||!media)return
  if(index===media.frameCount-1){setPlay(false);return}
  const from=media.frames[index]
  const to=media.frames[index+1]
  if(!from||!to){setPlay(false);return}
  const timer=setTimeout(()=>setIndex(i=>i+1),Math.max(1,to.offsetMs-from.offsetMs))
  return()=>clearTimeout(timer)
 },[play,index,media])
 const frame=media?.frames[index]
 return <div className="cine"><div className="cine-image">{media?<img src={apiUrl(path+'/frames/'+index)} alt={'源帧 '+(index+1)}/>:<p>{error||'正在读取影像…'}</p>}</div>{media&&<><input aria-label="帧时间轴" type="range" min="0" max={media.frameCount-1} value={index} onChange={e=>{setPlay(false);setIndex(+e.target.value)}}/><div className="cine-controls"><Button variant="outline" disabled={!index} onClick={()=>{setPlay(false);setIndex(i=>i-1)}}>上一帧</Button><Button onClick={()=>{if(index===media.frameCount-1)setIndex(0);setPlay(!play)}}>{play?'暂停':'播放'}</Button><Button variant="outline" disabled={index===media.frameCount-1} onClick={()=>{setPlay(false);setIndex(i=>i+1)}}>下一帧</Button><span>{index+1} / {media.frameCount} · {frame?(frame.offsetMs/1000).toFixed(3):'—'} s</span></div><p>{media.width} × {media.height} · {media.codec}{media.original&& (media.original.exists?' · 原件已保存':' · 原件缺失，仅可阅片')}</p></>}</div>
}

type ViewPrediction = {
 view: 'A4C' | 'PSAX' | null
 sampled_frames: number
 probabilities: Record<'A4C' | 'PSAX', number>
}

type AnalysisJob = {
 id:number
 model_type:string
 state:AnalysisJobState
 model_version:string
 output:ViewPrediction|null
 error:string|null
 created:string
}

const viewNames = { A4C: '心尖四腔心切面', PSAX: '胸骨旁短轴切面' }

export function ViewAnalysis({id, available, detail, scope='private'}:{id:string;available:boolean;detail:string;scope?:MediaScope}){
 const path=mediaApiPath(id,scope)
 const [busy,setBusy]=useState(false),[error,setError]=useState('')
 const [history,setHistory]=useState<AnalysisJob[]>([])
 const [historyError,setHistoryError]=useState('')
 const resultJob=history.find(job=>job.state==='succeeded'&&job.output)
 const result=resultJob?.output
 const latest=history[0]
 const [input,setInput]=useState<{id:string;status:InputStatus}|null>(null)
 const [inputError,setInputError]=useState('')
 useEffect(()=>{
  if(!id)return
  let current=true
  setInputError('')
  setHistory([])
  setHistoryError('')
  api<AnalysisJob[]>(path+'/analysis-jobs').then(rows=>{
   if(current)setHistory(rows.filter(job=>job.model_type==='stanford_view'))
  }).catch(e=>{if(current)setHistoryError(errorMessage(e))})
  api<Media>(path).then(media=>{
   if(current)setInput({id,status:media.modelInputs?.stanford_view ?? {status:'unknown',detail:'影像推理条件未提供。'}})
  }).catch(e=>{if(current)setInputError(errorMessage(e))})
  return ()=>{current=false}
 },[id,path])
 const eligibility=input?.id===id?input.status:null
 async function predict(){
  setBusy(true);setError('')
  try{
   const job=await api<AnalysisJob>(path+'/analysis-jobs',{})
   setHistory(rows=>[job,...rows.filter(row=>row.id!==job.id)])
  }
  catch(e){setError(errorMessage(e))}
  finally{setBusy(false)}
 }
 return <>
  <p>{detail || '当前影像尚无切面推理结果。'}</p>
  {result&&<div className="record" role="status">
   <b>{result.view===null?'证据不足':'AI 切面结论'}</b>
   <p>{result.view ? '模型判断这段视频更符合' + viewNames[result.view] + '。' : '模型未达到判定阈值，暂无法确认这段视频的切面。'}</p>
   {result.view===null&&<p>请复核当前影像，或补采所需切面后重试。</p>}
   <p>当前模型仅比较心尖四腔心切面与胸骨旁短轴切面。结果供研究复核。</p>
   <details>
    <summary>查看模型详情</summary>
    <p>抽样 {result.sampled_frames} 帧 · A4C {(result.probabilities.A4C*100).toFixed(1)}% · PSAX {(result.probabilities.PSAX*100).toFixed(1)}%</p>
   </details>
  </div>}
  {historyError&&<p role="alert">读取分析历史失败：{historyError}</p>}
  {id&&<p role="status">分析状态：{busy?'运行中':latest?analysisStateLabels[latest.state]:'暂无记录'}</p>}
  {latest?.error&&<p role="alert">{latest.error}</p>}
  {history.length>0&&<details><summary>历史分析（{history.length}）</summary>
   {history.map(job=><div className="record" key={job.id}>
    <p>任务 #{job.id} · {new Date(job.created).toLocaleString()} · {analysisStateLabels[job.state]}</p>
    <p style={{overflowWrap:'anywhere'}}>模型版本 {job.model_version}</p>
    {job.output&&<p>{job.output.view?viewNames[job.output.view]:'未判定'} · A4C {(job.output.probabilities.A4C*100).toFixed(1)}% · PSAX {(job.output.probabilities.PSAX*100).toFixed(1)}%</p>}
    {job.error&&<p>{job.error}</p>}
   </div>)}
  </details>}
  {id&&<p>{inputError||eligibility?.detail||'正在检查影像原件…'}</p>}
  {error&&<p role="alert">{error}</p>}
  <Button disabled={!id||!available||eligibility?.status!=='eligible'||busy} onClick={()=>void predict()}>{busy?'正在推理…':'执行切面识别'}</Button>
 </>
}
