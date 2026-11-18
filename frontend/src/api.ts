export type Capabilities={model:string;modelDetail?:string;structureConfigured:boolean;reviewConfigured?:boolean;reviewModel?:string|null;reviewDetail?:string}

const API_BASE='/api/v1'
const REQUEST_FAILED='请求失败，请检查输入'
const INVALID_RESPONSE='服务返回了无法识别的响应'

export function apiUrl(path:string):string{
 return API_BASE+path
}

function isRecord(value:unknown):value is Record<string,unknown>{
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

export async function apiRequest<T=unknown>(path:string,init?:RequestInit):Promise<T>{
 const response=await fetch(apiUrl(path),init)
 const text=await response.text()
 let body:unknown=undefined
 if(text){
  try{body=JSON.parse(text)}
  catch{if(response.ok)throw new Error(INVALID_RESPONSE)}
 }
 if(!response.ok)throw new Error(responseErrorMessage(body))
 return body as T
}

export async function api<T>(path:string,body?:unknown,method='POST'):Promise<T>{
 return apiRequest<T>(path,body===undefined?undefined:{method,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
}

export function apiUpload<T=unknown>(path:string,form:FormData):Promise<T>{
 return apiRequest<T>(path,{method:'POST',body:form})
}
