/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Module-facing coordinate/event helper. The touch-controller driver obtains raw
 * samples through BSP I2C or CS-owned SPI transactions; no controller protocol is guessed. */
    typedef enum
    {
        BSP_TOUCH_NONE,
        BSP_TOUCH_DOWN,
        BSP_TOUCH_MOVE,
        BSP_TOUCH_UP
    } BspTouchType;
    typedef struct
    {
        BspTouchType type;
        uint16_t x, y;
        uint32_t timestamp_ms;
    } BspTouchEvent;
    typedef struct
    {
        int32_t min_x, max_x, min_y, max_y;
        uint16_t width, height;
        bool swap_xy, invert_x, invert_y;
    } BspTouchCalibration;
    typedef struct
    {
        BspTouchCalibration calibration;
        bool initialized, pressed;
        uint16_t x, y;
    } BspTouch;
    BspStatus bsp_touch_init(BspTouch *touch, const BspTouchCalibration *calibration);
    BspStatus bsp_touch_update(BspTouch *touch, bool pressed, int32_t raw_x, int32_t raw_y,
                               uint32_t timestamp_ms, BspTouchEvent *event);
#ifdef __cplusplus
}
#endif
#endif
