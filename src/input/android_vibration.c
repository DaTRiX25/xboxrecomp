/*
 * android_vibration.c - the title's rumble on the device's own vibrator.
 *
 * A handheld's built-in pad is an input device with no motors (the Retroid
 * Pocket 5's shows up as "Xbox Wireless Controller" and cannot rumble), so
 * the pad's rumble goes nowhere. The device has a vibrator of its own:
 * android.os.Vibrator, driven from here through JNI (no Java of the title's
 * is involved). The approach follows the design of Xerenge's Android build
 * (device vibration as a fallback for a pad without motors); the code is this
 * runtime's own.
 *
 * RECOMP_ANDROID_VIBRATION = auto | controller | device | off (default auto):
 *   auto        the pad when it can rumble, else the device
 *   controller  the pad only (what every other host does)
 *   device      the device only; the pad is told to stop
 *   off         neither
 *
 * xbox_AndroidVibration() is called where the title's motor speeds reach the
 * host pad (xinput_device.c). It never blocks: it leaves a strength for a
 * worker thread, which makes the binder calls.
 *
 *   - Strength 0..255: the big motor in full, the small one at half weight,
 *     in steps of 16, so a hum that wanders every frame does not restart the
 *     motor every frame.
 *   - Each strength is one long one-shot (5 s) that replaces what plays; it
 *     is renewed after 4 s only while the title keeps asking, so the motor
 *     ends by itself if the title stops calling (a pause, a crash).
 *   - No replacement sooner than 30 ms after the last; a pulse lasts at
 *     least 40 ms (the title's shortest rumbles are a frame or two).
 *   - Without amplitude control the motor is on above a threshold, else off.
 */
#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_system.h>

enum { VIB_AUTO, VIB_CONTROLLER, VIB_DEVICE, VIB_OFF };

#define SHOT_MS       5000
#define RENEW_MS      4000
#define MIN_GAP_MS    30
#define MIN_PULSE_MS  40
#define ASK_STALE_MS  1000      /* the title re-sends a held rumble every 150 ms */
#define ONOFF_FLOOR   48        /* no amplitude control: on from here */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv;
static int      s_started, s_mode = -1;
static int      s_want;             /* 0..255, quantised */
static uint64_t s_asked_ms;         /* the title's last call */
static unsigned s_sent, s_cancelled;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int mode(void)
{
    if (s_mode < 0) {
        const char *m = getenv("RECOMP_ANDROID_VIBRATION");
        s_mode = !m || !*m || !strcmp(m, "auto") ? VIB_AUTO
               : !strcmp(m, "controller") ? VIB_CONTROLLER
               : !strcmp(m, "device") ? VIB_DEVICE
               : !strcmp(m, "off") || !strcmp(m, "0") ? VIB_OFF : VIB_AUTO;
    }
    return s_mode;
}

/* The worker: owns every JNI object. */
static void *worker(void *arg)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();     /* attaches this thread */
    jobject act, vib = NULL, name;
    jclass cls, eff = NULL;
    jmethodID get, vibrate = NULL, cancel = NULL, one_shot = NULL, has, amp;
    int have = 0, has_amp = 0, playing = 0;
    uint64_t last_call = 0, pulse_at = 0, shot_at = 0;
    struct timespec until;

    (void)arg;
    pthread_setname_np(pthread_self(), "vibration");
    act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    if (act) {
        cls = (*env)->GetObjectClass(env, act);
        get = (*env)->GetMethodID(env, cls, "getSystemService",
                                  "(Ljava/lang/String;)Ljava/lang/Object;");
        name = (*env)->NewStringUTF(env, "vibrator");
        vib = get ? (*env)->CallObjectMethod(env, act, get, name) : NULL;
        if (vib && !(*env)->ExceptionCheck(env)) {
            jclass vc = (*env)->GetObjectClass(env, vib);
            vib = (*env)->NewGlobalRef(env, vib);
            has = (*env)->GetMethodID(env, vc, "hasVibrator", "()Z");
            amp = (*env)->GetMethodID(env, vc, "hasAmplitudeControl", "()Z");
            vibrate = (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;)V");
            cancel = (*env)->GetMethodID(env, vc, "cancel", "()V");
            eff = (*env)->FindClass(env, "android/os/VibrationEffect");
            if (eff) {
                eff = (jclass)(*env)->NewGlobalRef(env, eff);
                one_shot = (*env)->GetStaticMethodID(env, eff, "createOneShot",
                                                     "(JI)Landroid/os/VibrationEffect;");
            }
            have = has && vibrate && cancel && one_shot
                && (*env)->CallBooleanMethod(env, vib, has);
            has_amp = have && amp && (*env)->CallBooleanMethod(env, vib, amp);
        }
        (*env)->ExceptionClear(env);
    }
    fprintf(stderr, "  [RUMBLE] device vibrator: %s%s\n", have ? "present" : "none",
            have ? (has_amp ? ", amplitude control" : ", on/off only (no amplitude control)") : "");
    if (!have)
        return NULL;

    pthread_mutex_lock(&s_mu);
    for (;;) {
        uint64_t t = now_ms();
        int want = s_want, level;

        if (want && t - s_asked_ms > ASK_STALE_MS)
            want = 0;                           /* the title went quiet */
        if (!has_amp)
            want = want >= ONOFF_FLOOR ? 255 : 0;
        level = want;
        if (level != playing || (playing && t - shot_at >= RENEW_MS)) {
            uint64_t wait_until = last_call + MIN_GAP_MS;
            if (!level && playing && pulse_at + MIN_PULSE_MS > wait_until)
                wait_until = pulse_at + MIN_PULSE_MS;
            if (t < wait_until) {
                clock_gettime(CLOCK_MONOTONIC, &until);
                until.tv_nsec += (long)(wait_until - t) * 1000000L;
                until.tv_sec += until.tv_nsec / 1000000000L;
                until.tv_nsec %= 1000000000L;
                pthread_cond_timedwait(&s_cv, &s_mu, &until);
                continue;
            }
            pthread_mutex_unlock(&s_mu);
            if (level) {
                jobject e = (*env)->CallStaticObjectMethod(env, eff, one_shot,
                                (jlong)SHOT_MS, (jint)(has_amp ? (level < 1 ? 1 : level) : -1));
                if (e) {
                    (*env)->CallVoidMethod(env, vib, vibrate, e);
                    (*env)->DeleteLocalRef(env, e);
                }
                s_sent++;
            } else {
                (*env)->CallVoidMethod(env, vib, cancel);
                s_cancelled++;
            }
            (*env)->ExceptionClear(env);
            pthread_mutex_lock(&s_mu);
            last_call = now_ms();
            if (level) {
                shot_at = last_call;            /* this one-shot runs from here */
                if (!playing)
                    pulse_at = last_call;       /* the motor started */
            }
            playing = level;
            continue;
        }
        /* Nothing to do: until a change, the renewal, or the title going quiet. */
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += (playing ? 100 : 1000) * 1000000L;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&s_cv, &s_mu, &until);
    }
    return NULL;
}

/* The title's motor speeds for a pad (0..65535 each, after the host's rumble
 * curve); pad_can_rumble: SDL says the pad has motors. Returns 1 when the pad
 * itself should rumble, 0 when it should be told to stop. */
int xbox_AndroidVibration(unsigned left, unsigned right, int pad_can_rumble)
{
    int m = mode(), device, q;

    if (m == VIB_OFF)
        return 0;
    if (m == VIB_CONTROLLER)
        return 1;
    device = m == VIB_DEVICE || !pad_can_rumble;
    if (!device)
        return 1;
    q = (int)((left >> 8) + (right >> 9));          /* big in full, small at half */
    if (q > 255)
        q = 255;
    q = q ? ((q + 15) & ~15) : 0;                   /* steps of 16, never rounded to 0 */
    if (q > 255)
        q = 255;
    pthread_mutex_lock(&s_mu);
    if (!s_started) {
        pthread_t th;
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&s_cv, &ca);
        s_started = 1;
        if (pthread_create(&th, NULL, worker, NULL) == 0)
            pthread_detach(th);
    }
    s_asked_ms = now_ms();
    if (q != s_want) {
        s_want = q;
        pthread_cond_signal(&s_cv);
    }
    pthread_mutex_unlock(&s_mu);
    return 0;
}

#else
typedef int xbox_android_vibration_unused;
#endif
