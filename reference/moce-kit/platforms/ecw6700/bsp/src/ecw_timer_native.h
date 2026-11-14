#ifndef ECW_TIMER_NATIVE_H
#define ECW_TIMER_NATIVE_H
#include "bsp_ecw6700.h"
extern const Ecw6700Board *ecw_timer_board;
bool ecw_timer_board_valid(const Ecw6700Board *board);
uint32_t ecw_timer_max_period_ms(void);
/* P16 returns channel handle >=0, -1 if occupied, -2 if invalid. No hidden retry.
 * Caller holds the existing SDK task critical section throughout lifecycle calls. */
int ecw_timer_create(unsigned channel, uint32_t period_us, bool periodic, void (*callback)(void *),
                     void *context);
int ecw_timer_start_native(int handle);
int ecw_timer_retire(int handle);
#endif
