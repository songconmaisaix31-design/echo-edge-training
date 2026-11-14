#include "bmp388.h"
#include <limits.h>
#include <string.h>
#include <math.h>
/* Manual pp5,16,23-28: STOP then START permitted for register reads.
 * Split transfers expose pointer-write versus read failure without private SDK access. */
static BspStatus fail(Bmp388*s,Bmp388Error e,BspStatus st)
{ s->last_error=e; s->initialized=false; return st; }
static BspStatus rd(Bmp388*s,uint8_t r,uint8_t*d,size_t n)
{
 s->last_register=r; s->transfer_stage=1;
 BspStatus st=bsp_i2c_write(s->config.port,s->config.address,&r,1,s->config.timeout_ms);
 if(st!=BSP_OK)return fail(s,BMP388_ERROR_WRITE,st);
 s->transfer_stage=2;
 st=bsp_i2c_read(s->config.port,s->config.address,d,n,s->config.timeout_ms);
 if(st!=BSP_OK)return fail(s,BMP388_ERROR_READ,st);
 s->transfer_stage=0; return BSP_OK;
}
static BspStatus wr(Bmp388*s,uint8_t r,uint8_t v)
{
 uint8_t data[2]={r,v}; s->last_register=r;s->transfer_stage=3;
 BspStatus st=bsp_i2c_write(s->config.port,s->config.address,data,2,s->config.timeout_ms);
 if(st!=BSP_OK)return fail(s,BMP388_ERROR_WRITE,st);
 s->transfer_stage=0;return BSP_OK;
}
static BspStatus delay(Bmp388*s,uint32_t n)
{ BspStatus st=bsp_delay_ms(n);return st==BSP_OK?st:fail(s,BMP388_ERROR_DELAY,st); }
static int32_t sign_extend(uint32_t v,unsigned bits)
{ return (v&(1u<<(bits-1)))?(int32_t)v-(int32_t)(1u<<bits):(int32_t)v; }
static int32_t be16(const uint8_t*b){return sign_extend(((uint32_t)b[0]<<8)|b[1],16);}
static BspStatus wait_status(Bmp388*s,uint8_t mask)
{
 for(unsigned i=0;i<=200;i++){
  BspStatus st=rd(s,0x08,&s->last_status,1);
  if(st!=BSP_OK)return st;
  if((s->last_status&mask)==mask)return BSP_OK;
  if(i<200 && (st=delay(s,10))!=BSP_OK)return st;
 }
 return fail(s,BMP388_ERROR_STATUS,BSP_TIMEOUT);
}
BspStatus bmp388_probe(Bmp388*s,const Bmp388Config*c)
{
 if(!s||!c)return BSP_INVALID_ARG;
 if(s->initialized)return BSP_INVALID_STATE;
 if((c->port>>8)!=BSP_I2C||(c->address!=0x76&&c->address!=0x77)||
    !c->timeout_ms||c->timeout_ms>INT32_MAX)return BSP_INVALID_ARG;
 Bmp388Config copy=*c;memset(s,0,sizeof(*s));s->config=copy;
 uint32_t missing=0;
 BspStatus st=bsp_port_require(copy.port,BSP_CAP_BLOCKING,&missing);
 if(st!=BSP_OK)return fail(s,BMP388_ERROR_CAPABILITY,st);
 if((st=delay(s,20))!=BSP_OK)return st;
 if((st=rd(s,0x0d,&s->chip_id,1))!=BSP_OK)return st;
 if(s->chip_id!=BMP388_CHIP_ID)return fail(s,BMP388_ERROR_ID,BSP_IO);
 return BSP_OK;
}
BspStatus bmp388_init(Bmp388*s,const Bmp388Config*c)
{
 BspStatus st=bmp388_probe(s,c);
 if(st!=BSP_OK)return st;
 if((st=wr(s,0x0c,0x09))!=BSP_OK || (st=delay(s,20))!=BSP_OK)return st;
 if((st=wait_status(s,0xc0))!=BSP_OK)return st;
 uint8_t b[21];
 if((st=rd(s,0x10,b,21))!=BSP_OK)return st;
 s->c0=sign_extend(((uint32_t)b[0]<<4)|(b[1]>>4),12);
 s->c1=sign_extend(((uint32_t)(b[1]&15)<<8)|b[2],12);
 s->c00=sign_extend(((uint32_t)b[3]<<12)|((uint32_t)b[4]<<4)|(b[5]>>4),20);
 s->c10=sign_extend(((uint32_t)(b[5]&15)<<16)|((uint32_t)b[6]<<8)|b[7],20);
 s->c01=be16(b+8);s->c11=be16(b+10);s->c20=be16(b+12);
 s->c21=be16(b+14);s->c30=be16(b+16);
 s->c31=sign_extend(((uint32_t)b[18]<<4)|(b[19]>>4),12);
 s->c40=sign_extend(((uint32_t)(b[19]&15)<<8)|b[20],12);
 /* 1Hz, x1 oversampling, internal temperature; no shift/FIFO/interrupt. */
 if((st=wr(s,0x06,0))!=BSP_OK||(st=wr(s,0x07,0))!=BSP_OK||
    (st=wr(s,0x09,0))!=BSP_OK||(st=wr(s,0x08,7))!=BSP_OK)return st;
 s->initialized=true;return BSP_OK;
}
BspStatus bmp388_measure(Bmp388*s,Bmp388Reading*out)
{
 if(out)memset(out,0,sizeof(*out));
 if(!s||!out)return BSP_INVALID_ARG;
 if(!s->initialized)return BSP_INVALID_STATE;
 BspStatus st=wait_status(s,0x30);if(st!=BSP_OK)return st;
 uint8_t b[6];if((st=rd(s,0,b,6))!=BSP_OK)return st;
 int32_t rp=sign_extend(((uint32_t)b[0]<<16)|((uint32_t)b[1]<<8)|b[2],24);
 int32_t rt=sign_extend(((uint32_t)b[3]<<16)|((uint32_t)b[4]<<8)|b[5],24);
 double p=rp/524288.0,t=rt/524288.0;
 double tc=s->c0*0.5+s->c1*t;
 double pa=s->c00+p*(s->c10+p*(s->c20+p*(s->c30+p*s->c40)))+
           t*(s->c01+p*(s->c11+p*(s->c21+p*s->c31)));
 if(!isfinite(tc)||!isfinite(pa)||tc < -40||tc >85||pa<30000||pa>110000)
  return fail(s,BMP388_ERROR_CALIBRATION,BSP_IO);
 out->raw_pressure=rp;out->raw_temperature=rt;
 out->temperature_milli_c=(int32_t)lround(tc*1000);out->pressure_pa=(uint32_t)lround(pa);
 out->sampled_at_ms=bsp_time_ms();out->valid=true;return BSP_OK;
}
void bmp388_deinit(Bmp388*s){if(s)memset(s,0,sizeof(*s));}
