#ifndef MOCE_CST816D_H
#define MOCE_CST816D_H
#include "bsp_i2c.h"
#include "bsp_gpio.h"
#include "bsp_touch.h"
/* Zero-initialize; one main-context owner. BSP owns I2C and GPIO lifecycle.
 * Single contact only. Disable auto sleep for periodic register sampling.
 * No firmware programming or automatic bus retry. */
typedef struct {
    BspPort i2c, reset;
    uint8_t address;
    uint32_t timeout_ms;
    BspTouchCalibration calibration;
} Cst816dConfig;
typedef struct {
    Cst816dConfig config;
    BspTouch touch;
    uint8_t chip_id, firmware, gesture;
    bool initialized, sleeping, configured;
    unsigned rotation;
} Cst816d;
BspStatus cst816d_init(Cst816d *touch, const Cst816dConfig *config);
BspStatus cst816d_read(Cst816d *touch, BspTouchEvent *event);
BspStatus cst816d_set_rotation(Cst816d *t, unsigned rotation);
BspStatus cst816d_sleep(Cst816d *t);
/* Wake/reset restores identity, awake mode and configured rotation.
 * BSP bus recovery/reinit is the resource owner's responsibility. */
BspStatus cst816d_wake(Cst816d *t);
BspStatus cst816d_recover(Cst816d *t);
#endif
