#include "ecw_internal.h"
BspStatus bsp_can_send(BspPort id, const BspCanFrame *frame)
{
    (void)id;
    (void)frame;
    if (!frame || frame->length > 8 ||
        (frame->extended ? frame->id > 0x1fffffffu : frame->id > 0x7ffu))
    {
        return BSP_INVALID_ARG;
    }
    return ecw_unsupported(id, BSP_CAN);
}
BspStatus bsp_can_receive(BspPort id, BspCanFrame *frame)
{
    (void)id;
    (void)frame;
    if (!frame)
    {
        return BSP_INVALID_ARG;
    }
    return ecw_unsupported(id, BSP_CAN);
}
BspStatus bsp_can_stats(BspPort id, BspCanStats *out)
{
    (void)id;
    (void)out;
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    return ecw_unsupported(id, BSP_CAN);
}
