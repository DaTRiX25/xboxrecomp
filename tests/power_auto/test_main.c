/*
 * power_auto test: the Auto power mode's controller (src/kernel/power_auto.c)
 * against synthetic hold traces (BO-175). Each trace is one sample per held
 * flip: when it returned, the interval the title asked for, the frame's
 * slack against that vblank and the wait's own lateness.
 */
#include "power_auto.h"

#include <stdio.h>
#include <stdlib.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

#define MS      1000000ll
#define VBL_NS  16666667ll

typedef struct {
    XboxPowerAuto a;
    uint64_t t;           /* the trace's clock: the last hold's return */
    int changes, min_level, max_level;
    unsigned rng;
} Run;

static void run_init(Run *r)
{
    xbox_PowerAutoInit(&r->a);
    r->t = 1000 * MS;
    r->changes = 0;
    r->min_level = r->max_level = xbox_PowerAutoLevel(&r->a);
    r->rng = 12345;
}

static unsigned rnd(Run *r)
{
    r->rng = r->rng * 1103515245u + 12345u;
    return (r->rng >> 16) & 0x7FFF;
}

/* One hold: the title's interval, the frame's slack (ms; < 0 a miss), the
 * wake lateness (ms; < 0 none), and how long since the last hold. */
static void hold(Run *r, uint32_t iv, double slack_ms, double wake_ms, int64_t gap_ns)
{
    XboxPowerAutoSample s;
    int lv;
    r->t += (uint64_t)gap_ns;
    s.now_ns = r->t;
    s.interval = iv;
    s.slack_ns = (int64_t)(slack_ms * MS);
    s.wake_ns = wake_ms < 0 ? -1 : (int64_t)(wake_ms * MS);
    r->changes += xbox_PowerAutoFeed(&r->a, &s);
    lv = xbox_PowerAutoLevel(&r->a);
    if (lv < r->min_level) r->min_level = lv;
    if (lv > r->max_level) r->max_level = lv;
}

/* Steady frames at an interval: slack 6-9 ms, wakes 0.05-0.3 ms. */
static void steady(Run *r, uint32_t iv, double seconds)
{
    long n = (long)(seconds * 60.0 / iv);
    for (long i = 0; i < n; i++)
        hold(r, iv, 6.0 + (rnd(r) % 300) / 100.0, 0.05 + (rnd(r) % 25) / 100.0,
             (int64_t)iv * VBL_NS);
}

/* A heavy scene whose frames miss now and then when the lead is short (the
 * Deck's clock): misses per 100 frames below lead_ok, none at or above. */
static void heavy(Run *r, double seconds, int lead_ok_level, int misses_per_100)
{
    long n = (long)(seconds * 60.0);
    for (long i = 0; i < n; i++) {
        int miss = xbox_PowerAutoLevel(&r->a) < lead_ok_level
                && (int)(rnd(r) % 100) < misses_per_100;
        if (miss)
            hold(r, 1, -3.0, -1, 2 * VBL_NS);
        else
            hold(r, 1, 1.0 + (rnd(r) % 200) / 100.0, 0.1, VBL_NS);
    }
}

/* 1. Steady 60: steps down to sleep only, one level at a time, and stays. */
static void test_steady_goes_down(void)
{
    Run r;
    run_init(&r);
    CHECK(xbox_PowerAutoLevel(&r.a) == XBOX_POWER_AUTO_START);
    CHECK(xbox_PowerAutoLeadNs(XBOX_POWER_AUTO_START) == 8000000ull);
    steady(&r, 1, 120);
    CHECK(xbox_PowerAutoLevel(&r.a) == 0);
    CHECK(r.a.tot.ups == 0 && r.a.tot.downs == XBOX_POWER_AUTO_START);
    CHECK(xbox_PowerAutoLeadNs(0) == 0);
}

/* 2. A 30 Hz phase (the crash camera, split screen) is not a miss: against
 * interval 2 the frames are on time. A fixed-60 view would see every other
 * vblank missed. */
static void test_30hz_phase_is_not_a_miss(void)
{
    Run r;
    run_init(&r);
    steady(&r, 1, 60);
    int lv = xbox_PowerAutoLevel(&r.a);
    for (int k = 0; k < 10; k++) {          /* ten crashes: 4 s at 30, 8 s at 60 */
        steady(&r, 2, 4);
        steady(&r, 1, 8);
    }
    CHECK(r.a.tot.near_misses == 0 && r.a.tot.hitches == 0);
    CHECK(r.a.tot.ups == 0);
    CHECK(xbox_PowerAutoLevel(&r.a) <= lv);
    /* Split screen: minutes at 30. */
    steady(&r, 2, 180);
    CHECK(r.a.tot.ups == 0 && xbox_PowerAutoLevel(&r.a) == 0);
}

/* 3. A loading stall (a long gap between holds, then frames far late) and
 * one-off hitches do not ratchet the lead up. */
static void test_loading_does_not_ratchet(void)
{
    Run r;
    run_init(&r);
    steady(&r, 1, 120);
    CHECK(xbox_PowerAutoLevel(&r.a) == 0);
    for (int k = 0; k < 20; k++) {
        /* The run-up: 5 s of frames that miss now and then (streaming
         * beside them), with no stall yet. */
        for (int i = 0; i < 300; i++)
            hold(&r, 1, rnd(&r) % 10 == 0 ? -4.0 : 3.0, 0.1, VBL_NS);
        /* Loading: a 1.5 s stall, 2 s of frames that each take 40-90 ms
         * (file IO beside them), a 600 ms stall, then the menu again. */
        hold(&r, 1, 2.0, 0.1, 1500 * MS);
        for (int i = 0; i < 30; i++)
            hold(&r, 1, -(25.0 + rnd(&r) % 50), -1, (40 + rnd(&r) % 50) * MS);
        hold(&r, 1, 3.0, 0.1, 600 * MS);
        steady(&r, 1, 20);
        /* A shader compile: one frame 30 ms late. */
        hold(&r, 1, -30.0, -1, 47 * MS);
        steady(&r, 1, 20);
    }
    CHECK(r.a.tot.hitches > 0);
    /* A run-up may start a step up; the stall voids it: nothing is kept. */
    CHECK(r.a.tot.reverts >= r.a.tot.ups - r.a.tot.downs);
    CHECK(xbox_PowerAutoLevel(&r.a) <= XBOX_POWER_AUTO_START);
    /* The scene changes take it back to the level last proven needed (the
     * start's, decaying); never above it. */
    CHECK(r.a.held <= XBOX_POWER_AUTO_START);
    /* Twenty minutes later the start's level has decayed. */
    CHECK(r.a.held < XBOX_POWER_AUTO_START);
}

/* 4. A heavy scene that misses at a short lead goes up until it stops, and
 * settles there without oscillating. */
static void test_heavy_goes_up_and_settles(void)
{
    Run r;
    run_init(&r);
    steady(&r, 1, 120);                    /* the menu: down to 0 */
    CHECK(xbox_PowerAutoLevel(&r.a) == 0);
    heavy(&r, 600, 3, 6);                  /* ten minutes of race, needs 8 ms */
    CHECK(xbox_PowerAutoLevel(&r.a) >= 3);
    CHECK(r.a.tot.ups >= 1);
    /* Few probes downward in ten minutes: the backoff holds. */
    CHECK(r.a.tot.downs <= 1 + 4);
    /* The near misses after the climb: the probes only. */
    long before = r.a.tot.near_misses;
    heavy(&r, 300, 3, 6);
    CHECK(r.a.tot.near_misses - before < 60);
}

/* 5. Late wakes (a timer that sleeps 2-4 ms long) at sleep only: a little
 * spin cures them, so it goes up a step and stays. */
static void test_late_wakes_go_up(void)
{
    Run r;
    run_init(&r);
    steady(&r, 1, 120);
    CHECK(xbox_PowerAutoLevel(&r.a) == 0);
    for (long i = 0; i < 60 * 60; i++) {
        double w = xbox_PowerAutoLevel(&r.a) == 0 && rnd(&r) % 10 == 0 ? 2.0 + rnd(&r) % 3 : 0.1;
        hold(&r, 1, 7.0, w, VBL_NS);
    }
    CHECK(xbox_PowerAutoLevel(&r.a) >= 1);
    CHECK(r.a.tot.ups >= 1);
}

/* 6. Misses that spinning does not cure (the GPU is the limit) go up once,
 * find no gain, and come back: no climb to the busy-wait. */
static void test_no_gain_comes_back(void)
{
    Run r;
    run_init(&r);
    steady(&r, 1, 120);
    for (long i = 0; i < 60 * 300; i++) {
        if (rnd(&r) % 100 < 8)
            hold(&r, 1, -4.0, -1, 2 * VBL_NS);
        else
            hold(&r, 1, 0.5, 0.1, VBL_NS);
    }
    CHECK(r.a.tot.reverts >= 1);
    CHECK(xbox_PowerAutoLevel(&r.a) <= 1);
    /* Five minutes: the climbs to the top are few (the ceiling's backoff). */
    CHECK(r.a.tot.reverts <= 3);
}

/* 7. No swap interval (0, or a title not paced by its vblank): ignored. */
static void test_no_interval_ignored(void)
{
    Run r;
    run_init(&r);
    for (int i = 0; i < 2000; i++)
        hold(&r, 0, -10.0, 5.0, VBL_NS);
    CHECK(r.a.tot.holds == 0 && xbox_PowerAutoLevel(&r.a) == XBOX_POWER_AUTO_START);
}

int main(void)
{
    test_steady_goes_down();
    test_30hz_phase_is_not_a_miss();
    test_loading_does_not_ratchet();
    test_heavy_goes_up_and_settles();
    test_late_wakes_go_up();
    test_no_gain_comes_back();
    test_no_interval_ignored();
    if (s_fail) {
        fprintf(stderr, "power_auto: %d failed\n", s_fail);
        return 1;
    }
    printf("power_auto: all passed\n");
    return 0;
}
