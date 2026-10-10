/*
 * kernel_pause.c - the host pause; see kernel_pause.h.
 */
#include "kernel.h"
#include "kernel_pause.h"
#include "xbox_crash.h"       /* g_xbox_flip_count */
#include "platform/host_time.h"

#define PAUSE_LISTENERS 4
/* The hold wakes this often even without a resume: the watchdog's sign of
 * life, and a bound on a resume that was signalled before the wait began. */
#define PAUSE_TICK_MS 50

static volatile LONG s_paused;
static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static void (*s_listener[PAUSE_LISTENERS])(int);
static int s_n_listeners;

static BOOL CALLBACK pause_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_cv);
    return TRUE;
}

static void pause_once(void)
{
    InitOnceExecuteOnce(&s_once, pause_init, NULL, NULL);
}

void xbox_HostPauseListener(void (*fn)(int))
{
    pause_once();
    EnterCriticalSection(&s_lock);
    if (fn && s_n_listeners < PAUSE_LISTENERS)
        s_listener[s_n_listeners++] = fn;
    LeaveCriticalSection(&s_lock);
}

int xbox_HostPaused(void)
{
    return s_paused != 0;
}

void xbox_HostPause(int on)
{
    int i, n, changed;
    void (*fns[PAUSE_LISTENERS])(int);

    pause_once();
    EnterCriticalSection(&s_lock);
    changed = (s_paused != 0) != (on != 0);
    s_paused = on ? 1 : 0;
    n = s_n_listeners;
    for (i = 0; i < n; i++)
        fns[i] = s_listener[i];
    LeaveCriticalSection(&s_lock);
    if (!changed)
        return;
    /* Listeners outside the lock: the APU takes its own. */
    for (i = 0; i < n; i++)
        fns[i](on != 0);
    if (!on)
        WakeAllConditionVariable(&s_cv);
}

#define FLIP_RING 512
static volatile uint64_t s_flip_ns[FLIP_RING];
static volatile uint32_t s_flip_n;       /* written by the executor only */

static void flip_note(uint64_t ns)
{
    uint32_t n = s_flip_n;
    s_flip_ns[n % FLIP_RING] = ns;
    s_flip_n = n + 1;
}

int xbox_HostFlipTimes(uint64_t *ns, int max)
{
    uint32_t n = s_flip_n, have = n < FLIP_RING ? n : FLIP_RING, i, from;
    if (max <= 0)
        return 0;
    if (have > (uint32_t)max)
        have = (uint32_t)max;
    from = n - have;
    for (i = 0; i < have; i++)
        ns[i] = s_flip_ns[(from + i) % FLIP_RING];
    return (int)have;
}

void xbox_HostPauseHold(void)
{
    flip_note(xbox_HostNowNs());
    if (!s_paused)
        return;
    pause_once();
    EnterCriticalSection(&s_lock);
    while (s_paused) {
        SleepConditionVariableCS(&s_cv, &s_lock, PAUSE_TICK_MS);
        g_xbox_flip_count++;
    }
    LeaveCriticalSection(&s_lock);
    flip_note(0);                /* a gap in the flip times */
}
