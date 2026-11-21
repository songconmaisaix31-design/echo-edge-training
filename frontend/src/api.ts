import type {InsufficientEvidence} from './analysis-api.generated'
import type {AnalysisJob} from './diagnosis-contract'

export type Capabilities={model:string;modelDetail?:string;structureConfigured:boolean;reviewConfigured?:boolean;reviewModel?:string|null;reviewDetail?:string;reviewSampleOnly?:boolean;reviewMaxFrames?:number|null}

const API_BASE='/api/v1'
const REQUEST_FAILED='请求失败，请检查输入'
const INVALID_RESPONSE='服务返回了无法识别的响应'
let sessionRequests=new AbortController()

export function apiUrl(path:string):string{
 return API_BASE+path
}

export type MediaScope='private'|'samples'

export function mediaApiPath(id:string,scope:MediaScope):string{
 return (scope==='samples'?'/public-samples/media/':'/media/')+id
}

export function isRecord(value:unknown):value is Record<string,unknown>{
 return typeof value==='object'&&value!==null&&!Array.isArray(value)
}

export function errorMessage(error:unknown):string{
 return error instanceof Error ? error.message : REQUEST_FAILED
}

function responseErrorMessage(body:unknown):string{
 if(!isRecord(body))return REQUEST_FAILED
 const {detail}=body
 if(typeof detail==='string'&&detail)return detail
 if(Array.isArray(detail)){
  const messages=detail.flatMap((item:unknown):string[]=>{
   if(!isRecord(item)||typeof item.msg!=='string')return []
   const loc=Array.isArray(item.loc)?item.loc.map(String).join('.')+'：':''
   return [loc+item.msg]
  })
  if(messages.length)return messages.join('；')
 }
 if(isRecord(detail)&&typeof detail.message==='string'&&detail.message)return detail.message
 return REQUEST_FAILED
}

async function sendRequest<T>(url:string,init?:RequestInit):Promise<T>{
 const response=await fetch(url,init)
 const text=await response.text()
 let body:unknown=undefined
 if(text){
  try{body=JSON.parse(text)}
  catch{if(response.ok)throw new Error(INVALID_RESPONSE)}
 }
 if(!response.ok)throw new Error(responseErrorMessage(body))
 return body as T
}

export function apiRequest<T=unknown>(path:string,init?:RequestInit):Promise<T>{
 const signal=init?.signal
  ? AbortSignal.any([sessionRequests.signal,init.signal])
  : sessionRequests.signal
 return sendRequest<T>(apiUrl(path),{...init,signal})
}

export function publicSampleCatalog(signal:AbortSignal):Promise<unknown>{
 return sendRequest('/samples/catalog.json',{credentials:'omit',signal})
}

function jsonRequest(body:unknown,method:string):RequestInit{
 return {method,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}
}

export async function loginSession<T>(credentials:unknown):Promise<T>{
 const user=await sendRequest<T>(apiUrl('/login'),jsonRequest(credentials,'POST'))
 sessionRequests=new AbortController()
 return user
}

export async function logoutSession():Promise<void>{
 sessionRequests.abort()
 try{await sendRequest(apiUrl('/logout'),{method:'POST'})}
 catch(error){
  sessionRequests=new AbortController()
  throw error
 }
}

export async function api<T>(path:string,body?:unknown,method='POST'):Promise<T>{
 return apiRequest<T>(path,body===undefined?undefined:jsonRequest(body,method))
}

function isInsufficientEvidence(value:unknown):value is InsufficientEvidence{
 return isRecord(value)&&(value.reason==='quality'||value.reason==='coverage'||value.reason==='out_of_scope')&&
  typeof value.message==='string'&&typeof value.next_step==='string'
}

function parseAnalysisJob<T>(value:unknown, isOutput:(output:unknown)=>output is T):AnalysisJob<T>{
 if(!isRecord(value)||typeof value.id!=='number'||typeof value.model_type!=='string'||
    typeof value.model_version!=='string'||typeof value.created!=='string')throw new Error(INVALID_RESPONSE)
 const {id,model_type,model_version,created,state,error,output}=value
 const fields={id,model_type,model_version,created}
 switch(state){
  case 'succeeded':
   if(error===null&&isOutput(output))return {...fields,state,error,output}
   break
  case 'insufficient_evidence':
   if(error===null&&isRecord(output)&&isInsufficientEvidence(value.insufficient_evidence))
    return {...fields,state,error,output,insufficient_evidence:value.insufficient_evidence}
   break
  case 'pending':
  case 'running':
  case 'failed':
  case 'interrupted':
   if(output===null&&(error===null||typeof error==='string'))return {...fields,state,error,output}
   break
 }
 throw new Error(INVALID_RESPONSE)
}

export async function analysisHistory<T>(path:string,modelType:string,isOutput:(output:unknown)=>output is T):Promise<AnalysisJob<T>[]>{
 const value=await api<unknown>(path)
 if(!Array.isArray(value)||!value.every(row=>isRecord(row)&&typeof row.model_type==='string'))throw new Error(INVALID_RESPONSE)
 return value.filter(row=>row.model_type===modelType).map(row=>parseAnalysisJob(row,isOutput))
}

export async function submitAnalysis<T>(path:string,body:unknown,isOutput:(output:unknown)=>output is T):Promise<AnalysisJob<T>>{
 return parseAnalysisJob(await api<unknown>(path,body),isOutput)
}

export function apiUpload<T=unknown>(path:string,form:FormData):Promise<T>{
 return apiRequest<T>(path,{method:'POST',body:form})
}
