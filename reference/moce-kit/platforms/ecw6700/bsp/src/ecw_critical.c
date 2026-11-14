#include "bsp_critical.h"
#include "ecw_sdk.h"

BspStatus bsp_critical_enter(uintptr_t *saved_mask)
{
    if (!saved_mask)
    {
        return BSP_INVALID_ARG;
    }
    *saved_mask = ecw_sdk_lock();
    return BSP_OK;
}

void bsp_critical_exit(uintptr_t saved_mask)
{
    ecw_sdk_unlock(saved_mask);
}
