#include "ecw_features.h"
#if !BSP_ECW_ENABLE_I2S
#include "ecw_audio.h"
#include "bsp_i2s.h"
/* No buffers borrowed, SDK calls, allocation or output writes in this backend. */
bool ecw_audio_hw_available(void) { return false; }
int ecw_audio_open(const Ecw6700Port *p, uint32_t rate)
{ (void)p; (void)rate; return -1; }
int ecw_audio_close(const Ecw6700Port *p) { (void)p; return -1; }
BspStatus bsp_i2s_start(BspPort id, uint16_t *samples, size_t n,
                        BspAudioRefill refill, void *user)
{
    (void)id; (void)samples; (void)n; (void)refill; (void)user;
    return BSP_UNSUPPORTED;
}
BspStatus bsp_i2s_duplex_start(BspPort id, uint16_t *tx, uint16_t *rx, size_t n,
                               BspAudioCapture capture, void *user)
{
    (void)id; (void)tx; (void)rx; (void)n; (void)capture; (void)user;
    return BSP_UNSUPPORTED;
}
BspStatus bsp_i2s_query(BspPort id, BspAudioState *out)
{ (void)id; (void)out; return BSP_UNSUPPORTED; }
BspStatus bsp_i2s_stop(BspPort id, BspAudioState *out)
{ (void)id; (void)out; return BSP_UNSUPPORTED; }
#endif
