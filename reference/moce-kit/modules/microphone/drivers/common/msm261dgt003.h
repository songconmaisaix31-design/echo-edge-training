#ifndef MOCE_MSM261DGT003_H
#define MOCE_MSM261DGT003_H
#include "bsp_i2s.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    BspPort port;
    uint16_t *tx, *rx, *record;
    size_t dma_halfwords, record_halfwords, discard_halfwords;
} Msm261Config;
typedef struct {
    Msm261Config config;
    size_t captured, discarded;
    bool initialized, borrowed, capture_error, stopped_cleanly;
} Msm261;
typedef struct {
    int32_t history[3][64], sum[3], previous, highpass;
    unsigned position;
} Msm261Filter;
/* Zero initialize. One lifecycle owner; no concurrent API calls. Port already
 * configured as raw 32-bit stereo duplex, 32 kframes/s (64 PDM bits/frame).
 * L/R=GND for rising-edge capture. Caller supplies disjoint, aligned buffers,
 * DMA-accessible TX/RX, and stable instance/storage until successful stop.
 * Recording is finite, first record_halfwords after the discarded startup bits.
 * No live reading: callback alone owns capture fields until safe stop.
 * Init checks capabilities, not electrical presence. Start may accept then fault.
 * Failed stop retains ALL storage and forbids restart/decode; reboot or explicit
 * later stop is application policy. No HAL, allocation, automatic retry or GPIO.
 */
BspStatus msm261_init(Msm261 *device, const Msm261Config *config);
BspStatus msm261_start(Msm261 *device);
BspStatus msm261_query(Msm261 *device, BspAudioState *state);
BspStatus msm261_stop(Msm261 *device, BspAudioState *state);
BspStatus msm261_recorded(const Msm261 *device, const uint16_t **raw, size_t *halfwords);
/* Three cascaded 64-tap moving sums, decimation64, DC blocker. Input is
 * high-halfword-first, MSB-first raw PDM, not PCM/I2S channel selection.
 * n must be multiple of4. State persists across chunks. Output n/4 samples.
 * Caller supplies disjoint filter/input/output memory; rejects invalid sizes
 * before modifying state. Filter is a basic voice/demo filter, not calibrated SPL.
 */
void msm261_filter_init(Msm261Filter *filter);
BspStatus msm261_decode(Msm261Filter *filter, const uint16_t *raw, size_t n,
                       int16_t *pcm, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
