/*
 * android_perf.c - what the game asks of Android for a steady 60 fps.
 *
 * Played with a controller alone, a phone sees no touches: some systems
 * (MIUI on a Poco F4) then let the display's refresh fall and end the
 * touch boost, and the game ran at ~30 fps until the screen was touched.
 * Two requests here, both looked up at run time (dlsym) so that a system
 * without them only says so:
 *
 *   frame rate   ANativeWindow_setFrameRate(60, FIXED_SOURCE) (API 30) on
 *                every window surface the presenter makes, the first and
 *                each one after the app comes back (fb_present_vulkan.c,
 *                make_surface). The display mode itself is asked for by the
 *                activity (GameActivity.requestDisplayMode).
 *
 *   ADPF         APerformanceHint (API 33): a session per hot thread
 *                (guest-main: the title; nv2a-ack: the pushbuffer executor;
 *                sdl-main: the present), each with a 16.67 ms target. Once
 *                per presented frame each session is told how long its
 *                thread ran since the frame before: the thread's own CPU
 *                time (its CLOCK_THREAD_CPUTIME), not the wall time between
 *                frames, so a thread that sleeps or waits on the GPU asks
 *                for nothing. Closed in the way out (fb_present_sdl.c).
 *
 * RECOMP_ANDROID_ADPF=0 / RECOMP_ANDROID_FRAMERATE=0: off, for A/B.
 */
#if defined(__ANDROID__)

#include <android/api-level.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGA "[ANDROID-PERF] "
/* RECOMP_ANDROID_ADPF_TARGET_MS=<ms>: another target, for tests. */
static int64_t s_target_ns = 16666667ll;
#define TARGET_NS s_target_ns

static void *libandroid(void)
{
    static void *h;
    static int tried;
    if (!tried) {
        tried = 1;
        h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    }
    return h;
}

static int env_off(const char *name)
{
    const char *e = getenv(name);
    return e && e[0] == '0';
}

/* ── frame rate ─────────────────────────────────────────────────────────── */

typedef int32_t (*set_frame_rate_fn)(ANativeWindow *, float, int8_t);

void xbox_AndroidSetFrameRate(void *window, const char *why)
{
    static set_frame_rate_fn fn;
    static int resolved;
    int api = android_get_device_api_level();
    int32_t r;

    if (!window)
        return;
    if (env_off("RECOMP_ANDROID_FRAMERATE")) {
        XR_NOTICE(LOGA "frame rate: not asked for (RECOMP_ANDROID_FRAMERATE=0)\n");
        return;
    }
    if (!resolved) {
        resolved = 1;
        if (api >= 30 && libandroid())
            fn = (set_frame_rate_fn)dlsym(libandroid(), "ANativeWindow_setFrameRate");
    }
    if (!fn) {
        XR_NOTICE(LOGA "frame rate: ANativeWindow_setFrameRate is not there (API %d)\n",
                api);
        return;
    }
    /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1 */
    r = fn((ANativeWindow *)window, 60.0f, 1);
    XR_NOTICE(LOGA "frame rate: 60 fps, fixed source, asked for on the %s surface"
            " (API %d): %s (%d)\n", why, api, r == 0 ? "ok" : "refused", (int)r);
}

/* ── ADPF performance hint sessions ─────────────────────────────────────── */

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef APerformanceHintManager *(*ph_get_manager_fn)(void);
typedef APerformanceHintSession *(*ph_create_fn)(APerformanceHintManager *, const int32_t *,
                                                 size_t, int64_t);
typedef int64_t (*ph_rate_fn)(APerformanceHintManager *);
typedef int (*ph_report_fn)(APerformanceHintSession *, int64_t);
typedef void (*ph_close_fn)(APerformanceHintSession *);

static struct {
    int state;                       /* 0 not tried, 1 ok, -1 not available */
    APerformanceHintManager *mgr;
    ph_create_fn create;
    ph_report_fn report;
    ph_close_fn close;
} s_ph;

static const char *const k_roles[] = { "guest-main", "nv2a-ack", "sdl-main" };
#define NROLES (int)(sizeof k_roles / sizeof k_roles[0])

struct hot_thread {
    volatile int32_t tid;            /* written last, by the thread itself */
    clockid_t clock;                 /* its CPU-time clock */
    int32_t session_tid;             /* the tid the session was made for */
    APerformanceHintSession *session;
    int64_t last_cpu;                /* its CPU time at the frame before */
    int failed;                      /* a session could not be made */
};
static struct hot_thread s_hot[NROLES];
static int64_t s_last_wall;
static volatile int s_closed;
static uint64_t s_frames;

static int ph_load(void)
{
    int api;
    ph_get_manager_fn get;
    ph_rate_fn rate;

    if (s_ph.state)
        return s_ph.state > 0;
    s_ph.state = -1;
    api = android_get_device_api_level();
    if (env_off("RECOMP_ANDROID_ADPF")) {
        XR_NOTICE(LOGA "ADPF: off (RECOMP_ANDROID_ADPF=0)\n");
        return 0;
    }
    if (api < 33 || !libandroid()) {
        XR_NOTICE(LOGA "ADPF: not on this system (API %d, needs 33)\n", api);
        return 0;
    }
    get = (ph_get_manager_fn)dlsym(libandroid(), "APerformanceHint_getManager");
    rate = (ph_rate_fn)dlsym(libandroid(), "APerformanceHint_getPreferredUpdateRateNanos");
    s_ph.create = (ph_create_fn)dlsym(libandroid(), "APerformanceHint_createSession");
    s_ph.report = (ph_report_fn)dlsym(libandroid(), "APerformanceHint_reportActualWorkDuration");
    s_ph.close = (ph_close_fn)dlsym(libandroid(), "APerformanceHint_closeSession");
    if (!get || !s_ph.create || !s_ph.report || !s_ph.close) {
        XR_NOTICE(LOGA "ADPF: APerformanceHint is missing from libandroid (API %d)\n", api);
        return 0;
    }
    {
        const char *e = getenv("RECOMP_ANDROID_ADPF_TARGET_MS");
        double ms = e ? atof(e) : 0.0;
        if (ms >= 1.0 && ms <= 100.0)
            s_target_ns = (int64_t)(ms * 1e6);
    }
    s_ph.mgr = get();
    if (!s_ph.mgr) {
        XR_NOTICE(LOGA "ADPF: no performance hint manager (API %d): the device"
                " does not support it\n", api);
        return 0;
    }
    XR_NOTICE(LOGA "ADPF: performance hint manager ok (API %d, preferred update"
            " every %.1f ms)\n", api, rate ? (double)rate(s_ph.mgr) / 1e6 : -1.0);
    s_ph.state = 1;
    return 1;
}

/* From xbox_log_thread_role (kernel_bridge.c), on the thread itself. */
void xbox_AndroidPerfThread(const char *role)
{
    int i;
    clockid_t c;
    for (i = 0; i < NROLES; i++)
        if (!strcmp(role, k_roles[i]))
            break;
    if (i == NROLES)
        return;
    if (pthread_getcpuclockid(pthread_self(), &c) != 0)
        return;
    s_hot[i].clock = c;
    __atomic_store_n(&s_hot[i].tid, (int32_t)gettid(), __ATOMIC_RELEASE);
}

static int64_t clock_ns(clockid_t c, int *ok)
{
    struct timespec t;
    if (clock_gettime(c, &t) != 0) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void close_slot(struct hot_thread *h)
{
    if (h->session)
        s_ph.close(h->session);
    h->session = NULL;
    h->session_tid = 0;
}

/* Once per presented frame, on the present thread (sdl-main). */
void xbox_AndroidPerfFrame(void)
{
    int i, ok;
    int64_t wall, gap;

    if (s_closed || !ph_load())
        return;
    wall = clock_ns(CLOCK_MONOTONIC, &ok);
    gap = s_last_wall ? wall - s_last_wall : 0;
    s_last_wall = wall;
    s_frames++;
    for (i = 0; i < NROLES; i++) {
        struct hot_thread *h = &s_hot[i];
        int32_t tid = __atomic_load_n(&h->tid, __ATOMIC_ACQUIRE);
        int64_t cpu;

        if (!tid || h->failed)
            continue;
        if (tid != h->session_tid) {
            close_slot(h);
            h->session = s_ph.create(s_ph.mgr, &tid, 1, TARGET_NS);
            if (!h->session) {
                h->failed = 1;
                XR_NOTICE(LOGA "ADPF: no session for %s (tid %d): the system refused it (no"
                          " ADPF support in this device's power HAL?)\n", k_roles[i], (int)tid);
                continue;
            }
            h->session_tid = tid;
            h->last_cpu = clock_ns(h->clock, &ok);
            XR_NOTICE(LOGA "ADPF: session for %s (tid %d), target %.2f ms: ok\n",
                    k_roles[i], (int)tid, (double)TARGET_NS / 1e6);
            continue;
        }
        cpu = clock_ns(h->clock, &ok);
        if (!ok) {
            /* The thread is gone. */
            XR_NOTICE(LOGA "ADPF: %s (tid %d) ended; its session is closed\n",
                    k_roles[i], (int)tid);
            close_slot(h);
            h->failed = 1;
            continue;
        }
        /* A gap (a pause, the app in the background, a long load) is no
         * frame's work: started over from here. */
        if (gap > 0 && gap < 100000000ll && cpu > h->last_cpu) {
            int64_t busy = cpu - h->last_cpu;
            int r = s_ph.report(h->session, busy);
            if (r != 0) {
                static int said;
                if (!said++)
                    XR_NOTICE(LOGA "ADPF: report for %s refused (%d)\n", k_roles[i], r);
            }
        }
        h->last_cpu = cpu;
    }
    if (s_frames == 600) {
        int n = 0;
        for (i = 0; i < NROLES; i++)
            n += s_hot[i].session != NULL;
        fprintf(stderr, LOGA "ADPF: %d session%s reporting each frame's work (600 frames so"
                " far)\n", n, n == 1 ? "" : "s");
    }
}

/* The way out (fb_present_sdl.c, android_teardown_begin), on sdl-main. */
void xbox_AndroidPerfClose(void)
{
    int i, n = 0;
    if (s_closed)
        return;
    s_closed = 1;
    if (s_ph.state <= 0)
        return;
    for (i = 0; i < NROLES; i++) {
        if (s_hot[i].session)
            n++;
        close_slot(&s_hot[i]);
    }
    XR_NOTICE(LOGA "ADPF: %d session%s closed\n", n, n == 1 ? "" : "s");
}

#else
typedef int xbox_android_perf_unused;
#endif
