#include "ecw_audio.h"
#include "ecw_sdk.h"
#include <string.h>
#include "ecw_storage_internal.h"
#define audio (*ecw_audio_storage)
static BspError error_of(BspStatus status, int32_t native)
{
    return (BspError){.code = status,
                      .native_valid = native != 0,
                      .native_domain = 0x45435741u,
                      .native_code = native};
}
int ecw_audio_open(const Ecw6700Port *p, uint32_t rate)
{
    int r = ecw_audio_hw_open(p, rate);
    if (r)
    {
        return r;
    }
    memset(&audio, 0, sizeof(audio));
    audio.port = p->id;
    audio.state.buffers_returned = true;
    audio.state.wire_complete = BSP_UNKNOWN;
    return 0;
}
int ecw_audio_close(const Ecw6700Port *p)
{
    return ecw_audio_hw_close(p);
}
static void event(unsigned half, int32_t native)
{
    uintptr_t key = ecw_sdk_lock();
    if (!audio.state.active || audio.stopping || audio.state.error.code != BSP_OK)
    {
        ecw_sdk_unlock(key);
        return;
    }
    if (native || half > 1)
    {
        audio.state.error = error_of(BSP_IO, native ? native : -101);
        ecw_stream_finish(audio.port, audio.state.error, false);
        ecw_sdk_unlock(key);
        return;
    }
    if (audio.duplex)
    {
        ecw_audio_hw_read(half, audio.rx + half * (audio.n / 2));
    }
    ++audio.state.boundaries;
    audio.in_callback = true;
    /* Do not run module DSP with all interrupts masked. Lifecycle calls from the
     * callback remain rejected; task stop cannot run before this ISR returns. */
    ecw_sdk_unlock(key);
    audio.notify(audio.port, half, audio.user);
    key = ecw_sdk_lock();
    audio.in_callback = false;
    if (!audio.duplex)
    {
        ecw_audio_hw_write(half, audio.tx + half * (audio.n / 2));
    }
    ecw_sdk_unlock(key);
}
static BspStatus start(BspPort id, uint16_t *tx, uint16_t *rx, size_t n, BspAudioRefill cb,
                       void *user)
{
    bool duplex = rx != NULL;
    if (!ecw_sdk_thread_ready() || (ecw_audio_storage && audio.in_callback))
    {
        return BSP_INVALID_STATE;
    }
    if (!tx || !cb || n < (duplex ? 8u : 4u) || n % (duplex ? 8u : 4u) || n > 65535u ||
        (uintptr_t)tx % 2 || (rx && (uintptr_t)rx % 2))
    {
        return BSP_INVALID_ARG;
    }
    if (rx)
    {
        uintptr_t a = (uintptr_t)tx, c = (uintptr_t)rx;
        if ((a < c ? c - a : a - c) < n * 2)
        {
            return BSP_INVALID_ARG;
        }
    }
    BspStatus r = ecw_stream_check(id, false);
    if (r != BSP_OK)
    {
        return r;
    }
    if (n > ECW_AUDIO_MAX_HALFWORDS || n < 128)
    {
        return BSP_UNSUPPORTED;
    }
    uint32_t missing;
    r = bsp_port_require(id, BSP_CAP_STREAM | (duplex ? BSP_CAP_DUPLEX_RX : 0), &missing);
    if (r != BSP_OK)
    {
        return r;
    }
    r = ecw_stream_check(id, true);
    if (r != BSP_OK)
    {
        return r;
    }
    r = ecw_audio_hw_prepare(duplex, n);
    if (r != BSP_OK)
    {
        ecw_stream_finish(id, error_of(r, 0), true);
        return r;
    }
    uintptr_t key = ecw_sdk_lock();
    audio.tx = tx;
    audio.rx = rx;
    audio.n = n;
    audio.notify = cb;
    audio.user = user;
    audio.duplex = duplex;
    audio.stopping = false;
    audio.state =
        (BspAudioState){.active = true, .buffers_returned = false, .wire_complete = BSP_UNKNOWN};
    ecw_audio_hw_write(0, tx);
    ecw_audio_hw_write(1, tx + n / 2);
    int native = ecw_audio_hw_start(event);
    if (native)
    {
        audio.state.error = error_of(BSP_IO, native);
        ecw_stream_finish(id, audio.state.error, false);
    }
    ecw_sdk_unlock(key);
    return BSP_OK; /* Accepted, including a hardware start fault. */
}
BspStatus bsp_i2s_start(BspPort id, uint16_t *tx, size_t n, BspAudioRefill cb, void *user)
{
    return start(id, tx, NULL, n, cb, user);
}
BspStatus bsp_i2s_duplex_start(BspPort id, uint16_t *tx, uint16_t *rx, size_t n, BspAudioCapture cb,
                               void *user)
{
    if (!rx)
    {
        return BSP_INVALID_ARG;
    }
    return start(id, tx, rx, n, cb, user);
}
BspStatus bsp_i2s_query(BspPort id, BspAudioState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = ecw_stream_check(id, false);
    if (r != BSP_OK)
    {
        return r;
    }
    uintptr_t k = ecw_sdk_lock();
    *out = audio.state;
    ecw_sdk_unlock(k);
    return BSP_OK;
}
BspStatus bsp_i2s_stop(BspPort id, BspAudioState *out)
{
    if (!ecw_sdk_thread_ready() || (ecw_audio_storage && audio.in_callback))
    {
        return BSP_INVALID_STATE;
    }
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus r = ecw_stream_check(id, false);
    if (r != BSP_OK)
    {
        return r;
    }
    uintptr_t k = ecw_sdk_lock();
    if (!audio.state.active || audio.stopping)
    {
        ecw_sdk_unlock(k);
        return BSP_INVALID_STATE;
    }
    audio.stopping = true;
    ecw_sdk_unlock(k);
    int native = ecw_audio_hw_stop();
    k = ecw_sdk_lock();
    /* Stop disables refill notification before waiting. Preserve a hardware
     * error arriving during that wait instead of replacing it with timeout. */
    int32_t hardware_error = ecw_audio_hw_error();
    if (hardware_error && audio.state.error.code == BSP_OK)
    {
        audio.state.error = error_of(BSP_IO, hardware_error);
    }
    if (native)
    {
        if (audio.state.error.code == BSP_OK)
        {
            audio.state.error = error_of(BSP_TIMEOUT, native);
        }
        /* Retain all references and ownership until a later explicit stop succeeds. */
    }
    else
    {
        audio.state.active = false;
        audio.state.buffers_returned = true;
        audio.tx = NULL;
        audio.rx = NULL;
        audio.notify = NULL;
        audio.user = NULL;
    }
    audio.state.wire_complete = BSP_UNKNOWN;
    ecw_stream_finish(id, audio.state.error, !native);
    audio.stopping = false;
    *out = audio.state;
    ecw_sdk_unlock(k);
    return native ? BSP_TIMEOUT : BSP_OK;
}
