#include "bsp_touch.h"
#include <string.h>
BspStatus bsp_touch_init(BspTouch *t, const BspTouchCalibration *c)
{
    if (!t || !c || !c->width || !c->height || c->min_x >= c->max_x || c->min_y >= c->max_y)
    {
        return BSP_INVALID_ARG;
    }
    memset(t, 0, sizeof(*t));
    t->calibration = *c;
    t->initialized = true;
    return BSP_OK;
}
static uint16_t scale(int32_t x, int32_t low, int32_t high, uint16_t pixels, bool invert)
{
    int64_t v = x;
    if (v < low)
    {
        v = low;
    }
    if (v > high)
    {
        v = high;
    }
    uint64_t value = (uint64_t)(v - low) * (pixels - 1u) / (uint64_t)((int64_t)high - low);
    return (uint16_t)(invert ? (pixels - 1u - value) : value);
}
BspStatus bsp_touch_update(BspTouch *t, bool pressed, int32_t x, int32_t y, uint32_t ms,
                           BspTouchEvent *e)
{
    if (!t || !e)
    {
        return BSP_INVALID_ARG;
    }
    if (!t->initialized)
    {
        return BSP_NOT_READY;
    }
    e->type = BSP_TOUCH_NONE;
    e->timestamp_ms = ms;
    e->x = t->x;
    e->y = t->y;
    if (pressed)
    {
        const BspTouchCalibration *c = &t->calibration;
        uint16_t px, py;
        if (c->swap_xy)
        {
            px = scale(y, c->min_y, c->max_y, c->width, c->invert_x);
            py = scale(x, c->min_x, c->max_x, c->height, c->invert_y);
        }
        else
        {
            px = scale(x, c->min_x, c->max_x, c->width, c->invert_x);
            py = scale(y, c->min_y, c->max_y, c->height, c->invert_y);
        }
        e->type = !t->pressed ? BSP_TOUCH_DOWN
                              : ((px != t->x || py != t->y) ? BSP_TOUCH_MOVE : BSP_TOUCH_NONE);
        t->x = px;
        t->y = py;
        e->x = px;
        e->y = py;
    }
    else if (t->pressed)
    {
        e->type = BSP_TOUCH_UP;
    }
    t->pressed = pressed;
    return BSP_OK;
}
