/*
 * kernel_pacing.c - see kernel_pacing.h.
 *
 * The wait is a generation counter under a lock with a condition variable.
 * A waker takes the lock, bumps the generation, drops the lock and
 * broadcasts. A waiter takes the lock and sleeps only while the generation
 * still equals the one it last saw; the condition variable releases the lock
 * atomically. The generation is never read outside the lock, so:
 *  - a wake between the lifted loop's memory read and the wait has already
 *    moved the generation, and the wait returns without sleeping (no lost
 *    wake-up);
 *  - a guest store that precedes a wake in program order (the vblank DPC's
 *    bump of the frame counter, then xbox_IrqlLeaveInterrupt) is visible to
 *    the loop's next MEM32 read after the wait returns, on x86-64 and arm64,
 *    because the lock pairs the waker's release with the waiter's acquire.
 * Only a site whose writer the kernel never signals times out, and then by
 * at most SPIN_TIMEOUT_MS.
 */
#include "kernel.h"
#include "kernel_pacing.h"
#include "recomp_env.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <time.h>
#endif

/* ── The vblank schedule ─────────────────────────────────── */

static uint64_t vblank_due(const XboxVblankSchedule *s, uint64_t n)
{
    /* n * 1e9 overflows 64 bits only after ~9.7 years of vblanks; the
     * 100 ms restart rule resets n long before a host runs that long. */
    return s->t0_ns + n * 1000000000ull / XBOX_VBLANK_HZ;
}

/* ── Locking the vblank to the display ─────────────────────
 *
 * RECOMP_VBLANK_SYNC=display. The emulated vblank is a host timer at
 * exactly 60 Hz; the display refreshes at its own 59.9x-60.0x Hz, from its
 * own clock. Free-running, the two drift through each other: a guest flip
 * that lands just before a display latch one second lands just after it the
 * next, and the screen repeats one frame and skips another -- the periodic
 * double/skip a FIFO swap chain only postpones. Emulators that care pace to
 * the host refresh instead (DuckStation's and PCSX2's "sync to host refresh
 * rate": when the guest rate is within a few percent of the display's, the
 * display is the clock).
 *
 * Here the vblank timer stays the clock -- a stalled or missing display
 * report never stops the title -- but its grid is moved onto the display's:
 * a present path reports each display refresh (xbox_VblankDisplayRefresh:
 * Metal's CVDisplayLink, D3D11's IDXGIOutput::WaitForVBlank, Vulkan's
 * present_wait timestamps), and each vblank's next edge is the display
 * refresh nearest one guest period on. Its period is the display's, or a
 * whole multiple of it (120 Hz: every second refresh), when that is within
 * 5% of 60 Hz; otherwise, or with no report for 250 ms, the timer runs on
 * its own 60.000 Hz grid as before. In phase with the display, a guest
 * flip's present always lands the same distance before the next latch:
 * each frame is shown exactly once. */
#define DISPLAY_STALE_NS 250000000ull
static volatile LONGLONG s_disp_ns;        /* last display refresh, host ns */
static volatile LONGLONG s_disp_period;    /* filtered refresh period, ns */
static int s_disp_lock = -1;               /* RECOMP_VBLANK_SYNC=display */

static int display_lock_on(void)
{
    if (s_disp_lock < 0) {
        const char *e = recomp_env(RENV_VBLANK_SYNC);
        s_disp_lock = e && !strcmp(e, "display");
    }
    return s_disp_lock;
}

int xbox_VblankDisplayLockOn(void)
{
    return display_lock_on();
}

void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns)
{
    uint64_t prev = (uint64_t)s_disp_ns, p = period_ns;

    if (!display_lock_on() || !when_ns)
        return;
    if (!p && prev && when_ns > prev)
        p = when_ns - prev;
    /* A report may be of a refresh still to come (CVDisplayLink's output
     * time): any point on the display's grid will do. */
    if (p >= 2000000u && p <= 50000000u) {
        uint64_t cur = (uint64_t)s_disp_period;
        /* A reported period is exact; one measured between two reports
         * jitters with the reporting thread's wake-up, so it is smoothed. */
        s_disp_period = (LONGLONG)(period_ns || !cur ? p : (cur * 15u + p) / 16u);
    }
    s_disp_ns = (LONGLONG)when_ns;
}

/* The display grid the vblank follows now: a refresh time and the guest
 * period on it. 0 when not locked. */
static int display_grid(uint64_t now_ns, uint64_t *ref_ns, uint64_t *period_ns)
{
    const uint64_t nominal = 1000000000ull / XBOX_VBLANK_HZ;
    uint64_t ref, pd, m, p;

    if (!display_lock_on())
        return 0;
    ref = (uint64_t)s_disp_ns;
    pd = (uint64_t)s_disp_period;
    if (!ref || !pd || (now_ns > ref && now_ns - ref > DISPLAY_STALE_NS))
        return 0;
    m = (nominal + pd / 2) / pd;           /* refreshes per guest vblank */
    if (!m)
        return 0;
    p = m * pd;
    if (p < nominal - nominal / 20 || p > nominal + nominal / 20)
        return 0;                          /* not within 5% of 60 Hz */
    *ref_ns = ref;
    *period_ns = p;
    return 1;
}

int xbox_VblankSchedule(XboxVblankSchedule *s, uint64_t now_ns,
                        uint64_t *next_due_ns, uint64_t *restarted_late_ns)
{
    uint64_t due;

    *restarted_late_ns = 0;
    if (!s->started) {
        s->started = 1;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    due = vblank_due(s, s->count);
    if (now_ns < due) {
        *next_due_ns = due;
        return 0;
    }
    if (now_ns - due > XBOX_VBLANK_RESTART_NS) {
        *restarted_late_ns = now_ns - due;
        s->t0_ns = now_ns;
        s->count = 0;
    }
    s->count++;
    *next_due_ns = vblank_due(s, s->count);
    {
        /* Locked: the next edge is one guest period (on the display's
         * clock) after the edge just raised, moved to the display refresh
         * nearest it -- from the edge, not from now, so at 120 Hz the same
         * refresh of each pair stays the guest's. The grid is re-anchored
         * there, so the free-running schedule takes over if the reports
         * stop. */
        uint64_t ref, p, pd, target, next;
        if (display_grid(now_ns, &ref, &p)) {
            pd = (uint64_t)s_disp_period;
            target = due + p;
            if (target >= ref)
                next = ref + (target - ref + pd / 2) / pd * pd;
            else
                next = ref - (ref - target + pd / 2) / pd * pd;
            while (next <= now_ns)
                next += p;
            s->t0_ns = next - s->count * 1000000000ull / XBOX_VBLANK_HZ;
            *next_due_ns = next;
        }
    }
    return 1;
}

/* ── Mode and the wait ───────────────────────────────────── */

#define SPIN_TIMEOUT_MS 1
/* A thread that comes back to a wait sooner than this after its last one is
 * still in the same loop (a pass is a few guest instructions; the game's
 * frame work between two loops takes milliseconds). Longer, and its last
 * wait was the loop's final pass: the outcome that let the loop exit. */
#define SPIN_EXIT_GAP_NS 500000ull
/* The waiting thread's CPU clock is read on one call in this many. */
#define SPIN_CPU_SAMPLE 64

volatile int g_xbox_spin_sleep;

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static uint32_t s_gen;                       /* under s_lock only */
static XBOX_THREAD_LOCAL uint32_t t_seen_gen;

/* Vblank waits (xbox_VblankWaitUntil): their own lock and condition
 * variable, so the vblank's broadcast wakes only the threads waiting for
 * one. */
static CRITICAL_SECTION s_vbl_lock;
static CONDITION_VARIABLE s_vbl_cv;

static BOOL CALLBACK pacing_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    InitializeCriticalSection(&s_vbl_lock);
    InitializeConditionVariable(&s_vbl_cv);
    return TRUE;
}

static void pacing_once(void)
{
    InitOnceExecuteOnce(&s_once, pacing_init, NULL, NULL);
}

void xbox_PacingSetMode(int mode)
{
    pacing_once();
    g_xbox_spin_sleep = (mode == XBOX_PACING_SLEEP);
}

int xbox_PacingMode(void)
{
    return g_xbox_spin_sleep ? XBOX_PACING_SLEEP : XBOX_PACING_SPIN;
}

/* Per-site counters: an open-addressed table keyed by the loop's VA. A
 * title has a handful of lowered sites; past the table's size they share
 * the last slot, reported as "other". */
#define SITE_SLOTS 31
typedef struct {
    volatile LONG va;          /* 0 = free */
    volatile LONG waits, immediate, wakes, timeouts, dispatch;
    /* How loops left: their last pass returned on a signal (or at once),
     * or on the 1 ms timeout. Timeouts mid-loop are normal (a vblank wait
     * spans up to 16 of them); a loop that keeps leaving on a timeout has a
     * writer the kernel never signals. */
    volatile LONG exit_wake, exit_timeout;
    volatile LONG inwait_us;   /* sampled CPU inside the wait, x SAMPLE */
    volatile LONG calls;       /* for the sampling stride */
    /* The waiting thread's CPU clock against wall time, first and last
     * sample in the window: its whole CPU share, game work included. */
    volatile LONGLONG cpu0_ns, wall0_ns, cpu1_ns, wall1_ns;
} SpinSite;
static SpinSite s_sites[SITE_SLOTS + 1];

/* The calling thread's last wait, for the exit count (mode sleep only). */
enum { LAST_NONE, LAST_WAKE, LAST_TIMEOUT };
static XBOX_THREAD_LOCAL SpinSite *t_last_site;
static XBOX_THREAD_LOCAL int t_last_outcome;
static XBOX_THREAD_LOCAL uint64_t t_last_ret_ns;

static SpinSite *site_for(uint32_t va)
{
    unsigned h = (va * 2654435761u) % SITE_SLOTS, i;

    for (i = 0; i < SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[(h + i) % SITE_SLOTS];
        LONG cur = s->va;
        if (cur == (LONG)va)
            return s;
        if (!cur && InterlockedCompareExchange(&s->va, (LONG)va, 0) == 0)
            return s;
        if (s->va == (LONG)va)
            return s;
    }
    return &s_sites[SITE_SLOTS];
}

void xbox_SpinWait(uint32_t va)
{
    SpinSite *site;
    int sample, woke = 0, immediate = 0;
    uint64_t c0 = 0, now;

    /* A lowered loop's pass is between two guest instructions: a holder
     * spinning here at DISPATCH takes a posted interrupt, as a CPU would.
     * At "spin" pacing the macro calls only for that. */
    xbox_IrqSafePoint();
#if !defined(_WIN32)
    /* And a hand-over point for guest_cpus=token (kernel_thread.c): a loop
     * still waiting lets a thread that wants the guest CPU run now rather
     * than at the holder's next preemption. */
    guest_turn_yield();
#endif
    if (!g_xbox_spin_sleep)
        return;
    site = site_for(va);
    now = xbox_HostNowNs();

    /* The previous wait on this thread ended its loop if the thread was
     * away long enough: count how that loop left. */
    if (t_last_site && t_last_outcome != LAST_NONE
        && now - t_last_ret_ns > SPIN_EXIT_GAP_NS)
        InterlockedIncrement(t_last_outcome == LAST_WAKE
                             ? &t_last_site->exit_wake
                             : &t_last_site->exit_timeout);
    t_last_site = site;
    t_last_outcome = LAST_NONE;
    t_last_ret_ns = now;

    InterlockedIncrement(&site->waits);
    /* At DISPATCH or with the gate held the DPC that ends this loop cannot
     * run until the thread leaves; sleeping would only add latency. */
    if (xbox_IrqlThisThreadBlocksDpcs()) {
        InterlockedIncrement(&site->dispatch);
        return;
    }
    sample = (InterlockedIncrement(&site->calls) % SPIN_CPU_SAMPLE) == 0;
    if (sample)
        c0 = xbox_ThreadCpuNs();

    pacing_once();
    EnterCriticalSection(&s_lock);
    if (s_gen != t_seen_gen) {
        immediate = 1;
    } else {
        SleepConditionVariableCS(&s_cv, &s_lock, SPIN_TIMEOUT_MS);
        woke = (s_gen != t_seen_gen);
    }
    t_seen_gen = s_gen;
    LeaveCriticalSection(&s_lock);

    if (immediate)
        InterlockedIncrement(&site->immediate);
    else if (woke)
        InterlockedIncrement(&site->wakes);
    else
        InterlockedIncrement(&site->timeouts);
    t_last_outcome = immediate || woke ? LAST_WAKE : LAST_TIMEOUT;
    t_last_ret_ns = xbox_HostNowNs();

    if (sample) {
        uint64_t c1 = xbox_ThreadCpuNs(), w = xbox_HostNowNs();
        InterlockedExchangeAdd(&site->inwait_us,
                               (LONG)((c1 - c0) / 1000u * SPIN_CPU_SAMPLE));
        if (!site->wall0_ns) {
            site->cpu0_ns = (LONGLONG)c1;
            site->wall0_ns = (LONGLONG)w;
        }
        site->cpu1_ns = (LONGLONG)c1;
        site->wall1_ns = (LONGLONG)w;
    }
}

/* ── Waiting for a vblank ────────────────────────────────── */

/* The timer thread, right after it counted a vblank. Taking the lock orders
 * the broadcast after a waiter's check of the count: a waiter either saw the
 * new count or is already asleep on the variable (no lost wake-up). */
void xbox_VblankSignal(void)
{
    pacing_once();
    EnterCriticalSection(&s_vbl_lock);
    LeaveCriticalSection(&s_vbl_lock);
    WakeAllConditionVariable(&s_vbl_cv);
}

int xbox_VblankWaitUntil(uint32_t target, uint32_t timeout_ms)
{
    uint64_t deadline = xbox_HostNowNs() + (uint64_t)timeout_ms * 1000000u;
    int ok = 1;

    pacing_once();
    EnterCriticalSection(&s_vbl_lock);
    while ((int32_t)(xbox_VblankCount() - target) < 0) {
        uint64_t now = xbox_HostNowNs();
        DWORD ms;
        if (now >= deadline) {
            ok = 0;
            break;
        }
        ms = (DWORD)((deadline - now + 999999u) / 1000000u);
        SleepConditionVariableCS(&s_vbl_cv, &s_vbl_lock, ms);
    }
    LeaveCriticalSection(&s_vbl_lock);
    return ok;
}

void xbox_SpinWake(void)
{
    if (!g_xbox_spin_sleep)
        return;
    pacing_once();
    EnterCriticalSection(&s_lock);
    s_gen++;
    LeaveCriticalSection(&s_lock);
    WakeAllConditionVariable(&s_cv);
}

int xbox_SpinSiteStats(uint32_t va, XboxSpinSiteStats *out)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++) {
        SpinSite *s = &s_sites[i];
        if (s->va != (LONG)va || !s->waits)
            continue;
        out->va = va;
        out->waits = s->waits;
        out->immediate = s->immediate;
        out->wakes = s->wakes;
        out->timeouts = s->timeouts;
        out->dispatch = s->dispatch;
        out->exit_wake = s->exit_wake;
        out->exit_timeout = s->exit_timeout;
        return 1;
    }
    return 0;
}

static void site_reset(SpinSite *s)
{
    InterlockedExchange(&s->waits, 0);
    InterlockedExchange(&s->immediate, 0);
    InterlockedExchange(&s->wakes, 0);
    InterlockedExchange(&s->timeouts, 0);
    InterlockedExchange(&s->dispatch, 0);
    InterlockedExchange(&s->exit_wake, 0);
    InterlockedExchange(&s->exit_timeout, 0);
    InterlockedExchange(&s->inwait_us, 0);
    /* Not atomic with a waiter's sample: a sample landing between these
     * stores and the next window's first one can pair an old cpu0 with a
     * new wall0, or count a wait in the window it did not finish in. Only
     * the trace's thread CPU share for one window can be off; nothing
     * reads these counters for pacing itself. */
    s->wall0_ns = 0;
}

void xbox_SpinStatsReset(void)
{
    unsigned i;

    for (i = 0; i <= SITE_SLOTS; i++)
        site_reset(&s_sites[i]);
}

/* ── CPU clocks ──────────────────────────────────────────── */

uint64_t xbox_ThreadCpuNs(void)
{
#ifdef _WIN32
    /* Wine advances these in system-tick steps (15.6 ms): unbiased over a
     * trace window, coarse within one. */
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t xbox_ProcessCpuNs(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000ull
           + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000ull;
#endif
}

/* ── RECOMP_TRACE=pacing ─────────────────────────────────── */

#define TRACE_WINDOW 600

static int s_trace = -1;          /* 0 off, 1 summaries, 2 also every flip */
/* The vblank gap range since the last summary, in µs (the timer thread
 * writes, the walker reads and resets). */
static volatile LONG s_gap_min_us = 0x7FFFFFFF, s_gap_max_us;
static uint64_t s_last_vblank_ns;   /* timer thread only */
/* RECOMP_VBLANK_SYNC=display: each vblank's distance after the display
 * refresh before it, in us, over the window. */
static volatile LONG s_ph_min_us = 0x7FFFFFFF, s_ph_max_us = -1;

static int trace_mode(void)
{
    if (s_trace < 0) {
        const char *v = recomp_env(RENV_PACING_TRACE);
        s_trace = !v || !*v || *v == '0' ? 0 : (strcmp(v, "all") == 0 ? 2 : 1);
    }
    return s_trace;
}

void xbox_PacingVblank(uint64_t now_ns)
{
    if (!trace_mode())
        return;
    if (s_last_vblank_ns) {
        LONG g = (LONG)((now_ns - s_last_vblank_ns) / 1000u), cur;
        while ((cur = s_gap_min_us) > g &&
               InterlockedCompareExchange(&s_gap_min_us, g, cur) != cur)
            ;
        while ((cur = s_gap_max_us) < g &&
               InterlockedCompareExchange(&s_gap_max_us, g, cur) != cur)
            ;
    }
    s_last_vblank_ns = now_ns;
    {
        uint64_t ref, p;
        if (display_grid(now_ns, &ref, &p)) {
            uint64_t pd = (uint64_t)s_disp_period;
            LONG ph = (LONG)((now_ns >= ref ? (now_ns - ref) % pd
                                           : pd - (ref - now_ns) % pd) / 1000u);
            if (ph < s_ph_min_us) s_ph_min_us = ph;
            if (ph > s_ph_max_us) s_ph_max_us = ph;
        }
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static double pct(const uint32_t *sorted, int n, double p)
{
    int i = (int)(p * (n - 1) + 0.5);
    return sorted[i] / 1000.0;
}

static void site_line(const SpinSite *s, double window_s, int other)
{
    double inwait = window_s > 0 ? s->inwait_us / 1e6 / window_s * 100.0 : 0;
    double thread = 0;
    if (s->wall1_ns > s->wall0_ns)
        thread = (double)(s->cpu1_ns - s->cpu0_ns)
                 / (double)(s->wall1_ns - s->wall0_ns) * 100.0;
    if (other)
        fprintf(stderr, "[PACING]   site other:");
    else
        fprintf(stderr, "[PACING]   site 0x%08X:", (uint32_t)s->va);
    fprintf(stderr, " waits %ld immediate %ld wakes %ld timeouts %ld"
            " dispatch %ld; exits wake %ld timeout %ld;"
            " cpu in-wait %.1f%% thread %.0f%%\n",
            (long)s->waits, (long)s->immediate, (long)s->wakes,
            (long)s->timeouts, (long)s->dispatch, (long)s->exit_wake,
            (long)s->exit_timeout, inwait, thread);
}

/* A host present (the backend's swap chain Present returned, or Metal's
 * presented handler with the time the drawable reached the display): under
 * RECOMP_TRACE=pacing=all one line each, so the present-to-present intervals
 * -- what the display was handed -- can be read beside the guest's flips
 * (tools/deck/stats.py). when_ns 0: now. */
void xbox_PacingPresent(uint64_t when_ns)
{
    static volatile LONG n;
    if (trace_mode() != 2)
        return;
    fprintf(stderr, "[PACING] present %ld t_us %llu\n", (long)InterlockedIncrement(&n),
            (unsigned long long)((when_ns ? when_ns : xbox_HostNowNs()) / 1000u));
}

void xbox_PacingFlip(uint32_t flip, uint32_t batches)
{
    /* Walker thread only. */
    static uint64_t last_ns, win_ns, win_cpu;
    static uint32_t iv_us[TRACE_WINDOW];
    static int n;
    uint64_t now;
    int mode = trace_mode();

    if (!mode)
        return;
    now = xbox_HostNowNs();
    if (mode == 2)
        fprintf(stderr, "[PACING] flip %u t_us %llu batches %u\n", flip,
                (unsigned long long)(now / 1000u), batches);
    if (!win_ns) {
        win_ns = now;
        win_cpu = xbox_ProcessCpuNs();
    }
    if (last_ns)
        iv_us[n++] = (uint32_t)((now - last_ns) / 1000u);
    last_ns = now;
    if (n < TRACE_WINDOW)
        return;

    {
        uint32_t s[TRACE_WINDOW];
        uint64_t cpu = xbox_ProcessCpuNs();
        double window_s = (double)(now - win_ns) / 1e9;
        LONG gmin = InterlockedExchange(&s_gap_min_us, 0x7FFFFFFF);
        LONG gmax = InterlockedExchange(&s_gap_max_us, 0);
        unsigned i;

        memcpy(s, iv_us, sizeof s);
        qsort(s, TRACE_WINDOW, sizeof s[0], cmp_u32);
        fprintf(stderr, "[PACING] flips %d: interval p5 %.1f p50 %.1f p95 %.1f"
                " max %.1f ms; ", TRACE_WINDOW, pct(s, TRACE_WINDOW, 0.05),
                pct(s, TRACE_WINDOW, 0.50), pct(s, TRACE_WINDOW, 0.95),
                s[TRACE_WINDOW - 1] / 1000.0);
        if (gmax)
            fprintf(stderr, "vblank gap %.1f-%.1f ms; ", gmin / 1000.0,
                    gmax / 1000.0);
        else
            fprintf(stderr, "vblank gap -; ");
        {
            LONG pmin = InterlockedExchange(&s_ph_min_us, 0x7FFFFFFF);
            LONG pmax = InterlockedExchange(&s_ph_max_us, -1);
            if (pmax >= 0)
                fprintf(stderr, "display %.3f ms, vblank %.2f-%.2f ms after its"
                        " refresh; ", (double)s_disp_period / 1e6, pmin / 1000.0,
                        pmax / 1000.0);
        }
        fprintf(stderr, "cpu process %.0f%%; mode %s\n",
                window_s > 0 ? (double)(cpu - win_cpu) / 1e9 / window_s * 100.0 : 0,
                g_xbox_spin_sleep ? "sleep" : "spin");
        for (i = 0; i <= SITE_SLOTS; i++) {
            SpinSite *st = &s_sites[i];
            if ((st->va || i == SITE_SLOTS) && st->waits)
                site_line(st, window_s, i == SITE_SLOTS);
            site_reset(st);
        }
        fflush(stderr);
        n = 0;
        win_ns = now;
        win_cpu = cpu;
    }
}
