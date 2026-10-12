/*
 * apu_level.h -- the custom-music leveller (BO-132).
 *
 * A player's own songs are modern masters (about -7.5 LUFS, true peaks at or
 * past 0 dBFS); the title sends a custom-soundtrack stream about 8 dB hotter
 * than its EA GAMES TRAX streams on every send, and in stereo the fold sums
 * the fronts, the centre and the surrounds on top. The sum clips. The
 * leveller is a slow gain on the voices of the custom soundtrack, and only
 * those (not EA TRAX, not effects), that brings the song to the level the
 * title's own music has in the game:
 *
 *   measure   mean square of the voice's channel sum (L^2 + R^2; a mono
 *             voice counts twice, as it plays on both sides), once a VP
 *             frame (32 samples), in dB. The reading is a LUFS estimate
 *             without the K weighting (calibrated on real songs, see
 *             APU_LEVEL_TARGET_DB).
 *   envelope  one pole, 3 s, started from the first reading (a song's
 *             first seconds are not averaged with silence). A second pole of
 *             0.3 s, less 2 dB, is read as well and the higher of the two
 *             counts: a step from quiet to loud is caught within a fraction
 *             of a second instead of the slow pole's several, and a song's
 *             beat, which stays within 2 dB of its average, never moves the
 *             gain.
 *   gain      target + offset - envelope, limited to -15 .. +6 dB, followed
 *             down with a 0.3 s time constant and up with 4 s. Computed once
 *             a frame and interpolated across it (no steps).
 *   freeze    the gain does not move while the envelope is under -40 dBFS
 *             (silence, a quiet intro do not wind it up); it does not climb
 *             while the 0.3 s reading is more than 3 dB under the 3 s one
 *             (a fade out); readings under -60 dBFS are not averaged at all.
 *   next song a new stream starts at the gain the previous one settled on.
 *
 * Off (the stock behaviour) is not a gain of 0 dB, it is no code at all:
 * a voice is never touched, so the mix is bit-identical to a build without
 * this file. The setting is a level: quieter / match / louder are offsets
 * of -4 / 0 / +4 dB on the target.
 */
#ifndef APU_LEVEL_H
#define APU_LEVEL_H

#include "apu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The level a song is brought to, in the units of the reading above: the
 * title's EA TRAX streams, seen by the same meter, less the 8 dB the title
 * adds to a custom stream.
 *
 * Until the in-game measurement of EA TRAX against custom music is in, this
 * is the level that undoes the title's +8 dB on a -7.5 LUFS master: -15.5
 * LUFS. The reading is 0.5 dB above LUFS on real songs (four modern
 * masters at -5.2 to -8.6 LUFS read 0.2 to 0.8 dB high, no K weighting), so
 * the constant is -15.0 in its units. One constant. */
#define APU_LEVEL_TARGET_DB   (-15.0f)
#define APU_LEVEL_MIN_DB      (-15.0f)
#define APU_LEVEL_MAX_DB      (6.0f)
#define APU_LEVEL_FREEZE_DB   (-40.0f)
#define APU_LEVEL_HOLD_DB     (-60.0f)

enum { APU_LEVEL_OFF = 0, APU_LEVEL_QUIETER = 1, APU_LEVEL_MATCH = 2, APU_LEVEL_LOUDER = 3,
       APU_LEVEL_MODES };
/* Offset of a mode on the target, dB. */
float apu_level_mode_offset_db(int mode);

/* The mode: RECOMP_AUDIO_CUSTOM_LEVEL (off, quieter, match, louder) on first
 * use, match when unset; any thread, followed live. */
void apu_level_set_mode(int mode);
int  apu_level_mode(void);

/* The title's port says which voices are the custom soundtrack. Called on
 * the frame thread when a voice starts. */
typedef int (*apu_level_class_fn)(const apu_bus_voice *v);
void apu_level_set_classifier(apu_level_class_fn fn);
/* 1 when voices have to be tagged at start and tested per frame: a
 * classifier is set and the mode is not off. */
int  apu_level_active(void);

/* One VP frame of one voice (frame thread): when the voice is a custom
 * soundtrack stream, its samples are scaled in place. */
void apu_level_voice(unsigned voice, unsigned serial,
                     int (*fill)(void *ctx, apu_bus_voice *v), void *ctx,
                     float (*samples)[2], unsigned channels);

/* The leveller alone, for tests and tools: state of one stream. */
typedef struct {
    double slow_num, slow_wt, fast_num, fast_wt;
    float  gain_db;
    int    primed;                      /* gain_db is a settled value (or was jumped to) */
    float  env_db;                      /* the envelope in use, last frame */
    unsigned frames;
} apu_level_state;
/* gain_db < -100: nothing known yet (the first reading sets the gain). */
void apu_level_state_init(apu_level_state *s, float gain_db);
/* One frame of 32 samples in place; target_db includes the mode's offset.
 * Returns the linear gain at the end of the frame. */
float apu_level_frame(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db);

#ifdef __cplusplus
}
#endif

#endif /* APU_LEVEL_H */
