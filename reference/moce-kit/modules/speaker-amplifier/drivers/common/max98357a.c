#include "max98357a.h"
#include <limits.h>

#define VOLUME_MASK 1023u
#define MUTE_MASK 65536u
_Static_assert(sizeof(unsigned) == 4u, "Audio controls use aligned 32-bit words");

static bool rate_supported(uint32_t rate)
{
    /* The schematic has no SD_MODE control. Exclude 8kHz/32fs: the chip's
     * special 256kHz startup/stop sequence cannot be enforced on this module. */
    switch (rate)
    {
    case 16000u: case 32000u: case 44100u: case 48000u: case 88200u: case 96000u:
        return true;
    default:
        return false;
    }
}

static void fill_half(Max98357a *d, unsigned half)
{
    size_t words = d->config.dma_halfwords / 2u;
    uint16_t *dst = d->config.dma_buffer + half * words;
    uintptr_t key;
    unsigned control = 0u;
    if (bsp_critical_enter(&key) == BSP_OK)
    {
        control = d->controls;
        bsp_critical_exit(key);
    }
    unsigned gain = (control & MUTE_MASK) ? 0u : control & VOLUME_MASK;
    for (size_t i = 0; i < words; i += 2u)
    {
        int32_t left = 0, right = 0;
        if (d->cursor < d->frames)
        {
            size_t offset = d->cursor * d->channels;
            left = d->pcm[offset];
            right = d->channels == 1u ? left : d->pcm[offset + 1u];
            ++d->cursor;
            if (d->cursor == d->frames)
            {
                if (d->loop)
                    d->cursor = 0;
                else
                {
                    if (bsp_critical_enter(&key) == BSP_OK)
                    {
                        d->source_exhausted = true;
                        bsp_critical_exit(key);
                    }
                }
            }
        }
        /* 32768*1000 fits int32_t; unsigned conversion preserves PCM bit pattern. */
        dst[i] = (uint16_t)(left * (int32_t)gain / 1000);
        dst[i + 1u] = (uint16_t)(right * (int32_t)gain / 1000);
    }
}

static void refill(BspPort port, unsigned half, void *user)
{
    Max98357a *d = user;
    if (d && port == d->config.port && half < 2u)
        fill_half(d, half);
}

BspStatus max98357a_init(Max98357a *d, const Max98357aConfig *c)
{
    if (!d || !c)
        return BSP_INVALID_ARG;
    if (d->initialized)
        return BSP_INVALID_STATE;
    uintptr_t address = (uintptr_t)c->dma_buffer;
    if ((c->port >> 8) != BSP_I2S || !rate_supported(c->sample_rate_hz) ||
        !c->dma_buffer || address % 4u || c->dma_halfwords < 4u ||
        c->dma_halfwords > UINT16_MAX || c->dma_halfwords % 4u ||
        c->dma_halfwords > (UINTPTR_MAX - address) / sizeof(uint16_t) ||
        c->volume_permille > 1000u)
        return BSP_INVALID_ARG;
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(c->port, BSP_CAP_STREAM, &missing);
    if (status != BSP_OK)
        return status;
    uintptr_t key;
    status = bsp_critical_enter(&key);
    if (status != BSP_OK)
        return status;
    bsp_critical_exit(key);
    d->config = *c;
    d->borrowed = false;
    d->pcm = NULL;
    d->last_transport = (BspAudioState){.buffers_returned = true};
    d->controls = c->volume_permille;
    d->source_exhausted = false;
    d->initialized = true;
    return BSP_OK;
}

BspStatus max98357a_start_pcm(Max98357a *d, const int16_t *pcm, size_t frames,
                            unsigned channels, bool loop)
{
    if (!d || !pcm || !frames || (channels != 1u && channels != 2u) ||
        (uintptr_t)pcm % _Alignof(int16_t) || frames > SIZE_MAX / channels / sizeof(int16_t))
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    if (d->borrowed)
        return BSP_INVALID_STATE;
    size_t bytes = frames * channels * sizeof(int16_t);
    uintptr_t p = (uintptr_t)pcm, b = (uintptr_t)d->config.dma_buffer;
    size_t buffer_bytes = d->config.dma_halfwords * sizeof(uint16_t);
    if (bytes > UINTPTR_MAX - p || (p < b + buffer_bytes && b < p + bytes))
        return BSP_INVALID_ARG;
    d->pcm = pcm;
    d->frames = frames;
    d->channels = channels;
    d->loop = loop;
    d->cursor = 0;
    d->source_exhausted = false; /* No callback owns the instance yet. */
    fill_half(d, 0);
    fill_half(d, 1);
    /* Prepare everything before BSP can enable its callback. */
    d->borrowed = true;
    BspStatus status = bsp_i2s_start(d->config.port, d->config.dma_buffer,
                                   d->config.dma_halfwords, refill, d);
    if (status != BSP_OK)
        d->borrowed = false; /* BSP contract: a rejection never borrows. */
    return status;
}

BspStatus max98357a_set_volume(Max98357a *d, unsigned volume)
{
    if (!d || volume > 1000u)
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    uintptr_t key;
    BspStatus status = bsp_critical_enter(&key);
    if (status != BSP_OK) return status;
    d->controls = (d->controls & ~VOLUME_MASK) | volume;
    bsp_critical_exit(key);
    return BSP_OK;
}

BspStatus max98357a_set_mute(Max98357a *d, bool muted)
{
    if (!d)
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    uintptr_t key;
    BspStatus status = bsp_critical_enter(&key);
    if (status != BSP_OK) return status;
    d->controls = muted ? d->controls | MUTE_MASK : d->controls & ~MUTE_MASK;
    bsp_critical_exit(key);
    return BSP_OK;
}

BspStatus max98357a_query(Max98357a *d, Max98357aState *out)
{
    if (!d || !out)
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    if (d->borrowed)
    {
        BspAudioState state = {0};
        BspStatus status = bsp_i2s_query(d->config.port, &state);
        if (status != BSP_OK)
            return status;
        d->last_transport = state;
        if (state.buffers_returned)
            d->borrowed = false;
    }
    uintptr_t key;
    BspStatus status = bsp_critical_enter(&key);
    if (status != BSP_OK) return status;
    bool exhausted = d->source_exhausted;
    bsp_critical_exit(key);
    *out = (Max98357aState){.transport = d->last_transport,
                           .borrowed = d->borrowed,
                           .source_exhausted = exhausted};
    return BSP_OK;
}

BspStatus max98357a_stop(Max98357a *d, Max98357aState *out)
{
    if (!d || !out)
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    if (!d->borrowed)
        return BSP_INVALID_STATE;
    BspAudioState state = d->last_transport;
    state.active = true;
    state.buffers_returned = false; /* Conservative if BSP rejects before writing. */
    BspStatus status = bsp_i2s_stop(d->config.port, &state);
    d->last_transport = state;
    if (state.buffers_returned)
        d->borrowed = false;
    uintptr_t key;
    bool exhausted = false;
    BspStatus sync_status = bsp_critical_enter(&key);
    if (sync_status == BSP_OK)
    {
        exhausted = d->source_exhausted;
        bsp_critical_exit(key);
    }
    *out = (Max98357aState){.transport = state, .borrowed = d->borrowed,
                           .source_exhausted = exhausted};
    if (status == BSP_OK && sync_status != BSP_OK) return sync_status;
    return status;
}

BspStatus max98357a_deinit(Max98357a *d)
{
    if (!d)
        return BSP_INVALID_ARG;
    if (!d->initialized)
        return BSP_NOT_READY;
    if (d->borrowed)
        return BSP_INVALID_STATE;
    d->initialized = false;
    d->pcm = NULL;
    return BSP_OK;
}
