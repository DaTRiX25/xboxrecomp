/*
 * apu_wav.h -- the WAV tap on the APU's output (RECOMP_AUDIO_WAV).
 *
 * Writes exactly the samples handed to the host device -- 48 kHz, 16-bit
 * stereo, after the mixdown, tone and software mixer -- so what a check reads
 * is what was played.
 *
 * The header is valid at every point a run can end: it is rewritten with the
 * current sizes once per second of audio (so a hard kill, including the
 * watchdog's _exit, loses at most the last second), and finalised by
 * apu_wav_close, which mcpx_apu_shutdown, an atexit handler and (on Windows)
 * the console-control handler all call.
 */
#ifndef APU_WAV_H
#define APU_WAV_H

#include <stdint.h>

#define APU_WAV_RATE     48000
#define APU_WAV_CHANNELS 2

/* Start a dump at `path`. max_secs > 0 caps it (the file is closed once that
 * much audio is written); <= 0 is uncapped. Returns 1 on success. A second
 * open while one is active is refused (returns 0). */
int  apu_wav_open(const char *path, double max_secs);

/* Append `count` stereo sample frames (interleaved L/R). No-op when closed. */
void apu_wav_write(const int16_t *samples, int count);

/* Finalise the header and close. Safe to call more than once and from any
 * thread. */
void apu_wav_close(void);

int  apu_wav_is_open(void);

#endif /* APU_WAV_H */
