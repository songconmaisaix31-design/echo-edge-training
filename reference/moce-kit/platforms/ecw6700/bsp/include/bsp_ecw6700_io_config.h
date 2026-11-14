#ifndef BSP_ECW6700_IO_CONFIG_H
#define BSP_ECW6700_IO_CONFIG_H
#include "bsp_ecw6700.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Project-owned buffers. Keep all storage immutable and alive after platform_init.
 * Composition is permitted only before platform_init, with disjoint source/output. */
    typedef struct
    {
        Ecw6700Board board;
        BspBoard descriptor;
        Ecw6700Port *ports;
        BspPortInfo *info;
        size_t capacity;
        uint8_t *pins;
        size_t pin_capacity;
        uint32_t *channels;
        size_t channel_capacity;
    } Ecw6700IoStorage;
    BspStatus bsp_ecw6700_io_compose(const Ecw6700Board *source, const BspIoChoice *choices,
                                     size_t count, Ecw6700IoStorage *output);
#ifdef __cplusplus
}
#endif
#endif
