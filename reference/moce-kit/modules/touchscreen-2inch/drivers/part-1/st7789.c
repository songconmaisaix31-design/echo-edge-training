#include "st7789.h"
#include <string.h>

BspStatus st7789_finish(St7789 *d)
{
    if (!d) { return BSP_INVALID_ARG; }
    if (!d->active) { return BSP_OK; }
    BspRequestSnapshot s;
    BspStatus status = bsp_request_query(&d->request, &s);
    if (status != BSP_OK) { return status; }
    if (s.state != BSP_REQ_TERMINAL || !s.result.buffers_returned) { return BSP_BUSY; }
    status = bsp_request_reset(&d->request);
    if (status != BSP_OK) { return status; }
    d->active = false;
    status = s.result.error.code;
    if (status == BSP_OK && s.result.wire_complete != BSP_YES) { status = BSP_IO; }
    if (status != BSP_OK) { d->failed = true; }
    return status;
}

static BspStatus send(St7789 *d, bool data_mode, size_t length)
{
    BspDisplaySegment segment = {data_mode, d->data, length, NULL};
    BspStatus status = bsp_display_submit(&d->config.bus, &segment, 1, &d->request);
    if (status != BSP_OK) { d->failed = true; return status; }
    d->active = true;
    uint32_t start = bsp_time_ms();
    for (;;) {
        status = st7789_finish(d);
        if (status != BSP_BUSY) { return status; }
        if ((uint32_t)(bsp_time_ms() - start) >= d->config.timeout_ms) {
            d->failed = true;
            return BSP_TIMEOUT;
        }
        status = bsp_delay_ms(1);
        if (status != BSP_OK) { d->failed = true; return status; }
    }
}

BspStatus st7789_command(St7789 *d, uint8_t command, const uint8_t *data, size_t length)
{
    if (!d || (length && !data) || length > sizeof(d->data)) { return BSP_INVALID_ARG; }
    if (!d->initialized || d->failed) { return BSP_NOT_READY; }
    if (d->active) { return BSP_BUSY; }
    d->data[0] = command;
    BspStatus status = send(d, false, 1);
    if (status == BSP_OK && length) {
        memcpy(d->data, data, length);
        status = send(d, true, length);
    }
    return status;
}

BspStatus st7789_init(St7789 *d, const St7789Config *c)
{
    if (!d || !c || !c->width || !c->height || !c->timeout_ms || c->timeout_ms > INT32_MAX ||
        (uint32_t)c->width + c->x_offset > ((c->madctl & 0x20u) ? 320u : 240u) ||
        (uint32_t)c->height + c->y_offset > ((c->madctl & 0x20u) ? 240u : 320u)) { return BSP_INVALID_ARG; }
    if (d->initialized || d->active) { return BSP_INVALID_STATE; }
    uint32_t missing;
    BspStatus status = bsp_port_require(c->bus.spi, BSP_CAP_ASYNC | BSP_CAP_DISPLAY, &missing);
    const BspPort pins[] = {c->bus.cs, c->bus.dc, c->reset};
    for (size_t i = 0; status == BSP_OK && i < 3; ++i) {
        status = bsp_port_require(pins[i], BSP_CAP_BLOCKING, &missing);
    }
    if (status != BSP_OK) { return status; }
    d->config = *c;
    const BspNotify notify = {.mode = BSP_NOTIFY_NONE};
    status = bsp_request_init(&d->request, &notify);
    if (status != BSP_OK) { return status; }
    d->initialized = true;
    return st7789_recover(d);
}

BspStatus st7789_recover(St7789 *d)
{
    if (!d || !d->initialized) { return BSP_NOT_READY; }
    if (d->active) { return BSP_BUSY; }
    d->failed = false;
    const St7789Config *c = &d->config;
    BspStatus status;
    status = bsp_gpio_write(c->reset, false);
    if (status == BSP_OK) { status = bsp_delay_ms(1); }
    if (status == BSP_OK) { status = bsp_gpio_write(c->reset, true); }
    if (status == BSP_OK) { status = bsp_delay_ms(150); }
    if (status == BSP_OK) { status = st7789_command(d, 0x11, NULL, 0); }
    if (status == BSP_OK) { status = bsp_delay_ms(150); }
    const uint8_t format = 0x55;
    if (status == BSP_OK) { status = st7789_command(d, 0x3a, &format, 1); }
    if (status == BSP_OK) { status = st7789_command(d, 0x36, &c->madctl, 1); }
    if (status == BSP_OK) { status = st7789_command(d, c->inverted ? 0x21 : 0x20, NULL, 0); }
    if (status == BSP_OK) { status = st7789_command(d, 0x29, NULL, 0); }
    if (status != BSP_OK) { d->failed = true; }
    if (status == BSP_OK) { d->sleeping = false; }
    return status;
}
BspStatus st7789_fill_rect(St7789 *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!d || !w || !h) { return BSP_INVALID_ARG; }
    if (!d->initialized || d->failed || d->sleeping) { return BSP_NOT_READY; }
    if (d->active) { return BSP_BUSY; }
    if ((uint32_t)x + w > d->config.width || (uint32_t)y + h > d->config.height) { return BSP_INVALID_ARG; }
    uint16_t x0 = x + d->config.x_offset, x1 = x0 + w - 1u;
    uint16_t y0 = y + d->config.y_offset, y1 = y0 + h - 1u;
    const uint8_t columns[] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t rows[] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    BspStatus status = st7789_command(d, 0x2a, columns, sizeof(columns));
    if (status == BSP_OK) { status = st7789_command(d, 0x2b, rows, sizeof(rows)); }
    if (status == BSP_OK) { status = st7789_command(d, 0x2c, NULL, 0); }
    if (status != BSP_OK) { return status; }
    for (size_t i = 0; i < sizeof(d->data); i += 2) {
        d->data[i] = (uint8_t)(color >> 8);
        d->data[i + 1] = (uint8_t)color;
    }
    size_t remaining = (size_t)w * h;
    while (remaining && status == BSP_OK) {
        size_t pixels = remaining > sizeof(d->data) / 2 ? sizeof(d->data) / 2 : remaining;
        status = send(d, true, pixels * 2);
        remaining -= pixels;
    }
    return status;
}


BspStatus st7789_write_pixels(St7789 *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, size_t count)
{
    if (!d || !pixels || !w || !h || count != (size_t)w*h) { return BSP_INVALID_ARG; }
    if (!d->initialized || d->failed || d->sleeping) { return BSP_NOT_READY; }
    if (d->active) { return BSP_BUSY; }
    if ((uint32_t)x+w>d->config.width || (uint32_t)y+h>d->config.height) { return BSP_INVALID_ARG; }
    uint16_t x0=x+d->config.x_offset, y0=y+d->config.y_offset;
    uint16_t x1=x0+w-1u, y1=y0+h-1u;
    uint8_t columns[]={(uint8_t)(x0>>8),(uint8_t)x0,(uint8_t)(x1>>8),(uint8_t)x1};
    uint8_t rows[]={(uint8_t)(y0>>8),(uint8_t)y0,(uint8_t)(y1>>8),(uint8_t)y1};
    BspStatus status=st7789_command(d,0x2a,columns,4);
    if (status==BSP_OK) { status=st7789_command(d,0x2b,rows,4); }
    if (status==BSP_OK) { status=st7789_command(d,0x2c,NULL,0); }
    for (size_t offset=0;status==BSP_OK && offset<count;) {
        size_t n=count-offset;
        if (n>sizeof(d->data)/2) { n=sizeof(d->data)/2; }
        for (size_t i=0;i<n;++i) {
            d->data[2*i]=(uint8_t)(pixels[offset+i]>>8);
            d->data[2*i+1]=(uint8_t)pixels[offset+i];
        }
        status=send(d,true,2*n);
        offset+=n;
    }
    return status;
}
BspStatus st7789_set_rotation(St7789 *d, unsigned rotation)
{
    if (!d || rotation>3) { return BSP_INVALID_ARG; }
    if (d->sleeping) { return BSP_NOT_READY; }
    if (d->config.x_offset || d->config.y_offset ||
        !((d->config.width==240 && d->config.height==320) ||
          (d->config.width==320 && d->config.height==240))) { return BSP_UNSUPPORTED; }
    const uint8_t orientations[]={0x00,0x60,0xc0,0xa0};
    uint8_t value=(d->config.madctl & 0x08u)|orientations[rotation];
    BspStatus status=st7789_command(d,0x36,&value,1);
    if (status==BSP_OK) {
        d->config.madctl=value;
        d->config.width=(rotation&1)?320:240;
        d->config.height=(rotation&1)?240:320;
    }
    return status;
}
BspStatus st7789_sleep(St7789 *d)
{
    if (!d || !d->initialized || d->failed) { return BSP_NOT_READY; }
    if (d->sleeping) { return BSP_OK; }
    BspStatus status=st7789_command(d,0x28,NULL,0);
    if (status==BSP_OK) { status=st7789_command(d,0x10,NULL,0); }
    if (status==BSP_OK) { status=bsp_delay_ms(120); }
    if (status==BSP_OK) { d->sleeping=true; } else { d->failed=true; }
    return status;
}
BspStatus st7789_wake(St7789 *d)
{
    if (!d || !d->initialized || d->failed) { return BSP_NOT_READY; }
    if (!d->sleeping) { return BSP_OK; }
    BspStatus status=st7789_command(d,0x11,NULL,0);
    if (status==BSP_OK) { status=bsp_delay_ms(150); }
    if (status==BSP_OK) { status=st7789_command(d,0x29,NULL,0); }
    if (status==BSP_OK) { d->sleeping=false; } else { d->failed=true; }
    return status;
}

