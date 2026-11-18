import {useEffect, useState} from 'react'
import {Activity} from 'lucide-react'
import {publicSampleCatalog} from './api'
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

export default function PublicSamples(){
 const [catalog,setCatalog]=useState<CatalogState>({status:'loading'})
 useEffect(()=>{
  const request=new AbortController()
  void publicSampleCatalog(request.signal).then(value=>{
   const archives=readCatalog(value)
   if(!request.signal.aborted)setCatalog({status:'ready',archives})
  }).catch(()=>{if(!request.signal.aborted)setCatalog({status:'unavailable'})})
  return ()=>request.abort()
 },[])
 return <main className="dashboard public-samples">
  <header className="public-sample-heading">
   <span className="wordmark"><Activity aria-hidden="true"/> Echo Edge</span>
   <h1>公开样例</h1>
   {catalog.status==='ready'&&<p>4 份脱敏档案 · 4 条检查 · 5 段超声视频</p>}
  </header>
  {catalog.status==='loading'&&<p role="status">正在读取样例…</p>}
  {catalog.status==='unavailable'&&<p role="alert">暂时无法读取样例。<button type="button" onClick={()=>location.reload()}>重新加载</button></p>}
  {catalog.status==='ready'&&<div className="public-sample-list">{catalog.archives.map(archive=><details key={archive.id} className="queue padded">
   <summary>{archive.name}</summary>
   <ArchiveFields fields={archive.fields}/>
   {archive.exams.length===0&&<p>此档案没有关联检查。</p>}
   {archive.exams.map(exam=><section className="public-sample-exam" key={exam.id}>
    <h2>{exam.kind}</h2>
    <ArchiveFields fields={exam.fields}/>
    <div className="public-sample-videos">{exam.videos.map(video=><figure key={video.id}>
     <figcaption>超声视频 {video.id} · {video.frames} 帧</figcaption>
     <video aria-label={`超声视频 ${video.id}`} controls muted playsInline preload="none" src={`/samples/video-${video.id}.mp4`}/>
    </figure>)}</div>
   </section>)}
  </details>)}</div>}
 </main>
}
