#ifndef MAX98357A_H
#define MAX98357A_H

#include "bsp_i2s.h"
#include "bsp_critical.h"

/* C11, no SDK/HAL dependency. Zero-initialize the instance before first init.
 * One task/main owns ALL API calls, including volume/mute. Public BSP critical
 * sections synchronize that owner with the serialized DMA callback. PCM and all
 * remaining fields are immutable while BSP owns buffers.
 * Never copy an initialized instance or call lifecycle functions from an ISR.
 */
typedef struct
{
    BspPort port;
    uint32_t sample_rate_hz; /* Must match the project's initialized I2S port. */
    uint16_t *dma_buffer;   /* Exclusive, platform DMA-accessible, aligned storage. */
    size_t dma_halfwords;   /* Full buffer: two halves, complete stereo frames. */
    unsigned volume_permille; /* 0..1000 digital attenuation, never hardware gain. */
} Max98357aConfig;

typedef struct
{
    BspAudioState transport;
    bool borrowed;
    bool source_exhausted; /* All PCM was QUEUED; does not prove wire completion. */
} Max98357aState;

typedef struct
{
    bool initialized, borrowed;
    Max98357aConfig config;
    const int16_t *pcm;
    size_t frames, cursor;
    unsigned channels;
    bool loop;
    unsigned controls;
    bool source_exhausted;
    BspAudioState last_transport;
} Max98357a;

BspStatus max98357a_init(Max98357a *device, const Max98357aConfig *config);
/* Signed native-endian PCM16: mono is duplicated to L/R; stereo is interleaved.
 * Source, DMA buffer and instance stay alive and disjoint until buffers_returned.
 * Callback processing is bounded by one half; no allocation, I/O or user callback.
 * BSP_OK means accepted/borrowed, including asynchronous native start faults.
 * Inspect query.transport.error immediately and during playback.
 * A finite source is followed by zero PCM; it does not automatically stop clocks.
 */
BspStatus max98357a_start_pcm(Max98357a *device, const int16_t *pcm, size_t frames,
                            unsigned channels, bool loop);
BspStatus max98357a_set_volume(Max98357a *device, unsigned volume_permille);
BspStatus max98357a_set_mute(Max98357a *device, bool muted);
BspStatus max98357a_query(Max98357a *device, Max98357aState *out);
/* Immediate transport stop may truncate queued audio. A failed stop can retain
 * ownership: keep all memory alive, inspect out->borrowed and explicitly retry.
 * Digital mute is not SD_MODE shutdown. No serial-drain/pop-free guarantee.
 */
BspStatus max98357a_stop(Max98357a *device, Max98357aState *out);
BspStatus max98357a_deinit(Max98357a *device);

#endif
