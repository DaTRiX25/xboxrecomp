/*
 * smooth_keeper.h - the arithmetic of the Vulkan Smooth keeper
 * (fb_present_vulkan.c, BO-110/BO-207) that keeps state between frames,
 * pulled out so tests/vblank_schedule can drive it (BO-211). Header-only,
 * pure: no clock, lock or platform call.
 *
 * The refresh period from present_wait's steps (sk_period_feed). The old
 * filter smoothed in every step shorter than 1.5 x the current period. That
 * ratcheted: at 90 Hz (11.1 ms) the 60 Hz steps (16.67) sit right on 1.5 x,
 * and a burst of back-to-back present_wait returns (steps near 0) pulled the
 * period down; once under 11.1 ms the 16.67 steps were never accepted again,
 * and the keeper misread the queue depth (skips) for the rest of the session
 * (BO-210: the period seen at 4.8 ms on the Deck). Now:
 *   - a step under 2 ms is two returns of one refresh, not a refresh: not fed;
 *   - a step within 7/8 .. 9/8 of the period is smoothed in (1/16), on both
 *     sides alike (a report 4 ms late is not a refresh 4 ms long);
 *   - any other step is a candidate for a new period; eight in a row that
 *     agree within 5 % become it (the display changed its rate);
 *   - the caller feeds only steps between two new frames at the title's 60,
 *     so a 30 fps crash camera's 33 ms steps never become the period.
 *
 * The depth reading (sk_depth) and the three-in-a-row rule (sk_run) are the
 * gamescope formula, unchanged.
 *
 * Windows' stand-down (sk_down_*): the keeper stands down when a refill does
 * not stay queued twice. It used to stay down until the swapchain was made
 * again; now it clears after 60 s (doubling to 10 min on repeats) and the
 * empty-queue base B is measured again.
 */
#ifndef XBOX_SMOOTH_KEEPER_H
#define XBOX_SMOOTH_KEEPER_H

#include <math.h>
#include <stdint.h>

#define SK_STEP_MIN_NS   2000000ull
#define SK_STEP_MAX_NS   60000000ull
#define SK_RESEED_N      8
#define SK_KEEP_SAME     3

typedef struct {
    uint64_t p;          /* the period, ns; 0 not known */
    uint64_t cand;       /* a new period the steps show */
    int      cand_n;     /* steps in a row that showed it */
    unsigned reseeds;    /* times the period was taken anew */
} SkPeriod;

/* One step between two frames shown (both new frames at the title's 60).
 * Returns the period. */
static inline uint64_t sk_period_feed(SkPeriod *s, uint64_t d)
{
    if (d < SK_STEP_MIN_NS || d > SK_STEP_MAX_NS)
        return s->p;
    if (!s->p) {
        s->p = d;
        return d;
    }
    if (d * 8 >= s->p * 7 && d * 8 <= s->p * 9) {
        s->p = (s->p * 15 + d) / 16;
        s->cand_n = 0;
        return s->p;
    }
    if (s->cand_n && d * 20 >= s->cand * 19 && d * 20 <= s->cand * 21) {
        s->cand = (s->cand * (uint64_t)s->cand_n + d) / (uint64_t)(s->cand_n + 1);
        s->cand_n++;
    } else {
        s->cand = d;
        s->cand_n = 1;
    }
    if (s->cand_n >= SK_RESEED_N) {
        s->p = s->cand;
        s->cand_n = 0;
        s->reseeds++;
    }
    return s->p;
}

/* Refreshes a frame waited behind others, from its present-to-display time.
 * The report comes up to ~1 ms after the refresh and the present ~1 ms after
 * one: 1.5 ms of slack keeps 2 refreshes plus that at one frame queued. */
static inline int sk_depth(uint64_t p2d, uint64_t p)
{
    int d = (int)ceil(((double)p2d - 1.5e6) / (double)p) - 1;
    return d < 0 ? 0 : d;
}

typedef struct {
    int run_d, run_n;
} SkRun;

static inline void sk_run_reset(SkRun *r)
{
    r->run_d = -1;
    r->run_n = 0;
}

/* One depth reading. Returns +1 (refill: present once more), -1 (skip one)
 * or 0, when SK_KEEP_SAME readings in a row agree on a depth other than 1
 * and no correction is pending. */
static inline int sk_run(SkRun *r, int d, int pending)
{
    if (d == r->run_d)
        r->run_n++;
    else {
        r->run_d = d;
        r->run_n = 1;
    }
    if (r->run_n >= SK_KEEP_SAME && d != 1 && !pending) {
        r->run_n = 0;
        return d < 1 ? 1 : -1;
    }
    return 0;
}

/* Windows: the stand-down's clock. */
#define SK_DOWN_FIRST_NS  60000000000ull     /* 60 s */
#define SK_DOWN_MAX_NS    600000000000ull    /* 10 min */
#define SK_DOWN_FORGET_NS 1800000000000ull   /* 30 min without one: back to 60 s */

typedef struct {
    int      down;
    uint64_t since_ns, hold_ns;
    unsigned downs;
} SkDown;

static inline void sk_down_set(SkDown *s, uint64_t now)
{
    if (s->downs && now - s->since_ns > SK_DOWN_FORGET_NS)
        s->downs = 0;
    s->hold_ns = !s->downs ? SK_DOWN_FIRST_NS
               : s->hold_ns * 2 > SK_DOWN_MAX_NS ? SK_DOWN_MAX_NS : s->hold_ns * 2;
    s->downs++;
    s->down = 1;
    s->since_ns = now;
}

/* 1 when a stand-down just ran out (the caller measures B again). */
static inline int sk_down_expired(SkDown *s, uint64_t now)
{
    if (s->down && now - s->since_ns >= s->hold_ns) {
        s->down = 0;
        return 1;
    }
    return 0;
}

#endif /* XBOX_SMOOTH_KEEPER_H */
