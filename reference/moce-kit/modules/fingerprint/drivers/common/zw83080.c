#include "zw83080.h"
#include <string.h>
/* Temporary diagnostic counters; no packet contents are retained. */
volatile unsigned zw83080_diag_command, zw83080_diag_tx_status;
volatile unsigned zw83080_diag_rx_bytes, zw83080_diag_boot_bytes;
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static void put16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(24-8*i)); }
static BspStatus broken(Zw83080 *d,BspStatus status) { d->fault=true; return status; }
static BspStatus healthy(Zw83080 *d)
{
    BspUartRxStats stats;
    BspStatus s=bsp_uart_rx_stats(d->config.port,&stats);
    if(s!=BSP_OK)return broken(d,s);
    if(!stats.active||stats.dropped||stats.hal_errors)return broken(d,BSP_IO);
    return BSP_OK;
}
/* Consume only one byte at a time so coalesced multi-response enrollment
 * packets remain in the BSP ring for the next parser call. */
static BspStatus receive(Zw83080 *d,uint8_t *payload,size_t *length,uint32_t start,uint32_t timeout)
{
    uint8_t frame[44];size_t used=0,needed=0;unsigned noise=0;
    *length=0;
    for(;;) {
        BspStatus s=healthy(d);if(s!=BSP_OK)return s;
        if((uint32_t)(bsp_time_ms()-start)>=timeout)return broken(d,BSP_TIMEOUT);
        uint8_t b=0;size_t n=0;
        s=bsp_uart_read_cached(d->config.port,&b,1,&n);if(s!=BSP_OK)return broken(d,s);
        if(!n) { s=bsp_delay_ms(1);if(s!=BSP_OK)return broken(d,s);continue; }
        zw83080_diag_rx_bytes += (unsigned)n;
        if(!used&&b!=0xef) { if(++noise>64)return broken(d,BSP_IO);continue; }
        if(used==1&&b!=1) { if(++noise>64)return broken(d,BSP_IO);used=b==0xef?1:0;continue; }
        frame[used++]=b;
        if(used==9) {
            size_t wire=be16(frame+7);
            if(be32(frame+2)!=d->config.address||frame[6]!=7||wire<3||wire>35)
                return broken(d,BSP_IO);
            needed=9+wire;
        }
        if(needed&&used==needed) {
            uint16_t sum=0;for(size_t i=6;i<needed-2;++i)sum=(uint16_t)(sum+frame[i]);
            if(sum!=be16(frame+needed-2))return broken(d,BSP_IO);
            s=healthy(d);if(s!=BSP_OK)return s;
            *length=needed-11;memcpy(payload,frame+9,*length);d->confirmation=payload[0];return BSP_OK;
        }
    }
}
static BspStatus send(Zw83080 *d,const uint8_t *command,size_t n)
{
    if(!d||!command||!n||n>6)return BSP_INVALID_ARG;
    if(!d->initialized||d->fault)return BSP_INVALID_STATE;
    d->confirmation=0xff;
    zw83080_diag_command=command[0];
    zw83080_diag_tx_status=BSP_NOT_READY;
    zw83080_diag_rx_bytes=0;
    BspStatus s=healthy(d);if(s!=BSP_OK)return s;
    /* Unexpected pending reply cannot safely be associated with a new command. */
    uint8_t old;size_t count=0;
    s=bsp_uart_read_cached(d->config.port,&old,1,&count);
    if(s!=BSP_OK)return broken(d,s);
    if(count)return broken(d,BSP_IO);
    uint8_t frame[17]={0xef,1};put32(frame+2,d->config.address);frame[6]=1;put16(frame+7,(uint16_t)(n+2));
    memcpy(frame+9,command,n);uint16_t sum=0;
    for(size_t i=6;i<9+n;++i)sum=(uint16_t)(sum+frame[i]);
    put16(frame+9+n,sum);
    s=bsp_uart_write(d->config.port,frame,n+11,d->config.timeout_ms);
    zw83080_diag_tx_status=(unsigned)s;
    return s==BSP_OK?s:broken(d,s);
}
static BspStatus exchange(Zw83080 *d,const uint8_t *cmd,size_t n,uint8_t *reply,size_t expected)
{
    BspStatus s=send(d,cmd,n);if(s!=BSP_OK)return s;
    size_t got=0;s=receive(d,reply,&got,bsp_time_ms(),d->config.timeout_ms);
    if(s!=BSP_OK)return s;
    /* Error confirmations can be short or retain command-specific fields. */
    if((reply[0]==0&&got!=expected)||(reply[0]!=0&&got!=1&&got!=expected))return broken(d,BSP_IO);
    return BSP_OK;
}
BspStatus zw83080_init(Zw83080 *d,const Zw83080Config *c)
{
    if(!d||!c||c->timeout_ms<10||c->timeout_ms>10000||(c->port>>8)!=BSP_UART)return BSP_INVALID_ARG;
    if(d->initialized||d->receiver_owned||d->fault)return BSP_INVALID_STATE;
    uint32_t missing=0;BspStatus s=bsp_port_require(c->port,BSP_CAP_BLOCKING|BSP_CAP_BACKGROUND_RX,&missing);
    if(s!=BSP_OK)return s;
    d->config=*c;d->confirmation=0xff;
    /* Preserve uncertain ownership on failure; caller must not deinit port blindly. */
    d->receiver_owned=true;s=bsp_uart_rx_start(c->port);if(s!=BSP_OK)return broken(d,s);
    d->initialized=true;
    s=bsp_delay_ms(100);if(s!=BSP_OK)return broken(d,s);
    /* Drain bounded boot handshake before first command, never after TX. */
    for(unsigned i=0;i<256;++i) {
        uint8_t b;size_t n=0;s=bsp_uart_read_cached(c->port,&b,1,&n);
        if(s!=BSP_OK)return broken(d,s);
        if(!n)break;
        zw83080_diag_boot_bytes += (unsigned)n;
        if(i==255)return broken(d,BSP_IO);
    }
    uint8_t cmd[5]={0x13},reply[33];put32(cmd+1,c->password);
    s=exchange(d,cmd,5,reply,1);if(s!=BSP_OK)return s;
    if(d->confirmation)return broken(d,BSP_IO);
    cmd[0]=0x0f;s=exchange(d,cmd,1,reply,17);if(s!=BSP_OK)return s;
    if(d->confirmation)return broken(d,BSP_IO);
    Zw83080Parameters p={be16(reply+1),be16(reply+3),be16(reply+5),be16(reply+7),be16(reply+13),be16(reply+15),be32(reply+9)};
    if(!p.capacity||!p.enroll_times||p.enroll_times>255||p.address!=c->address)return broken(d,BSP_IO);
    d->parameters=p;return BSP_OK;
}
BspStatus zw83080_deinit(Zw83080 *d)
{
    if(!d)return BSP_INVALID_ARG;
    if(d->receiver_owned) {
        BspStatus s=bsp_uart_rx_stop(d->config.port);if(s!=BSP_OK)return broken(d,s);
        d->receiver_owned=false;
    }
    d->initialized=false; /* fault deliberately persists until module reset and new instance */
    return BSP_OK;
}
BspStatus zw83080_read_count(Zw83080 *d,uint16_t *count)
{
    if(!count)return BSP_INVALID_ARG;
    uint8_t cmd=0x1d,r[33];BspStatus s=exchange(d,&cmd,1,r,3);
    if(s==BSP_OK&&!d->confirmation) { *count=be16(r+1); }
    return s;
}
BspStatus zw83080_capture(Zw83080 *d) { uint8_t c=1,r[33];return exchange(d,&c,1,r,1); }
BspStatus zw83080_generate(Zw83080 *d,uint8_t buffer)
{
    if(!buffer)return BSP_INVALID_ARG;
    uint8_t c[2]={2,buffer},r[33];return exchange(d,c,2,r,1);
}
BspStatus zw83080_search(Zw83080 *d,uint16_t *id,uint16_t *score)
{
    if(!d||!id||!score)return BSP_INVALID_ARG;
    uint8_t c[6]={4,1,0,0},r[33];put16(c+4,d->parameters.capacity);
    BspStatus s=exchange(d,c,6,r,5);
    if(s==BSP_OK&&!d->confirmation) {
        if(be16(r+1)>=d->parameters.capacity)return broken(d,BSP_IO);
        *id=be16(r+1);*score=be16(r+3);
    }
    return s;
}
BspStatus zw83080_delete_one(Zw83080 *d,uint16_t id)
{
    if(!d||id>=d->parameters.capacity)return BSP_INVALID_ARG;
    uint8_t c[5]={0x0c,0,0,0,1},r[33];put16(c+1,id);return exchange(d,c,5,r,1);
}
BspStatus zw83080_enroll(Zw83080 *d,uint16_t id,uint32_t timeout,Zw83080ProgressCallback cb,void *ctx)
{
    if(!d||id>=d->parameters.capacity||timeout<1000||timeout>300000)return BSP_INVALID_ARG;
    uint8_t enroll_times=d->config.enroll_times?d->config.enroll_times:(uint8_t)d->parameters.enroll_times;
    uint8_t c[6]={0x31,0,0,enroll_times,0,0x10},r[33];put16(c+1,id);
    BspStatus s=send(d,c,6);if(s!=BSP_OK)return s;
    uint32_t start=bsp_time_ms();
    for(unsigned packets=0;packets<1024;++packets) {
        size_t n=0;s=receive(d,r,&n,start,timeout);if(s!=BSP_OK)return s;
        if(n!=3&&!(n==1&&r[0]!=0))return broken(d,BSP_IO);
        Zw83080Progress progress={r[0],n==3?r[1]:0,n==3?r[2]:0};
        if(cb)cb(&progress,ctx);
        /* V1.5.1, PS_AutoEnroll step 3: 07 02 n retries acquisition
         * inside the active enrollment. Keep the original deadline and do
         * not resend the command or discard subsequent progress packets. */
        if(n==3&&r[0]==0x07&&r[1]==2&&r[2]>=1&&r[2]<=enroll_times)
            continue;
        if(r[0])return BSP_OK;
        if(r[1]>6)return broken(d,BSP_IO);
        if(r[1]==6) {
            if(r[2]!=0xf2)return broken(d,BSP_IO);
            return BSP_OK;
        }
    }
    return broken(d,BSP_IO);
}

