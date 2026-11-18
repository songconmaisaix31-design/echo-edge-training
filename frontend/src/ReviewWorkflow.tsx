import {useEffect,useState} from 'react'
import {Button} from './button'
import {api,apiUrl,errorMessage} from './api'

type Review={id:number;version:number;author:string;created:string;text:string}
type Job={id:number;state:string;model_type:string;model_version:string}
type Report={id:number;version:number;review:Review;author:string;created:string;text:string;analysis:Job[]}

export default function ReviewWorkflow({exam,status,role,page,consultation,onChange}:{
 exam:string;status:string;role:string;page:string;consultation:string|undefined;onChange:()=>Promise<void>
}){
 const [reviews,setReviews]=useState<Review[]>([])
 const [reports,setReports]=useState<Report[]>([])
 const [jobs,setJobs]=useState<Job[]>([])
 const [error,setError]=useState('')
 const [busy,setBusy]=useState(false)
 const [revision,refresh]=useState(0)
 const allowed=role==='reviewer'||role==='admin'
 const base='/examinations/'+exam
 useEffect(()=>{
  let active=true
  Promise.all([api<Review[]>(base+'/formal-reviews'),
   page==='reports'?api<Report[]>(base+'/report-versions'):Promise.resolve([]),
   page==='reports'?api<Job[]>(base+'/analysis-jobs'):Promise.resolve([])
  ]).then(([r,v,j])=>{if(active){setReviews(r);setReports(v);setJobs(j)}})
  .catch(e=>{if(active)setError(errorMessage(e))})
  return ()=>{active=false}
 },[base,page,revision])
 async function save(action:()=>Promise<unknown>){
  setBusy(true);setError('')
  try{await action();refresh(v=>v+1);await onChange()}
  catch(e){setError(errorMessage(e))}
  finally{setBusy(false)}
 }
 return <section className="queue padded">
  <h2>{page==='experts'?'正式检查审核':'签发研究报告'}</h2>
  <p>检查：{{draft:'草稿',submitted:'待正式审核',reviewed:'已正式审核'}[status]||status} · 专家咨询：{{submitted:'待反馈',feedback:'已反馈',needs_more:'待补充',closed:'已关闭'}[consultation||'']||'未发起'}</p>
  {error&&<p role="alert">{error}</p>}
  {page==='experts'?<>
   <p>咨询意见与正式审核分别保存。</p>
   {status==='draft'&&role!=='reviewer'&&<Button disabled={busy} onClick={()=>void save(()=>api('/cases/'+exam+'/submit',{}))}>提交正式审核</Button>}
   {allowed&&status!=='draft'&&<form className="business-form" onSubmit={e=>{
    e.preventDefault();const form=e.currentTarget
    const data=new FormData(form)
    void save(async()=>{await api('/cases/'+exam+'/reviews',{text:data.get('text')});form.reset()})
   }}><label>正式审核结论<textarea name="text" required maxLength={8000}/></label>
    <Button disabled={busy}>保存正式审核版本</Button></form>}
   {reviews.map(r=><article className="record" key={r.id}><b>审核版本 #{r.version} · {r.author} · {r.created}</b><p className="preserve">{r.text}</p></article>)}
  </>:<>
   {allowed&&status==='reviewed'&&reviews.length>0?<form className="business-form" onSubmit={e=>{
    e.preventDefault();const form=e.currentTarget
    const data=new FormData(form)
    void save(async()=>{await api(base+'/report-versions',{review_id:Number(data.get('review')),text:data.get('text'),analysis_ids:data.getAll('job').map(Number)});form.reset()})
   }}>
    <label>引用审核版本<select name="review" required>{reviews.map(r=><option value={r.id} key={r.id}>#{r.version} · {r.author} · {r.created}</option>)}</select></label>
    <label>签发正文<textarea name="text" required maxLength={12000}/></label>
    {jobs.filter(j=>j.state==='succeeded'&&j.model_type==='stanford_view').map(j=><label key={j.id}><input type="checkbox" name="job" value={j.id}/> 引用切面任务 #{j.id}</label>)}
    <Button disabled={busy}>签发研究报告版本</Button>
   </form>:<p>签发需要已完成的正式审核版本和审核账号。下方仍可依权限保存研究草稿。</p>}
   {reports.map(r=><article className="record" key={r.id}>
    <b>签发版本 #{r.version} · 审核 #{r.review.version} · {r.author}</b>
    <p className="preserve">{r.text}</p>
    <p>引用切面任务：{r.analysis.map(j=>'#'+j.id).join('、')||'无'}</p>
    <Button variant="outline" asChild><a href={apiUrl(base+'/report-versions/'+r.id+'/export')}>导出签发版本</a></Button>
   </article>)}
  </>}
 </section>
}
