/*
 * kernel_pause.h - the host pause: the in-game settings overlay stops the
 * title while it is open.
 *
 * The title is held where the console's GPU would hold it. The executor (the
 * nv2a-ack thread, which stands in for the GPU) stops at FLIP_STALL, after
 * the frame is presented (xbox_HostPauseHold); the title's CPU then runs into
 * its own fence and pushbuffer-space waits within a frame or two and stops
 * there, which is what a stalled GPU does on the console. Nothing in the
 * title is suspended from outside, so no lock can be held across the pause
 * and no guest thread is stopped mid-instruction.
 *
 * Listeners are told of each change on the calling thread: the APU stops its
 * frame thread, so the title's sound does not play on (and its streaming,
 * which follows the play cursor, waits with it).
 *
 * The game time is not frozen: the vblank keeps counting and the host
 * clocks run. A title that steps its simulation by elapsed time is bounded
 * by its own catch-up limit (Burnout 3's sub_001B58E0: at most four steps a
 * frame), so it comes back with one frame of catch-up at most.
 *
 * A pause is taken at the next flip, so it waits out a load or a movie that
 * does not flip; the overlay opens at once either way.
 */
#ifndef XBOX_KERNEL_PAUSE_H
#define XBOX_KERNEL_PAUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pause (1) or resume (0) the title. Any thread; repeated calls are fine. */
void xbox_HostPause(int on);
int  xbox_HostPaused(void);

/* The executor, at the end of FLIP_STALL: notes the flip's time
 * (xbox_HostFlipTimes), then returns at once unless paused, otherwise blocks
 * until resumed. Counts as a flip for the hang watchdog while it waits (a
 * pause is not a hang). Holds no lock. */
void xbox_HostPauseHold(void);

/* The executor's recent flips, for the overlay's frame-time graph: the host
 * times (xbox_HostNowNs) of the last flips, oldest first, up to max; a 0
 * entry is a gap (the title was paused there). Returns how many. Any
 * thread; a read racing the executor may see one stale entry. */
int  xbox_HostFlipTimes(uint64_t *ns, int max);

/* fn(paused) is called, after the flag changes, on every xbox_HostPause that
 * changes it. At most 4; call before the guest starts. */
void xbox_HostPauseListener(void (*fn)(int paused));

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_PAUSE_H */
