/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_I2C_H
#define BSP_I2C_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    BspStatus bsp_i2c_write(BspPort id, uint8_t address, const uint8_t *data, size_t n,
                            uint32_t timeout_ms);
    BspStatus bsp_i2c_read(BspPort id, uint8_t address, uint8_t *data, size_t n,
                           uint32_t timeout_ms);
    /* 8/16-bit register address with repeated START, using HAL Mem_Read (not STOP+START). */
    BspStatus bsp_i2c_mem_read(BspPort id, uint8_t address, uint16_t reg, uint8_t reg_bytes,
                               uint8_t *data, size_t n, uint32_t timeout_ms);
    BspStatus bsp_i2c_mem_write(BspPort id, uint8_t address, uint16_t reg, uint8_t reg_bytes,
                                const uint8_t *data, size_t n, uint32_t timeout_ms);
    /* Arbitrary TX + Sr + RX. Both lengths positive; no programmable HAL IT transfer deadline. */
    BspStatus bsp_i2c_write_read_async(BspPort id, uint8_t address, const uint8_t *tx, size_t nt,
                                       uint8_t *rx, size_t nr, BspRequest *request);
#ifdef __cplusplus
}
#endif
#endif
