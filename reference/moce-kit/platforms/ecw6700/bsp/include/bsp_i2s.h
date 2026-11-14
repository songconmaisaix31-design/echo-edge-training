/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_I2S_H
#define BSP_I2S_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Continuous 16-bit stereo DMA stream over two alternating buffer halves.
 * Hardware circular DMA or native completion-driven DMA rearm is platform-specific.
 * Callback runs in DMA ISR and must finish
 * refilling the indicated half before the next boundary. Entire buffer remains borrowed.
 * Missed deadlines require application fault handling; callbacks do not prove real-time load. */
    typedef void (*BspAudioRefill)(BspPort id, unsigned half, void *user);
    BspStatus bsp_i2s_start(BspPort id, uint16_t *samples, size_t halfwords, BspAudioRefill refill,
                            void *user);
    /* Full-duplex capture: equal-length, disjoint TX/RX buffers in DMA SRAM.
 * Halfwords counts uint16_t elements per buffer, not HAL Size or PCM samples.
 * TX must remain immutable until BOTH buffers are returned. Silence is all zero.
 * RX callback runs in the RX DMA ISR: read/copy only the indicated half, finish
 * before the next boundary. It does not grant permission to update TX.
 * Callback/user storage must be prepared before start and outlive safe stop.
 * Lifecycle is serialized by one application owner; callbacks cannot stop/restart.
 * BSP_OK means borrowed/accepted, even if the HAL start later reports a fault;
 * inspect query.error and buffers_returned. Other returns do not borrow.
 * Payload is raw serial data; a PDM microphone requires module-level decoding. */
    typedef void (*BspAudioCapture)(BspPort id, unsigned half, void *user);
    BspStatus bsp_i2s_duplex_start(BspPort id, uint16_t *tx, uint16_t *rx, size_t halfwords,
                                   BspAudioCapture capture, void *user);
    typedef struct
    {
        bool active, buffers_returned;
        uint32_t boundaries;
        BspError error;
        BspEvidence wire_complete;
    } BspAudioState;
    BspStatus bsp_i2s_query(BspPort id, BspAudioState *out);
    BspStatus bsp_i2s_stop(BspPort id, BspAudioState *out);
#ifdef __cplusplus
}
#endif
#endif
