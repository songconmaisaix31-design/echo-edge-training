import {useEffect,useState} from 'react'
import {Button} from './button'
import {api,errorMessage} from './api'
import {readExamParameters,vitalFields,type ExamParameters} from './exam-parameters'
type ParameterVersion=ExamParameters&{id:number;created:string;author:number}
type Measurement={
 id:number
 item:string
 value:number
 unit:string
 view:string
 phase:string
 source:'manual'|'ai'
 review:'pending'|'confirmed'|'uncertain'
 media_id:string|null
 frame_index:number|null
 created:string
 replaces_id:number|null
}
const parameterFields=[['indication','检查指征'],['doctor','检查医生'],['device','检查设备']] as const
function ParameterFields({initial={},birth}:{initial:ExamParameters|undefined;birth:string|undefined}){
 const [h,setH]=useState(String(initial.height_cm??'')),[w,setW]=useState(String(initial.weight_kg??'')),[day,setDay]=useState(initial.exam_date||new Date().toLocaleDateString('sv-SE'))
 const age=birth?Math.floor((Date.parse(day)-Date.parse(birth))/86400000):null
 const bsa=h&&w&&+h>0&&+w>0&&+h<=300&&+w<=500?Math.sqrt(+h*+w/3600):null
 return <><label>检查日期<input name="exam_date" type="date" required value={day} onChange={e=>setDay(e.target.value)} min={birth} max={new Date().toLocaleDateString('sv-SE')}/></label><p>检查时年龄 {age!==null&&age>=0?age+' 天':'待填写'}</p>{vitalFields.map(([key,label,max])=><label key={key}>{label}<input name={key} type="number" step="any" min={key==='spo2'?0:0.001} max={max} defaultValue={initial[key]??''} onChange={key==='height_cm'?e=>setH(e.target.value):key==='weight_kg'?e=>setW(e.target.value):undefined}/></label>)}<p>BSA {bsa===null?'未计算（缺少有效身高或体重）':bsa.toFixed(4)+' m²'} · Mosteller</p>{parameterFields.map(([key,label])=><label key={key}>{label}<input name={key} defaultValue={initial[key]||''}/></label>)}</>
}
export default function ExamDetails({id,birth,initialTab="临床参数",hideTabs=false}:{id:string;birth?:string;initialTab?:string;hideTabs?:boolean}){
 const [editing,setEditing]=useState<Measurement|null>(null)
 const [tab,setTab]=useState(initialTab),[versions,setVersions]=useState<ParameterVersion[]>([]),[measurements,setMeasurements]=useState<Measurement[]>([]),[media,setMedia]=useState<string[]>([]),[error,setError]=useState(''),[busy,setBusy]=useState(false)
 async function load(){const [v,m,c]=await Promise.all([api<ParameterVersion[]>('/examinations/'+id+'/parameters'),api<Measurement[]>('/examinations/'+id+'/measurements'),api<{media:string[]}>('/cases/'+id)]);setVersions(v);setMeasurements(m);setMedia(c.media)}
 useEffect(()=>{void load().catch(e=>setError(errorMessage(e)))},[id])
 async function save(action:()=>Promise<void>){setBusy(true);setError('');try{await action();await load()}catch(e){setError(errorMessage(e))}finally{setBusy(false)}}
 return <section className="queue padded"><h2>检查 #{id} 详情</h2>{!hideTabs&&<div className="tabs">{['临床参数','超声测量','修改历史'].map(t=><Button key={t} onClick={()=>setTab(t)} variant={tab===t?'default':'outline'}>{t}</Button>)}</div>}{error&&<p role="alert">{error}</p>}
 {tab==='临床参数'?<form className="business-form" key={id+'-'+versions[0]?.id} onSubmit={e=>{e.preventDefault();const values=readExamParameters(new FormData(e.currentTarget));void save(async()=>{await api('/examinations/'+id+'/parameters',values)})}}><ParameterFields initial={versions[0]} birth={birth}/><Button disabled={busy}>保存本次检查参数</Button></form>:tab==='修改历史'?versions.map(v=><article className="record" key={v.id}><b>{v.created} · 操作者 {v.author}</b><p>{v.exam_date} · 年龄 {v.age_days} 天 · 身高 {v.height_cm??'—'} cm · 体重 {v.weight_kg??'—'} kg · BSA {v.bsa_m2?.toFixed(4)??'—'} m²</p><p>心率 {v.heart_rate??'—'} · 血压 {v.systolic??'—'}/{v.diastolic??'—'} · SpO₂ {v.spo2??'—'}%</p><p>{v.indication} · {v.doctor} · {v.device}</p><p>{v.clinical_notes||"未记录临床备注"}</p></article>):<><p>Z-score 参考公式未配置。</p><form className="business-form" key={editing?.id||"new-measurement"} onSubmit={e=>{e.preventDefault();const form=e.currentTarget;const data=Object.fromEntries(new FormData(form));void save(async()=>{await api('/examinations/'+id+'/measurements',{...data,replaces_id:editing?.id||null,value:Number(data.value),media_id:data.media_id||null,frame_index:data.frame_index===''?null:Number(data.frame_index)});setEditing(null);form.reset()})}}>{([['item','测量项目'],['value','测量数值'],['unit','单位'],['view','测量切面'],['phase','时相']] as const).map(([key,label])=><label key={key}>{label}<input name={key} defaultValue={editing?.[key]??""} required={['item','value','unit'].includes(key)} type={key==='value'?'number':'text'} step="any"/></label>)}<label>来源<select name="source"><option value="manual">人工</option><option disabled value="ai">AI（模型未接入）</option></select></label><label>关联影像<select name="media_id" defaultValue={editing?.media_id||""}><option value="">未关联</option>{media.map((m,i)=><option key={m} value={m}>影像 {i+1}</option>)}</select></label><label>关键帧索引（从 0 开始）<input name="frame_index" defaultValue={editing?.frame_index??""} type="number" min={0} step={1}/></label><label>医生审核状态<select name="review" defaultValue={editing?.review||"pending"}><option value="pending">待审核</option><option value="confirmed">已确认</option><option value="uncertain">无法判断</option></select></label><Button disabled={busy}>{editing?"保存测量修订":"保存测量记录"}</Button>{editing&&<Button variant="outline" type="button" onClick={()=>setEditing(null)}>取消修订</Button>}</form>{measurements.map(m=><article className="record" key={m.id}><b>{m.item} · {m.value} {m.unit}</b><span>{m.view} · {m.phase} · {m.source} · {m.review}</span><span>{m.media_id||'未关联影像'} · 帧 {m.frame_index??'—'}</span><span>{m.created} · {m.replaces_id?"修订自记录 #"+m.replaces_id:"原始记录"}</span><Button variant="outline" onClick={()=>setEditing(m)}>修订记录 #{m.id}</Button></article>)}</>}
 </section>
}
