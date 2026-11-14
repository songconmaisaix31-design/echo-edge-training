#ifndef WS2812B_DO_DMA_H
#define WS2812B_DO_DMA_H
#include "bsp_timer_dma_gpio.h"

/* WS2812B-Mini-V6 8x8, one serialized owner. Zero-initialize before init.
 * Uses ordinary Board GPIO through the public timer/DMA/GPIO extension.
 * No CPU bit-banging, I2S, PWM pin function or vendor headers. */
#define WS2812B_DO_PIXELS 64u
#define WS2812B_DO_CELLS 8192u
typedef struct { uint8_t red, green, blue; } Ws2812bDoColor;
typedef struct { bool serpentine, column_major, flip_x, flip_y; } Ws2812bDoLayout;
typedef struct {
    BspPort port;
    BspTimer timer;
    uint16_t pin_mask; /* One physical output bit from the composed Board port. */
    uint32_t tick_hz, period_ticks;
    uint32_t *words; /* 8192 aligned uint32_t cells in DMA-accessible SRAM. */
    size_t capacity;
    uint8_t brightness;
    Ws2812bDoLayout layout;
} Ws2812bDoConfig;
typedef struct {
    bool borrowed;
    BspTimerDmaGpioState dma;
    BspStatus error;
} Ws2812bDoState;
typedef struct {
    bool initialized;
    Ws2812bDoConfig config;
    Ws2812bDoColor pixels[WS2812B_DO_PIXELS];
    Ws2812bDoState state;
} Ws2812bDo;
BspStatus ws2812b_do_init(Ws2812bDo *device, const Ws2812bDoConfig *config);
BspStatus ws2812b_do_set_pixel(Ws2812bDo *device, unsigned x, unsigned y, Ws2812bDoColor color);
BspStatus ws2812b_do_fill(Ws2812bDo *device, Ws2812bDoColor color);
BspStatus ws2812b_do_draw_bitmap(Ws2812bDo *device, const uint8_t rows[8],
                                Ws2812bDoColor foreground, Ws2812bDoColor background);
BspStatus ws2812b_do_set_brightness(Ws2812bDo *device, uint8_t brightness);
/* Source/instance remain alive and immutable until stop=OK, even after an error.
 * Successful start/query is not confirmation that LEDs accepted the frame. */
BspStatus ws2812b_do_start(Ws2812bDo *device);
BspStatus ws2812b_do_query(Ws2812bDo *device, Ws2812bDoState *out);
BspStatus ws2812b_do_stop(Ws2812bDo *device, Ws2812bDoState *out);
/* Stop does not transmit black; the LED may retain its last latched color. */
BspStatus ws2812b_do_deinit(Ws2812bDo *device);
#endif
