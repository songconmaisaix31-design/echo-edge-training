#ifndef ECW_I2C_ASYNC_H
#define ECW_I2C_ASYNC_H
#include "bsp_ecw6700.h"
int ecw_i2c_open(const Ecw6700Port *port, const Ecw6700PortConfig *config);
int ecw_i2c_close(const Ecw6700Port *port);
BspStatus ecw_i2c_dispatch(size_t *count);
#endif
