import {LayoutDashboard,Users,ScanLine,FileText,Settings} from 'lucide-react'

const groups=[
 {id:'home',label:'工作台',icon:LayoutDashboard,pages:[['home','工作概览']]},
 {id:'patients',label:'患儿档案',icon:Users,pages:[['patients','患儿信息'],['exams','检查记录'],['media','影像资料']]},
 {id:'workspace',label:'分析工作站',icon:ScanLine,pages:[['workspace','影像阅览与分析'],['multi','多切面评估'],['diagnosis','辅助诊断']]},
 {id:'reports',label:'报告与协作',icon:FileText,pages:[['reports','检查报告'],['experts','专家复核']]},
 {id:'settings',label:'设置',icon:Settings,pages:[['settings','系统设置']]},
] as const
const demoPages=new Set(['home','patients','exams','media','workspace','diagnosis'])

export function workspaceNavigation(page:string,demo:boolean){
 const active=page.split('/')[0]
 const available=groups.map(group=>({...group,pages:group.pages.filter(([id])=>!demo||demoPages.has(id))})).filter(group=>group.pages.length>0)
 return {active,groups:available,selected:available.find(group=>group.pages.some(([id])=>id===active))}
}

export function WorkspaceSidebar({page,demo=false}:{page:string;demo?:boolean}){
 const navigation=workspaceNavigation(page,demo)
 return <nav aria-label="主导航">{navigation.groups.map(({id,label,icon:Icon,pages})=><a key={id} href={'#'+(pages[0]?.[0]??id)} aria-current={navigation.selected?.id===id?'page':undefined}><Icon size={18}/>{label}</a>)}</nav>
}

export function WorkspaceSections({page,demo=false}:{page:string;demo?:boolean}){
 const navigation=workspaceNavigation(page,demo)
 if(!navigation.selected||navigation.selected.pages.length<2)return null
 return <nav className="workspace-sections" aria-label={navigation.selected.label+'内导航'}>{navigation.selected.pages.map(([id,label])=><a key={id} href={'#'+id} aria-current={navigation.active===id?'page':undefined}>{label}</a>)}</nav>
}
