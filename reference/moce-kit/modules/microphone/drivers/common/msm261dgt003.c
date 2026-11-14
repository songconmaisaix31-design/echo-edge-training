#include "msm261dgt003.h"
#include <string.h>

static bool region(const void *p, size_t bytes)
{
    return p && bytes && (uintptr_t)p <= UINTPTR_MAX - bytes;
}
static bool overlap(const void *a, size_t na, const void *b, size_t nb)
{
    return (uintptr_t)a < (uintptr_t)b + nb && (uintptr_t)b < (uintptr_t)a + na;
}
static void capture(BspPort port, unsigned half, void *user)
{
    Msm261 *d = user;
    if (port != d->config.port || half > 1) {
        d->capture_error = true;
        return;
    }
    size_t count = d->config.dma_halfwords / 2u;
    const uint16_t *src = d->config.rx + half * count;
    size_t skip = d->config.discard_halfwords - d->discarded;
    if (skip > count) {
        skip = count;
    }
    d->discarded += skip;
    src += skip;
    count -= skip;
    size_t room = d->config.record_halfwords - d->captured;
    if (count > room) {
        count = room;
    }
    if (count) {
        memcpy(d->config.record + d->captured, src, count * sizeof(*src));
        d->captured += count;
    }
}
BspStatus msm261_init(Msm261 *d, const Msm261Config *c)
{
    if (!d || !c || (c->port >> 8) != BSP_I2S ||
        c->dma_halfwords < 128 || c->dma_halfwords > 2048 || c->dma_halfwords % 8 ||
        !c->record_halfwords || c->record_halfwords > SIZE_MAX / 2 ||
        c->record_halfwords % 4 || c->discard_halfwords % 4) {
        return BSP_INVALID_ARG;
    }
    if (d->initialized) {
        return BSP_INVALID_STATE;
    }
    size_t bytes = c->dma_halfwords * 2, recorded = c->record_halfwords * 2;
    const void *p[] = {d, c->tx, c->rx, c->record};
    const size_t n[] = {sizeof(*d), bytes, bytes, recorded};
    for (unsigned i = 0; i < 4; ++i) {
        if (!region(p[i], n[i]) || (uintptr_t)p[i] % 2) {
            return BSP_INVALID_ARG;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (overlap(p[i], n[i], p[j], n[j])) {
                return BSP_INVALID_ARG;
            }
        }
    }
    uint32_t missing;
    BspStatus status = bsp_port_require(c->port, BSP_CAP_STREAM | BSP_CAP_DUPLEX_RX, &missing);
    if (status != BSP_OK) {
        return status;
    }
    d->config = *c;
    d->initialized = true;
    return BSP_OK;
}
BspStatus msm261_start(Msm261 *d)
{
    if (!d || !d->initialized) {
        return BSP_NOT_READY;
    }
    if (d->borrowed) {
        return BSP_INVALID_STATE;
    }
    d->captured = 0;
    d->discarded = 0;
    d->capture_error = false;
    d->stopped_cleanly = false;
    memset(d->config.tx, 0, d->config.dma_halfwords * 2);
    /* Context is fully prepared before BSP can invoke the callback. */
    BspStatus status = bsp_i2s_duplex_start(d->config.port, d->config.tx, d->config.rx,
                                         d->config.dma_halfwords, capture, d);
    d->borrowed = status == BSP_OK;
    return status;
}
BspStatus msm261_query(Msm261 *d, BspAudioState *state)
{
    if (!d || !d->initialized || !state) {
        return BSP_INVALID_ARG;
    }
    return bsp_i2s_query(d->config.port, state);
}
BspStatus msm261_stop(Msm261 *d, BspAudioState *state)
{
    if (!d || !d->initialized || !state) {
        return BSP_INVALID_ARG;
    }
    if (!d->borrowed) {
        return BSP_INVALID_STATE;
    }
    BspStatus status = bsp_i2s_stop(d->config.port, state);
    if (status != BSP_OK || !state->buffers_returned || state->active) {
        return status != BSP_OK ? status : BSP_IO;
    }
    d->borrowed = false;
    d->stopped_cleanly = state->error.code == BSP_OK && !d->capture_error;
    return d->stopped_cleanly ? BSP_OK : BSP_IO;
}
BspStatus msm261_recorded(const Msm261 *d, const uint16_t **raw, size_t *n)
{
    if (!d || !raw || !n || !d->initialized) {
        return BSP_INVALID_ARG;
    }
    if (d->borrowed || !d->stopped_cleanly) {
        return BSP_INVALID_STATE;
    }
    *raw = d->config.record;
    *n = d->captured;
    return d->captured == d->config.record_halfwords ? BSP_OK : BSP_NOT_READY;
}
void msm261_filter_init(Msm261Filter *f)
{
    if (f) {
        memset(f, 0, sizeof(*f));
    }
}
BspStatus msm261_decode(Msm261Filter *f, const uint16_t *raw, size_t n,
                       int16_t *pcm, size_t capacity)
{
    if (!f || !n || n % 4 || n > SIZE_MAX / 2 || capacity < n / 4 ||
        !region(raw, n * 2) || !region(pcm, n / 2) || f->position >= 64 ||
        overlap(raw, n * 2, pcm, n / 2) ||
        overlap(f, sizeof(*f), raw, n * 2) || overlap(f, sizeof(*f), pcm, n / 2)) {
        return BSP_INVALID_ARG;
    }
    size_t output = 0;
    for (size_t i = 0; i < n; ++i) {
        for (unsigned bit = 0; bit < 16; ++bit) {
            int32_t value = (raw[i] & (0x8000u >> bit)) ? 1 : -1;
            for (unsigned stage = 0; stage < 3; ++stage) {
                f->sum[stage] += value - f->history[stage][f->position];
                f->history[stage][f->position] = value;
                value = f->sum[stage];
            }
            if (++f->position == 64) {
                f->position = 0;
                int32_t sample = value / 8;
                f->highpass = sample - f->previous + f->highpass * 255 / 256;
                f->previous = sample;
                sample = f->highpass;
                if (sample > 32767) {
                    sample = 32767;
                }
                if (sample < -32768) {
                    sample = -32768;
                }
                pcm[output++] = (int16_t)sample;
            }
        }
    }
    return BSP_OK;
}
