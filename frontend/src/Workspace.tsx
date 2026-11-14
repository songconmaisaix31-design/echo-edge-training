import {useEffect,useRef,useState} from 'react'

import {Activity,Upload,Play,Pause,SkipBack,SkipForward,ScanLine,Info} from 'lucide-react'
import {Button} from './button'
import './style.css'

type Media={id:string;width:number;height:number;codec:string;frameCount:number;sizeBytes:number;frames:{index:number;offsetMs:number}[]}
type CaseRow={id:number;title:string;notes:string;status:string}
type Detail=CaseRow&{media:string[];reviews:{text:string;author:string;created:string}[]}
async function api<T>(path:string,method='GET',body?:unknown):Promise<T>{
 const response=await fetch('/api/v1'+path,{method,headers:body?{'Content-Type':'application/json'}:undefined,body:body?JSON.stringify(body):undefined})
 const data=await response.json()
 if(!response.ok)throw new Error(typeof data.detail==='string'?data.detail:'请求失败')
 return data
}
export default function Workspace(){
 const [user,setUser]=useState<{name:string;role:string}|null>(null)
 const [cases,setCases]=useState<CaseRow[]>([])
 const [selected,setSelected]=useState<Detail|null>(null)
 const [query,setQuery]=useState('')
 const [status,setStatus]=useState('')
 async function refresh(){setCases(await api<CaseRow[]>('/cases?'+new URLSearchParams({q:query,status})))}
 async function selectCase(id:number){
  const item=await api<Detail>('/cases/'+id)
  setSelected(item);setPlaying(false);setIndex(0)
  setMedia(item.media.length?await api<Media>('/media/'+item.media[item.media.length-1]):null)
 }
 async function act(action:()=>Promise<void>){
  setError('')
  try{await action()}catch(e){setError(e instanceof Error?e.message:'请求失败')}
 }
 useEffect(()=>{void api<{name:string;role:string}>('/me').then(setUser).catch(()=>{})},[])
 useEffect(()=>{if(user)void act(refresh)},[user,query,status])
 const [media,setMedia]=useState<Media|null>(null)
 const [index,setIndex]=useState(0)
 const [playing,setPlaying]=useState(false)
 const [busy,setBusy]=useState(false)
 const [error,setError]=useState('')
 const input=useRef<HTMLInputElement>(null)
 const frame=media?.frames[index]
 useEffect(()=>{
  if(!playing||!media)return
  if(index>=media.frameCount-1){setPlaying(false);return}
  const delay=media.frames[index+1].offsetMs-media.frames[index].offsetMs
  const timer=setTimeout(()=>setIndex(index+1),Math.max(1,delay))
  return ()=>clearTimeout(timer)
 },[media,index,playing])
 async function upload(file:File){
  setBusy(true);setPlaying(false);setMedia(null);setIndex(0);setError('')
  try{
   if(file.size>64*1024*1024)throw new Error('文件不得超过 64 MiB。')
   const body=new FormData()
   body.append('media',file);body.append('case_id',String(selected!.id))
   const response=await fetch('/api/v1/media',{method:'POST',body})
   const payload=await response.json()
   if(!response.ok)throw new Error(payload.detail?.message||'解析失败。')
   setMedia(payload);setSelected(current=>current?{...current,media:[...current.media,payload.id]}:current)
  }catch(reason){setError(reason instanceof Error?reason.message:'解析失败。')}
  finally{setBusy(false)}
 }
 function seek(next:number){setPlaying(false);setIndex(next)}
 if(!user)return <main><section className="panel mx-auto mt-20 max-w-md"><div className="panel-head">Echo Edge · 登录</div><form className="analysis grid gap-4" onSubmit={e=>{
  e.preventDefault();const form=new FormData(e.currentTarget)
  void act(async()=>setUser(await api('/login','POST',{name:form.get('name'),password:form.get('password')})))
 }}><label>账户<input className="field" name="name" required autoComplete="username"/></label><label>密码<input className="field" type="password" name="password" required autoComplete="current-password"/></label><Button>登录</Button>{error&&<p role="alert" className="error">{error}</p>}</form></section></main>
 return <>
 <header className="top"><div className="brand"><span className="brand-icon"><Activity size={22}/></span><b>Echo Edge</b></div><div className="top-current"><ScanLine size={17}/>影像分析</div><span className="top-note">{user.name} · {user.role} <Button variant="outline" onClick={()=>void act(async()=>{await api("/logout","POST");setUser(null);setSelected(null);setMedia(null);setPlaying(false);setCases([])})}>退出</Button></span></header>
 <main><div className="heading"><div><h1>影像分析工作台</h1><p>导入超声视频并查看解码后的源帧</p></div><Button disabled={busy||selected?.status!=='draft'} onClick={()=>input.current?.click()}><Upload size={16}/>{busy?'正在解析…':'导入视频'}</Button><input hidden type="file" ref={input} accept=".mp4,.mov,.avi,.webm,.dcm" onChange={e=>{const file=e.target.files?.[0];if(file)void upload(file);e.target.value=''}}/></div>
 <section className="panel mb-4"><div className="panel-head">病例管理</div><div className="analysis grid gap-3">
 <div className="flex flex-wrap gap-2"><input className="field" placeholder="搜索病例" value={query} onChange={e=>setQuery(e.target.value)}/><select className="field" value={status} onChange={e=>setStatus(e.target.value)}><option value="">全部状态</option><option value="draft">草稿</option><option value="submitted">待审核</option><option value="reviewed">已审核</option></select></div>
 <form className="flex flex-wrap gap-2" onSubmit={e=>{e.preventDefault();const form=e.currentTarget;const title=new FormData(form).get('title');void act(async()=>{const row=await api<CaseRow>('/cases','POST',{title});form.reset();await refresh();await selectCase(row.id)})}}><input className="field" name="title" placeholder="新病例名称" required maxLength={120}/><Button>创建病例</Button></form>
 <div className="flex flex-wrap gap-2">{cases.map(row=><Button key={row.id} variant={selected?.id===row.id?'default':'outline'} onClick={()=>void act(()=>selectCase(row.id))}>{row.title} · {row.status}</Button>)}</div>
 {selected&&<div className="grid gap-3" key={selected.id}>
 <form className="flex flex-wrap gap-2" onSubmit={e=>{e.preventDefault();const data=new FormData(e.currentTarget);void act(async()=>{await api('/cases/'+selected.id,'PUT',{title:data.get('title'),notes:data.get('notes')});await refresh();await selectCase(selected.id)})}}><input className="field" aria-label="病例名称" name="title" defaultValue={selected.title} required/><input className="field" aria-label="病例备注" name="notes" defaultValue={selected.notes}/><Button variant="outline">保存病例</Button></form>
 <div className="flex flex-wrap gap-2">{selected.media.map((id,i)=><Button key={id} variant="outline" onClick={()=>void act(async()=>{setPlaying(false);setIndex(0);setMedia(await api('/media/'+id))})}>影像 {i+1}</Button>)}
 <Button variant="outline" disabled={selected.status!=='draft'||!selected.media.length} onClick={()=>void act(async()=>{await api('/cases/'+selected.id+'/submit','POST');await refresh();await selectCase(selected.id)})}>提交审核</Button><Button variant="outline" asChild><a href={'/api/v1/cases/'+selected.id+'/report'}>导出报告</a></Button></div>
 {selected.reviews.map((r,i)=><p key={i} className="text-sm">{r.author} · {r.created} · {r.text}</p>)}
 {selected.status!=='draft'&&user.role!=='operator'&&<form className="flex gap-2" onSubmit={e=>{e.preventDefault();const form=e.currentTarget;const text=new FormData(form).get('text');void act(async()=>{await api('/cases/'+selected.id+'/reviews','POST',{text});form.reset();await refresh();await selectCase(selected.id)})}}><input className="field" name="text" placeholder="审核意见" required/><Button>保存审核意见</Button></form>}
 </div>}</div></section>
 <div className="notice"><Info size={17}/>内部研究验证。仅导入合成或获准使用的非患者测试影像。</div>
 {error&&<p className="error" role="alert">{error}</p>}
 <div className="layout"><section className="panel"><div className="panel-head"><span>影像阅览</span><span className="pill">{busy?'解析中':media?'已解析':'未导入'}</span></div>
 <div className="screen">{media&&frame?<img className="absolute inset-0 h-full w-full object-contain" src={`/api/v1/media/${media.id}/frames/${frame.index}`} alt={`源帧 ${index+1}`}/>:<span className="text-slate-400">{busy?'正在解码视频帧':'请选择视频文件'}</span>}</div>
 <div className="transport"><div className="timeline"><span>{((frame?.offsetMs??0)/1000).toFixed(3)} s</span><input type="range" min={0} max={Math.max(1,(media?.frameCount??1)-1)} value={index} disabled={!media||media.frameCount<2} aria-label="源帧时间轴" onChange={e=>seek(+e.target.value)}/></div>
 <div className="controls"><div className="playback"><Button variant="ghost" disabled={!media||index===0} aria-label="上一帧" onClick={()=>seek(index-1)}><SkipBack size={17}/></Button><Button disabled={!media||media.frameCount<2} aria-label={playing?'暂停':'播放'} onClick={()=>{if(!playing&&index===media!.frameCount-1)setIndex(0);setPlaying(!playing)}}>{playing?<Pause size={17}/>:<Play size={17}/>}</Button><Button variant="ghost" disabled={!media||index>=media.frameCount-1} aria-label="下一帧" onClick={()=>seek(index+1)}><SkipForward size={17}/></Button></div>{media&&<span>第 {index+1} / {media.frameCount} 帧</span>}</div></div></section>
 <aside className="side"><section className="panel"><div className="panel-head"><span>解析结果</span></div><div className="analysis">{media?<dl className="space-y-2 text-sm"><div>尺寸　{media.width} × {media.height}</div><div>编码　{media.codec}</div><div>帧数　{media.frameCount}</div><div>文件　{media.sizeBytes.toLocaleString()} 字节</div></dl>:<p className="empty">导入后显示实际视频属性。</p>}</div></section><section className="panel"><div className="panel-head"><span>切面识别</span><span className="pill">模型未部署</span></div><div className="analysis"><Button disabled>运行分析</Button><p className="empty mt-3">切面识别、结构分割、采集质量和异常分析均未运行。</p></div></section></aside></div></main></>
}



