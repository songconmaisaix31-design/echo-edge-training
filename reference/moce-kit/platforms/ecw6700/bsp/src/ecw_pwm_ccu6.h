#ifndef ECW_PWM_CCU6_H
#define ECW_PWM_CCU6_H
#include "ecw_sdk.h"
/* BSP detection, not a code returned by P16. Never report as a native error. */
#define ECW_CCU6_CONFIG_MISMATCH (-1001)
bool ecw_ccu6_available(void);
int ecw_ccu6_open(const Ecw6700Port *port, uint32_t rate_hz);
int ecw_ccu6_close(const Ecw6700Port *port);
EcwIoResult ecw_ccu6_set(const Ecw6700Port *port, size_t channel, uint16_t duty, bool stop);
#endif
