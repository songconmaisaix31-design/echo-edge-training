import ReviewWorkflow from './ReviewWorkflow'
import {useEffect,useState} from 'react'
import {Button} from './button'
type Row={id:number;name?:string;birth?:string;sex?:string;notes?:string;history?:string;treatment_history?:string;title?:string;status?:string;patient_id?:number;created?:string;kind?:string;reason?:string;role?:string;author?:string;text?:string;state?:string;snapshot?:{name:string;birth:string}}
type InputStatus={status:string;detail:string}
type Media={original?:{exists:boolean;format:string|null};modelInputs?:{stanford_view?:InputStatus};id:string;width:number;height:number;codec:string;frameCount:number;frames:{index:number;offsetMs:number}[]}
export async function api<T>(path:string,body?:unknown,method='POST'):Promise<T>{
 const r=await fetch('/api/v1'+path,body===undefined?undefined:{method,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
 const data=await r.json()
 if(!r.ok)throw new Error(typeof data.detail==='string'?data.detail:Array.isArray(data.detail)?data.detail.map((e:{loc:string[];msg:string})=>e.loc.join('.')+'：'+e.msg).join('；'):data.detail?.message||'请求失败，请检查输入')
 return data
}
export function Player({id}:{id:string}){
 const [media,setMedia]=useState<Media|null>(null),[index,setIndex]=useState(0),[play,setPlay]=useState(false),[error,setError]=useState('')
 useEffect(()=>{let active=true;setMedia(null);setIndex(0);setPlay(false);setError('');api<Media>('/media/'+id).then(v=>{if(active)setMedia(v)}).catch(e=>{if(active)setError(e.message)});return()=>{active=false}},[id])
 useEffect(()=>{if(!play||!media)return;if(index===media.frameCount-1){setPlay(false);return}const timer=setTimeout(()=>setIndex(i=>i+1),Math.max(1,media.frames[index+1].offsetMs-media.frames[index].offsetMs));return()=>clearTimeout(timer)},[play,index,media])
 return <div className="cine"><div className="cine-image">{media?<img src={'/api/v1/media/'+id+'/frames/'+index} alt={'源帧 '+(index+1)}/>:<p>{error||'正在读取影像…'}</p>}</div>{media&&<><input aria-label="帧时间轴" type="range" min="0" max={media.frameCount-1} value={index} onChange={e=>{setPlay(false);setIndex(+e.target.value)}}/><div className="cine-controls"><Button variant="outline" disabled={!index} onClick={()=>{setPlay(false);setIndex(i=>i-1)}}>上一帧</Button><Button onClick={()=>{if(index===media.frameCount-1)setIndex(0);setPlay(!play)}}>{play?'暂停':'播放'}</Button><Button variant="outline" disabled={index===media.frameCount-1} onClick={()=>{setPlay(false);setIndex(i=>i+1)}}>下一帧</Button><span>{index+1} / {media.frameCount} · {(media.frames[index].offsetMs/1000).toFixed(3)} s</span></div><p>{media.width} × {media.height} · {media.codec}{media.original&& (media.original.exists?' · 原件已保存':' · 原件缺失，仅可阅片')}</p></>}</div>
}
type ViewPrediction = {
 view: 'A4C' | 'PSAX' | null
 sampled_frames: number
 probabilities: Record<'A4C' | 'PSAX', number>
}

type TerminalReceipt = {hint:string;sequence:number;ack_ms:number}

type AnalysisJob = {
 id:number
 model_type:string
 state:'pending'|'running'|'succeeded'|'failed'|'interrupted'
 model_version:string
 output:ViewPrediction|null
 error:string|null
 created:string
}

const viewNames = { A4C: '心尖四腔心切面', PSAX: '胸骨旁短轴切面' }

export function ViewAnalysis({id, available, detail}:{id:string;available:boolean;detail:string}){
 const [busy,setBusy]=useState(false),[error,setError]=useState('')
 const [history,setHistory]=useState<AnalysisJob[]>([])
 const [historyError,setHistoryError]=useState('')
 const resultJob=history.find(job=>job.state==='succeeded'&&job.output)
 const result=resultJob?.output
 const [terminalConfigured,setTerminalConfigured]=useState(false)
 const [terminalBusy,setTerminalBusy]=useState(false)
 const [terminalReceipt,setTerminalReceipt]=useState<{jobId:number;value:TerminalReceipt}|null>(null)
 const [terminalError,setTerminalError]=useState('')
 const latest=history[0]
 const [input,setInput]=useState<{id:string;status:InputStatus}|null>(null)
 const [inputError,setInputError]=useState('')
 useEffect(()=>{
  if(!id)return
  let current=true
  setInputError('')
  setHistory([])
  setHistoryError('')
  setTerminalReceipt(null)
  setTerminalError('')
  api<{terminalConfigured:boolean}>('/capabilities').then(capabilities=>{
   if(current)setTerminalConfigured(capabilities.terminalConfigured)
  }).catch(()=>{if(current)setTerminalConfigured(false)})
  api<AnalysisJob[]>('/media/'+id+'/analysis-jobs').then(rows=>{
   if(current)setHistory(rows.filter(job=>job.model_type==='stanford_view'))
  }).catch(e=>{if(current)setHistoryError(e.message)})
  api<Media>('/media/'+id).then(media=>{
   if(current)setInput({id,status:media.modelInputs?.stanford_view ?? {status:'unknown',detail:'影像推理条件未提供。'}})
  }).catch(e=>{if(current)setInputError(e.message)})
  return ()=>{current=false}
 },[id])
 const eligibility=input?.id===id?input.status:null
 async function predict(){
  setBusy(true);setError('');setTerminalError('')
  try{
   const job=await api<AnalysisJob>('/media/'+id+'/analysis-jobs',{})
   setHistory(rows=>[job,...rows.filter(row=>row.id!==job.id)])
  }
  catch(e){setError((e as Error).message)}
  finally{setBusy(false)}
 }
 async function sendToTerminal(jobId:number){
  setTerminalBusy(true);setTerminalError('');setTerminalReceipt(null)
  try{
   const value=await api<TerminalReceipt>('/analysis-jobs/'+jobId+'/terminal',{})
   setTerminalReceipt({jobId,value})
  }
  catch(e){setTerminalError((e as Error).message)}
  finally{setTerminalBusy(false)}
 }
 return <>
  <p>{detail || '当前影像尚无切面推理结果。'}</p>
  {result&&<div className="record" role="status">
   <b>AI 切面结论</b>
   <p>{result.view ? '模型判断这段视频更符合' + viewNames[result.view] + '。' : '模型未达到判定阈值，暂无法确认这段视频的切面。'}</p>
   <span>当前模型仅比较心尖四腔心切面与胸骨旁短轴切面。结果供研究复核。</span>
   <details>
    <summary>查看模型详情</summary>
    <p>抽样 {result.sampled_frames} 帧 · A4C {(result.probabilities.A4C*100).toFixed(1)}% · PSAX {(result.probabilities.PSAX*100).toFixed(1)}%</p>
   </details>
  </div>}
  {resultJob&&terminalConfigured&&<div className="record">
   <Button variant="outline" disabled={terminalBusy||busy} onClick={()=>void sendToTerminal(resultJob.id)}>
    {terminalBusy?'等待终端回执…':'发送切面到终端'}
   </Button>
   {terminalReceipt?.jobId===resultJob.id&&<p role="status">终端已接收 {terminalReceipt.value.hint} · 回执 #{terminalReceipt.value.sequence}</p>}
   {terminalError&&<p role="alert">{terminalError}</p>}
  </div>}
  {historyError&&<p role="alert">读取分析历史失败：{historyError}</p>}
  {id&&<p role="status">分析状态：{busy?'运行中':latest?{pending:'待处理',running:'运行中',succeeded:'成功',failed:'失败',interrupted:'已中断'}[latest.state]:'暂无记录'}</p>}
  {latest?.error&&<p role="alert">{latest.error}</p>}
  {history.length>0&&<details><summary>历史分析（{history.length}）</summary>
   {history.map(job=><div className="record" key={job.id}>
    <p>任务 #{job.id} · {new Date(job.created).toLocaleString()} · {{pending:'待处理',running:'运行中',succeeded:'成功',failed:'失败',interrupted:'已中断'}[job.state]}</p>
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

const labels:Record<string,string>={patients:'患儿管理',exams:'检查管理',media:'影像管理',workspace:'AI影像分析',multi:'多切面分析',experts:'专家协作',reports:'报告中心',settings:'系统管理'}
export default function Modules({page,role}:{page:string;role:string}){
 const [patients,setPatients]=useState<Row[]>([]),[exams,setExams]=useState<Row[]>([]),[cases,setCases]=useState<Row[]>([]),[entries,setEntries]=useState<Row[]>([]),[users,setUsers]=useState<Row[]>([]),[logs,setLogs]=useState<any[]>([])
 const [caseId,setCaseId]=useState(sessionStorage.getItem('echo-exam')||''),[media,setMedia]=useState<string[]>([]),[active,setActive]=useState(''),[compare,setCompare]=useState('')
 const [query,setQuery]=useState(''),[error,setError]=useState(''),[notice,setNotice]=useState(''),[busy,setBusy]=useState(false),[tab,setTab]=useState('切面识别'),[capability,setCapability]=useState('读取中')
 const [modelReady,setModelReady]=useState(false),[modelDetail,setModelDetail]=useState('')
 async function load(){const [p,e,c]=await Promise.all([api<Row[]>('/patients'),api<Row[]>('/examinations'),api<Row[]>('/cases')]);setPatients(p);setExams(e);setCases(c)}
 async function details(id:string){if(!id){setMedia([]);setEntries([]);setActive('');return}const [c,r]=await Promise.all([api<{media:string[]}>('/cases/'+id),api<Row[]>('/cases/'+id+'/entries')]);setMedia(c.media);setEntries(r);setActive(v=>c.media.includes(v)?v:c.media[0]||'');setCompare(c.media[1]||'')}
 async function run(action:()=>Promise<void>){setBusy(true);setError('');setNotice('');try{await action();setNotice('操作已完成')}catch(e){setError((e as Error).message)}finally{setBusy(false)}}
 useEffect(()=>{void run(load);api<{model:string;modelDetail?:string}>('/capabilities').then(c=>{setCapability(c.model==='unavailable'?'模型未接入':c.model);setModelReady(c.model==='Stanford 切面模型');setModelDetail(c.modelDetail||'')}).catch(()=>setCapability('服务未连接'))},[])
 useEffect(()=>{sessionStorage.setItem('echo-exam',caseId);void run(()=>details(caseId))},[caseId])
 useEffect(()=>{if(page==='settings'&&role==='admin')void run(async()=>{setUsers(await api<Row[]>('/users'));setLogs(await api<any[]>('/audit'))})},[page,role])
 function formData(form:HTMLFormElement){return Object.fromEntries(new FormData(form))}
 async function saveEntry(kind:string,form:HTMLFormElement){const data=formData(form);await api('/cases/'+caseId+'/entries',{kind,text:data.text,state:data.state||'saved'});await details(caseId);form.reset()}
 const current=cases.find(c=>String(c.id)===caseId)
 const isWork=page==='workspace'
 return <main className={'dashboard business '+(isWork?'dark-work':'')}><div className="dashboard-title"><div><span className="eyebrow">Echo Edge · 研究工作区</span><h1>{labels[page]}</h1></div><Button variant="outline" disabled={busy} onClick={()=>void run(async()=>{await load();await details(caseId)})}>刷新数据</Button></div>
 {error&&<p role="alert" className="error">{error}</p>}{notice&&<p role="status">{notice}</p>}
 {page==='settings'?<><section className="queue padded"><h2>模型状态</h2>{['切面识别','影像质量','结构分割','异常征象'].map(t=><div className="record" key={t}><b>{t}</b><span>{t==='切面识别'?capability:'模型未接入'}</span></div>)}</section>{role==='admin'?<><section className="queue padded"><h2>用户与角色</h2><form className="business-form" onSubmit={e=>{e.preventDefault();const form=e.currentTarget;void run(async()=>{await api('/users',formData(form));setUsers(await api('/users'));form.reset()})}}><label>用户名<input name="name" required/></label><label>初始密码<input name="password" type="password" required minLength={12} autoComplete="new-password"/></label><label>角色<select name="role"><option value="operator">基层医生</option><option value="reviewer">审核专家</option><option value="admin">管理员</option></select></label><Button disabled={busy}>创建用户</Button></form>{users.map(u=><div className="record" key={u.id}><b>{u.name}</b><select aria-label={'角色 '+u.name} value={u.role} onChange={e=>{const role=e.target.value;if(confirm('确认修改用户角色并使其现有登录失效？'))void run(async()=>{await api('/users/'+u.id+'/role',{role},'PUT');setUsers(await api('/users'))})}}><option value="operator">基层医生</option><option value="reviewer">审核专家</option><option value="admin">管理员</option></select></div>)}</section><section className="queue padded"><h2>操作日志</h2>{logs.map(l=><div className="record" key={l.created+l.action+l.target}>{l.created} · 用户 {l.actor} · {l.action} · {l.target}</div>)}</section></>:<section className="queue padded"><h2>账号权限</h2><p>当前角色 {role}。用户和角色修改、全局操作日志需要管理员账号。</p></section>}</>:<>
 <div className="context-bar"><label>当前检查<select value={caseId} onChange={e=>setCaseId(e.target.value)}><option value="">选择检查 / 既有病例</option>{cases.map(c=><option key={c.id} value={c.id}>#{c.id} {c.title}</option>)}</select></label><span>{current?.status==='draft'?'可导入影像':current?'已提交审核':''}</span></div>
 {!caseId?<section className="queue empty-state"><h2>先选择检查</h2><p>该页面的数据与当前检查关联。</p><Button asChild><a href="#exams">创建检查</a></Button></section>:<>
 {page==='media'&&<section className="queue padded"><h2>导入影像</h2><p>MP4、AVI、MOV、WebM · 单文件不超过 64 MiB · DICOM 需独立解码接入</p><input type="file" multiple accept=".mp4,.avi,.mov,.webm" disabled={busy||current?.status!=='draft'} onChange={e=>{const files=Array.from(e.target.files||[]);e.target.value='';void run(async()=>{for(const file of files){if(file.size>64*1024*1024)throw Error(file.name+' 超过 64 MiB');setNotice('正在上传并解析 '+file.name);const body=new FormData();body.append('media',file);body.append('case_id',caseId);const r=await fetch('/api/v1/media',{method:'POST',body});const data=await r.json();if(!r.ok){await details(caseId);throw Error(file.name+' · '+(data.detail?.message||data.detail))}}await details(caseId)})}}/></section>}
 {(page==='media'||isWork||page==='multi')&&<><div className={isWork?'workbench-grid':'business-columns'}><section className="queue padded series-list"><h2>检查影像 · {media.length}</h2>{media.map((id,i)=><button className={'record '+(id===active?'selected':'')} key={id} onClick={()=>setActive(id)}><img src={'/api/v1/media/'+id+'/frames/0'} alt={'影像缩略图 '+(i+1)}/><b>影像 {i+1}</b><small>解析完成 · 待人工分类</small></button>)}{!media.length&&<p>暂无影像，请先导入。</p>}</section><section className="queue padded">{active?<Player key={active} id={active}/>:<div className="empty-state">当前检查暂无可显示影像</div>}</section>{isWork&&<section className="queue padded"><h2>AI 分析结果</h2><div className="tabs">{['切面识别','影像质量','心脏结构','异常征象'].map(t=><Button key={t} variant={tab===t?'default':'outline'} onClick={()=>setTab(t)}>{t}</Button>)}</div><div className="availability"><b>{tab} · {tab==='切面识别'?capability:'模型未接入'}</b></div>{tab==='切面识别'?<ViewAnalysis key={active} id={active} available={modelReady} detail={modelDetail}/>:<p>此分析项当前没有可用模型。</p>}</section>}</div>{page==='multi'&&<section className="queue padded"><h2>对照影像</h2><select value={compare} onChange={e=>setCompare(e.target.value)}><option value="">选择第二段影像</option>{media.map((id,i)=><option key={id} value={id}>影像 {i+1}</option>)}</select>{active&&compare&&<div className="business-columns"><Player key={'a'+active} id={active}/><Player key={'b'+compare} id={compare}/></div>}<p>资料覆盖情况由医生记录。此页面不生成临床完整性评分。</p></section>}</>}
 {(page==='experts'||page==='reports')&&current&&<ReviewWorkflow key={caseId+current.status} exam={caseId} status={current.status||''} role={role} page={page} consultation={entries.find(r=>r.kind==='consultation')?.state} onChange={load}/>}
 {(isWork||page==='experts'||page==='reports'||page==='multi')&&<section className="queue padded"><h2>{isWork?'医生人工审核':page==='experts'?'专家复核申请与反馈':page==='reports'?'检查报告编辑':'检查资料整理'}</h2>{page==='reports'&&<p>保存为研究报告版本。切面识别结果不会自动纳入报告。</p>}<form className="business-form" onSubmit={e=>{e.preventDefault();const form=e.currentTarget;void run(()=>saveEntry(isWork?'review':page==='experts'?'consultation':page==='reports'?'report':'organization',form))}}><label>{page==='experts'?'申请问题 / 专家意见':'记录内容'}<textarea name="text" required maxLength={12000} placeholder={isWork?'记录人工判断、无法判断的内容和复核备注':''}/></label>{page==='experts'&&<label>协作操作<select name="state"><option value="submitted">提交 / 重新提交申请</option>{role!=='operator'&&<><option value="feedback">专家反馈</option><option value="needs_more">要求补充资料</option></>}<option value="closed">关闭协作</option></select></label>}<Button disabled={busy}>保存记录</Button></form>{page==='reports'&&<Button variant="outline" asChild><a href={'/api/v1/cases/'+caseId+'/report-export'}>导出检查记录 TXT</a></Button>}<h3>历史记录与预览</h3>{entries.filter(r=>r.kind===(isWork?'review':page==='experts'?'consultation':page==='reports'?'report':'organization')).map(r=><article className="record" key={r.id}><b>{r.author} · {r.created} · {r.state}</b><p className="preserve">{r.text}</p></article>)}</section>}
 </>}
 </>}
 </main>
}


