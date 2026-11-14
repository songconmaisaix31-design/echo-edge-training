#include "board.h"
#include "mainboard/pins.inc"
#ifdef MOCE_SHEILD_PINS
#include MOCE_SHEILD_PINS
#endif
#define MOCE_BOARD_PORT(port_id, caps, label, ...) \
    {.id = (port_id), .capabilities = (caps), .name = (label)},
static const BspPortInfo info[] = {
#include "mainboard/ports.inc"
#ifdef MOCE_SHEILD_PORTS
#include MOCE_SHEILD_PORTS
#endif
};
#undef MOCE_BOARD_PORT
static const BspBoard desc = {.name = "ECW6700 2ADC mainboard",
                              .revision = "20261009-2adc-unverified",
                              .ports = info,
                              .count = sizeof(info) / sizeof(info[0])};
#define MOCE_BOARD_PORT(port_id, caps, label, ...) \
    {.id = (port_id), __VA_ARGS__},
static const Ecw6700Port ports[] = {
#include "mainboard/ports.inc"
#ifdef MOCE_SHEILD_PORTS
#include MOCE_SHEILD_PORTS
#endif
};
#undef MOCE_BOARD_PORT
#include "mainboard/resources.inc"
static const BspIoResource io_resources[] = {
#include "mainboard/io_resources.inc"
#ifdef MOCE_SHEILD_IO
#include MOCE_SHEILD_IO
#endif
/* Sentinel permits an assembly with no configurable IO. */
{BSP_PORT_NONE, BSP_PORT_NONE, 0, 0, 0, NULL, 0}
};
static const Ecw6700Board platform = {.io_resources = io_resources,
 .io_resource_count = sizeof(io_resources)/sizeof(io_resources[0])-1,
 .board = &desc,
                                      .ports = ports,
                                      .count = sizeof(ports) / sizeof(ports[0]),
                                      .timers = timer_bindings,
                                      .timer_count =
                                          sizeof(timer_bindings) / sizeof(timer_bindings[0])};
_Static_assert(sizeof(ports) / sizeof(ports[0]) <= BSP_MAX_PORTS, "port limit");
const BspBoard *bsp_board_get_descriptor(void)
{
    return &desc;
}
const Ecw6700Board *board_get_platform_descriptor(void)
{
    return &platform;
}
