import {useEffect,useState} from 'react'
import {Player,ViewAnalysis} from './MediaAnalysis'
import {api,errorMessage} from './api'
import {Button} from './button'
import {diagnosisHref,structureEvidenceHref} from './diagnosis-contract'
import StructureEvidence from './StructureEvidence'
import MedicalReview from './MedicalReview'
import {useCapabilities} from './use-capabilities'
import './Diagnosis.css'

type Exam = {id:number;patient_id:number;snapshot:{name:string};kind:string;status:string;parameters?:{exam_date?:string|null}}
type Case = {id:number;media:string[]}
type ExamMedia = {examId:number;ids:string[]}
const sections = ['AI 诊断结果','诊断证据','鉴别与复核','医生审核'] as const
const states:Record<string,string> = {draft:'草稿',submitted:'待审核',reviewed:'已审核'}

export default function Diagnosis({route}:{route:string}){
 const [,examId='',panel,mediaId=''] = route.split('/')
 const [exams,setExams] = useState<Exam[]|null>(null)
 const [data,setData] = useState<ExamMedia|null>(null)
 const [selection,setSelection] = useState(mediaId)
 const [section,setSection] = useState<(typeof sections)[number]>(panel==='structure'?sections[1]:sections[0])
 const [error,setError] = useState('')
 const [revision,reload] = useState(0)
 const modelStatus = useCapabilities(revision)
 const capError = modelStatus==='disconnected'
 const cap = modelStatus==='disconnected'?null:modelStatus
 const viewReady = cap?.model==='Stanford 切面模型'
 const exam = exams?.find(e=>String(e.id)===examId)
 const media = data && exam && data.examId===exam.id ? data.ids : []
 const active = media.includes(selection) ? selection : media[0] ?? ''

 useEffect(()=>{
  let current=true
  api<Exam[]>('/examinations').then(rows=>{if(current)setExams(rows)}).catch(e=>{if(current)setError(errorMessage(e))})
  return ()=>{current=false}
 },[revision])

 useEffect(()=>{
  if(!exam)return
  let current=true
  api<Case>('/cases/'+exam.id).then(c=>{
   if(!current)return
   if(c.id!==exam.id||!Array.isArray(c.media))throw new Error('影像与当前检查的关联无法验证。')
   setData({examId:exam.id,ids:c.media})
  }).catch(e=>{if(current)setError(errorMessage(e))})
  return ()=>{current=false}
 },[exam,revision])

 function refresh(){
  setError('');setExams(null);setData(null);reload(v=>v+1)
 }

 return <main className="dashboard business diagnosis">
  <div className="dashboard-title diagnosis-heading"><div><span className="eyebrow">Echo Edge · 研究工作区</span><h1>AI 辅助诊断</h1><p>关联影像与复核证据。当前仅供研究复核，不用于临床诊断或转诊决策。</p></div>
   <div className="diagnosis-header-actions">
    {exam&&<span className={'state '+exam.status}>检查 · {states[exam.status]||exam.status}</span>}
    <span className="state">疾病诊断 · 未接入</span>
    <span className={'state '+(viewReady?'reviewed':'')}>切面模型 · {capError?'状态读取失败':viewReady?'可用':cap?'未接入':'读取中'}</span>
    <Button variant="outline" disabled={!exams&&!error} onClick={refresh}>刷新检查</Button>
   </div></div>
  <section className="queue padded diagnosis-context" aria-label="当前检查">
   <div className="diagnosis-toolbar"><h2>当前检查</h2>
   <div className="context-bar"><label>选择检查<select value={exam?.id??''} disabled={!exams} onChange={e=>{const id=e.target.value;location.hash=section==='诊断证据'?structureEvidenceHref(id):id?diagnosisHref(id):'#diagnosis'}}>
    <option value="">请选择检查</option>{exams?.map(e=><option key={e.id} value={e.id}>#{e.id} · {e.snapshot.name} · {e.parameters?.exam_date||'日期未记录'}</option>)}</select></label>
    {exam&&<Button variant="outline" asChild><a href={'#exams/'+exam.id}>查看检查详情</a></Button>}
    {active&&section!=='诊断证据'&&<Button variant="outline" onClick={()=>setSection('诊断证据')}>打开四腔分割</Button>}
   </div></div>
   {error&&<p className="error" role="alert">{error}</p>}
   {!exams&&!error&&<p role="status">正在读取检查…</p>}
   {exams&&!exam&&<p>{examId?'检查不存在或当前账号无权访问。':exams.length?'选择检查后查看关联影像和证据。':'尚无可访问的检查。'} <a href="#exams">打开检查管理</a></p>}
   {exam&&<dl className="diagnosis-facts">{[
    ['患儿名称',exam.snapshot.name],['检查编号','#'+exam.id],['检查日期',exam.parameters?.exam_date||'未记录'],['检查状态',states[exam.status]||exam.status],['关联影像',data?String(media.length):error?'读取失败':'读取中'],
   ].map(([label,value])=><div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl>}
  </section>
  <div className="diagnosis-layout">
   <section className="queue padded diagnosis-viewer" aria-label="检查影像">
    <div className="diagnosis-toolbar"><h2>超声影像</h2>
    {active&&<div className="context-bar"><label>选择影像<select value={active} onChange={e=>setSelection(e.target.value)}>
     {media.map((id,i)=><option key={id} value={id}>影像 {i+1} / {media.length}</option>)}
    </select></label></div>}</div>
    {active?<Player key={active} id={active}/>:
    <div className="diagnosis-viewer-empty"><p>{!exam?'请先选择检查。':data?'当前检查尚无影像。':error?'无法读取影像。':'正在读取关联影像…'}</p>
    {exam&&data&&!active&&<Button variant="outline" asChild><a href="#media" onClick={()=>sessionStorage.setItem('echo-exam',String(exam.id))}>导入影像</a></Button>}</div>}
   </section>
   <section className="queue diagnosis-results" aria-label="诊断与复核">
    <div className="diagnosis-tablist" role="tablist" aria-label="切换诊断区域">{sections.map((s,i)=><button
     key={s} type="button" role="tab" id={'diagnosis-tab-'+i} aria-controls="diagnosis-panel"
     aria-selected={s===section} tabIndex={s===section?0:-1} onClick={()=>setSection(s)}
     onKeyDown={e=>{
      const last=sections.length-1
      const next=e.key==='ArrowRight'?(i+1)%sections.length:e.key==='ArrowLeft'?(i+last)%sections.length:e.key==='Home'?0:e.key==='End'?last:null
      if(next===null)return
      const nextSection=sections[next]
      if(nextSection===undefined)return
      e.preventDefault();setSection(nextSection);document.getElementById('diagnosis-tab-'+next)?.focus()
     }}>{s}</button>)}</div>
    <div id="diagnosis-panel" role="tabpanel" aria-labelledby={'diagnosis-tab-'+sections.indexOf(section)} tabIndex={0} className="diagnosis-panel">
     {section==='AI 诊断结果'?<>
      <article className="diagnosis-card">
       <div className="diagnosis-toolbar"><h2>疾病诊断</h2><span className="state">未接入</span></div>
       <p>疾病诊断模型尚未接入，当前无法生成 AI 疑似诊断。</p>
       <Button variant="outline" disabled>启动疾病诊断</Button>
      </article>
      <article className="diagnosis-card">
       <div className="diagnosis-toolbar"><h2>切面识别</h2><span className="state">阅片参考</span></div>
       <p>对当前选中的影像进行切面分类。此结果不用于判断是否患病。</p>
       {active?<ViewAnalysis key={active} id={active} available={viewReady} detail={capError?'无法读取模型状态。':cap?(viewReady?cap.modelDetail||'Stanford 切面模型可用。':'切面模型未接入。'):'正在读取模型状态…'}/>:<p>选择关联影像后可执行切面识别。</p>}
      </article>
      {active&&<MedicalReview key={active+'-quality'} id={active} kind="quality" capabilities={cap} statusFailed={capError}/>}
     </>:section==='诊断证据'?<>
     {active&&capError&&<p role="alert">无法读取四腔分割模型状态。</p>}
     {active&&<StructureEvidence key={active} id={active} configured={cap?.structureConfigured??null}/>}
     {active?<MedicalReview key={active+'-findings'} id={active} kind="findings" capabilities={cap} statusFailed={capError}/>:<p>请选择正式检查中已导入的影像。</p>}
     </>:section==='鉴别与复核'?<article className="diagnosis-card">
      <h2>鉴别诊断与复核提示</h2><p>尚未接入经过临床审核的知识规则。当前不生成鉴别诊断或医学复核建议。</p>
     </article>:<article className="diagnosis-card">
      <h2>医生审核</h2><p id="diagnosis-review-status">诊断审核接口未接入。初步诊断、复核意见及审核操作目前不可保存。</p>
      <fieldset disabled className="business-form" aria-describedby="diagnosis-review-status">
       <label>医生初步诊断<textarea placeholder="未接入"/></label><label>复核意见<textarea placeholder="未接入"/></label>
       <div className="tabs"><Button disabled>确认</Button><Button disabled variant="outline">修改</Button><Button disabled variant="outline">驳回</Button></div>
      </fieldset>
      {exam&&<Button variant="outline" asChild><a href="#experts" onClick={()=>sessionStorage.setItem('echo-exam',String(exam.id))}>打开现有专家协作</a></Button>}
     </article>}
    </div>
   </section>
  </div>
 </main>
}
