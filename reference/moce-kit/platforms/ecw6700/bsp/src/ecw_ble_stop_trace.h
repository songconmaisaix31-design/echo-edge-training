#ifndef ECW_BLE_STOP_TRACE_H
#define ECW_BLE_STOP_TRACE_H

/* Optional diagnostic only; no callbacks, resource allocation or SDK changes. */
#if defined(BSP_ECW_BLE_STOP_DIAGNOSTICS) && BSP_ECW_BLE_STOP_DIAGNOSTICS
#include "oshal.h"
#define ECW_BLE_STOP_TRACE(step, rc) \
    os_printf(LL_INFO, "[MOCE BLE STOP] stage=%s native=%d\n", step, (int)(rc))
#else
#define ECW_BLE_STOP_TRACE(step, rc) ((void)0)
#endif

#endif
