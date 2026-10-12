/*
 * apu_limit.h -- the safety limiter on the final output (BO-132).
 *
 * The last step of the mix is a clamp to 16-bit full scale. A mix that
 * reaches past it (a loud master through the title's sends, then the stereo
 * fold, which sums the fronts, the centre and the surrounds) is cut flat
 * there, and that is heard as distortion. This stage stands in front of the
 * clamp:
 *
 *   - peak limiter, all channels linked (the stereo image does not move),
 *   - ceiling -0.5 dBFS, instant attack (no look-ahead, so no added
 *     latency: the sample that would pass the ceiling is the one scaled),
 *   - release about 50 ms (exponential),
 *   - gain exactly 1 and no arithmetic on the samples while nothing is over
 *     the ceiling: a signal that never reaches -0.5 dBFS comes out bit for
 *     bit as without this stage (tests/apu_bus holds that).
 *
 * It sits after the mixer's own output stage (apu_bus.h), whose limiter or
 * soft clip, when chosen, comes first.
 */
#ifndef APU_LIMIT_H
#define APU_LIMIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define APU_LIMIT_FRAME       32
#define APU_LIMIT_CEILING_DB  (-0.5f)
#define APU_LIMIT_RELEASE_MS  50.0f

/* On by default. Off is the clamp alone, as before this stage (tests). */
void apu_limit_enable(int on);
int  apu_limit_enabled(void);
/* Gain back to 1 and the counters to zero. */
void apu_limit_reset(void);
/* n samples (n <= 32) of nch planar feeds, in place. Frame thread. */
void apu_limit(float (*feeds)[APU_LIMIT_FRAME], int nch, int n);
/* Gain reduction now, dB >= 0. */
float apu_limit_reduction_db(void);
/* Samples this stage has touched (peak over the ceiling, or gain below 1
 * while releasing) since apu_limit_reset. */
unsigned long apu_limit_engaged(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_LIMIT_H */
