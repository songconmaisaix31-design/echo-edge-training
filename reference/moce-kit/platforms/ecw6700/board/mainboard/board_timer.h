#ifndef MOCE_BOARD_TIMER_H
#define MOCE_BOARD_TIMER_H
#include "bsp_timer.h"
/* Physical PIT0 channels. Shared with PWM IO16/21/22/19 respectively;
 * inventory does not claim or start them. P16 allocator also checks SDK owners. */
#define BOARD_TIMER_0 ((BspTimer)0)
#define BOARD_TIMER_1 ((BspTimer)1)
#define BOARD_TIMER_2 ((BspTimer)2)
#define BOARD_TIMER_3 ((BspTimer)3)
#endif
