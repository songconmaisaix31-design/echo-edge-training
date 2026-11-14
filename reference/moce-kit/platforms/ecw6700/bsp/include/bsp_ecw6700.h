#ifndef BSP_ECW6700_H
#define BSP_ECW6700_H
#include "bsp_timer_dma_gpio.h"
#include "bsp.h"
#include "bsp_io_config.h"
/* 1.0 native binding: for ordinary GPIO, Board carries physical coordinates
 * and io_resources capabilities only. Executable mode/pull/initial/speed are
 * filled exclusively by io_compose from project config before platform_init.
 * GPIO zero fields in a capability Board are NOT runtime defaults.
 * Protocol alternate-function wiring remains a fixed hardware binding. */

/* Fixed PWM controller selector for the Board's two servo outputs.
 * CCU6 channel0/PWM4 -> pins[0], channel1/PWM6 -> pins[1]; shared period.
 * Other existing PWM instances retain their PIT pin-modulo-four mapping. */
#define ECW6700_PWM_CCU6 0x100u
#ifdef __cplusplus
extern "C"
{
#endif
    /* Board pull encoding intentionally differs from the SDK: zero keeps the
     * legacy no-pull behavior. Direction remains the fixed Board output flag. */
    typedef enum
    {
        ECW_GPIO_PULL_NONE = 0,
        ECW_GPIO_PULL_UP = 1,
        ECW_GPIO_PULL_DOWN = 2
    } Ecw6700GpioPull;
    /* Fixed Board metadata, never a module-facing arbitrary pin API. */
    typedef struct
    {
        BspPort id;
        unsigned instance;
        const uint8_t *pins;
        size_t pin_count;
        bool output, initial_high;
        /* Plain GPIO only; other protocols require NONE. Rebuild Board and BSP
         * together after this appended metadata field; no binary ABI promise. */
        Ecw6700GpioPull gpio_pull;
    } Ecw6700Port;
    /* Fixed PIT0 channel binding; shared with PWM/SDK owners, not reserved by getter. */
    typedef struct
    {
        BspTimer id;
        unsigned channel;
    } Ecw6700Timer;
    typedef struct
    {
        const BspBoard *board;
        const Ecw6700Port *ports;
        size_t count;
        const Ecw6700Timer *timers;
        size_t timer_count;
        const BspIoResource *io_resources;
        size_t io_resource_count;
        bool io_composed; /* Private project-binding state, never set by Board. */
    } Ecw6700Board;
    typedef struct
    {
        const Ecw6700Board *board;
        struct Ecw6700RuntimePort *runtime_ports;
        size_t runtime_port_count;
        uint32_t wifi_worker_stack_bytes, wifi_control_stack_bytes;
        unsigned wifi_priority;
        /* Zero disables socket admission; stack bytes belong to retained IO jobs. */
        uint32_t net_worker_stack_bytes;
        unsigned net_priority;
        /* True only when startup omits wifi_conn_main and other native/CLI owners.
         * Wi-Fi init disables SDK runtime auto-save; it never writes or erases NV. */
        bool wifi_exclusive_control;
        /* SDK startup owns a synced NimBLE Host/controller. BSP owns scanning;
         * omit native scan/connect CLI/application competitors during use. */
        uint32_t ble_worker_stack_bytes;
        unsigned ble_priority;
        bool ble_exclusive_control;
    } Ecw6700PlatformConfig;
    /* GPIO: rate_hz=0. SPI slot 0: configurable mode/bit order, fixed 8-bit; exact rate only.
 * P16 CPU160/AHB80 baseline, 40 MHz / integer divisor 1..255.
 * Board fixes SCK/MISO/MOSI; no implicit CS. I2C: integer target 1000..400000 Hz, quantized; see I2C_RATE_COMPAT_20261019.md. UART: baud plus explicit frame format (zero defaults to 8N1). PWM: Hz;
 * per-duty 16-bit half-period representability checked on set. I2S: frame Hz 8000..48000; Philips
 * stereo, TX-only 16-bit slots, duplex 32-bit slots (upper halfword first). Candidate clock
 * accuracy requires measurement. */
    typedef struct
    {
        uint32_t rate_hz;
        /* I2C only: fixed IRQ, no DMA/task/timer/staging. Borrow TX/RX until the
         * result returns buffers. Missing IRQ/held SCL has no implicit deadline. */
        bool i2c_async_enabled;
        /* UART only, explicitly opt in. Caller supplies exclusive 4-byte-aligned
         * P16 IRAM storage until port deinit; no hidden allocation, worker or timer.
         * One native-auto-selected DMA channel per active request; no fixed-channel
         * selector exists in this SDK. Buffer capacity bounds each request length.
         * TX requires BUFFER_RELEASED; RX completes after the requested byte count. */
        bool uart_async_enabled;
        uint8_t *uart_dma_buffer;
        size_t uart_dma_buffer_bytes;
        /* SPI only; explicitly reserve aligned exclusive IRAM until deinit.
         * Capacity is 4..512, a multiple of four, including DMA tail padding.
         * RX storage is optional for TX-only applications, otherwise disjoint.
         * Per transfer: one DMA channel for TX, two for duplex, allocated by SDK.
         * No hidden task/timer/CS. Error/abort can retain all resources until reset
         * of the system; active cancel/recovery is not implemented. */
        bool spi_async_enabled;
        uint8_t *spi_dma_tx_buffer, *spi_dma_rx_buffer;
        size_t spi_dma_buffer_bytes;
        /* UART payload bits: 0=>8 or 5..8; parity: 0 none, 1 odd, 2 even;
         * stop: 0/1=>1, 2=>2. Five-bit payload + two stops is unsupported
         * (native hardware encodes 1.5). Zero preserves legacy 8N1.
         * Appended config changes ABI: rebuild all consumers with BSP. */
        uint8_t uart_data_bits, uart_parity, uart_stop_bits;
        /* SPI only: mode 0..3 (CPOL/CPHA); bit order 0 MSB, 1 LSB.
         * Eight-bit words remain fixed. Zero preserves legacy mode 0/MSB. */
        uint8_t spi_mode, spi_bit_order;
    } Ecw6700PortConfig;
#include "bsp_ecw6700_storage.h"
    BspStatus bsp_platform_init(const Ecw6700PlatformConfig *config);
    BspStatus bsp_ecw6700_port_init(BspPort port, const Ecw6700PortConfig *config);
    const char *bsp_ecw6700_sdk_identity(void);
#ifdef __cplusplus
}
#endif
#endif
