/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_RADIO_H
#define BSP_RADIO_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Fixed installed resources, not a list of initialized drivers. */
    typedef uint16_t BspRadio;
    enum
    {
        BSP_RADIO_WIFI = 1u,
        BSP_RADIO_BLE = 2u,
        BSP_RADIO_CLASSIC = 4u
    };
    typedef struct
    {
        BspRadio id;
        const char *name;
        uint32_t hardware_features; /* Verified hardware only; unknown bits remain clear. */
        uint16_t shared_group; /* 0: none; same nonzero group shares RF/controller resources. */
    } BspRadioInfo;
    typedef struct
    {
        const BspRadioInfo *radios;
        size_t count;
    } BspRadioInventory;
    /* Pure getter, count=0/NULL radios for no verified installed radio. No allocation/init.
 * Platform-private binding owns native instance or external-module transport.
 * Inventory presence does not assert driver implementation or hardware validation. */
    const BspRadioInventory *bsp_board_get_radio_inventory(void);

#ifdef __cplusplus
}
#endif
#endif
