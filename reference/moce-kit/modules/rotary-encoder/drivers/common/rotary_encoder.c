#include "rotary_encoder.h"
#include <limits.h>
#include <string.h>
static void increment(uint32_t *n) { if (*n != UINT32_MAX) ++*n; }
static void position_fields(RotaryEncoder *e)
{
    e->reading.turns = e->reading.position / 24;
    e->reading.angle_millidegrees = (e->reading.position % 24) * 15000;
}
static void desync(RotaryEncoder *e)
{
    e->partial = e->pending = 0;
    e->synced = false;
    e->have_sample = false;
    e->reading.valid = false;
    e->reading.position_certain = false;
}
BspStatus rotary_encoder_init(RotaryEncoder *e, const RotaryEncoderConfig *c, uint32_t now)
{
    if (!e || !c) return BSP_INVALID_ARG;
    if (e->initialized) return BSP_INVALID_STATE;
    if ((c->phase_a_port >> 8) != BSP_GPIO || (c->phase_b_port >> 8) != BSP_GPIO ||
        c->phase_a_port == c->phase_b_port || c->debounce_ms > INT32_MAX ||
        !c->max_sample_gap_ms || c->max_sample_gap_ms > INT32_MAX || c->detent_state > 3)
        return BSP_INVALID_ARG;
    uint32_t missing=0;
    BspStatus s=bsp_port_require(c->phase_a_port,BSP_CAP_BLOCKING,&missing);
    if (s != BSP_OK) return s;
    s=bsp_port_require(c->phase_b_port,BSP_CAP_BLOCKING,&missing);
    if (s != BSP_OK) return s;
    RotaryEncoderConfig copy=*c;
    memset(e,0,sizeof(*e)); e->config=copy; e->initialized=true;
    e->last_update_ms=now; e->reading.position_certain=true;
    s=rotary_encoder_update(e,now);
    if (s != BSP_OK) e->initialized=false;
    return s;
}
BspStatus rotary_encoder_update(RotaryEncoder *e, uint32_t now)
{
    static const int8_t table[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
    if (!e) return BSP_INVALID_ARG;
    if (!e->initialized) return BSP_INVALID_STATE;
    e->reading.delta=0; e->reading.direction=ROTARY_ENCODER_DIRECTION_NONE;
    e->last_error=ROTARY_ENCODER_ERROR_NONE;
    bool gap=(uint32_t)(now-e->last_update_ms)>e->config.max_sample_gap_ms;
    e->last_update_ms=now;
    if (gap) { increment(&e->reading.gaps); desync(e); e->last_error=ROTARY_ENCODER_ERROR_GAP; }
    bool a=false,b=false;
    BspStatus s=bsp_gpio_read(e->config.phase_a_port,&a);
    if (s==BSP_OK) s=bsp_gpio_read(e->config.phase_b_port,&b);
    if (s!=BSP_OK) {
        increment(&e->reading.read_errors); desync(e);
        e->last_error=ROTARY_ENCODER_ERROR_READ; return s;
    }
    uint8_t next=(uint8_t)((a?2u:0u)|(b?1u:0u));
    e->reading.phase_a=a; e->reading.phase_b=b;
    e->reading.sampled_at_ms=now; increment(&e->reading.samples);
    if (!e->have_sample) {
        e->state=next; e->have_sample=true; e->settled_since_ms=now;
    } else if (next!=e->state) {
        int step=table[(e->state<<2)|next];
        e->pending=0; e->settled_since_ms=now;
        if (!step) {
            increment(&e->reading.invalid_transitions);
            e->synced=false; e->partial=0; e->reading.position_certain=false;
            e->last_error=ROTARY_ENCODER_ERROR_TRANSITION;
        } else if (e->synced) {
            e->partial=(int8_t)(e->partial+step);
            if (next==e->config.detent_state) {
                if (e->partial==4 || e->partial==-4) e->pending=e->partial>0?1:-1;
                e->partial=0;
            }
        }
        e->state=next;
    }
    if (next==e->config.detent_state &&
        (uint32_t)(now-e->settled_since_ms)>=e->config.debounce_ms) {
        if (e->synced && e->pending) {
            int d=e->config.invert_direction?-e->pending:e->pending;
            if ((d>0 && e->reading.position==INT32_MAX) ||
                (d<0 && e->reading.position==INT32_MIN)) {
                e->reading.overflow=true; e->reading.position_certain=false;
            } else {
                e->reading.position+=d; e->reading.delta=d;
                e->reading.direction=(RotaryEncoderDirection)d;
            }
        }
        e->pending=0; e->synced=true;
    }
    position_fields(e); e->reading.valid=e->synced;
    return gap?BSP_NOT_READY:BSP_OK;
}
BspStatus rotary_encoder_get(const RotaryEncoder *e, uint32_t now, RotaryEncoderReading *out)
{
    if (out) memset(out,0,sizeof(*out));
    if (!e || !out) return BSP_INVALID_ARG;
    if (!e->initialized) return BSP_INVALID_STATE;
    *out=e->reading;
    if (!out->valid || (uint32_t)(now-out->sampled_at_ms)>e->config.max_sample_gap_ms) {
        out->valid=false; out->position_certain=false; out->delta=0;
        out->direction=ROTARY_ENCODER_DIRECTION_NONE; return BSP_NOT_READY;
    }
    return BSP_OK;
}
BspStatus rotary_encoder_set_position(RotaryEncoder *e, int32_t position)
{
    if (!e) return BSP_INVALID_ARG;
    if (!e->initialized) return BSP_INVALID_STATE;
    desync(e); e->reading.position=position; e->reading.delta=0;
    e->reading.direction=ROTARY_ENCODER_DIRECTION_NONE;
    e->reading.position_certain=true; e->reading.overflow=false;
    position_fields(e); return BSP_OK;
}
BspStatus rotary_encoder_reset(RotaryEncoder *e, uint32_t now)
{
    if (!e) return BSP_INVALID_ARG;
    if (!e->initialized) return BSP_INVALID_STATE;
    memset(&e->reading,0,sizeof(e->reading));
    e->last_update_ms=now; e->last_error=ROTARY_ENCODER_ERROR_NONE;
    return rotary_encoder_set_position(e,0);
}
void rotary_encoder_deinit(RotaryEncoder *e) { if(e) memset(e,0,sizeof(*e)); }
