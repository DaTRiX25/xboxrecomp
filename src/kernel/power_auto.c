/*
 * power_auto.c - Power mode "Auto": the vblank hold's spin lead picked at
 * run time (BO-175). The rules are in power_auto.h.
 */
#include "power_auto.h"

#include <stdio.h>
#include <string.h>

#define MS                 1000000ull
#define PERIOD_NS          16666667ll           /* one emulated vblank */
#define WINDOW_NS          (2000 * MS)
#define HITCH_GAP_NS       (250 * MS)           /* no hold for this long: a stall */
#define QUIET_NS           (3000 * MS)          /* after a hitch, decide nothing */
#define LATE_WAKE_NS       (1 * MS)
#define DOWN_RUN           5                    /* clean windows before a step down */
#define DOWN_RUN_MAX       150                  /* 5 min */
#define OSC_NS             (30000 * MS)         /* up this soon after a down: oscillation */
#define DOWN_RESET_NS      (600000 * MS)        /* no step up for this long: back to DOWN_RUN */
#define CEIL_WAIT_NS       (60000 * MS)
#define CEIL_WAIT_MAX_NS   (600000 * MS)
#define HELD_DECAY_NS      (600000 * MS)
#define RUNUP_NS           (20000 * MS)         /* a climb this soon before a stall: its run-up */
#define MAX_LEVEL          (XBOX_POWER_AUTO_LEVELS - 1)

static const uint32_t k_lead_us[XBOX_POWER_AUTO_LEVELS] = { 0, 2000, 4000, 8000, 12000, 17000 };

uint64_t xbox_PowerAutoLeadNs(int level)
{
    if (level < 0)
        level = 0;
    if (level > MAX_LEVEL)
        level = MAX_LEVEL;
    return (uint64_t)k_lead_us[level] * 1000u;
}

int xbox_PowerAutoLevel(const XboxPowerAuto *a)
{
    return a->level;
}

void xbox_PowerAutoInit(XboxPowerAuto *a)
{
    memset(a, 0, sizeof *a);
    a->level = XBOX_POWER_AUTO_START;
    a->held = XBOX_POWER_AUTO_START;
    a->ceiling = MAX_LEVEL;
    a->down_need = DOWN_RUN;
    a->ceiling_wait_ns = CEIL_WAIT_NS;
    a->probe_from = -1;
    snprintf(a->why, sizeof a->why, "start (as Balanced)");
}

static void window_reset(XboxPowerAuto *a, uint64_t now)
{
    a->win_start_ns = now;
    a->n = a->ontime = a->tight = a->waited = a->late_wakes = a->nears = a->hitch = 0;
}

/* The end of a 2 s window: the decisions. Returns 1 when the level moved. */
static int window_end(XboxPowerAuto *a, uint64_t now)
{
    int score = a->nears + a->late_wakes;
    int disturbed = a->hitch || now < a->quiet_until_ns;
    int bad = !disturbed && (a->nears >= 2
                             || (a->waited >= 20 && a->late_wakes * 20 >= a->waited));
    int before = a->level;

    /* What has run out. */
    if (a->ceiling < MAX_LEVEL && now >= a->ceiling_until_ns)
        a->ceiling = MAX_LEVEL;
    if (a->held > 0 && now - a->held_at_ns > HELD_DECAY_NS) {
        a->held--;
        a->held_at_ns = now;
    }
    if (a->down_need > DOWN_RUN && now - a->last_up_ns > DOWN_RESET_NS)
        a->down_need = DOWN_RUN;

    if (disturbed) {
        /* A stall while a step up is on trial: the trouble that caused it
         * was the run-up to a load, not the wait (what follows is another
         * scene). The step is void. */
        if (a->probe_from >= 0) {
            a->level = a->probe_from;
            a->probe_from = -1;
            a->tot.reverts++;
            snprintf(a->why, sizeof a->why, "a stall came: the step up is void");
        }
        a->bad_run = a->clean_run = 0;
    } else if (a->probe_from >= 0) {
        /* The two windows after a step up: did the climb help yet? A third
         * less trouble than before it started: yes, and this level is the
         * one needed. Not yet: one more step, up to the top; no help even
         * there: back to where the climb started (the GPU or the timer is
         * the limit, not the wait). */
        a->probe_score += score;
        if (++a->probe_windows >= 2) {
            int up = a->level + 1 > a->ceiling ? a->ceiling : a->level + 1;
            if (a->probe_score * 3 <= a->probe_base * 2) {
                if (a->level > a->held) {
                    a->held = a->level;
                    a->held_at_ns = now;
                }
                a->probe_from = -1;
            } else if (up > a->level) {
                a->level = up;
                a->last_up_ns = now;
                a->tot.ups++;
                snprintf(a->why, sizeof a->why, "trouble %d -> %d after the last step: one more",
                         a->probe_base, a->probe_score);
            } else {
                a->level = a->probe_from;
                a->ceiling = a->probe_from;
                a->ceiling_until_ns = now + a->ceiling_wait_ns;
                a->ceiling_wait_ns = a->ceiling_wait_ns * 2 > CEIL_WAIT_MAX_NS
                                   ? CEIL_WAIT_MAX_NS : a->ceiling_wait_ns * 2;
                a->tot.reverts++;
                snprintf(a->why, sizeof a->why, "more spin did not help (%d -> %d): back",
                         a->probe_base, a->probe_score);
                a->probe_from = -1;
            }
            a->probe_score = a->probe_windows = 0;
            a->bad_run = a->clean_run = 0;
        }
    } else if (bad) {
        a->clean_run = 0;
        if (a->bad_run++ == 0) {
            a->prev_heavy = a->nears * 10 >= a->n;
            a->prev_bad_score = score;
        } else {
            int step = a->prev_heavy && a->nears * 10 >= a->n ? 2 : 1;
            int to = a->level + step > a->ceiling ? a->ceiling : a->level + step;
            if (to > a->level) {
                if (a->last_down_ns && now - a->last_down_ns < OSC_NS)
                    a->down_need = a->down_need * 2 > DOWN_RUN_MAX ? DOWN_RUN_MAX : a->down_need * 2;
                /* The start of a climb (not a step within one): where it
                 * began and what was held, for a stall that may follow. */
                if (!a->climb_ns || now - a->last_up_ns > WINDOW_NS * 3) {
                    a->climb_from = a->level;
                    a->climb_held = a->held;
                    a->climb_ns = now;
                }
                a->probe_from = a->level;
                a->probe_base = a->prev_bad_score + score;
                a->probe_score = a->probe_windows = 0;
                a->level = to;
                a->last_up_ns = now;
                a->tot.ups++;
                snprintf(a->why, sizeof a->why, "%d near misses, %d late wakes of %d waits",
                         a->nears, a->late_wakes, a->waited);
            }
            a->bad_run = 0;
        }
    } else {
        a->bad_run = 0;
        if (++a->clean_run >= a->down_need && a->level > 0
                && a->ontime >= 30 && a->tight * 10 <= a->ontime) {
            a->level--;
            a->last_down_ns = now;
            a->clean_run = 0;
            a->tot.downs++;
            snprintf(a->why, sizeof a->why, "steady with headroom for %d s",
                     a->down_need * (int)(WINDOW_NS / (1000 * MS)));
        }
    }
    window_reset(a, now);
    return a->level != before;
}

int xbox_PowerAutoFeed(XboxPowerAuto *a, const XboxPowerAutoSample *s)
{
    uint64_t now = s->now_ns;
    int gap = a->last_ns && now - a->last_ns > HITCH_GAP_NS;
    int changed = 0;

    a->last_ns = now;
    if (!a->win_start_ns)
        a->win_start_ns = a->held_at_ns = now;
    if (!s->interval || s->interval > 4)
        return 0;
    a->n++;
    a->tot.holds++;
    if (gap || s->slack_ns < -PERIOD_NS) {
        /* Loading, file IO, a shader compile: not the wait's to cure. The
         * first of a cluster takes the level back to the one last proven
         * needed (a scene change). */
        a->hitch++;
        a->tot.hitches++;
        if (now >= a->quiet_until_ns) {
            int to;
            /* A climb that began shortly before this stall was the run-up
             * to a load (the near misses of a loading screen, streaming
             * beside the frames), not the scene's need: undo it, and what
             * it taught. */
            if (a->climb_ns && now - a->climb_ns < RUNUP_NS && a->level > a->climb_from) {
                a->level = a->climb_from;
                a->held = a->climb_held;
                a->probe_from = -1;
                a->tot.reverts++;
                snprintf(a->why, sizeof a->why, "a stall followed: the climb was the run-up to a load");
                changed = 1;
            }
            a->climb_ns = 0;
            to = a->held < a->ceiling ? a->held : a->ceiling;
            if (to > a->level) {
                a->level = to;
                a->tot.restores++;
                a->probe_from = -1;
                snprintf(a->why, sizeof a->why, "a stall (scene change?): back to the level last needed");
                changed = 1;
            }
        }
        a->quiet_until_ns = now + QUIET_NS;
    } else if (s->slack_ns < 0) {
        a->nears++;
        a->tot.near_misses++;
    } else {
        a->ontime++;
        if (s->slack_ns < (int64_t)s->interval * PERIOD_NS / 4)
            a->tight++;
    }
    if (s->wake_ns >= 0) {
        a->waited++;
        a->tot.waits++;
        if (s->wake_ns > (int64_t)LATE_WAKE_NS) {
            a->late_wakes++;
            a->tot.late_wakes++;
        }
    }
    if (now - a->win_start_ns >= WINDOW_NS)
        changed |= window_end(a, now);
    return changed;
}
