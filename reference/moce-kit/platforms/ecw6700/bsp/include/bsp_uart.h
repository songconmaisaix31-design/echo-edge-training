/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_UART_H
#define BSP_UART_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    BspStatus bsp_uart_write(BspPort id, const uint8_t *data, size_t n, uint32_t timeout_ms);
    BspStatus bsp_uart_read(BspPort id, uint8_t *data, size_t n, uint32_t timeout_ms);
    BspStatus bsp_uart_write_async(BspPort id, const uint8_t *data, size_t n, BspRequest *request);
    typedef enum
    {
        BSP_UART_TX_WIRE_COMPLETE = 0,
        BSP_UART_TX_BUFFER_RELEASED = 1
    } BspUartTxCompletion;
    typedef struct
    {
        BspUartTxCompletion completion;
    } BspUartTxConfig;
    /* Explicit success condition. Existing write_async keeps WIRE_COMPLETE.
     * BUFFER_RELEASED permits success once no backend can read caller TX memory;
     * it does not promise the earliest possible callback or peer reception.
     * result.wire_complete remains independent evidence (YES/NO/UNKNOWN).
     * The request still owns the port until reset, as for existing async calls.
     * No hidden timer, software UART, retry or change of caller-selected mode.
     * NULL/unknown config is INVALID_ARG; unsupported mode rejects before admission. */
    BspStatus bsp_uart_write_async_configured(BspPort id, const uint8_t *data, size_t n,
                                              const BspUartTxConfig *config, BspRequest *request);
    BspStatus bsp_uart_read_async(BspPort id, uint8_t *data, size_t n, BspRequest *request);
    /* Continuous byte RX into a platform-owned fixed ring; can coexist with TX.
 * Stop before deinit. No framing or automatic retries. Empty read returns OK/count0. */
    BspStatus bsp_uart_rx_start(BspPort id);
    BspStatus bsp_uart_rx_stop(BspPort id);
    BspStatus bsp_uart_read_cached(BspPort id, uint8_t *data, size_t capacity, size_t *count);
    typedef struct
    {
        uint32_t queued, dropped, hal_errors;
        bool active;
    } BspUartRxStats;
    BspStatus bsp_uart_rx_stats(BspPort id, BspUartRxStats *out);
#ifdef __cplusplus
}
#endif
#endif
