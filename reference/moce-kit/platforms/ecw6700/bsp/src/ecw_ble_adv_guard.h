#ifndef ECW_BLE_ADV_GUARD_H
#define ECW_BLE_ADV_GUARD_H

#include "host/ble_gap.h"
#include "syscfg/syscfg.h"
#include <stdbool.h>
#include <stdint.h>

/* P16 nimble/ble.h defines BLE_ADV_INSTANCES as MULTI_ADV_INSTANCES + 1.
 * Use the same bound: ble_gap_get_adv_state does not validate its index.
 * Default P16 config has MULTI_ADV_INSTANCES=2, hence Host slots 0, 1, 2.
 * This is a Host storage bound, not a claim about controller RF capacity. */
#define ECW_BLE_ADV_SLOTS (MYNEWT_VAL(BLE_MULTI_ADV_INSTANCES) + 1)
#if ECW_BLE_ADV_SLOTS < 1 || ECW_BLE_ADV_SLOTS > 256
#error "P16 advertising instance index exceeds uint8_t"
#endif

/* Sole Host owner only; new GAP admission must remain closed until mutation
 * completes. An idle observation alone is not a lock or an event fence. */
static inline bool ecw_ble_adv_active_from(unsigned first)
{
    for (unsigned i = first; i < ECW_BLE_ADV_SLOTS; ++i)
    {
        if (ble_gap_get_adv_state((uint8_t)i))
        {
            return true;
        }
    }
    return false;
}

#endif
