#ifndef MOCE_BOARD_PLATFORM_H
#define MOCE_BOARD_PLATFORM_H
/* Read-only vendor binding view of the selected Board; not runtime configuration. */
#include "bsp_ecw6700.h"
#ifdef __cplusplus
extern "C"
{
#endif
    const Ecw6700Board *board_get_platform_descriptor(void);
#ifdef __cplusplus
}
#endif
#endif
