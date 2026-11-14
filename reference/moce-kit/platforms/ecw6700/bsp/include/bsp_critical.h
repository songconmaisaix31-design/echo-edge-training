#ifndef BSP_CRITICAL_H
#define BSP_CRITICAL_H
#include "bsp.h"

#ifdef __cplusplus
extern "C" {
#endif
/* ECW compatibility entry for existing module code, not a multicore mutex.
 * Short, paired task/ISR critical sections only: no wait, sleep or allocation.
 * Nesting restores the saved interrupt mask in reverse order. No implicit init.
 * MOCE-SYNC: critical compatibility | ../AUDIO_LIFECYCLE.md */
BspStatus bsp_critical_enter(uintptr_t *saved_mask);
void bsp_critical_exit(uintptr_t saved_mask);
#ifdef __cplusplus
}
#endif
#endif
