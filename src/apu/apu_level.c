/* The custom-music leveller (apu_level.h). */
#include "apu_level.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT          (32.0 / 48000.0)    /* one VP frame */
#define TAU_SLOW    3.0
#define TAU_FAST    0.3
#define TAU_DOWN    0.3
#define TAU_UP      4.0
#define FAST_DISCOUNT_DB 2.0f
#define FALLING_DB   3.0f

static const double A_SLOW = 0.99977780;    /* exp(-DT / TAU_SLOW) */
static const double A_FAST = 0.99777901;    /* exp(-DT / TAU_FAST) */
static const float  C_DOWN = 0.00222099f;   /* 1 - exp(-DT / TAU_DOWN) */
static const float  C_UP   = 0.00016664f;   /* 1 - exp(-DT / TAU_UP) */

float apu_level_mode_offset_db(int mode)
{
    return mode == APU_LEVEL_QUIETER ? -4.0f : mode == APU_LEVEL_LOUDER ? 4.0f : 0.0f;
}

static inline float db_lin(float db) { return powf(10.0f, db * 0.05f); }

void apu_level_state_init(apu_level_state *s, float gain_db)
{
    memset(s, 0, sizeof(*s));
    s->primed = gain_db > -100.0f;
    /* A first song has no history: until it has been heard for half a
     * second the gain is -6 dB (a modern master is far more likely than a
     * quiet one), then it jumps to the first reading's. */
    s->gain_db = s->primed ? gain_db : -6.0f;
}

float apu_level_frame(apu_level_state *s, float (*samples)[2], unsigned channels, float target_db)
{
    double e = 0.0;
    float env_db, slow_db, fast_db, g0, g1;

    for (int i = 0; i < 32; i++) {
        const double l = samples[i][0];
        const double r = channels > 1 ? samples[i][1] : l;
        e += l * l + r * r;
    }
    e /= 32.0;
    /* Readings under -60 dBFS are silence: not averaged, so a pause does
     * not drag the envelope down and the song after it is not boosted. */
    if (e >= 1e-6) {
        s->slow_num = A_SLOW * s->slow_num + (1.0 - A_SLOW) * e;
        s->slow_wt  = A_SLOW * s->slow_wt  + (1.0 - A_SLOW);
        s->fast_num = A_FAST * s->fast_num + (1.0 - A_FAST) * e;
        s->fast_wt  = A_FAST * s->fast_wt  + (1.0 - A_FAST);
    }
    slow_db = s->slow_wt > 0.0 ? 10.0f * log10f((float)(s->slow_num / s->slow_wt) + 1e-20f) : -200.0f;
    fast_db = s->fast_wt > 0.0 ? 10.0f * log10f((float)(s->fast_num / s->fast_wt) + 1e-20f) : -200.0f;
    env_db = fast_db - FAST_DISCOUNT_DB > slow_db ? fast_db - FAST_DISCOUNT_DB : slow_db;
    s->env_db = env_db;
    s->frames++;

    g0 = db_lin(s->gain_db);
    if (env_db >= APU_LEVEL_FREEZE_DB) {
        float want = target_db - env_db;
        if (want < APU_LEVEL_MIN_DB) want = APU_LEVEL_MIN_DB;
        if (want > APU_LEVEL_MAX_DB) want = APU_LEVEL_MAX_DB;
        if (!s->primed) {
            if (s->slow_wt >= 0.15) {   /* half a second of sound heard */
                s->gain_db = want;      /* the very first song: no history to slew from */
                s->primed = 1;
            }
        } else if (want > s->gain_db && fast_db < slow_db - FALLING_DB) {
            /* The level is dropping (a fade out, a break): that is not a
             * quiet song, the gain does not climb after it. */
        } else {
            s->gain_db += (want - s->gain_db) * (want < s->gain_db ? C_DOWN : C_UP);
        }
    }
    g1 = db_lin(s->gain_db);
    /* Interpolated across the frame: the gain at the end of sample i is
     * g0 + (g1 - g0) (i + 1) / 32. */
    if (g0 == 1.0f && g1 == 1.0f)
        return 1.0f;
    for (int i = 0; i < 32; i++) {
        const float g = g0 + (g1 - g0) * (float)(i + 1) * (1.0f / 32.0f);
        samples[i][0] *= g;
        samples[i][1] *= g;
    }
    return g1;
}

/* ── the voices ───────────────────────────────────────────────────── */

static volatile int s_mode = -1;                /* -1: the variable is not read yet */
static apu_level_class_fn s_classify;
static int s_log = -1;

void apu_level_set_mode(int mode)
{
    s_mode = mode < 0 || mode >= APU_LEVEL_MODES ? APU_LEVEL_MATCH : mode;
}

int apu_level_mode(void)
{
    if (s_mode < 0) {
        const char *e = recomp_env(RENV_AUDIO_CUSTOM_LEVEL);
        int m = APU_LEVEL_MATCH;
        if (e && *e) {
            if (!strcmp(e, "off") || !strcmp(e, "0")) m = APU_LEVEL_OFF;
            else if (!strcmp(e, "quieter")) m = APU_LEVEL_QUIETER;
            else if (!strcmp(e, "louder")) m = APU_LEVEL_LOUDER;
        }
        s_mode = m;
    }
    return s_mode;
}

void apu_level_set_classifier(apu_level_class_fn fn) { s_classify = fn; }

int apu_level_active(void)
{
    return s_classify && apu_level_mode() != APU_LEVEL_OFF;
}

static struct { unsigned key; int custom; apu_level_state st; unsigned log; } s_v[256];
static float s_last_gain_db = -1000.0f;         /* what the last song settled on */

void apu_level_voice(unsigned voice, unsigned serial,
                     int (*fill)(void *ctx, apu_bus_voice *v), void *ctx,
                     float (*samples)[2], unsigned channels)
{
    int mode = apu_level_mode();
    if (mode == APU_LEVEL_OFF || !s_classify || voice >= 256)
        return;
    if (s_v[voice].key != serial + 1u) {
        apu_bus_voice v;
        memset(&v, 0, sizeof(v));
        s_v[voice].key = serial + 1u;
        s_v[voice].custom = fill && fill(ctx, &v) && v.stream && s_classify(&v);
        if (s_v[voice].custom) {
            apu_level_state_init(&s_v[voice].st, s_last_gain_db);
            s_v[voice].log = 0;
            if (s_log < 0) {
                const char *e = recomp_env(RENV_AUDIO_LEVEL_LOG);
                s_log = e && *e && *e != '0';
            }
        }
    }
    if (!s_v[voice].custom)
        return;
    apu_level_frame(&s_v[voice].st, samples, channels,
                    APU_LEVEL_TARGET_DB + apu_level_mode_offset_db(mode));
    if (s_v[voice].st.primed)
        s_last_gain_db = s_v[voice].st.gain_db;
    if (s_log > 0 && ++s_v[voice].log % 750 == 0)
        fprintf(stderr, "[LEVEL] voice %u: envelope %.1f dB, gain %+.2f dB (mode %d)\n",
                voice, s_v[voice].st.env_db, s_v[voice].st.gain_db, mode);
}
