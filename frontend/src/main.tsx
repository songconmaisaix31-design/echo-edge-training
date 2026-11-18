import {useEffect,useState} from 'react'
import {createRoot} from 'react-dom/client'
import {Activity,LayoutDashboard,Users,ClipboardList,Film,ScanLine,Layers,MessagesSquare,FileText,Settings,ArrowUpRight,Search,LogOut,RefreshCw} from 'lucide-react'
import {Button} from './button'
import Modules from './Modules'
import Exams from './Exams'
import Patients from './Patients'
import {apiRequest,errorMessage,loginSession,logoutSession} from './api'
import {structureEvidenceHref} from './diagnosis-contract'
import {useCapabilities} from './use-capabilities'
import './style.css'
import './shell.css'
type User={name:string;role:string}
type Case={id:number;title:string;notes:string;status:string}
import Diagnosis from './Diagnosis'
const navigation=[['home','临床工作台',LayoutDashboard],['patients','患儿管理',Users],['exams','检查管理',ClipboardList],['media','影像管理',Film],['workspace','AI影像分析',ScanLine],['multi','多切面分析',Layers],['diagnosis','AI 辅助诊断',ScanLine],['experts','专家协作',MessagesSquare],['reports','报告中心',FileText],['settings','系统管理',Settings]] as const
function ModelAvailability(){
 const status=useCapabilities()
 const models=status&&status!=='disconnected'?[status.model!=='unavailable'?status.model:'',status.structureConfigured?'CardiacUDC 四腔分割':'',status.reviewConfigured?status.reviewModel+' 影像质量与异常征象复核':''].filter(Boolean):[]
 const heading=status==='disconnected'?'无法读取模型状态':!status?'正在读取模型状态':models.length?'已接入的 AI 模型':'AI 模型未接入'
 return <div className="availability"><ScanLine size={20}/><div><b>{heading}</b>{models.length>0&&<p>{models.join(' · ')}</p>}{status&&status!=='disconnected'&&status.structureConfigured&&<a href={structureEvidenceHref('')}>打开四腔分割</a>}</div></div>
}
function App(){
 const [user,setUser]=useState<User|null>(null)
 const [loading,setLoading]=useState(true)
 const [busy,setBusy]=useState(false)
 const [signingOut,setSigningOut]=useState(false)
 const [error,setError]=useState('')
 const [page,setPage]=useState(location.hash.slice(1)||'home')
 const [cases,setCases]=useState<Case[]>([])
 const [search,setSearch]=useState('')
 const [status,setStatus]=useState('')
 async function load(){
  setLoading(true);setError('')
  try{setCases(await apiRequest<Case[]>('/cases'))}catch(e){setError(errorMessage(e))}
  finally{setLoading(false)}
 }
 useEffect(()=>{const onHash=()=>setPage(location.hash.slice(1)||'home');addEventListener('hashchange',onHash);return()=>removeEventListener('hashchange',onHash)},[])
 useEffect(()=>{void apiRequest<User>('/me').then(setUser).catch(()=>{}).finally(()=>setLoading(false))},[])
 useEffect(()=>{if(user)void load()},[user])
 useEffect(()=>{
  if(!signingOut)return
  void logoutSession().then(()=>{
   setUser(null)
   setCases([])
   setError('')
  }).catch(error=>setError(errorMessage(error))).finally(()=>setSigningOut(false))
 },[signingOut])
 async function login(event:React.FormEvent<HTMLFormElement>){
  event.preventDefault();setBusy(true);setError('')
  const form=new FormData(event.currentTarget)
  try{setUser(await loginSession<User>(Object.fromEntries(form)))}
  catch(e){setError(errorMessage(e))}finally{setBusy(false)}
 }
 if(signingOut)return <main role="status">正在退出登录…</main>
 if(!user)return <div className="auth-layout"><section className="auth-brand"><div className="wordmark"><Activity/> Echo Edge</div><div><span className="eyebrow">儿童心脏超声 · 研究工作区</span><h1>让每一段影像<br/>得到细致审阅。</h1><p>连接影像、分析与专家复核的专业工作空间。</p></div><small>内部研究与验证</small></section><section className="auth-form"><form onSubmit={login}><span className="eyebrow">欢迎回来</span><h2>登录工作区</h2><p>使用由管理员开通的账号继续。</p><label>账号<input name="name" required autoComplete="username" placeholder="请输入账号"/></label><label>密码<input name="password" type="password" required autoComplete="current-password" placeholder="请输入密码"/></label>{error&&<div role="alert" className="error">{error}</div>}<Button disabled={busy||loading}>{busy?'正在登录…':'登录'}<ArrowUpRight size={16}/></Button><details><summary>首次使用，如何创建账号？</summary><p>请在本机运行已提供的 Create-Operator.cmd，设置密码后使用 operator 登录。其他账号由管理员开通。</p></details></form></section></div>
 const visible=cases.filter(c=>(c.title.includes(search)||String(c.id).includes(search))&&(!status||c.status===status))
 const selected=navigation.find(n=>n[0]===page.split('/')[0])
 return <div className="app-shell"><aside className="sidebar"><a className="wordmark" href="#home"><Activity/> Echo Edge</a><div className="workspace-label">研究工作区</div><nav>{navigation.map(([id,name,Icon])=><a key={id} href={'#'+id} aria-current={page.split('/')[0]===id?'page':undefined}><Icon size={18}/>{name}</a>)}</nav><div className="sidebar-foot"><span className="status-dot"/> 本地工作环境</div></aside><div className="shell-body"><header className="shell-header"><span>{selected?.[1]||'页面不存在'}</span><div className="user-area"><span className="avatar">{user.name.slice(0,1).toUpperCase()}</span>{user.name}<Button variant="ghost" aria-label="退出登录" onClick={()=>setSigningOut(true)}><LogOut size={17}/></Button></div></header>
 {page==='home'?<main className="dashboard"><div className="dashboard-title"><div><span className="eyebrow">工作概览</span><h1>临床工作台</h1><p>从病例开始，继续影像阅览与人工复核。</p></div><Button asChild><a href="#workspace">进入影像工作站<ArrowUpRight size={16}/></a></Button></div><div className="overview-grid">{[['可访问病例',cases.length],['待人工审核',cases.filter(c=>c.status==='submitted').length],['已完成审核',cases.filter(c=>c.status==='reviewed').length]].map(([label,value])=><section className="metric" key={label}><span>{label}</span><strong>{loading?'—':value}</strong><small>当前账号可访问的最近 200 条</small></section>)}</div><section className="queue"><div className="queue-title"><div><h2>病例队列</h2><p>选择工作站中的病例，继续处理影像。</p></div><Button variant="ghost" aria-label="刷新病例" disabled={loading} onClick={()=>void load()}><RefreshCw size={16}/></Button></div><div className="queue-filters"><label><Search size={17}/><input aria-label="搜索病例" placeholder="搜索编号或病例名称" value={search} onChange={e=>setSearch(e.target.value)}/></label><select aria-label="病例状态" value={status} onChange={e=>setStatus(e.target.value)}><option value="">全部状态</option><option value="draft">草稿</option><option value="submitted">待审核</option><option value="reviewed">已审核</option></select></div>{error&&<p role="alert" className="error">{error}</p>}<table><thead><tr><th>病例编号</th><th>病例名称</th><th>处理状态</th><th>备注</th></tr></thead><tbody>{visible.map(c=><tr key={c.id}><td className="mono">#{c.id}</td><td>{c.title}</td><td><span className={'state '+c.status}>{{draft:'草稿',submitted:'待审核',reviewed:'已审核'}[c.status]||c.status}</span></td><td>{c.notes||'—'}</td></tr>)}</tbody></table>{!visible.length&&<div className="empty-state"><ClipboardList size={30}/><h3>{loading?'正在读取病例':'暂无匹配病例'}</h3><p>在影像工作站创建病例后，会自动出现在这里。</p></div>}</section><ModelAvailability/></main>:page.split('/')[0]==='patients'?<Patients key={page} route={page}/>:page.split('/')[0]==='exams'?<Exams key={page} route={page}/>:page.split('/')[0]==='diagnosis'?<Diagnosis key={page} route={page}/>:<Modules key={page} page={page} role={user.role}/>} </div></div>
}
const root=document.getElementById('root')
if(!root)throw new Error('找不到应用根节点')
createRoot(root).render(<App/>)



