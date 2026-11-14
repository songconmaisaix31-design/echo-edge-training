import {Slot} from '@radix-ui/react-slot'
import {cva,type VariantProps} from 'class-variance-authority'
import {clsx} from 'clsx'
import {twMerge} from 'tailwind-merge'
import type {ComponentProps} from 'react'
const variants=cva('button',{variants:{variant:{default:'primary',outline:'outline',ghost:'ghost'}},defaultVariants:{variant:'default'}})
export function Button({className,variant,asChild,...props}:ComponentProps<'button'>&VariantProps<typeof variants>&{asChild?:boolean}){
 const Component=asChild?Slot:'button'
 return <Component className={twMerge(clsx(variants({variant}),className))} {...props}/>
}
