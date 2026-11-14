#ifndef BMP388_H
#define BMP388_H
#include "bsp_i2c.h"
/* TOKMAS BMP388, supplied 32-page datasheet. NOT Bosch BMP388.
 * Zero-initialize instance. Caller owns initialized I2C port, single-task access.
 * timeout_ms is per transfer. Probe changes register pointer only, no register writes. */
#define BMP388_ADDRESS_LOW 0x76u
#define BMP388_ADDRESS_HIGH 0x77u
#define BMP388_CHIP_ID 0x11u
typedef enum { BMP388_ERROR_NONE, BMP388_ERROR_ARGUMENT, BMP388_ERROR_CAPABILITY,
 BMP388_ERROR_DELAY, BMP388_ERROR_READ, BMP388_ERROR_WRITE, BMP388_ERROR_ID,
 BMP388_ERROR_CALIBRATION, BMP388_ERROR_STATUS, BMP388_ERROR_STATE } Bmp388Error;
typedef struct { BspPort port; uint8_t address; uint32_t timeout_ms; } Bmp388Config;
typedef struct { bool valid; int32_t raw_pressure,raw_temperature,temperature_milli_c;
 uint32_t pressure_pa,sampled_at_ms; } Bmp388Reading;
typedef struct {
 Bmp388Config config; bool initialized; Bmp388Error last_error;
 uint8_t last_register,chip_id,last_status,transfer_stage;
 int32_t c0,c1,c00,c10,c01,c11,c20,c21,c30,c31,c40;
} Bmp388;
BspStatus bmp388_probe(Bmp388*,const Bmp388Config*);
BspStatus bmp388_init(Bmp388*,const Bmp388Config*);
BspStatus bmp388_measure(Bmp388*,Bmp388Reading*);
void bmp388_deinit(Bmp388*);
#endif
