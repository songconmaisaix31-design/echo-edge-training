import {useEffect,useState} from 'react'
import {api,type Capabilities} from './api'

export function useCapabilities(revision=0,enabled=true){
 const [status,setStatus]=useState<Capabilities|'disconnected'|null>(null)
 useEffect(()=>{
  if(!enabled)return
  let current=true
  setStatus(null)
  api<Capabilities>('/capabilities').then(value=>{
   if(current)setStatus(value)
  }).catch(()=>{
   if(current)setStatus('disconnected')
  })
  return ()=>{current=false}
 },[enabled,revision])
 return status
}
