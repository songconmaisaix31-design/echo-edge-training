#ifndef EPD0426_H
#define EPD0426_H
#include "bsp_core.h"
#define EPD0426_WIDTH 800u
#define EPD0426_HEIGHT 480u
#define EPD0426_FRAME_BYTES 48000u
/* GDEY0426T82 / SSD1677, BS1 high, D/C tied low. Public BSP 1.0.
 * Main/task only, exclusive SPI/CS ownership for entire operation.
 * Caller zero-initializes instance; config and frame must remain valid during call.
 * No heap, no automatic retry/recovery. Fault requires hardware reset/re-init.
 * Only full refresh at 0..50 C; no partial refresh or controller readback.
 * Pixel data: row-major, 100 bytes/row, MSB left, 1 white / 0 black.
 * Native RAM traversal is X increasing/Y decreasing; verify glass orientation on bench.
 */
typedef struct {
    BspPort spi, cs, reset, busy;
    uint32_t transfer_timeout_ms, busy_timeout_ms;
} Epd0426Config;
typedef struct {
    Epd0426Config config;
    bool ready, fault;
    BspStatus cleanup_status;
} Epd0426;
BspStatus epd0426_init(Epd0426 *device, const Epd0426Config *config);
BspStatus epd0426_show(Epd0426 *device, const uint8_t *frame, size_t length);
BspStatus epd0426_clear(Epd0426 *device, bool white);
BspStatus epd0426_sleep(Epd0426 *device);
#endif

