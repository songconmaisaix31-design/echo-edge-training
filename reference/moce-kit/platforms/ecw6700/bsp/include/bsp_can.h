/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_CAN_H
#define BSP_CAN_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        uint32_t id;
        bool extended, remote;
        uint8_t length, data[8];
    } BspCanFrame;
    /* HAL copies eight bytes to a mailbox; return means mailbox acceptance, not wire/ACK. */
    BspStatus bsp_can_send(BspPort id, const BspCanFrame *frame);
    BspStatus bsp_can_receive(BspPort id, BspCanFrame *frame);
    typedef struct
    {
        uint32_t queued, software_dropped, hal_errors;
    } BspCanStats;
    BspStatus bsp_can_stats(BspPort id, BspCanStats *out);
#ifdef __cplusplus
}
#endif
#endif
