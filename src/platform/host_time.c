/*
 * host_time.c - see host_time.h.
 *
 * Windows (and Wine): QueryPerformanceCounter, and a high-resolution
 * waitable timer per thread (Windows 10 1803+; Wine has it too), which waits
 * to the 100 ns unit where Sleep rounds to the system tick. Without one,
 * Sleep in whole milliseconds, rounded up.
 * POSIX: CLOCK_MONOTONIC and nanosleep.
 */
#include "host_time.h"

#ifdef _WIN32
#include <windows.h>
/* Not xbox_winnt.h's XBOX_THREAD_LOCAL: that header is the POSIX build's
 * Win32 vocabulary and is not included on Windows here. */
#if defined(_MSC_VER)
#define HOST_THREAD_LOCAL __declspec(thread)
#else
#define HOST_THREAD_LOCAL _Thread_local
#endif
#else
#include <time.h>
#ifdef __APPLE__
#include <pthread.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#endif

uint64_t xbox_HostNowNs(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    /* Split so the product never overflows: QPC runs at 10 MHz under Wine
     * and on most hosts, and t * 1e9 would wrap after 29 minutes. */
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000ull
           + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000ull
             / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void xbox_HostSleepNs(uint64_t ns)
{
    if (!ns)
        return;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    {
        static HOST_THREAD_LOCAL HANDLE timer;
        static HOST_THREAD_LOCAL int timer_failed;

        if (!timer && !timer_failed) {
            timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
            timer_failed = !timer;
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
                return;
        }
        Sleep((DWORD)((ns + 999999u) / 1000000u));
    }
#else
    {
        struct timespec ts = { (time_t)(ns / 1000000000u),
                               (long)(ns % 1000000000u) };
        nanosleep(&ts, NULL);
    }
#endif
}

void xbox_HostTimerThreadInit(void)
{
#ifdef __APPLE__
    /* A 60 Hz period. The computation budget is generous because the timer
     * thread runs guest ISRs and DPCs between sleeps, and it is preemptible.
     * XNU demotes a real-time thread only after seconds of continuous work
     * past its budget, so a guest routine that spins on this thread can hold
     * a core for that long before the scheduler steps in. Refused (a
     * sandbox, a future macOS), the thread stays a normal one: the vblank
     * grid keeps its rate and only the wake-up jitter is the old one. */
    mach_timebase_info_data_t tb;
    thread_time_constraint_policy_data_t p;
    double per_ms;

    if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.numer)
        return;
    per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    p.period = (uint32_t)(per_ms * 1000.0 / 60.0);
    p.computation = (uint32_t)(per_ms * 2.0);
    p.constraint = (uint32_t)(per_ms * 8.0);
    p.preemptible = 1;
    {
        kern_return_t kr = thread_policy_set(
            pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (kr != KERN_SUCCESS)
            fprintf(stderr, "[KERNEL] timer thread: time-constraint policy refused"
                    " (%d); vblank wake-ups may run late by a few ms\n", (int)kr);
    }
#endif
}
