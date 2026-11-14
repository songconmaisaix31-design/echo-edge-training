#ifndef ECW_SERIAL_POLL_H
#define ECW_SERIAL_POLL_H
#include "ecw_spi_poll.h"
typedef EcwSpiPollIo EcwRegIo;
typedef EcwSpiPollResult EcwIoResult;
EcwIoResult ecw_i2c_poll(const EcwRegIo *, uint8_t, const uint8_t *, size_t, const uint8_t *,
                         size_t, uint8_t *, size_t, uint32_t);
EcwIoResult ecw_uart_poll(const EcwRegIo *, const uint8_t *, uint8_t *, size_t, uint32_t);
BspStatus ecw_pwm_counts(uint32_t, uint16_t, uint32_t *);
#endif
