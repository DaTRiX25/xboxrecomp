/*
 * power_auto.h - Power mode "Auto": the vblank hold's spin lead picked at
 * run time (BO-175). Pure arithmetic; tests/power_auto drives it with
 * synthetic traces, kernel_pacing.c feeds it one sample per held flip.
 *
 * What it changes: only how the executor waits for the vblank its flip is
 * held for (sleep, then spin the last `lead` ms; kernel_pacing.c). The flip
 * still leaves at the same vblank, so the title's frames, its simulation
 * steps and its own 30 fps phases are what they are in any other mode.
 *
 * What it measures, per held flip, against the vblank the title itself asked
 * for (its swap interval: 1 in a race, 2 in the crash camera or split
 * screen), never against a fixed 60:
 *   slack  the target vblank's edge minus the frame's arrival at the hold.
 *          Negative: the frame missed the vblank it asked for. Missed by
 *          less than a vblank period is a near miss (what a low CPU clock
 *          does to a heavy frame, and what spinning cures); missed by more,
 *          or a gap of over 250 ms between holds, is a hitch (loading, file
 *          IO, a shader compile) that the wait method cannot cure.
 *   wake   the hold's return minus the moment the timer thread raised that
 *          vblank: the wait's own lateness (a thread woken late from its
 *          sleep). The timer thread's lateness is not in it: no spin on
 *          this thread can make it raise sooner.
 *
 * Decisions, in windows of 2 s of holds:
 *   up      two windows in a row with >= 2 near misses, or late wakes
 *           (> 1 ms) in >= 5 % of the waits: one level more spin (two when
 *           both windows had near misses in >= 10 % of their holds).
 *   check   every two windows after a step up: a third less trouble than
 *           before the climb started ends the climb there; otherwise one
 *           more step. No help even at the top: spinning is not the cure
 *           (the GPU is the limit, or the timer): back to where the climb
 *           started, which is the ceiling for 60 s (doubling on each
 *           repeat, up to 10 min).
 *   down    after a run of clean windows (10 s at first) with headroom
 *           (90 % of the on-time frames arrived with at least a quarter of
 *           their interval to spare): one level less. A step up within 30 s
 *           of a step down doubles the run needed (up to 5 min), so a level
 *           that is needed is not left again and again.
 *   hitch   a window with a hitch, and the 3 s after one, decide nothing.
 *           A hitch is usually a scene change, and the level returns to the
 *           level last proven needed (held, decaying one step per 10 min
 *           without a step up), so a race after a light menu starts where
 *           the last race ended instead of stuttering its way up.
 *
 * Levels (ms of spin before the vblank): 0 (Battery saver), 2, 4, 8
 * (Balanced; the start), 12, 17 (the whole frame: the busy-wait).
 */
#ifndef XBOX_POWER_AUTO_H
#define XBOX_POWER_AUTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_POWER_AUTO_LEVELS 6
#define XBOX_POWER_AUTO_START  3            /* 8 ms: Balanced */

typedef struct {
    uint64_t now_ns;      /* the hold's return */
    uint32_t interval;    /* vblanks the title asked for (swap interval); 0: none (not counted) */
    int64_t  slack_ns;    /* target edge minus arrival; < 0 the frame missed its vblank */
    int64_t  wake_ns;     /* return minus the vblank's raise; < 0: the hold did not sleep through a raise */
} XboxPowerAutoSample;

/* Totals since init, for the overlay, the trace and the tests. */
typedef struct {
    long holds, near_misses, hitches, late_wakes, waits;
    long ups, downs, reverts, restores;
} XboxPowerAutoTotals;

typedef struct {
    int      level, ceiling, held;
    uint64_t ceiling_until_ns, held_at_ns;
    /* the window being filled */
    uint64_t win_start_ns, last_ns;
    int      n, ontime, tight, waited, late_wakes, near, hitch;
    /* the run of windows */
    int      bad_run, clean_run, down_need;
    int      prev_near, prev_bad_score;
    uint64_t quiet_until_ns, last_down_ns, last_up_ns, down_ok_since_ns;
    /* after a step up: did it help? */
    int      probe_from, probe_windows, probe_base, probe_score;
    uint64_t ceiling_wait_ns;
    char     why[96];
    XboxPowerAutoTotals tot;
} XboxPowerAuto;

void xbox_PowerAutoInit(XboxPowerAuto *a);
/* One held flip. Returns 1 when the level changed (a->why says why). */
int  xbox_PowerAutoFeed(XboxPowerAuto *a, const XboxPowerAutoSample *s);
int  xbox_PowerAutoLevel(const XboxPowerAuto *a);
/* The spin lead of a level, in ns. */
uint64_t xbox_PowerAutoLeadNs(int level);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_POWER_AUTO_H */
