/* Finite-DMA adapter using the reviewed SDK four-wire initialization extension.
 * Only private IRAM storage is submitted to DMA. Native DMA dispatcher owns its
 * vector. Board-selected pins are configured without an external MCLK output.
 * Target waveform, sustained load and acoustics still require hardware tests. */
#include "ecw_audio.h"
#include "ecw_sdk.h"
#include "chip_memmap.h"
#include "ecw_clock_profile.h"
#include "dma.h"
#include "i2s_ll.h"
#include "oshal.h"
#include "ecw_sdk_version.h"
#include <string.h>
#ifndef MOCE_P16_I2S_FOUR_WIRE_API
#error "Four-wire I2S extension missing; prepare the selected SDK as documented in integration/SDK_COMPATIBILITY.md"
#endif

typedef struct
{
    uint32_t tx[1024];
    uint32_t rx[1024];
} AudioSlab;

enum
{
    AUDIO_TX = 0,
    AUDIO_RX = 1,
    AUDIO_OPEN_UNAVAILABLE = -110,
    AUDIO_NO_STORAGE = -111,
    AUDIO_STORAGE_RANGE = -112,
    AUDIO_STOP_PENDING = -120,
    AUDIO_CLOSE_BUSY = -121,
    AUDIO_CFG_FAILED = -122,
    AUDIO_LATE_CALLBACK = -123,
    AUDIO_SEM_FAILED = -124
};

static AudioSlab *slab;
static os_sem_handle_t stop_sem;
static int channels[2] = {-1, -1};
static bool in_flight[2], dma_faulted[2];
/* MOCE-SYNC: audio abort retirement | ../AUDIO_LIFECYCLE.md */
static bool abort_requested[2], abort_seen[2];
static unsigned next_half[2];
static uint32_t rate, period_ms, last_ms[2];
static size_t halfwords;
static bool opened, duplex, draining;
static EcwAudioEvent notify;
static int32_t hardware_error;

bool ecw_audio_hw_available(void)
{
    return ecw_clock_profile_ready();
}

int ecw_audio_hw_open(const Ecw6700Port *p, uint32_t hz)
{
    if (opened || !p || !p->pins || p->pin_count != 4 || p->pins[0] != 6 || p->pins[1] != 7 ||
        p->pins[2] != 8 || p->pins[3] != 3 || hz < 8000 || hz > 48000 || !ecw_audio_hw_available())
    {
        return AUDIO_OPEN_UNAVAILABLE;
    }
    /* P12 interprets the second argument as a region selector, not an address.
     * STRICT prevents fallback into a region the DMA engine cannot reach.
     * The actual address is still checked below before any DMA ownership. */
#if BSP_ECW_SDK_B02P12
    slab = os_malloc_assign(sizeof(*slab), RAM_TYPE_OUT_CPU_IRAM_STRICT);
#else
    slab = os_malloc_assign(sizeof(*slab), MEM_BASE_RAM0);
#endif
    if (!slab)
    {
        return AUDIO_NO_STORAGE;
    }
    uintptr_t address = (uintptr_t)slab;
    if (address % 4 || address < MEM_BASE_RAM0 || address > IRAM_END_ADDR - sizeof(*slab))
    {
        os_free(slab);
        slab = NULL;
        return AUDIO_STORAGE_RANGE;
    }
    stop_sem = os_sem_create(1, 0);
    if (!stop_sem)
    {
        os_free(slab);
        slab = NULL;
        return AUDIO_SEM_FAILED;
    }
    int result = drv_i2s_bus_init_four_wire(p->pins[0], p->pins[1], p->pins[2], p->pins[3]);
    if (result)
    {
        os_sem_destroy(stop_sem);
        stop_sem = NULL;
        os_free(slab);
        slab = NULL;
        return result;
    }
    memset(slab, 0, sizeof(*slab));
    rate = hz;
    opened = true;
    return 0;
}

static bool idle(void)
{
    return !in_flight[AUDIO_TX] && !in_flight[AUDIO_RX];
}

static void signal_drained(void)
{
    if (draining && idle())
    {
        /* P16 os_sem_post selects the native ISR-safe implementation. A failed
         * wake never proves completion: stop also checks both in-flight flags. */
        (void)os_sem_post(stop_sem);
    }
}

int32_t ecw_audio_hw_error(void)
{
    uintptr_t key = ecw_sdk_lock();
    int32_t error = hardware_error;
    ecw_sdk_unlock(key);
    return error;
}

static void fault(int32_t error)
{
    if (!hardware_error)
    {
        hardware_error = error;
    }
    EcwAudioEvent callback = notify;
    draining = true;
    notify = NULL;
    if (callback)
    {
        callback(0, error);
    }
    signal_drained();
}

static int arm(unsigned direction, unsigned half)
{
    bool receive = direction == AUDIO_RX;
    uint8_t *buffer = (uint8_t *)(receive ? slab->rx : slab->tx) + half * halfwords * 2u;
    /* drv_dma_cfg MUTATES len from bytes to transfer units. Rebuild a fresh
     * local configuration on every arm; reusing it would shrink later blocks. */
    T_DMA_CFG_INFO cfg = {
        .src = receive ? drv_i2s_reg_rx_dma_addr() : (unsigned int)(uintptr_t)buffer,
        .dst = receive ? (unsigned int)(uintptr_t)buffer : drv_i2s_reg_tx_dma_addr(),
        .len = (unsigned int)(halfwords * 2u),
        .mode = receive ? DMA_CHN_I2S_RX : DMA_CHN_I2S_TX,
        .dst_trans_byte_size = duplex ? CHN_WIDTH_WORLD : CHN_WIDTH_HALF_WORLD,
        .src_trans_byte_size = duplex ? CHN_WIDTH_WORLD : CHN_WIDTH_HALF_WORLD};
    int result = drv_dma_cfg(channels[direction], &cfg);
    if (result)
    {
        return result;
    }
    next_half[direction] = half;
    in_flight[direction] = true;
    drv_dma_start(channels[direction]);
    return 0;
}

static void completed(unsigned direction)
{
    uintptr_t key = ecw_sdk_lock();
    if (!in_flight[direction] || dma_faulted[direction])
    {
        ecw_sdk_unlock(key);
        return;
    }
    in_flight[direction] = false;
    unsigned half = next_half[direction];
    uint32_t now = ecw_sdk_time_ms();
    if (!draining && (uint32_t)(now - last_ms[direction]) >= period_ms * 2u)
    {
        fault(AUDIO_LATE_CALLBACK);
    }
    last_ms[direction] = now;
    /* During duplex stop keep feeding TX until the last RX block completes:
     * RX consumes TX's wire clocks. Then let the final TX block finish too. */
    bool keep_tx_clock = draining && duplex && direction == AUDIO_TX && in_flight[AUDIO_RX] &&
                         !dma_faulted[AUDIO_TX] && !dma_faulted[AUDIO_RX];
    if (!draining || keep_tx_clock)
    {
        if (arm(direction, half ^ 1u))
        {
            fault(AUDIO_CFG_FAILED);
        }
    }
    EcwAudioEvent callback = !draining && (direction == AUDIO_RX || !duplex) ? notify : NULL;
    ecw_sdk_unlock(key);
    /* Native dispatcher invokes this in DMA ISR. Next DMA block is already
     * armed; only the completed half is exposed to the upper audio adapter. */
    if (callback)
    {
        callback(half, 0);
    }
    key = ecw_sdk_lock();
    signal_drained();
    ecw_sdk_unlock(key);
}

static void dma_event(unsigned direction, unsigned int events)
{
    uintptr_t key = ecw_sdk_lock();
    if (!in_flight[direction])
    {
        ecw_sdk_unlock(key);
        return;
    }
    if (abort_requested[direction] && (events & DRV_DMA_EVENT_ABORT))
    {
        /* Notification alone is insufficient: stop also reads ChEN before
         * retiring the channel. A late TC never substitutes for this event. */
        abort_seen[direction] = true;
        (void)os_sem_post(stop_sem);
        if (!(events & DRV_DMA_EVENT_ERROR))
        {
            ecw_sdk_unlock(key);
            return; /* A requested abort is cleanup, not a new data error. */
        }
    }
    if (dma_faulted[direction])
    {
        ecw_sdk_unlock(key);
        return;
    }
    if (events & (DRV_DMA_EVENT_ERROR | DRV_DMA_EVENT_ABORT))
    {
        uint32_t bit = 1u << (unsigned)channels[direction];
        uint32_t raw = ((events & DRV_DMA_EVENT_TC) ? bit << 16 : 0) |
                       ((events & DRV_DMA_EVENT_ABORT) ? bit << 8 : 0) |
                       ((events & DRV_DMA_EVENT_ERROR) ? bit : 0);
        /* An error/abort notification is not a retirement barrier. Keep this
         * channel and its slab half leased; a later TC cannot erase the fault.
         * Stop rearming the other direction too, rather than keeping TX clocks
         * alive forever while a faulted RX can no longer prove completion. */
        dma_faulted[direction] = true;
        fault((int32_t)raw);
        ecw_sdk_unlock(key);
        return;
    }
    ecw_sdk_unlock(key);
    if (events == DRV_DMA_EVENT_TC)
    {
        completed(direction);
    }
}

static void tx_completed(void *unused, unsigned int events)
{
    (void)unused;
    dma_event(AUDIO_TX, events);
}

static void rx_completed(void *unused, unsigned int events)
{
    (void)unused;
    dma_event(AUDIO_RX, events);
}

static void release_channels(void)
{
    for (unsigned direction = 0; direction < 2; ++direction)
    {
        if (channels[direction] >= 0)
        {
            drv_dma_event_register(channels[direction], NULL, NULL);
            drv_dma_ch_release(channels[direction]);
            channels[direction] = -1;
        }
    }
    notify = NULL;
}

BspStatus ecw_audio_hw_prepare(bool both, size_t n)
{
    if (!opened || channels[AUDIO_TX] >= 0 || channels[AUDIO_RX] >= 0)
    {
        return BSP_BUSY;
    }
    if (n < 128 || n > ECW_AUDIO_MAX_HALFWORDS || n % (both ? 8u : 4u))
    {
        return BSP_UNSUPPORTED;
    }
    uint32_t ms = (uint32_t)((n / (both ? 8u : 4u)) * 1000u / rate);
    if (ms < 2)
    {
        return BSP_UNSUPPORTED;
    }
    duplex = both;
    halfwords = n / 2u;
    period_ms = ms;
    draining = false;
    hardware_error = 0;
    memset(in_flight, 0, sizeof(in_flight));
    memset(dma_faulted, 0, sizeof(dma_faulted));
    memset(abort_requested, 0, sizeof(abort_requested));
    memset(abort_seen, 0, sizeof(abort_seen));
    uintptr_t key = ecw_sdk_lock();
    channels[AUDIO_TX] = drv_dma_ch_alloc();
    if (channels[AUDIO_TX] < 0)
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    if (both)
    {
        channels[AUDIO_RX] = drv_dma_ch_alloc();
        if (channels[AUDIO_RX] < 0)
        {
            release_channels();
            ecw_sdk_unlock(key);
            return BSP_BUSY;
        }
    }
    drv_dma_event_register(channels[AUDIO_TX], tx_completed, NULL);
    if (both)
    {
        drv_dma_event_register(channels[AUDIO_RX], rx_completed, NULL);
    }
    ecw_sdk_unlock(key);
    return BSP_OK;
}

void ecw_audio_hw_write(unsigned half, const uint16_t *data)
{
    if (duplex)
    {
        uint32_t *out = slab->tx + half * (halfwords / 2u);
        for (size_t i = 0; i < halfwords / 2u; ++i)
        {
            out[i] = ((uint32_t)data[i * 2u] << 16) | data[i * 2u + 1u];
        }
    }
    else
    {
        memcpy((uint16_t *)slab->tx + half * halfwords, data, halfwords * 2u);
    }
}

void ecw_audio_hw_read(unsigned half, uint16_t *data)
{
    const uint32_t *in = slab->rx + half * (halfwords / 2u);
    for (size_t i = 0; i < halfwords / 2u; ++i)
    {
        uint32_t word = in[i];
        data[i * 2u] = (uint16_t)(word >> 16);
        data[i * 2u + 1u] = (uint16_t)word;
    }
}

int ecw_audio_hw_start(EcwAudioEvent callback)
{
    unsigned width = duplex ? 32u : 16u;
    drv_i2s_tx_dis();
    drv_i2s_rx_dis();
    drv_i2s_cfg_slot(1, width, width);
    drv_i2s_cfg_clk(rate, width);
    drv_i2s_cfg_tx(0, 1);
    if (duplex)
    {
        drv_i2s_cfg_rx(0, 2);
    }
    notify = callback;
    draining = false;
    last_ms[AUDIO_TX] = last_ms[AUDIO_RX] = ecw_sdk_time_ms();
    if (duplex)
    {
        int result = arm(AUDIO_RX, 0);
        if (result)
        {
            draining = true;
            notify = NULL;
            return result;
        }
        drv_i2s_rx_en();
    }
    int result = arm(AUDIO_TX, 0);
    if (result)
    {
        draining = true;
        notify = NULL;
        return result; /* RX storage stays owned if RX was already started. */
    }
    drv_i2s_tx_en();
    return 0;
}

static void abort_owned_channels(void)
{
    drv_i2s_tx_dis();
    drv_i2s_rx_dis();
    for (unsigned d = 0; d < 2; ++d)
    {
        if (channels[d] >= 0 && in_flight[d] && !abort_requested[d])
        {
            abort_requested[d] = true;
            (void)drv_dma_stop(channels[d]); /* Request, not a completion result. */
        }
    }
}

static void retire_aborted_channels(void)
{
    /* Fixed P12/P03 dma.c:T_DMA_REG_MAP: channel enable at +0x34.
     * Both the native ABORT event after our request and disabled readback are
     * required. Do not reset the controller or touch another owner's channel.
     * Silicon bus retirement and fault injection remain hardware acceptance. */
    uint32_t enabled = READ_REG(MEM_BASE_DMAC + 0x34u);
    for (unsigned d = 0; d < 2; ++d)
    {
        if (channels[d] >= 0 && abort_requested[d] && abort_seen[d] &&
            !(enabled & (1u << (unsigned)channels[d])))
        {
#if defined(__riscv)
            __asm__ volatile("fence iorw, iorw" ::: "memory");
#endif
            in_flight[d] = false;
        }
    }
}

int ecw_audio_hw_stop(void)
{
    uintptr_t key = ecw_sdk_lock();
    draining = true;
    notify = NULL;
    if (hardware_error || abort_requested[AUDIO_TX] || abort_requested[AUDIO_RX])
    {
        abort_owned_channels();
        retire_aborted_channels();
    }
    bool done = idle();
    ecw_sdk_unlock(key);
    if (!done)
    {
        /* No abort-as-completion assumption, busy polling or invented events.
         * A missing TC retains all DMA storage/channels for an explicit retry. */
        (void)os_sem_wait(stop_sem, period_ms * 2u + 10u);
    }
    key = ecw_sdk_lock();
    if (!idle())
    {
        abort_owned_channels();
        retire_aborted_channels();
    }
    if (!idle())
    {
        ecw_sdk_unlock(key);
        return AUDIO_STOP_PENDING;
    }
    drv_i2s_tx_dis();
    drv_i2s_rx_dis();
    release_channels();
    ecw_sdk_unlock(key);
    return 0; /* DMA storage is safe; this is not evidence of wire drain. */
}

int ecw_audio_hw_close(const Ecw6700Port *p)
{
    if (channels[AUDIO_TX] >= 0 || channels[AUDIO_RX] >= 0 || !idle())
    {
        return AUDIO_CLOSE_BUSY;
    }
    if (!opened)
    {
        return 0;
    }
    for (size_t i = 0; i < p->pin_count; ++i)
    {
        int result = ecw_sdk_gpio_close(p->pins[i]);
        if (result)
        {
            return result;
        }
    }
    int result = os_sem_destroy(stop_sem);
    if (result)
    {
        return result;
    }
    stop_sem = NULL;
    drv_i2s_bus_release();
    os_free(slab);
    slab = NULL;
    opened = false;
    return 0;
}
