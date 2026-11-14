#ifndef ECW_AUDIO_H
#define ECW_AUDIO_H
#include "ecw_internal.h"
#define ECW_AUDIO_MAX_HALFWORDS 2048u
/* Private P16 binding. User pointers are NEVER placed in DMA descriptors. */
typedef void (*EcwAudioEvent)(unsigned half, int32_t error);
bool ecw_audio_hw_available(void);
int ecw_audio_hw_open(const Ecw6700Port *, uint32_t rate);
int ecw_audio_hw_close(const Ecw6700Port *);
BspStatus ecw_audio_hw_prepare(bool duplex, size_t n);
int ecw_audio_hw_start(EcwAudioEvent event);
int ecw_audio_hw_stop(void);
int32_t ecw_audio_hw_error(void);
void ecw_audio_hw_write(unsigned half, const uint16_t *data);
void ecw_audio_hw_read(unsigned half, uint16_t *data);
int ecw_audio_open(const Ecw6700Port *, uint32_t);
int ecw_audio_close(const Ecw6700Port *);
BspStatus ecw_stream_check(BspPort, bool claim);
void ecw_stream_finish(BspPort, BspError, bool release);
#endif
