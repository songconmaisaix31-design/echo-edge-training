#ifndef ECW_I2C_IRQ_H
#define ECW_I2C_IRQ_H
#include "bsp_core.h"
#include "ecw_serial_poll.h"
typedef void (*EcwI2cIrqDone)(void *context, BspStatus status, uint32_t native, size_t tx_done,
                              bool tx_valid, size_t rx_done);
/* Fixed I2C0, initialized by the existing Board/basic backend. No DMA, queue,
 * worker, timer or allocation; borrowed memory is accessed only in this ISR.
 * Missing IRQ/held SCL leaves the request active; there is no hidden deadline.
 */
BspStatus ecw_i2c_irq_start(uint8_t address, const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr,
                            EcwI2cIrqDone done, void *context);
bool ecw_i2c_irq_active(void);
void ecw_i2c_irq_handler(void);
/* Platform binding, independently replaceable by the stateful test register IO. */
uint32_t ecw_i2c_irq_read(unsigned offset);
void ecw_i2c_irq_write(unsigned offset, uint32_t value);
void ecw_i2c_irq_mask(bool masked);
void ecw_i2c_irq_install(void (*handler)(void));
#endif
