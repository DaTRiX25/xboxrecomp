/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Number of device buffers the voice may hold. */
int xa2_get_buffer_count(void);

/* 256-sample granules handed over and not yet played (queued buffers plus
 * the partly filled one). Also counts underruns: see xa2_underruns. */
int xa2_queued_granules(void);

/* Times the voice ran dry after audio started ([XA2] underrun x%u). */
unsigned xa2_underruns(void);

#endif /* APU_XAUDIO2_H */
