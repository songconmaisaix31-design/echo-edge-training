#include "cst816d.h"

BspStatus cst816d_init(Cst816d *t, const Cst816dConfig *c)
{
    if (!t || !c || c->address < 8 || c->address > 0x77 ||
        !c->timeout_ms || c->timeout_ms > INT32_MAX) { return BSP_INVALID_ARG; }
    if (t->initialized) { return BSP_INVALID_STATE; }
    BspStatus status = bsp_touch_init(&t->touch, &c->calibration);
    if (status != BSP_OK) { return status; }
    uint32_t missing;
    status = bsp_port_require(c->i2c, BSP_CAP_BLOCKING | BSP_CAP_RESTART, &missing);
    if (status == BSP_OK) { status = bsp_port_require(c->reset, BSP_CAP_BLOCKING, &missing); }
    if (status != BSP_OK) { return status; }
    t->config = *c;
    t->configured = true;
    status = bsp_gpio_write(c->reset, false);
    if (status == BSP_OK) { status = bsp_delay_ms(5); }
    if (status == BSP_OK) { status = bsp_gpio_write(c->reset, true); }
    if (status == BSP_OK) { status = bsp_delay_ms(100); }
    uint8_t identity[3];
    if (status == BSP_OK) {
        status = bsp_i2c_mem_read(c->i2c, c->address, 0xa7, 1, identity, sizeof(identity), c->timeout_ms);
    }
    if (status != BSP_OK) { return status; }
    t->chip_id = identity[0];
    t->firmware = identity[2];
    if (t->chip_id != 0xb6) { return BSP_UNSUPPORTED; }
    const uint8_t awake = 1;
    status = bsp_i2c_mem_write(c->i2c, c->address, 0xfe, 1, &awake, 1, c->timeout_ms);
    if (status == BSP_OK) { t->initialized = true; }
    return status;
}

BspStatus cst816d_read(Cst816d *t, BspTouchEvent *event)
{
    if (!t || !event) { return BSP_INVALID_ARG; }
    if (!t->initialized || t->sleeping) { return BSP_NOT_READY; }
    uint8_t data[6];
    BspStatus status = bsp_i2c_mem_read(t->config.i2c, t->config.address, 1, 1,
                                       data, sizeof(data), t->config.timeout_ms);
    if (status != BSP_OK) { return status; }
    unsigned fingers = data[1] & 0x0fu;
    unsigned kind = data[2] >> 6;
    if (fingers > 1) { return BSP_UNSUPPORTED; }
    if (fingers && kind == 3) { return BSP_NOT_READY; }
    int32_t x = ((data[2] & 0x0f) << 8) | data[3];
    int32_t y = ((data[4] & 0x0f) << 8) | data[5];
    bool pressed = fingers == 1 && kind != 1;
    const BspTouchCalibration *c = &t->config.calibration;
    if (pressed && (x < c->min_x || x > c->max_x || y < c->min_y || y > c->max_y)) {
        return BSP_IO;
    }
    status = bsp_touch_update(&t->touch, pressed, x, y, bsp_time_ms(), event);
    if (status == BSP_OK) {
        t->gesture = data[0];
        uint16_t x0=event->x, y0=event->y;
        uint16_t w=c->width, h=c->height;
        if (t->rotation==1) { event->x=h-1u-y0; event->y=x0; }
        else if (t->rotation==2) { event->x=w-1u-x0; event->y=h-1u-y0; }
        else if (t->rotation==3) { event->x=y0; event->y=w-1u-x0; }
    }
    return status;
}

BspStatus cst816d_set_rotation(Cst816d *t, unsigned rotation)
{
    if (!t || rotation>3) { return BSP_INVALID_ARG; }
    if (!t->initialized || t->sleeping) { return BSP_NOT_READY; }
    if (t->touch.pressed) { return BSP_BUSY; }
    t->rotation=rotation;
    return BSP_OK;
}
BspStatus cst816d_sleep(Cst816d *t)
{
    if (!t || !t->initialized) { return BSP_NOT_READY; }
    if (t->sleeping) { return BSP_OK; }
    const uint8_t sleep=3;
    BspStatus status=bsp_i2c_mem_write(t->config.i2c,t->config.address,0xa5,1,&sleep,1,t->config.timeout_ms);
    if (status==BSP_OK) { t->sleeping=true; t->touch.pressed=false; }
    return status;
}
BspStatus cst816d_recover(Cst816d *t)
{
    if (!t || !t->configured) { return BSP_NOT_READY; }
    Cst816dConfig config=t->config;
    t->initialized=false;
    t->sleeping=true;
    BspStatus status=cst816d_init(t,&config);
    if (status==BSP_OK) { t->sleeping=false; }
    return status;
}
BspStatus cst816d_wake(Cst816d *t)
{
    if (!t || !t->initialized) { return BSP_NOT_READY; }
    return t->sleeping?cst816d_recover(t):BSP_OK;
}

