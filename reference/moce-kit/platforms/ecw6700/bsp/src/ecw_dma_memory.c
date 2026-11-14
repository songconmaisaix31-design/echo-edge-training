#include "bsp_ecw6700_dma.h"
#include "ecw_sdk.h"
#include "dma.h"
#include "chip_memmap.h"
/* B02P03 dma.c:T_DMA_REG_MAP ChEN at 0x34. Readback is necessary: the
 * SDK's drv_dma_stop merely requests abort and returns remaining size. */
static bool enabled(int channel)
{
    return (*(volatile uint32_t *)(uintptr_t)(MEM_BASE_DMAC+0x34u)&(1u<<(unsigned)channel))!=0;
}
static bool span(const void *p,size_t n)
{
    uintptr_t a=(uintptr_t)p;
    return p&&a>=MEM_BASE_RAM0&&a<IRAM_END_ADDR&&n<=IRAM_END_ADDR-a;
}
static void event(void *user,unsigned events)
{
    Ecw6700DmaStorage *a=user;
    a->state.events|=events;
    if(events&(DRV_DMA_EVENT_ERROR|DRV_DMA_EVENT_ABORT))a->state.error=BSP_IO;
    if(events==DRV_DMA_EVENT_TC && a->state.error==BSP_OK)a->state.completed=true;
}
BspStatus bsp_ecw6700_dma_copy_begin(Ecw6700DmaStorage *a,const void *src,void *dst,size_t n)
{
    if(!a||!n||n>65535||!span(src,n)||!span(dst,n))return BSP_INVALID_ARG;
    uintptr_t x=(uintptr_t)src,y=(uintptr_t)dst;
    if(x<y?y-x<n:x-y<n)return BSP_INVALID_ARG;
    uintptr_t work=(uintptr_t)a;
    if((x<=work?work-x<n:x-work<sizeof(*a))||
       (y<=work?work-y<n:y-work<sizeof(*a)))return BSP_INVALID_ARG;
    if(ecw_sdk_in_isr())return BSP_INVALID_STATE;
    if(!ecw_sdk_thread_ready())return BSP_NOT_READY;
#if defined(CONFIG_CPU_DCACHE_ENABLE) && CONFIG_CPU_DCACHE_ENABLE
    (void)event; /* Keep cache-disabled backend warning-clean in this build. */
    return BSP_UNSUPPORTED;
#else
    uintptr_t key=ecw_sdk_lock();
    if(a->state.active){ecw_sdk_unlock(key);return BSP_BUSY;}
    int ch=drv_dma_ch_alloc();
    if(ch<0){ecw_sdk_unlock(key);return BSP_NO_RESOURCE;}
    T_DMA_CFG_INFO cfg={.src=(unsigned int)x,.dst=(unsigned int)y,
        .len=(unsigned int)n,.mode=DMA_CHN_MEM};
    if(drv_dma_cfg(ch,&cfg)) {
        drv_dma_ch_release(ch);ecw_sdk_unlock(key);return BSP_IO;
    }
    *a=(Ecw6700DmaStorage){.state={.active=true},.channel=ch};
    drv_dma_event_register(ch,event,a);drv_dma_start(ch);
    ecw_sdk_unlock(key);return BSP_OK;
#endif
}
BspStatus bsp_ecw6700_dma_query(Ecw6700DmaStorage *a,Ecw6700DmaState *out)
{
    if(!a||!out)return BSP_INVALID_ARG;
    if(ecw_sdk_in_isr())return BSP_INVALID_STATE;
    uintptr_t key=ecw_sdk_lock();*out=a->state;ecw_sdk_unlock(key);return BSP_OK;
}
BspStatus bsp_ecw6700_dma_finish(Ecw6700DmaStorage *a,bool cancel)
{
    if(!a)return BSP_INVALID_ARG;
    if(ecw_sdk_in_isr())return BSP_INVALID_STATE;
    uintptr_t key=ecw_sdk_lock();
    if(!a->state.active){ecw_sdk_unlock(key);return BSP_INVALID_STATE;}
    if(cancel) {a->state.cancelled=true;(void)drv_dma_stop(a->channel);}
    if(enabled(a->channel)||(!cancel&&!a->state.completed&&a->state.error==BSP_OK)) {
        ecw_sdk_unlock(key);return BSP_BUSY;
    }
    drv_dma_event_register(a->channel,NULL,NULL);drv_dma_ch_release(a->channel);
    a->state.active=false;a->state.buffers_returned=true;
    ecw_sdk_unlock(key);return BSP_OK; /* Transfer error remains in query state. */
}
