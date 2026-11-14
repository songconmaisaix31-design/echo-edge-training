#ifndef ECW_STORAGE_INTERNAL_H
#define ECW_STORAGE_INTERNAL_H
#include "bsp_ecw6700.h"
extern Ecw6700I2CStorage *ecw_i2c_storage;
extern Ecw6700SPIStorage *ecw_spi_storage;
extern Ecw6700UARTStorage *ecw_uart_storage[2];
extern Ecw6700I2SStorage *ecw_audio_storage;
#endif
