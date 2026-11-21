import {useEffect,useRef,useState} from 'react'
import {apiUrl,errorMessage,getFrameOptions,mediaApiPath,requestAssessment} from './api'
import {Button} from './button'
import {structureEvidenceHref} from './diagnosis-contract'
import {InsufficientEvidenceNotice} from './InsufficientEvidenceNotice'
import type {AssessmentResponse,FrameEvidence,Status,View} from './multiview-api.generated'
import {availableViews,isView,viewLabels,type SelectedFrame} from './multiview-contract'

const coverageLabels={covered:'已有确认帧',missing:'缺少确认帧'} satisfies Record<Status,string>
const frameKey=(frame:SelectedFrame)=>frame.media_id+'/'+frame.frame_index
const frameUrl=(frame:SelectedFrame)=>apiUrl(mediaApiPath(frame.media_id,'private')+'/frames/'+frame.frame_index)

function AssessmentResult({result}:{result:AssessmentResponse}){
 switch(result.state){
  case 'insufficient_evidence':
   return <InsufficientEvidenceNotice evidence={result.insufficient_evidence}
    examId={result.required_views.length?result.exam_id:undefined}/>
  case 'succeeded':
   return <>
    <p role="status">必要切面均已有确认帧。</p>
    <h3>待人工复核的征象观察</h3>
    {!result.findings.length&&<p>暂无符合本次证据要求的征象观察。</p>}
    {result.findings.map(finding=><article className="record" key={finding.analysis_job_id+'-'+finding.observation_number}>
     <p>征象复核任务 #{finding.analysis_job_id} · 第 {finding.observation_number} 条观察</p>
     <a href={structureEvidenceHref(result.exam_id,finding.evidence_frames[0].media_id)}>打开该影像的复核记录</a>
     <ul>{finding.evidence_frames.map(frame=><li key={frameKey(frame)}><EvidenceLink frame={frame}/></li>)}</ul>
     {!!finding.missing_views.length&&<p>该条观察尚缺证据　{finding.missing_views.map(view=>viewLabels[view]).join('、')}</p>}
    </article>)}
   </>
  default: {
   const unreachable:never=result
   throw new Error('无法识别的评估状态 '+unreachable)
  }
 }
}

function EvidenceLink({frame}:{frame:FrameEvidence}){
 return <a href={frameUrl(frame)} target="_blank" rel="noreferrer">查看{viewLabels[frame.view]} · 第 {frame.frame_index+1} 帧 · {frame.timestamp_ms} ms</a>
}

export default function MultiviewAssessment({examId,media,refresh}:{examId:string;media:string[];refresh:number}){
 const [saved,setSaved]=useState<AssessmentResponse|null>(null)
 const [required,setRequired]=useState<View[]>([])
 const [frames,setFrames]=useState<SelectedFrame[]>([])
 const [dirty,setDirty]=useState(false)
 const [busy,setBusy]=useState(false)
 const [error,setError]=useState('')
 const [mediaId,setMediaId]=useState('')
 const [view,setView]=useState<View|''>('')
 const [frameIndex,setFrameIndex]=useState(0)
 const [options,setOptions]=useState<{index:number;offsetMs:number}[]>([])
 const [frameError,setFrameError]=useState('')
 const appliedRevision=useRef(-1)
 const currentFrame=options.find(frame=>frame.index===frameIndex)

 function loadResult(result:AssessmentResponse){
  if(result.revision<appliedRevision.current)return
  appliedRevision.current=result.revision
  setSaved(result)
  setRequired(result.required_views)
  setFrames(result.frames.map(({view,media_id,frame_index})=>({view,media_id,frame_index})))
  setDirty(false)
 }

 useEffect(()=>{
  let active=true
  setError('')
  requestAssessment(examId).then(result=>{if(active)loadResult(result)}).catch(failure=>{if(active)setError(errorMessage(failure))})
  return ()=>{active=false}
 },[examId,refresh])

 useEffect(()=>{
  let active=true
  setOptions([]);setFrameError('')
  if(mediaId)getFrameOptions(mediaId).then(result=>{if(active)setOptions(result)}).catch(failure=>{if(active)setFrameError(errorMessage(failure))})
  return ()=>{active=false}
 },[mediaId])

 async function save(){
  if(!saved)return
  setBusy(true);setError('')
  try{loadResult(await requestAssessment(examId,{revision:saved.revision,required_views:required,frames}))}
  catch(failure){setError(errorMessage(failure))}
  finally{setBusy(false)}
 }

 function addFrame(){
  if(!mediaId||!view||!currentFrame)return
  const entry={view,media_id:mediaId,frame_index:currentFrame.index}
  setFrames(previous=>[...previous.filter(frame=>frameKey(frame)!==frameKey(entry)),entry])
  setDirty(true)
 }

 return <section className="queue padded" aria-label="多切面评估">
  <h2>多切面评估</h2>
  <p>操作者为本次研究评估指定切面。覆盖状态仅表示已有确认帧，影像质量和观察由医生复核。</p>
  {error&&<p role="alert">{error}</p>}
  {!saved&&!error&&<p role="status">正在读取本次评估…</p>}
  {saved&&<>
   {!dirty&&<AssessmentResult result={saved}/>}
   {!dirty&&!!saved.coverage.length&&<ul aria-label="切面覆盖">
    {saved.coverage.map(item=><li key={item.view}>{viewLabels[item.view]} · {coverageLabels[item.status]}</li>)}
   </ul>}
   {!dirty&&!!saved.frames.length&&<ul aria-label="已确认的原始帧">
    {saved.frames.map(frame=><li key={frameKey(frame)}><EvidenceLink frame={frame}/></li>)}
   </ul>}
   {saved.editable&&<form className="business-form" onSubmit={event=>{event.preventDefault();void save()}}>
    <fieldset className="tabs" disabled={busy}>
     <legend>本次评估的必要切面</legend>
     {availableViews.map(code=><label key={code}><input type="checkbox" checked={required.includes(code)} onChange={event=>{
      const checked=event.target.checked
      setRequired(previous=>checked?[...previous,code]:previous.filter(item=>item!==code));setDirty(true)
     }}/>{viewLabels[code]}</label>)}
    </fieldset>
    {!!media.length&&<fieldset disabled={busy}>
     <legend>确认原始帧</legend>
     <div className="context-bar">
      <label>证据影像<select value={mediaId} onChange={event=>{setMediaId(event.target.value);setFrameIndex(0);setView('');setOptions([])}}>
       <option value="">选择影像</option>{media.map((id,index)=><option key={id} value={id}>影像 {index+1}</option>)}
      </select></label>
      <label>原始帧<select value={frameIndex} disabled={!options.length} onChange={event=>{setFrameIndex(Number(event.target.value));setView('')}}>
       {options.map(frame=><option key={frame.index} value={frame.index}>第 {frame.index+1} 帧 · {frame.offsetMs} ms</option>)}
      </select></label>
      <label>确认切面<select value={view} onChange={event=>{const code=event.target.value;setView(isView(code)?code:'')}}>
       <option value="">选择切面</option>{availableViews.map(code=><option key={code} value={code}>{viewLabels[code]}</option>)}
      </select></label>
     </div>
     {frameError&&<p role="alert">{frameError}</p>}
     {mediaId&&currentFrame&&<div className="cine-image">
      <img src={apiUrl(mediaApiPath(mediaId,'private')+'/frames/'+frameIndex)} alt={'待确认原始帧 '+(frameIndex+1)}/>
     </div>}
     <Button type="button" variant="outline" disabled={!currentFrame||!view} onClick={addFrame}>添加已确认帧</Button>
    </fieldset>}
    {!!frames.length&&<ul aria-label="评估表单中的确认帧">
     {frames.map(frame=><li key={frameKey(frame)}>
      {viewLabels[frame.view]} · 影像 {media.indexOf(frame.media_id)+1} · 第 {frame.frame_index+1} 帧{' '}
      <Button type="button" variant="outline" disabled={busy} aria-label={'移除'+viewLabels[frame.view]+'第 '+(frame.frame_index+1)+' 帧'} onClick={()=>{
       setFrames(previous=>previous.filter(item=>frameKey(item)!==frameKey(frame)));setDirty(true)
      }}>移除该帧</Button>
     </li>)}
    </ul>}
    <Button type="submit" disabled={busy||!dirty}>{busy?'正在保存…':'保存本次评估'}</Button>
   </form>}
  </>}
 </section>
}
