#include "ecw_timer_native.h"
const Ecw6700Board *ecw_timer_board;

bool ecw_timer_board_valid(const Ecw6700Board *board)
{
    const BspTimerInventory *inventory = bsp_board_get_timer_inventory();
    if (!board || !inventory || board->timer_count > 4 || board->timer_count != inventory->count ||
        (board->timer_count && (!board->timers || !inventory->timers)))
    {
        return false;
    }
    for (size_t i = 0; i < board->timer_count; ++i)
    {
        if (board->timers[i].id != inventory->timers[i].id || board->timers[i].channel >= 4)
        {
            return false;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (board->timers[j].id == board->timers[i].id ||
                board->timers[j].channel == board->timers[i].channel)
            {
                return false;
            }
        }
    }
    /* PWM overlap is legal inventory. P16's common allocator enforces runtime exclusion. */
    return true;
}
