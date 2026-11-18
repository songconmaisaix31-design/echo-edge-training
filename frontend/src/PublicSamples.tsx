import {useEffect, useState} from 'react'
import {publicSampleCatalog} from './api'
import {Button} from './button'
import {Player,ViewAnalysis} from './MediaAnalysis'
import StructureEvidence from './StructureEvidence'
import {useCapabilities} from './use-capabilities'
import './public-samples.css'

type Field = {label:string; value:string}
type Video = {id:number; frames:number}
type Exam = {id:number; kind:string; fields:Field[]; videos:Video[]}
type Archive = {id:number; name:string; fields:Field[]; exams:Exam[]}
type CatalogState = {status:'loading'} | {status:'unavailable'} | {status:'ready'; archives:Archive[]}

function isRecord(value:unknown):value is Record<string,unknown>{
 return typeof value==='object'&&value!==null&&!Array.isArray(value)
}

function isId(value:unknown):value is number{
 return typeof value==='number'&&Number.isSafeInteger(value)&&value>0
}

function isField(value:unknown):value is Field{
 return isRecord(value)&&typeof value.label==='string'&&typeof value.value==='string'
}

function isVideo(value:unknown):value is Video{
 return isRecord(value)&&isId(value.id)&&value.id<=5&&isId(value.frames)
}

function isExam(value:unknown):value is Exam{
 return isRecord(value)&&isId(value.id)&&typeof value.kind==='string'
  &&Array.isArray(value.fields)&&value.fields.every(isField)&&Array.isArray(value.videos)&&value.videos.every(isVideo)
}

function isArchive(value:unknown):value is Archive{
 return isRecord(value)&&isId(value.id)&&typeof value.name==='string'&&Array.isArray(value.fields)&&value.fields.every(isField)
  &&Array.isArray(value.exams)&&value.exams.every(isExam)
}

function readCatalog(value:unknown):Archive[]{
 if(!isRecord(value)||!Array.isArray(value.archives)||!value.archives.every(isArchive))throw new Error('无法读取样例目录')
 const archives=value.archives
 const exams=archives.flatMap(archive=>archive.exams)
 const videos=exams.flatMap(exam=>exam.videos)
 if(archives.length!==4||exams.length!==4||videos.length!==5||new Set(videos.map(video=>video.id)).size!==5){
  throw new Error('样例目录不完整')
 }
 return archives
}

function ArchiveFields({fields}:{fields:Field[]}){
 if(fields.length===0)return null
 return <dl className="public-sample-fields">{fields.map(field=><div key={field.label}>
  <dt>{field.label}</dt><dd>{field.value}</dd>
 </div>)}</dl>
}

function SampleAnalysis({route,archives}:{route:string;archives:Archive[]}){
 const [section='workspace',requested='1',tab]=route.split('/')
 const options=archives.flatMap(archive=>archive.exams.flatMap(exam=>exam.videos.map(video=>({...video,archive:archive.name}))))
 const id=options.some(video=>String(video.id)===requested)?requested:'1'
 const structure=tab==='structure'
 const routeFor=(video:string,showStructure:boolean)=>`#${section}/${video}${showStructure?'/structure':''}`
 const status=useCapabilities()
 const models=status==='disconnected'?null:status
 const configured=models?.model==='Stanford 切面模型'
 const title=section==='media'?'影像管理':section==='diagnosis'?'AI 辅助诊断':'AI影像分析'
 return <main className="dashboard business diagnosis">
  <div className="dashboard-title"><div><h1>{title}</h1><p>选择公开样例，运行研究模型。</p></div></div>
  <div className="diagnosis-layout">
   <section className="queue padded" aria-label="检查影像">
    <div className="diagnosis-toolbar"><h2>超声影像</h2><label>选择影像 <select value={id} onChange={event=>{location.hash=routeFor(event.target.value,structure)}}>
     {options.map(video=><option key={video.id} value={video.id}>超声视频 {video.id} · {video.archive}</option>)}
    </select></label></div>
    <Player key={id} id={id} scope="samples"/>
   </section>
   <section className="queue padded" aria-label="模型分析">
    <div className="tabs">
     <Button variant={structure?'outline':'default'} aria-pressed={!structure} onClick={()=>{location.hash=routeFor(id,false)}}>切面识别</Button>
     <Button variant={structure?'default':'outline'} aria-pressed={structure} onClick={()=>{location.hash=routeFor(id,true)}}>四腔结构分割</Button>
    </div>
    <div className="diagnosis-panel">
     {structure?<StructureEvidence key={id} id={id} configured={models?.structureConfigured??null} scope="samples"/>:
      <ViewAnalysis key={id} id={id} available={configured} detail={models?.modelDetail??(status==='disconnected'?'无法读取模型状态。':'正在读取模型状态…')} scope="samples"/>}
    </div>
   </section>
  </div>
 </main>
}

export default function PublicSamples({route}:{route:string}){
 const [catalog,setCatalog]=useState<CatalogState>({status:'loading'})
 useEffect(()=>{
  const request=new AbortController()
  void publicSampleCatalog(request.signal).then(value=>{
   const archives=readCatalog(value)
   if(!request.signal.aborted)setCatalog({status:'ready',archives})
  }).catch(()=>{if(!request.signal.aborted)setCatalog({status:'unavailable'})})
  return ()=>request.abort()
 },[])
 const [section='home']=route.split('/')
 if(['media','workspace','diagnosis'].includes(section)&&catalog.status==='ready')return <SampleAnalysis route={route} archives={catalog.archives}/>
 const title=section==='patients'?'患儿管理':section==='exams'?'检查管理':'临床工作台'
 return <main className="dashboard">
  <div className="dashboard-title">
   <div><h1>{title}</h1>{catalog.status==='ready'&&<p>4 份脱敏档案 · 4 条检查 · 5 段超声视频</p>}</div>
   <Button asChild><a href="#workspace">进入影像工作站</a></Button>
  </div>
  {catalog.status==='loading'&&<p role="status">正在读取样例…</p>}
  {catalog.status==='unavailable'&&<p role="alert">暂时无法读取样例。<button type="button" onClick={()=>location.reload()}>重新加载</button></p>}
  {catalog.status==='ready'&&<div className="public-sample-list"><h2>公开样例</h2>{catalog.archives.map(archive=><details key={archive.id} className="queue padded">
   <summary>{archive.name}</summary>
   <ArchiveFields fields={archive.fields}/>
   {archive.exams.length===0&&<p>此档案没有关联检查。</p>}
   {archive.exams.map(exam=><section className="public-sample-exam" key={exam.id}>
    <h2>{exam.kind}</h2>
    <ArchiveFields fields={exam.fields}/>
    <div className="public-sample-videos">{exam.videos.map(video=><figure key={video.id}>
     <figcaption>超声视频 {video.id} · {video.frames} 帧</figcaption>
     <video aria-label={`超声视频 ${video.id}`} controls muted playsInline preload="none" src={`/samples/video-${video.id}.mp4`}/>
     <a href={`#workspace/${video.id}`}>分析视频 {video.id}</a>
    </figure>)}</div>
   </section>)}
  </details>)}</div>}
 </main>
}
