export type Capabilities={model:string;modelDetail?:string;structureConfigured:boolean;terminalConfigured:boolean;reviewConfigured?:boolean;reviewModel?:string|null;reviewDetail?:string}

export async function api<T>(path:string,body?:unknown,method='POST'):Promise<T>{
 const r=await fetch('/api/v1'+path,body===undefined?undefined:{method,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
 const data=await r.json()
 if(!r.ok)throw new Error(typeof data.detail==='string'?data.detail:Array.isArray(data.detail)?data.detail.map((e:{loc:string[];msg:string})=>e.loc.join('.')+'：'+e.msg).join('；'):data.detail?.message||'请求失败，请检查输入')
 return data
}
