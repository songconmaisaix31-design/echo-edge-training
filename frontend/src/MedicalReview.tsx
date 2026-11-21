import {useEffect,useState} from 'react'
import {analysisHistory,api,apiUrl,errorMessage,isRecord,submitAnalysis,type Capabilities} from './api'
import {analysisStateLabels as states} from './analysis-state'
import {analysisOutcome,type AnalysisJob} from './diagnosis-contract'
import {InsufficientEvidenceNotice} from './InsufficientEvidenceNotice'
import {Button} from './button'
import './MedicalReview.css'

type Kind='quality'|'findings'
type Frame={media_id:string;frame_index:number;timestamp_ms:number}
type Observation={title:string;observation:string;interpretation:string;confidence:'low'|'moderate'|'high';evidence_frames:Frame[]}
type SampledReview={
 sampled_frames:Frame[]
 source_frame_count:number
 duration_ms:number
 model:string
 prompt_version:string
}
type Result=SampledReview&{
 kind:Kind
 assessment:string
 summary:string
 observations:Observation[]
 limitations:string[]
 recommendations:string[]
}
function isFrame(value:unknown):value is Frame{
 return isRecord(value)&&typeof value.media_id==='string'&&typeof value.frame_index==='number'&&typeof value.timestamp_ms==='number'
}

function isObservation(value:unknown):value is Observation{
 return isRecord(value)&&typeof value.title==='string'&&typeof value.observation==='string'&&typeof value.interpretation==='string'&&
  (value.confidence==='low'||value.confidence==='moderate'||value.confidence==='high')&&
  Array.isArray(value.evidence_frames)&&value.evidence_frames.every(isFrame)
}

function isTextList(value:unknown):value is string[]{
 return Array.isArray(value)&&value.every(item=>typeof item==='string')
}

function hasSampledFrames(value:unknown):value is SampledReview{
 return isRecord(value)&&typeof value.model==='string'&&typeof value.prompt_version==='string'&&
  typeof value.source_frame_count==='number'&&typeof value.duration_ms==='number'&&
  Array.isArray(value.sampled_frames)&&value.sampled_frames.every(isFrame)
}

function isResult(value:unknown):value is Result{
 return isRecord(value)&&(
  (value.kind==='quality'&&(value.assessment==='adequate'||value.assessment==='limited'))||
  (value.kind==='findings'&&(value.assessment==='suspected_findings'||value.assessment==='no_specific_finding'))
 )&&
  typeof value.summary==='string'&&
  Array.isArray(value.observations)&&value.observations.every(isObservation)&&
  isTextList(value.limitations)&&isTextList(value.recommendations)&&hasSampledFrames(value)
}

const labels={quality:'影像质量',findings:'异常征象'}
const assessments:Record<string,string>={adequate:'可评估',limited:'评估受限',suspected_findings:'疑似征象',no_specific_finding:'未见特定征象'}
const confidence={low:'低',moderate:'中',high:'高'}

function SampledFrames({result,onSelect}:{result:SampledReview;onSelect:(frame:Frame)=>void}){
 return <>
  <p>模型 {result.model} · 采样 {result.sampled_frames.length} / {result.source_frame_count} 帧 · 视频时长 {(result.duration_ms/1000).toFixed(3)} s</p>
  <details><summary>查看送入模型的采样帧</summary><div className="medical-evidence-list">
   {result.sampled_frames.map(frame=><Button key={frame.frame_index} variant="outline" onClick={()=>onSelect(frame)}>第 {frame.frame_index+1} 帧 · {frame.timestamp_ms} ms</Button>)}
  </div></details>
 </>
}

export default function MedicalReview({id,kind,capabilities,statusFailed=false,examId}:{id:string;kind:Kind;capabilities:Capabilities|null;statusFailed?:boolean;examId?:number|string}){
 const [jobs,setJobs]=useState<AnalysisJob<Result>[]>([])
 const [selected,setSelected]=useState<number|null>(null)
 const [loading,setLoading]=useState(true)
 const [busy,setBusy]=useState(false)
 const [error,setError]=useState('')
 const [evidence,setEvidence]=useState<Frame|null>(null)
 const [revision,reload]=useState(0)
 const [allowed,setAllowed]=useState(false)
 const sampleOnly=capabilities?.reviewSampleOnly===true
 const modelStatus=statusFailed?'模型状态读取失败，请刷新页面重试。'
  :sampleOnly&&!loading&&!allowed?'阶跃星辰复核仅支持本次公开样例。'
  :capabilities?.reviewConfigured?null:capabilities?.reviewDetail||'正在读取视觉模型配置…'
 const job=jobs.find(item=>item.id===selected)??jobs[0]
 const outcome=analysisOutcome(job)
 const result=outcome?.kind==='result'?outcome.output:null
 const showNarrative=result?.observations.every(item=>item.confidence!=='low')

 useEffect(()=>{
  let current=true
  setLoading(true);setError('');setJobs([]);setSelected(null);setEvidence(null);setAllowed(false)
  const media=sampleOnly?api<{reviewAllowed:boolean}>('/media/'+id):null
  Promise.all([analysisHistory('/media/'+id+'/analysis-jobs','llm_'+kind,isResult),media]).then(([history,item])=>{
   if(current){setJobs(history);setAllowed(item?.reviewAllowed===true)}
  }).catch(failure=>{if(current)setError(errorMessage(failure))})
   .finally(()=>{if(current)setLoading(false)})
  return()=>{current=false}
 },[id,kind,revision,sampleOnly])

 async function analyze(){
  setBusy(true);setError('');setEvidence(null)
  try{
   const created=await submitAnalysis('/media/'+id+'/review-jobs',{kind},isResult)
   setJobs(history=>[created,...history.filter(item=>item.id!==created.id)]);setSelected(created.id)
  }catch(failure){setError(errorMessage(failure))}
  finally{setBusy(false)}
 }

 return <article className="medical-review" aria-label={labels[kind]+'大模型复核'}>
  <h2>{labels[kind]}复核</h2>
  <p>按心脏超声专业阅片结构生成 AI 研究意见，结论需医生确认。</p>
  {modelStatus&&<p>{modelStatus}</p>}
  <div className="tabs">
   <Button disabled={!capabilities?.reviewConfigured||(sampleOnly&&!allowed)||loading||busy} onClick={()=>void analyze()}>{busy?'正在分析…':'运行'+labels[kind]+'分析'}</Button>
   <Button variant="outline" disabled={busy||loading} onClick={()=>reload(value=>value+1)}>刷新分析记录</Button>
  </div>
  {busy&&<p role="status">正在提交影像帧并等待模型结果…</p>}
  {loading&&<p role="status">正在读取分析记录…</p>}
  {error&&<p role="alert">{error}</p>}
  {jobs.length>0&&<label>分析记录<select value={job?.id??''} disabled={busy} onChange={event=>{setSelected(Number(event.target.value));setEvidence(null)}}>
   {jobs.map(item=><option key={item.id} value={item.id}>#{item.id} · {new Date(item.created).toLocaleString()} · {states[item.state]}</option>)}
  </select></label>}
  {job&&<p>任务 #{job.id} · {states[job.state]}</p>}
  {job?.error&&<p role="alert">{job.error}</p>}
  {!loading&&!job&&!error&&<p>当前影像尚无{labels[kind]}分析记录。</p>}
  {outcome?.kind==='insufficient_evidence'&&<>
   <InsufficientEvidenceNotice evidence={outcome.evidence} examId={examId}/>
   {hasSampledFrames(outcome.output)&&<SampledFrames result={outcome.output} onSelect={setEvidence}/>}
  </>}
  {result&&<div className="medical-review-result">
   <h3>{assessments[result.assessment]||result.assessment}</h3>
   {showNarrative&&<p>{result.summary}</p>}
   <SampledFrames result={result} onSelect={setEvidence}/>
   {result.observations.map((item,index)=><section className="medical-observation" key={index}>
    {item.confidence==='low'?<h4>该条观察证据不足</h4>:<>
     <h4>{item.title}</h4>
     <p>{item.observation}</p><p>{item.interpretation}</p>
    </>}
    <p>观察置信程度（模型自评） · {confidence[item.confidence]}</p>
    <div className="medical-evidence-list">{item.evidence_frames.map(frame=><Button key={frame.frame_index} variant="outline" onClick={()=>setEvidence(frame)}>证据 · 第 {frame.frame_index+1} 帧 · {frame.timestamp_ms} ms</Button>)}</div>
   </section>)}
   {showNarrative&&<>
    <h4>评估限制</h4><ul>{result.limitations.map((text,index)=><li key={index}>{text}</li>)}</ul>
    <h4>医生复核建议</h4><ul>{result.recommendations.map((text,index)=><li key={index}>{text}</li>)}</ul>
   </>}
  </div>}
  {evidence&&<figure><img src={apiUrl('/media/'+evidence.media_id+'/frames/'+evidence.frame_index)} alt={'复核证据 · 第 '+(evidence.frame_index+1)+' 帧'}/><figcaption>第 {evidence.frame_index+1} 帧 · {evidence.timestamp_ms} ms</figcaption></figure>}
 </article>
}
