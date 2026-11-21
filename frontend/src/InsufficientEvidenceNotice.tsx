import type {ReactNode} from 'react'
import {Info} from 'lucide-react'
import type {InsufficientEvidence} from './analysis-api.generated'
import {Button} from './button'

export function InsufficientEvidenceNotice({evidence,examId,children}:{
 evidence:InsufficientEvidence;examId:number|string|undefined;children?:ReactNode
}){
 return <div className="record" role="status">
  <b><Info size={16} aria-hidden="true" style={{display:'inline',verticalAlign:'text-bottom',marginInlineEnd:6}}/>证据不足</b>
  <p>{evidence.message}</p>
  <p>{evidence.next_step}</p>
  {examId&&<Button variant="outline" asChild>
   <a href="#media" onClick={()=>sessionStorage.setItem('echo-exam',String(examId))}>补充影像</a>
  </Button>}
  {children}
 </div>
}
