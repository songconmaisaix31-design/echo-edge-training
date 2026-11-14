#include "ecw_clock_profile.h"
#include "ecw_adc.h"
#include "ecw_i2c_async.h"
#include "ecw_i2c_timing.h"
#include "ecw_internal.h"
#include "ecw_pwm_ccu6.h"
#include "ecw_sdk.h"
#include "ecw_sdk_version.h"
#include "ecw_uart_format.h"
#include "ecw_spi_async.h"
#include "ecw_uart_async.h"
#include "ecw_uart_rx.h"
#ifdef BSP_ECW_NATIVE_WIFI
#include "ecw_wifi_service.h"
#endif
#ifdef BSP_ECW_NATIVE_NET
#include "ecw_net_service.h"
#endif
#ifdef BSP_ECW_NATIVE_BLE
#include "ecw_ble_service.h"
#endif
#include "bsp_gpio_event_queue.h"
#include "bsp_request_internal.h"
#include "ecw_audio.h"
#include "ecw_features.h"
#include "ecw_gpio_irq.h"
#include "ecw_storage_internal.h"
#include "ecw_timer_native.h"
#include <limits.h>
#include <string.h>
Ecw6700I2CStorage *ecw_i2c_storage;
Ecw6700SPIStorage *ecw_spi_storage;
Ecw6700UARTStorage *ecw_uart_storage[2];
Ecw6700I2SStorage *ecw_audio_storage;
typedef Ecw6700RuntimePort State;
static size_t protocol_bytes(unsigned kind)
{
    switch (kind)
    {
    case BSP_I2C:
        return sizeof(Ecw6700I2CStorage);
    case BSP_SPI:
        return sizeof(Ecw6700SPIStorage);
    case BSP_UART:
        return sizeof(Ecw6700UARTStorage);
    case BSP_I2S:
        return sizeof(Ecw6700I2SStorage);
    default:
        return 0;
    }
}
static size_t protocol_alignment(unsigned kind)
{
    switch (kind)
    {
    case BSP_I2C:
        return _Alignof(Ecw6700I2CStorage);
    case BSP_SPI:
        return _Alignof(Ecw6700SPIStorage);
    case BSP_UART:
        return _Alignof(Ecw6700UARTStorage);
    case BSP_I2S:
        return _Alignof(Ecw6700I2SStorage);
    default:
        return 1;
    }
}
static bool storage_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an && bn && (x <= y ? y - x < an : x - y < bn);
}
/* Arithmetic validation only: the project must supply live writable objects. */
static bool storage_span(const void *pointer, size_t bytes)
{
    return !bytes || (pointer && bytes - 1u <= UINTPTR_MAX - (uintptr_t)pointer);
}
static const Ecw6700Board *board;
static State *state;
static size_t state_count;
static uint64_t edge_serial;
static void clear_state(State *s)
{
    s->held = s->ready = s->fault = s->busy = false;
    s->error = (BspError){0};
}
static bool basic_kind(unsigned k)
{
    return k == BSP_I2C || k == BSP_UART || k == BSP_PWM || k == BSP_ADC;
}
static uintptr_t enter(void *ctx)
{
    (void)ctx;
    return ecw_sdk_lock();
}
static void leave(void *ctx, uintptr_t key)
{
    (void)ctx;
    ecw_sdk_unlock(key);
}
static int find(BspPort id)
{
    if (!board)
    {
        return -1;
    }
    for (size_t i = 0; i < state_count; ++i)
    {
        if ((*state[i].port).id == id)
        {
            return (int)i;
        }
    }
    return -1;
}
static int inventory_index(BspPort id)
{
    /* Raw-Board compatibility exposes only the project's selected protocols. */
    if (board && !board->io_composed && find(id) < 0)
        return -1;
    if (board)
    {
        for (size_t i = 0; i < board->count; ++i)
        {
            if (board->ports[i].id == id)
            {
                return (int)i;
            }
        }
    }
    return -1;
}
static BspStatus lookup(BspPort id, int *index)
{
    if (!board)
    {
        return BSP_NOT_READY;
    }
    *index = find(id);
    return *index >= 0 ? BSP_OK : inventory_index(id) >= 0 ? BSP_NOT_READY : BSP_INVALID_ARG;
}
static BspStatus sdk_result(int i, int rc)
{
    if (!rc)
    {
        state[i].error = (BspError){.code = BSP_OK};
        return BSP_OK;
    }
    state[i].fault = true;
    state[i].error = (BspError){.code = BSP_IO,
                                .native_valid = true,
                                .native_domain = ((*state[i].port).id >> 8) == BSP_SPI ? 0x45435753u
                                                 : ((*state[i].port).id >> 8) == BSP_GPIO
                                                     ? 0x45435747u
                                                     : 0x45430000u | ((*state[i].port).id >> 8),
                                .native_code = rc};
    return BSP_IO;
}
const char *bsp_ecw6700_sdk_identity(void)
{
#if BSP_ECW_SDK_B02P12
    return "ECW6700FSDK_V1.0.0B02P12; standalone; partial operation support";
#else
    return "ECW6700FSDK_V1.0.0B02P03; standalone; partial operation support";
#endif
}
static const Ecw6700Port *fixed_descriptor(const Ecw6700Board *b, BspPort id)
{
    for (size_t i = 0; i < b->count; ++i)
        if (b->ports[i].id == id)
            return &b->ports[i];
    return NULL;
}
/* MOCE-SYNC: fixed protocol config compatibility | ../IO_MIGRATION.md
 * runtime_ports already supplies the project selection and lifetime storage.
 * Do not invent GPIO defaults, an ADC mask, a hidden binding array or a heap.
 * Called after Board shape/pins and runtime array bounds have been validated. */
static BspStatus fixed_selection_valid(const Ecw6700PlatformConfig *c)
{
    if (!c->runtime_port_count)
        return BSP_INVALID_ARG; /* Platform-only configs use explicit empty composition. */
    for (size_t i = 0; i < c->runtime_port_count; ++i)
    {
        BspPort id = c->runtime_ports[i].id;
        unsigned kind = id >> 8;
        if (kind != BSP_I2C && kind != BSP_SPI && kind != BSP_UART &&
            kind != BSP_PWM && kind != BSP_I2S)
            return BSP_INVALID_ARG;
        const Ecw6700Port *p = fixed_descriptor(c->board, id);
        if (!p)
            return BSP_INVALID_ARG;
        for (size_t j = 0; j < i; ++j)
        {
            if (c->runtime_ports[j].id == id)
                return BSP_INVALID_ARG;
            const Ecw6700Port *prior = fixed_descriptor(c->board, c->runtime_ports[j].id);
            /* A previous iteration established prior's descriptor. Protocol
             * storage is controller-owned; two aliases must not replace it. */
            if (kind != BSP_PWM && (prior->id >> 8) == kind && prior->instance == p->instance)
                return BSP_BUSY;
            for (size_t a = 0; a < p->pin_count; ++a)
                for (size_t b = 0; b < prior->pin_count; ++b)
                    if (p->pins[a] == prior->pins[b])
                        return BSP_BUSY;
        }
    }
    return BSP_OK;
}
BspStatus bsp_platform_init(const Ecw6700PlatformConfig *c)
{
    if (!c || !c->board || !c->board->board || (c->board->count && !c->board->ports) || (c->board->count && !c->board->board->ports))
    {
        return BSP_INVALID_ARG;
    }
    if (board)
    {
        return BSP_INVALID_STATE;
    }
    const Ecw6700Board *b = c->board;
    if (b->count != b->board->count)
    {
        return BSP_INVALID_ARG;
    }
    if (!ecw_timer_board_valid(b))
    {
        return BSP_INVALID_ARG;
    }
    for (size_t i = 0; i < b->count; ++i)
    {
        const Ecw6700Port *p = &b->ports[i];
        unsigned kind = p->id >> 8;
        if (kind < BSP_GPIO || kind > BSP_CAN || p->id != b->board->ports[i].id || !p->pins ||
            !p->pin_count)
        {
            return BSP_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (b->ports[j].id == p->id)
            {
                return BSP_INVALID_ARG;
            }
        }
        for (size_t j = 0; j < p->pin_count; ++j)
        {
            if (p->pins[j] > 24)
            {
                return BSP_INVALID_ARG;
            }
        }
        if ((unsigned)p->gpio_pull > ECW_GPIO_PULL_DOWN)
        {
            return BSP_INVALID_ARG;
        }
        if (kind != BSP_GPIO && p->gpio_pull != ECW_GPIO_PULL_NONE)
        {
            return BSP_UNSUPPORTED;
        }
        if (kind == BSP_GPIO && p->pin_count != 1)
        {
            return BSP_INVALID_ARG;
        }
        if (kind == BSP_SPI && (p->pin_count != 3 || p->instance != 0 || p->pins[0] != 14 ||
                                p->pins[1] != 12 || p->pins[2] != 13))
        {
            return BSP_UNSUPPORTED;
        }
        if (kind == BSP_I2C && (p->pin_count != 2 || p->instance))
        {
            return BSP_UNSUPPORTED;
        }
        if (kind == BSP_UART && (p->pin_count != 2 || p->instance > 1))
        {
            return BSP_UNSUPPORTED;
        }
        if (kind == BSP_PWM && p->instance == ECW6700_PWM_CCU6)
        {
            if (p->pin_count != 2 || (p->pins[0] != 19 && p->pins[0] != 15) || p->pins[1] != 16)
            {
                return BSP_UNSUPPORTED;
            }
        }
        else if (kind == BSP_PWM)
        {
            for (size_t z = 0; z < p->pin_count; ++z)
            {
                if (p->pins[z] != 16 && p->pins[z] != 19 && p->pins[z] != 21 && p->pins[z] != 22)
                {
                    return BSP_UNSUPPORTED;
                }
                for (size_t k = 0; k < z; ++k)
                {
                    if (p->pins[k] % 4 == p->pins[z] % 4)
                    {
                        return BSP_INVALID_ARG;
                    }
                }
            }
        }
        if (kind == BSP_I2S && (p->instance || p->pin_count != 4 || p->pins[0] != 6 ||
                                p->pins[1] != 7 || p->pins[2] != 8 || p->pins[3] != 3))
        {
            return BSP_UNSUPPORTED;
        }
        uint32_t allowed = kind == BSP_I2S   ? (BSP_CAP_STREAM | BSP_CAP_DUPLEX_RX)
                           : kind == BSP_I2C ? (BSP_CAP_BLOCKING | BSP_CAP_RESTART | BSP_CAP_ASYNC)
                           : kind == BSP_UART
                               ? (BSP_CAP_BLOCKING | BSP_CAP_ASYNC | BSP_CAP_BACKGROUND_RX)
                           : kind == BSP_SPI ? (BSP_CAP_BLOCKING | BSP_CAP_ASYNC | BSP_CAP_DISPLAY)
                           : (kind == BSP_GPIO || basic_kind(kind)) ? BSP_CAP_BLOCKING
                                                                    : 0u;
        if (b->board->ports[i].capabilities & ~allowed)
        {
            return BSP_UNSUPPORTED;
        }
    }
    if ((c->runtime_port_count && !c->runtime_ports) || c->runtime_port_count > b->count ||
        c->runtime_port_count > INT_MAX || c->runtime_port_count > SIZE_MAX / sizeof(State) ||
        (uintptr_t)c->runtime_ports % _Alignof(State) ||
        !storage_span(c->runtime_ports, c->runtime_port_count * sizeof(State)))
        return BSP_INVALID_ARG;
    if (!b->io_composed)
    {
        BspStatus selected = fixed_selection_valid(c);
        if (selected != BSP_OK)
            return selected;
    }
    for (size_t i = 0; i < c->runtime_port_count; ++i)
    {
        State *current = &c->runtime_ports[i];
        size_t bytes = protocol_bytes(current->id >> 8);
        if (!storage_span(current->protocol.memory, bytes) ||
            (bytes && (!current->protocol.memory || (uintptr_t)current->protocol.memory %
                                                        protocol_alignment(current->id >> 8))) ||
            (!bytes && current->protocol.memory))
            return BSP_INVALID_ARG;
        size_t queue_bytes = current->gpio_events ? sizeof(*current->gpio_events) : 0;
        size_t array_bytes = c->runtime_port_count * sizeof(State);
        if (!storage_span(current->gpio_events, queue_bytes) ||
            storage_overlap(current->protocol.memory, bytes, c->runtime_ports, array_bytes) ||
            storage_overlap(current->gpio_events, queue_bytes, c->runtime_ports, array_bytes) ||
            storage_overlap(current->protocol.memory, bytes, current->gpio_events, queue_bytes))
            return BSP_INVALID_ARG;
        bool found = false;
        for (size_t j = 0; j < b->count; ++j)
            found |= b->ports[j].id == current->id;
        if (!found || (current->gpio_events && (current->id >> 8) != BSP_GPIO))
            return BSP_INVALID_ARG;
        if (current->gpio_events && (uintptr_t)current->gpio_events % _Alignof(BspGpioEventQueue))
            return BSP_INVALID_ARG;
        for (size_t j = 0; j < i; ++j)
        {
            const State *prior = &c->runtime_ports[j];
            size_t prior_bytes = protocol_bytes(prior->id >> 8);
            size_t prior_queue = prior->gpio_events ? sizeof(*prior->gpio_events) : 0;
            if (storage_overlap(current->protocol.memory, bytes, prior->protocol.memory,
                                prior_bytes) ||
                storage_overlap(current->protocol.memory, bytes, prior->gpio_events, prior_queue) ||
                storage_overlap(current->gpio_events, queue_bytes, prior->protocol.memory,
                                prior_bytes) ||
                storage_overlap(current->gpio_events, queue_bytes, prior->gpio_events, prior_queue))
                return BSP_INVALID_ARG;
            if ((current->protocol.memory &&
                 current->protocol.memory == c->runtime_ports[j].protocol.memory) ||
                current->id == c->runtime_ports[j].id ||
                (current->gpio_events && current->gpio_events == c->runtime_ports[j].gpio_events))
                return BSP_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < c->runtime_port_count; ++i)
    {
        State *current = &c->runtime_ports[i];
        if ((current->id >> 8) != BSP_UART)
            continue;
        Ecw6700UARTRxStorage *rx = current->protocol.uart->background;
        if (!rx)
            continue;
        if ((uintptr_t)rx % _Alignof(Ecw6700UARTRxStorage) || !storage_span(rx, sizeof(*rx)))
            return BSP_INVALID_ARG;
        if (!rx->bytes || !rx->capacity || rx->capacity > UINT_MAX ||
            !storage_span(rx->bytes, rx->capacity))
            return BSP_INVALID_ARG;
        if (storage_overlap(rx, sizeof(*rx), rx->bytes, rx->capacity) ||
            storage_overlap(rx, sizeof(*rx), c->runtime_ports,
                            c->runtime_port_count * sizeof(State)) ||
            storage_overlap(rx->bytes, rx->capacity, c->runtime_ports,
                            c->runtime_port_count * sizeof(State)))
            return BSP_INVALID_ARG;
        for (size_t j = 0; j < c->runtime_port_count; ++j)
        {
            State *other = &c->runtime_ports[j];
            size_t n = protocol_bytes(other->id >> 8);
            size_t q = other->gpio_events ? sizeof(*other->gpio_events) : 0;
            if (storage_overlap(rx, sizeof(*rx), other->protocol.memory, n) ||
                storage_overlap(rx->bytes, rx->capacity, other->protocol.memory, n) ||
                storage_overlap(rx, sizeof(*rx), other->gpio_events, q) ||
                storage_overlap(rx->bytes, rx->capacity, other->gpio_events, q))
                return BSP_INVALID_ARG;
            if (j >= i || (other->id >> 8) != BSP_UART)
                continue;
            Ecw6700UARTRxStorage *prior = other->protocol.uart->background;
            if (prior && (storage_overlap(rx, sizeof(*rx), prior, sizeof(*prior)) ||
                          storage_overlap(rx, sizeof(*rx), prior->bytes, prior->capacity) ||
                          storage_overlap(rx->bytes, rx->capacity, prior, sizeof(*prior)) ||
                          storage_overlap(rx->bytes, rx->capacity, prior->bytes, prior->capacity)))
                return BSP_INVALID_ARG;
        }
    }
    if (!ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    if (!ecw_sdk_clock_ready())
    {
        return BSP_NOT_READY;
    }
    BspRequestSync sync = {enter, leave, NULL};
#ifdef BSP_ECW_NATIVE_WIFI
    BspStatus configured =
        ecw_wifi_service_configure(c->wifi_worker_stack_bytes, c->wifi_control_stack_bytes,
                                   c->wifi_priority, c->wifi_exclusive_control);
    if (configured != BSP_OK)
    {
        return configured;
    }
#endif
#ifdef BSP_ECW_NATIVE_NET
    BspStatus net_configured = ecw_bsp_net_configure(c->net_worker_stack_bytes, c->net_priority);
    if (net_configured != BSP_OK)
    {
        return net_configured;
    }
#endif
#ifdef BSP_ECW_NATIVE_BLE
    BspStatus ble_configured = ecw_bsp_ble_service_configure(
        c->ble_worker_stack_bytes, c->ble_priority, c->ble_exclusive_control);
    if (ble_configured != BSP_OK)
    {
        return ble_configured;
    }
#endif
    BspStatus r = bsp_request_runtime_init(&sync);
    if (r != BSP_OK)
    {
        return r;
    }
    state = c->runtime_ports;
    state_count = c->runtime_port_count;
    for (size_t i = 0; i < state_count; ++i)
    {
        clear_state(&state[i]);
        state[i].index = i;
        for (size_t j = 0; j < b->count; ++j)
        {
            if (state[i].id != b->ports[j].id)
                continue;
            state[i].port = &b->ports[j];
            state[i].info = &b->board->ports[j];
            size_t bytes = protocol_bytes(state[i].id >> 8);
            if ((state[i].id >> 8) == BSP_UART)
            {
                Ecw6700UARTRxStorage *rx = state[i].protocol.uart->background;
                memset(state[i].protocol.memory, 0, bytes);
                state[i].protocol.uart->background = rx;
                if (rx)
                {
                    uint8_t *ring = rx->bytes;
                    size_t capacity = rx->capacity;
                    *rx = (Ecw6700UARTRxStorage){.bytes = ring, .capacity = capacity};
                }
            }
            else if (bytes)
                memset(state[i].protocol.memory, 0, bytes);
            switch (state[i].id >> 8)
            {
            case BSP_I2C:
                ecw_i2c_storage = state[i].protocol.i2c;
                break;
            case BSP_SPI:
                ecw_spi_storage = state[i].protocol.spi;
                break;
            case BSP_UART:
                ecw_uart_storage[state[i].port->instance] = state[i].protocol.uart;
                break;
            case BSP_I2S:
                ecw_audio_storage = state[i].protocol.i2s;
                break;
            default:
                break;
            }
            break;
        }
        if (state[i].gpio_events)
            memset(state[i].gpio_events, 0, sizeof(*state[i].gpio_events));
    }
    board = b;
    ecw_timer_board = b;
    return BSP_OK;
}
BspStatus ecw_adc_descriptor(BspPort id, const Ecw6700Port **port, uint32_t *caps)
{
    if (!board)
    {
        return BSP_NOT_READY;
    }
    int index = inventory_index(id);
    if ((id >> 8) != BSP_ADC || index < 0)
    {
        return BSP_INVALID_ARG;
    }
    *port = &board->ports[index];
    return bsp_port_capabilities(id, caps);
}

BspStatus bsp_port_capabilities(BspPort id, uint32_t *caps)
{
    if (!caps)
        return BSP_INVALID_ARG;
    if (!board)
        return BSP_NOT_READY;
    if (!board->io_composed && find(id) < 0)
        return BSP_INVALID_ARG;
    for (size_t i = 0; i < board->count; ++i)
    {
        if (board->ports[i].id != id)
            continue;
        *caps = board->board->ports[i].capabilities;
        if ((id >> 8) == BSP_ADC &&
            (!ecw_adc_available() || !ecw_adc_port_valid(&board->ports[i])))
            *caps = 0;
        if ((id >> 8) == BSP_SPI && !ecw_spi_dma_available())
            *caps &= ~(BSP_CAP_ASYNC | BSP_CAP_DISPLAY);
        if ((id >> 8) == BSP_UART && !BSP_ECW_ENABLE_DMA)
            *caps &= ~BSP_CAP_ASYNC;
        if ((id >> 8) == BSP_I2S && !ecw_audio_hw_available())
            *caps = 0;
        if ((id >> 8) == BSP_PWM && board->ports[i].instance == ECW6700_PWM_CCU6 &&
            !ecw_ccu6_available())
            *caps = 0;
        return BSP_OK;
    }
    return BSP_INVALID_ARG;
}
BspStatus bsp_port_require(BspPort id, uint32_t required, uint32_t *missing)
{
    uint32_t caps;
    if (!missing)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = bsp_port_capabilities(id, &caps);
    if (r != BSP_OK)
    {
        return r;
    }
    *missing = required & ~caps;
    return *missing ? BSP_UNSUPPORTED : BSP_OK;
}
BspStatus bsp_ecw6700_port_init(BspPort id, const Ecw6700PortConfig *cfg)
{
    int i;
    if (!cfg)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    unsigned kind = id >> 8;
    /* Reject excluded backends before validating staging or claiming resources. */
    if ((!BSP_ECW_ENABLE_DMA &&
         ((kind == BSP_SPI && cfg->spi_async_enabled) ||
          (kind == BSP_UART && cfg->uart_async_enabled))) ||
        (!BSP_ECW_ENABLE_I2S && kind == BSP_I2S))
        return BSP_UNSUPPORTED;
    if ((kind != BSP_SPI && (cfg->spi_mode || cfg->spi_bit_order)) ||
        cfg->spi_mode > 3 || cfg->spi_bit_order > 1)
        return BSP_INVALID_ARG;
    if (kind != BSP_UART && (cfg->uart_data_bits || cfg->uart_parity || cfg->uart_stop_bits))
        return BSP_INVALID_ARG;
    if (kind == BSP_UART)
    {
        uint32_t lcr;
        BspStatus framing = ecw_uart_format(cfg, &lcr);
        if (framing != BSP_OK) return framing;
    }
    if (cfg->i2c_async_enabled && kind != BSP_I2C)
    {
        return BSP_INVALID_ARG;
    }
    if (cfg->i2c_async_enabled &&
        ((*state[i].info).capabilities & (BSP_CAP_ASYNC | BSP_CAP_RESTART)) !=
            (BSP_CAP_ASYNC | BSP_CAP_RESTART))
    {
        return BSP_UNSUPPORTED;
    }
    bool spi_storage =
        cfg->spi_dma_tx_buffer || cfg->spi_dma_rx_buffer || cfg->spi_dma_buffer_bytes;
    if ((kind != BSP_SPI && (cfg->spi_async_enabled || spi_storage)) ||
        (!cfg->spi_async_enabled && spi_storage))
    {
        return BSP_INVALID_ARG;
    }
    if (kind == BSP_SPI && cfg->spi_async_enabled)
    {
        if (!ecw_spi_dma_storage_valid(cfg->spi_dma_tx_buffer, cfg->spi_dma_buffer_bytes) ||
            (cfg->spi_dma_rx_buffer &&
             !ecw_spi_dma_storage_valid(cfg->spi_dma_rx_buffer, cfg->spi_dma_buffer_bytes)))
        {
            return BSP_INVALID_ARG;
        }
        uintptr_t a = (uintptr_t)cfg->spi_dma_tx_buffer, b = (uintptr_t)cfg->spi_dma_rx_buffer;
        if (b && (a <= b ? b - a : a - b) < cfg->spi_dma_buffer_bytes)
        {
            return BSP_INVALID_ARG;
        }
        if (!ecw_spi_dma_available() || !((*state[i].info).capabilities & BSP_CAP_ASYNC))
        {
            return BSP_UNSUPPORTED;
        }
    }
    if ((kind != BSP_UART &&
         (cfg->uart_async_enabled || cfg->uart_dma_buffer || cfg->uart_dma_buffer_bytes)) ||
        (kind == BSP_UART && !cfg->uart_async_enabled &&
         (cfg->uart_dma_buffer || cfg->uart_dma_buffer_bytes)) ||
        (kind == BSP_UART && cfg->uart_async_enabled &&
         !ecw_uart_dma_storage_valid(cfg->uart_dma_buffer, cfg->uart_dma_buffer_bytes)))
    {
        return BSP_INVALID_ARG;
    }
    if (kind != BSP_GPIO && kind != BSP_SPI && kind != BSP_I2S && !basic_kind(kind))
    {
        return BSP_UNSUPPORTED;
    }
    if (!((*state[i].info).capabilities & (kind == BSP_I2S ? BSP_CAP_STREAM : BSP_CAP_BLOCKING)))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_I2S &&
        (!ecw_audio_hw_available() || cfg->rate_hz < 8000 || cfg->rate_hz > 48000))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_ADC &&
        (cfg->rate_hz || !ecw_adc_available() || !ecw_adc_port_valid(state[i].port)))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_GPIO && cfg->rate_hz)
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_SPI &&
        (!cfg->rate_hz || cfg->rate_hz > ECW_SPI_INPUT_HZ || ECW_SPI_INPUT_HZ % cfg->rate_hz ||
         ECW_SPI_INPUT_HZ / cfg->rate_hz > 255u))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_I2C && !ecw_i2c_timing_setup(cfg->rate_hz))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_UART && (cfg->rate_hz < 1200 || cfg->rate_hz > 2000000))
    {
        return BSP_UNSUPPORTED;
    }
    if (kind == BSP_PWM)
    {
        uint32_t unused;
        if ((*state[i].port).instance == ECW6700_PWM_CCU6
                ? (!ecw_ccu6_available() || !cfg->rate_hz || cfg->rate_hz > 20000000u)
                : ecw_pwm_counts(cfg->rate_hz, 5000, &unused) != BSP_OK)
        {
            return BSP_UNSUPPORTED;
        }
    }
    if (!ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = ecw_sdk_lock();
    const Ecw6700Port *p = &(*state[i].port);
    if (state[i].busy)
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    if (state[i].held)
    {
        r = state[i].fault ? BSP_IO : BSP_INVALID_STATE;
        ecw_sdk_unlock(key);
        return r;
    }
    for (size_t j = 0; j < state_count; ++j)
    {
        if (state[j].held)
        {
            if ((kind == BSP_SPI || kind == BSP_I2C || kind == BSP_UART || kind == BSP_I2S || kind == BSP_ADC ||
                 (kind == BSP_PWM && p->instance == ECW6700_PWM_CCU6)) &&
                ((*state[j].port).id >> 8) == kind && (*state[j].port).instance == p->instance)
            {
                ecw_sdk_unlock(key);
                return BSP_BUSY;
            }
            for (size_t k = 0; k < (*state[j].port).pin_count; ++k)
            {
                for (size_t z = 0; z < p->pin_count; ++z)
                {
                    if ((*state[j].port).pins[k] == p->pins[z] ||
                        (kind == BSP_PWM && p->instance != ECW6700_PWM_CCU6 &&
                         (*state[j].port).instance != ECW6700_PWM_CCU6 &&
                         ((*state[j].port).id >> 8) == BSP_PWM &&
                         (*state[j].port).pins[k] % 4 == p->pins[z] % 4))
                    {
                        ecw_sdk_unlock(key);
                        return BSP_BUSY;
                    }
                }
            }
        }
    }
    state[i].held = true;
    state[i].busy = true;
    ecw_sdk_unlock(key);
    if (kind == BSP_GPIO)
    {
        key = ecw_sdk_lock();
    }
    int rc = kind == BSP_GPIO   ? ecw_sdk_gpio_open_pull(p->pins[0], p->output, p->initial_high, (unsigned)p->gpio_pull)
             : kind == BSP_SPI  ? ecw_spi_open(p, cfg)
             : kind == BSP_I2S  ? ecw_audio_open(p, cfg->rate_hz)
             : kind == BSP_UART ? ecw_uart_open(p, cfg)
             : kind == BSP_I2C  ? ecw_i2c_open(p, cfg)
             : kind == BSP_ADC  ? ecw_adc_open(p)
                                : ecw_basic_open(p, cfg->rate_hz);
    if (kind == BSP_GPIO)
    {
        ecw_sdk_unlock(key);
    }
    key = ecw_sdk_lock();
    if ((kind == BSP_UART && rc == ECW_UART_RESOURCE_CONFLICT) ||
        (kind == BSP_SPI && rc == ECW_SPI_RESOURCE_CONFLICT))
    {
        clear_state(&state[i]);
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    r = sdk_result(i, rc);
    if (kind == BSP_PWM && p->instance == ECW6700_PWM_CCU6 && rc == ECW_CCU6_CONFIG_MISMATCH)
    {
        state[i].error.native_valid = false;
        state[i].error.native_domain = 0;
        state[i].error.native_code = 0;
    }
    state[i].ready = (r == BSP_OK);
    state[i].busy = false;
    ecw_sdk_unlock(key);
    return r;
}
static BspStatus port_close(BspPort id, bool recover)
{
    if (recover && !ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    int i;
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    unsigned kind = id >> 8;
    if (kind != BSP_GPIO && kind != BSP_SPI && kind != BSP_I2S && !basic_kind(kind))
    {
        return BSP_UNSUPPORTED;
    }
    if (recover && kind != BSP_SPI && kind != BSP_I2C && kind != BSP_UART && kind != BSP_ADC)
    {
        return BSP_UNSUPPORTED;
    }
    if (!ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = ecw_sdk_lock();
    if (state[i].busy || (state[i].gpio_events && state[i].gpio_events->held))
    {
        ecw_sdk_unlock(key);
        return recover ? BSP_INVALID_STATE : BSP_BUSY;
    }
    if (recover && !state[i].fault)
    {
        ecw_sdk_unlock(key);
        return BSP_INVALID_STATE;
    }
    if (!state[i].held)
    {
        ecw_sdk_unlock(key);
        return BSP_NOT_READY;
    }
    if (kind == BSP_UART && !recover && ecw_uart_rx_active((*state[i].port).instance))
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    BspError original = state[i].error;
    state[i].busy = true;
    ecw_sdk_unlock(key);
    const Ecw6700Port *p = &(*state[i].port);
    if (kind == BSP_GPIO)
    {
        key = ecw_sdk_lock();
    }
    int rc = kind == BSP_GPIO   ? ecw_sdk_gpio_close(p->pins[0])
             : kind == BSP_SPI  ? ecw_spi_close(p)
             : kind == BSP_I2S  ? ecw_audio_close(p)
             : kind == BSP_UART ? ecw_uart_close(p)
             : kind == BSP_I2C  ? ecw_i2c_close(p)
             : kind == BSP_ADC  ? ecw_adc_close(p)
                                : ecw_basic_close(p);
    if (kind == BSP_GPIO)
    {
        ecw_sdk_unlock(key);
    }
    key = ecw_sdk_lock();
    r = sdk_result(i, rc);
    if (r == BSP_OK)
    {
        clear_state(&state[i]);
    }
    else
    {
        state[i].busy = false;
    }
    /* Like F407 recover: return cleanup status, retain the triggering error. */
    if (recover)
    {
        state[i].error = original;
    }
    ecw_sdk_unlock(key);
    return r;
}
BspStatus bsp_port_deinit(BspPort id)
{
    return port_close(id, false);
}
BspStatus bsp_spi_transfer(BspPort id, const uint8_t *tx, uint8_t *rx, size_t n,
                           uint32_t timeout_ms)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    if (!tx || !n || n > UINT16_MAX || timeout_ms == UINT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    int i;
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    if ((id >> 8) != BSP_SPI)
    {
        return BSP_INVALID_ARG;
    }
    if (!((*state[i].info).capabilities & BSP_CAP_BLOCKING))
    {
        return BSP_UNSUPPORTED;
    }
    if (n > 512)
    {
        return BSP_UNSUPPORTED;
    }
    if (rx && rx != tx)
    {
        uintptr_t a = (uintptr_t)tx, b = (uintptr_t)rx;
        if ((a < b ? b - a : a - b) < n)
        {
            return BSP_UNSUPPORTED;
        }
    }
    uintptr_t key = ecw_sdk_lock();
    if (state[i].busy)
    {
        r = BSP_BUSY;
    }
    else if (state[i].fault)
    {
        r = BSP_IO;
    }
    else if (!state[i].ready)
    {
        r = BSP_NOT_READY;
    }
    else
    {
        r = BSP_OK;
    }
    if (r != BSP_OK)
    {
        ecw_sdk_unlock(key);
        return r;
    }
    state[i].busy = true;
    ecw_sdk_unlock(key);
    EcwSpiPollResult out = ecw_sdk_spi_transfer((*state[i].port).instance, tx, rx, n, timeout_ms);
    key = ecw_sdk_lock();
    state[i].busy = false;
    state[i].fault |= out.fault;
    state[i].error = (BspError){.code = out.status,
                                .native_valid = out.native_status != 0,
                                .native_domain = 0x45435753u,
                                .native_code = (int32_t)out.native_status};
    ecw_sdk_unlock(key);
    return out.status;
}
BspStatus bsp_port_error(BspPort id, BspError *out)
{
    int i;
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = lookup(id, &i);
    if (r == BSP_NOT_READY && board && inventory_index(id) >= 0)
    {
        /* An unselected port has never borrowed state or recorded an error. */
        *out = (BspError){0};
        return BSP_OK;
    }
    if (r != BSP_OK)
    {
        return r;
    }
    uintptr_t key = ecw_sdk_lock();
    *out = state[i].error;
    ecw_sdk_unlock(key);
    return BSP_OK;
}
static BspStatus gpio(BspPort id, bool write, bool *value)
{
    int i;
    if (!value)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    if ((id >> 8) != BSP_GPIO)
    {
        return BSP_INVALID_ARG;
    }
    if (!ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = ecw_sdk_lock();
    const Ecw6700Port *p = &(*state[i].port);
    if (state[i].busy)
    {
        r = BSP_BUSY;
    }
    else if (state[i].fault)
    {
        r = BSP_IO;
    }
    else if (!state[i].ready)
    {
        r = BSP_NOT_READY;
    }
    else if (write != p->output)
    {
        r = BSP_UNSUPPORTED; /* No output-latch masquerading as pad read. */
    }
    else if (write)
    {
        r = sdk_result(i, ecw_sdk_gpio_write(p->pins[0], *value));
    }
    else
    {
        unsigned v = 0;
        r = sdk_result(i, ecw_sdk_gpio_read(p->pins[0], &v));
        if (r == BSP_OK)
        {
            *value = v != 0;
        }
    }
    ecw_sdk_unlock(key);
    return r;
}
BspStatus bsp_gpio_read(BspPort id, bool *high)
{
    return gpio(id, false, high);
}
BspStatus bsp_gpio_write(BspPort id, bool high)
{
    return gpio(id, true, &high);
}
uint32_t bsp_time_ms(void)
{
    return board ? ecw_sdk_time_ms() : 0;
}
BspStatus bsp_delay_ms(uint32_t ms)
{
    if (ms > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (ecw_sdk_in_isr())
    {
        return BSP_INVALID_STATE;
    }
    if (!board || !ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    return !ms || !ecw_sdk_delay_ms(ms) ? BSP_OK : BSP_IO;
}
BspStatus bsp_service(size_t *n)
{
    if (!n)
    {
        return BSP_INVALID_ARG;
    }
    if (!board)
    {
        return BSP_NOT_READY;
    }
    *n = 0;
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    BspStatus status = ecw_uart_dispatch(n);
    if (status == BSP_OK)
    {
        status = ecw_spi_dispatch(n);
    }
    return status == BSP_OK ? ecw_i2c_dispatch(n) : status;
}
BspStatus ecw_unsupported(BspPort id, BspKind kind)
{
    int i;
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    return (id >> 8) == (unsigned)kind ? BSP_UNSUPPORTED : BSP_INVALID_ARG;
}
BspStatus ecw_reject_request(BspPort id, BspKind kind, BspRequest *req)
{
    if (!req)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = ecw_unsupported(id, kind);
    if (r != BSP_UNSUPPORTED)
    {
        return r;
    }
    BspRequestSnapshot snapshot;
    r = bsp_request_query(req, &snapshot);
    if (r != BSP_OK)
    {
        return r;
    }
    return snapshot.state == BSP_REQ_READY ? BSP_UNSUPPORTED : BSP_INVALID_STATE;
}
BspStatus bsp_port_recover(BspPort id)
{
    return port_close(id, true);
}

BspStatus ecw_claim_basic(BspPort id, BspKind kind, const Ecw6700Port **out)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    int i;
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    if ((id >> 8) != (unsigned)kind)
    {
        return BSP_INVALID_ARG;
    }
    if (!((*state[i].info).capabilities & BSP_CAP_BLOCKING))
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t k = ecw_sdk_lock();
    r = state[i].busy     ? BSP_BUSY
        : state[i].fault  ? BSP_IO
        : !state[i].ready ? BSP_NOT_READY
                          : BSP_OK;
    if (r == BSP_OK)
    {
        state[i].busy = true;
        *out = &(*state[i].port);
    }
    ecw_sdk_unlock(k);
    return r;
}
/* Caller holds the BSP lock; cached RX reads do not claim the TX request. */
BspStatus ecw_uart_inspect(BspPort id, const Ecw6700Port **port, bool *ready)
{
    int i;
    BspStatus status = lookup(id, &i);
    if (status != BSP_OK)
    {
        return status;
    }
    if ((id >> 8) != BSP_UART)
    {
        return BSP_INVALID_ARG;
    }
    if (!((*state[i].info).capabilities & BSP_CAP_BACKGROUND_RX))
    {
        return BSP_UNSUPPORTED;
    }
    *port = &(*state[i].port);
    *ready = state[i].ready;
    return BSP_OK;
}

BspStatus ecw_finish_basic(BspPort id, EcwIoResult result)
{
    int i = find(id);
    uintptr_t k = ecw_sdk_lock();
    if ((id >> 8) == BSP_UART)
    {
        result = ecw_uart_rx_result((*state[i].port).instance, result);
    }
    state[i].busy = false;
    state[i].fault |= result.fault;
    state[i].error = (BspError){.code = result.status,
                                .native_valid = result.native_status != 0,
                                .native_domain = 0x45430000u | (id >> 8),
                                .native_code = result.native_status};
    ecw_sdk_unlock(k);
    return result.status;
}

void ecw_record_async(BspPort id, EcwIoResult result)
{
    int i = find(id);
    uintptr_t key = ecw_sdk_lock();
    state[i].fault |= result.fault;
    state[i].error = (BspError){.code = result.status,
                                .native_valid = result.native_status != 0,
                                .native_domain = 0x45430000u | (id >> 8),
                                .native_code = result.native_status};
    ecw_sdk_unlock(key);
}

/* Stream keeps the same port/pin ownership as blocking operations. */
BspStatus ecw_stream_check(BspPort id, bool claim)
{
    int i;
    BspStatus r = lookup(id, &i);
    if (r != BSP_OK)
    {
        return r;
    }
    if ((id >> 8) != BSP_I2S)
    {
        return BSP_INVALID_ARG;
    }
    if (!((*state[i].info).capabilities & BSP_CAP_STREAM) || !ecw_audio_hw_available())
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t k = ecw_sdk_lock();
    r = !state[i].ready ? BSP_NOT_READY : BSP_OK;
    if (claim && r == BSP_OK)
    {
        r = state[i].busy ? BSP_BUSY : state[i].fault ? BSP_IO : BSP_OK;
        if (r == BSP_OK)
        {
            state[i].busy = true;
        }
    }
    ecw_sdk_unlock(k);
    return r;
}
void ecw_stream_finish(BspPort id, BspError error, bool release)
{
    int i = find(id);
    uintptr_t k = ecw_sdk_lock();
    if (i >= 0)
    {
        state[i].error = error;
        state[i].fault |= error.code == BSP_IO || error.code == BSP_TIMEOUT;
        if (release)
        {
            state[i].busy = false;
        }
    }
    ecw_sdk_unlock(k);
}

/* Single core: caller task holds IRQ lock across subscription mutation. SDK
 * callback storage is copied and remains static; stop clears pending before reuse. */
static BspStatus edge_lookup(BspPort id, int *i, const Ecw6700Port **port)
{
    if (ecw_sdk_in_isr())
    {
        return BSP_INVALID_STATE;
    }
    if (!board)
    {
        return BSP_NOT_READY;
    }
    int fixed = inventory_index(id);
    if (fixed < 0)
    {
        return BSP_INVALID_ARG;
    }
    if (!ecw_sdk_thread_ready())
    {
        return BSP_NOT_READY;
    }
    if ((id >> 8) != BSP_GPIO)
    {
        return BSP_INVALID_ARG;
    }
    *port = &board->ports[fixed];
    *i = find(id);
    return BSP_OK;
}
static void edge_callback(void *context)
{
    size_t i = *(size_t *)context;
    uintptr_t k = ecw_sdk_lock();
    unsigned high = 0;
    int native = ecw_sdk_gpio_read((*state[i].port).pins[0], &high);
    if (native)
    {
        state[i].fault = true;
        state[i].error = (BspError){.code = BSP_IO,
                                    .native_valid = true,
                                    .native_domain = 0x45434750u,
                                    .native_code = native};
        bsp_gpio_queue_pause(state[i].gpio_events);
    }
    else
    {
        bsp_gpio_queue_push(state[i].gpio_events, state[i].gpio_events->generation, bsp_time_ms(),
                            high != 0);
    }
    ecw_sdk_unlock(k);
}
BspStatus bsp_gpio_event_capabilities(BspPort id, BspGpioEventCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    int i;
    const Ecw6700Port *port;
    BspStatus r = edge_lookup(id, &i, &port);
    if (r != BSP_OK)
    {
        return r;
    }
    bool input = !port->output;
    *out = (BspGpioEventCapabilities){input ? 3u : 0u, 2000u, input ? BSP_GPIO_EVENT_DEPTH : 0u};
    return BSP_OK;
}
BspStatus bsp_gpio_event_start(BspPort id, uint32_t edges, uint64_t *generation)
{
    if (!generation || !edges || (edges & ~3u))
    {
        return BSP_INVALID_ARG;
    }
    int i;
    const Ecw6700Port *port;
    BspStatus r = edge_lookup(id, &i, &port);
    if (r != BSP_OK)
    {
        return r;
    }
    if (port->output)
    {
        return BSP_UNSUPPORTED;
    }
    if (i < 0)
    {
        return BSP_NOT_READY;
    }
    if (!state[i].gpio_events)
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t k = ecw_sdk_lock();
    if (state[i].busy || (state[i].gpio_events && state[i].gpio_events->held))
    {
        r = BSP_BUSY;
    }
    else if (state[i].fault)
    {
        r = BSP_IO;
    }
    else if (!state[i].ready)
    {
        r = BSP_NOT_READY;
    }
    else
    {
        r = BSP_OK;
    }
    if (r != BSP_OK)
    {
        ecw_sdk_unlock(k);
        return r;
    }
    uint64_t g;
    r = bsp_gpio_queue_start(state[i].gpio_events, &edge_serial, &g);
    if (r != BSP_OK)
    {
        ecw_sdk_unlock(k);
        return r;
    }
    state[i].index = (size_t)i;
    state[i].busy = true;
    int native =
        ecw_gpio_irq_start((*state[i].port).pins[0], edges, edge_callback, &state[i].index);
    if (native)
    {
        bsp_gpio_queue_pause(state[i].gpio_events);
        if (!ecw_gpio_irq_stop((*state[i].port).pins[0]))
        {
            bsp_gpio_queue_retire(state[i].gpio_events);
        }
    }
    r = sdk_result(i, native);
    state[i].busy = false;
    if (r == BSP_OK)
    {
        *generation = g;
    }
    ecw_sdk_unlock(k);
    return r;
}
BspStatus bsp_gpio_event_read(BspPort id, BspGpioEvent *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    int i;
    const Ecw6700Port *port;
    BspStatus r = edge_lookup(id, &i, &port);
    if (r != BSP_OK)
    {
        return r;
    }
    if (i < 0 || !state[i].gpio_events)
    {
        return BSP_NOT_READY;
    }
    uintptr_t k = ecw_sdk_lock();
    r = bsp_gpio_queue_read(state[i].gpio_events, out, capacity, count);
    ecw_sdk_unlock(k);
    return r;
}
BspStatus bsp_gpio_event_query(BspPort id, BspGpioEventState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    int i;
    const Ecw6700Port *port;
    BspStatus r = edge_lookup(id, &i, &port);
    if (r != BSP_OK)
    {
        return r;
    }
    if (i < 0 || !state[i].gpio_events)
    {
        *out = (BspGpioEventState){0};
        return BSP_OK;
    }
    uintptr_t k = ecw_sdk_lock();
    r = bsp_gpio_queue_query(state[i].gpio_events, out);
    ecw_sdk_unlock(k);
    return r;
}
BspStatus bsp_gpio_event_stop(BspPort id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    int i;
    const Ecw6700Port *port;
    BspStatus r = edge_lookup(id, &i, &port);
    if (r != BSP_OK)
    {
        return r;
    }
    if (i < 0 || !state[i].gpio_events)
    {
        return BSP_OK;
    }
    uintptr_t k = ecw_sdk_lock();
    if (!(state[i].gpio_events && state[i].gpio_events->held))
    {
        ecw_sdk_unlock(k);
        return BSP_OK;
    }
    if (state[i].busy)
    {
        ecw_sdk_unlock(k);
        return BSP_BUSY;
    }
    state[i].busy = true;
    bsp_gpio_queue_pause(state[i].gpio_events);
    int native = ecw_gpio_irq_stop((*state[i].port).pins[0]);
    if (!native)
    {
        bsp_gpio_queue_retire(state[i].gpio_events);
    }
    r = sdk_result(i, native);
    state[i].busy = false;
    ecw_sdk_unlock(k);
    return r;
}

/* Fixed SPI + CS + optional DC reservation; no generic resource framework.
 * Validate every participant before changing any busy bit or touching hardware. */
BspStatus ecw_claim_spi_plan(const BspDisplayBinding *binding, bool display,
                             const Ecw6700Port **ports)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    BspPort ids[3] = {binding->spi, binding->cs, binding->dc};
    int indices[3];
    size_t count = binding->dc == BSP_PORT_NONE ? 2 : 3;
    uintptr_t key = ecw_sdk_lock();
    BspStatus status = BSP_OK;
    for (size_t i = 0; i < count; ++i)
    {
        status = lookup(ids[i], &indices[i]);
        if (status != BSP_OK)
        {
            break;
        }
        int index = indices[i];
        const Ecw6700Port *port = &(*state[index].port);
        if ((ids[i] >> 8) != (i ? BSP_GPIO : BSP_SPI))
        {
            status = BSP_INVALID_ARG;
            break;
        }
        uint32_t required = i ? BSP_CAP_BLOCKING : BSP_CAP_ASYNC | (display ? BSP_CAP_DISPLAY : 0u);
        if (((*state[index].info).capabilities & required) != required || (i && !port->output) ||
            (!i && !ecw_spi_dma_available()))
        {
            status = BSP_UNSUPPORTED;
            break;
        }
        status = state[index].busy     ? BSP_BUSY
                 : state[index].fault  ? BSP_IO
                 : !state[index].ready ? BSP_NOT_READY
                                       : BSP_OK;
        if (status != BSP_OK)
        {
            break;
        }
    }
    if (status == BSP_OK)
    {
        for (size_t i = 0; i < count; ++i)
        {
            state[indices[i]].busy = true;
            /* Runtime selection/order is independent of the fixed Board table. */
            ports[i] = state[indices[i]].port;
        }
        if (count == 2)
        {
            ports[2] = NULL;
        }
    }
    ecw_sdk_unlock(key);
    return status;
}

void ecw_unclaim_spi_plan(const Ecw6700Port **ports)
{
    uintptr_t key = ecw_sdk_lock();
    for (size_t i = 0; i < 3; ++i)
    {
        if (ports[i])
        {
            int index = find(ports[i]->id);
            state[index].busy = false;
        }
    }
    ecw_sdk_unlock(key);
}
