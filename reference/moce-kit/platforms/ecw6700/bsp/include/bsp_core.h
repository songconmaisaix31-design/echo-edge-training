/* MOCE BSP 1.0.0-alpha.1 public contract; operation support and validation are platform-specific. */
#ifndef BSP_CORE_H
#define BSP_CORE_H
#include "bsp_request.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define BSP_API_VERSION "1.0.0-alpha.1"
#define BSP_MAX_PORTS 16u
#define BSP_PLAN_MAX_SEGMENTS 8u
#define BSP_CAN_RX_DEPTH 16u
#define BSP_UART_RX_DEPTH 128u
    typedef uint16_t BspPort;
    typedef enum
    {
        BSP_GPIO = 1,
        BSP_I2C,
        BSP_SPI,
        BSP_UART,
        BSP_ADC,
        BSP_PWM,
        BSP_I2S,
        BSP_CAN
    } BspKind;
#define BSP_PORT(kind, index) ((BspPort)(((unsigned)(kind) << 8) | (unsigned)(index)))
#define BSP_PORT_NONE ((BspPort)0xffffu)
    enum
    {
        BSP_CAP_BLOCKING = 1u,
        BSP_CAP_ASYNC = 2u,
        BSP_CAP_RESTART = 4u,
        BSP_CAP_STREAM = 8u,
        BSP_CAP_BACKGROUND_RX = 16u,
        BSP_CAP_DISPLAY = 32u,
        BSP_CAP_DUPLEX_RX = 64u /* Circular RX with immutable circular TX, not live TX refill. */
    };
    typedef struct
    {
        BspPort id;
        uint32_t capabilities;
        const char *name;
    } BspPortInfo;
    typedef struct
    {
        const char *name, *revision;
        const BspPortInfo *ports;
        size_t count;
    } BspBoard;
    /* Implement this pure getter once in a verified board translation unit. */
    const BspBoard *bsp_board_get_descriptor(void);
    BspStatus bsp_port_capabilities(BspPort id, uint32_t *capabilities);
    BspStatus bsp_port_require(BspPort id, uint32_t required, uint32_t *missing);
    BspStatus bsp_port_deinit(BspPort id);
    BspStatus bsp_port_error(BspPort id, BspError *out);
    /* Monotonic wrapping ms; zero before platform init. Compare spans <2^31 ms. */
    uint32_t bsp_time_ms(void);
    /* Task/main only. Max INT32_MAX; zero is no-wait/no-yield. Positive delay
 * waits at least the requested duration; scheduling may extend it. Invalid
 * argument precedes ISR INVALID_STATE, then initialization NOT_READY. */
    BspStatus bsp_delay_ms(uint32_t milliseconds);
    /* Bounded notification dispatch only; never wraps polling HAL in an async task. */
    BspStatus bsp_service(size_t *notifications);
    /* Recovery of a held fault is a thread-context action, not a general cancel API. */
    BspStatus bsp_port_recover(BspPort id);
/* Blocking calls require thread/main context and a running HAL tick. UINT32_MAX is rejected.
 * Timeout is the HAL-native argument, not a guaranteed whole-operation deadline.
 * Caller memory is returned on every blocking return; error details via bsp_port_error. */
#ifdef __cplusplus
}
#endif
#endif
