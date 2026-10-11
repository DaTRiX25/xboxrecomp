/*
 * xbox_settings.c - the overlay's settings and settings.toml; see xbox_settings.h.
 */
#include "xbox_settings.h"

#include "recomp_cfg.h"
#include "recomp_save_dir.h"
#include "input_backend.h"
#include "input_map.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"
#include "apu_xaudio2.h"
#include "apu.h"                /* mcpx_apu_set_fx_level */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define sx_strcasecmp _stricmp
/* Set unless already set: what the title's env_default does. */
static void sx_setenv_keep(const char *n, const char *v)
{
    if (!getenv(n))
        _putenv_s(n, v);
}
#else
#include <strings.h>
#define sx_strcasecmp strcasecmp
#define sx_setenv_keep(n, v) setenv((n), (v), 0)
#endif

#define LOGP "[SETTINGS] "

static const char *const k_aspects[]  = { "4:3", "16:9", NULL };
static const char *const k_aspect_env[] = { "0", "1", NULL };
static const char *const k_filters[]  = { "nearest", "linear", "integer", NULL };
static const char *const k_overscan[] = { "off", "crop", NULL };
static const char *const k_fits[]     = { "letterbox", "fill16x10", NULL };
static const char *const k_speakers[] = { "auto", "stereo", "5.1", NULL };
static const char *const k_aniso[]    = { "off", "2x", "4x", "8x", "16x", NULL };
static const char *const k_aniso_env[] = { "1", "2", "4", "8", "16", NULL };
static const char *const k_aa[]       = { "off", "fxaa", "smaa", NULL };
static const char *const k_units[]    = { "mph", "kmh", NULL };
static const char *const k_power[]    = { "stock", "performance", "battery", NULL };

/* Designated, so the state half of the struct starts zero. */
#define SETTING(k, t, f, e, c, ev, mn, mx, b) \
    { .key = (k), .type = (t), .flags = (f), .env = (e), .choices = (c), \
      .env_vals = (ev), .min = (mn), .max = (mx), .builtin = (b) }
static xs_setting s_set[XS_COUNT] = {
    [XS_DISPLAY_FULLSCREEN] = SETTING("display.fullscreen", XS_BOOL, 0, RENV_PRESENT_FULLSCREEN, NULL, NULL, 0, 0, 0),
    [XS_DISPLAY_ASPECT]     = SETTING("display.aspect", XS_CHOICE, XS_RESTART, RENV_DISPLAY_WIDESCREEN, k_aspects, k_aspect_env, 0, 0, 0),
    [XS_DISPLAY_FILTER]     = SETTING("display.filter", XS_CHOICE, 0, RENV_PRESENT_FILTER, k_filters, NULL, 0, 0, 0),
    [XS_DISPLAY_OVERSCAN]   = SETTING("display.overscan", XS_CHOICE, 0, RENV_PRESENT_OVERSCAN, k_overscan, NULL, 0, 0, 0),
    [XS_DISPLAY_FIT]        = SETTING("display.fit", XS_CHOICE, 0, RENV_PRESENT_FIT, k_fits, NULL, 0, 0, 0),
    [XS_GRAPHICS_RENDER_SCALE] = SETTING("graphics.render_scale", XS_INT, XS_RESTART, RENV_RENDER_SCALE, NULL, NULL, 1, 4, 1),
    [XS_GRAPHICS_ANISO]     = SETTING("graphics.aniso", XS_CHOICE, 0, RENV_GFX_ANISO, k_aniso, k_aniso_env, 0, 0, 0),
    [XS_GRAPHICS_AA]        = SETTING("graphics.aa", XS_CHOICE, 0, RENV_GFX_AA, k_aa, NULL, 0, 0, 0),
    [XS_GRAPHICS_SHARPEN]   = SETTING("graphics.sharpen", XS_FLOAT, 0, RENV_GFX_SHARPEN, NULL, NULL, 0, 1, 0),
    [XS_AUDIO_VOLUME]       = SETTING("audio.volume", XS_FLOAT, 0, RENV_AUDIO_VOLUME, NULL, NULL, 0, 1, 1),
    [XS_AUDIO_MUTE]         = SETTING("audio.mute", XS_BOOL, 0, RENV_AUDIO_MUTE, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_MUTE_BG]      = SETTING("audio.mute_background", XS_BOOL, 0, RENV_AUDIO_MUTE_BG, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_SPEAKERS]     = SETTING("audio.speakers", XS_CHOICE, XS_RESTART, RENV_AUDIO_SPEAKERS, k_speakers, NULL, 0, 0, 0),
    [XS_AUDIO_FX_LEVEL]     = SETTING("audio.fx_level", XS_FLOAT, 0, RENV_AUDIO_FX_LEVEL, NULL, NULL, 0, 1, 0.25),
    [XS_INPUT_DEADZONE]     = SETTING("input.deadzone", XS_INT, 0, RENV_PAD_DEADZONE, NULL, NULL, 0, 32767, 0),
    [XS_INPUT_TRIGGER_DEADZONE] = SETTING("input.trigger_deadzone", XS_INT, 0, RENV_PAD_TRIGGER_DEADZONE, NULL, NULL, 0, 254, 0),
    [XS_INPUT_RUMBLE]       = SETTING("input.rumble", XS_FLOAT, 0, RENV_INPUT_RUMBLE_GAIN, NULL, NULL, 0, 2, 1),
    [XS_POWER_MODE]         = SETTING("power.mode", XS_CHOICE, XS_VIRTUAL, RENV_COUNT, k_power, NULL, 0, 0, 0),
    [XS_ADVANCED_PERF]      = SETTING("advanced.perf_overlay", XS_BOOL, 0, RENV_OVERLAY_PERF, NULL, NULL, 0, 0, 0),
    [XS_GAME_SPEED_UNITS]   = SETTING("game.speed_units", XS_CHOICE, 0, RENV_SPEED_UNITS, k_units, NULL, 0, 0, 0),
    [XS_DISPLAY_HUD_FIX]    = SETTING("display.hud_fix", XS_BOOL, 0, RENV_GFX_HUD_FIX, NULL, NULL, 0, 0, 0),
};

/* The power mode is two knobs. */
#define POWER_LEAD_MS 8.0
static int s_power_locked_pacing, s_power_locked_lead;
static char s_game_pacing[16], s_game_lead[16];

static char s_path[1024];
static int s_dirty, s_ready, s_finished;
static int s_background;

/* Keys in the file no setting knows: kept as they were on the next write. */
#define KEEP_MAX 64
static struct { char key[64]; char val[192]; } s_keep[KEEP_MAX];
static int s_n_keep;

/* ── value <-> text ────────────────────────────────────────────────── */

static int parse_bool(const char *v, int *out)
{
    if (!sx_strcasecmp(v, "1") || !sx_strcasecmp(v, "true") || !sx_strcasecmp(v, "yes")
            || !sx_strcasecmp(v, "on")) {
        *out = 1;
        return 1;
    }
    if (!sx_strcasecmp(v, "0") || !sx_strcasecmp(v, "false") || !sx_strcasecmp(v, "no")
            || !sx_strcasecmp(v, "off")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static double clampd(const xs_setting *s, double v)
{
    if (s->type == XS_BOOL)
        return v != 0.0;
    if (s->type == XS_CHOICE) {
        int n = 0;
        while (s->choices[n])
            n++;
        return v < 0 ? 0 : v >= n ? n - 1 : floor(v + 0.5);
    }
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    return s->type == XS_INT ? floor(v + 0.5) : v;
}

/* The environment's spelling of v. */
void xbox_SettingEnvString(const xs_setting *s, double v, char *out, unsigned n)
{
    switch (s->type) {
    case XS_BOOL:   snprintf(out, n, "%d", v != 0.0); break;
    case XS_INT:    snprintf(out, n, "%d", (int)v); break;
    case XS_FLOAT:  snprintf(out, n, "%g", v); break;
    default: {
        int i = (int)clampd(s, v);
        snprintf(out, n, "%s", s->env_vals ? s->env_vals[i] : s->choices[i]);
        break;
    }
    }
}

/* A knob's text as a value; 0 when it does not parse. */
static int parse_env(const xs_setting *s, const char *v, double *out)
{
    char *end;
    double d;
    int b, i;

    if (!v || !*v)
        return 0;
    switch (s->type) {
    case XS_BOOL:
        if (!parse_bool(v, &b))
            return 0;
        *out = b;
        return 1;
    case XS_INT:
    case XS_FLOAT:
        d = strtod(v, &end);
        if (end == v || *end)
            return 0;
        *out = clampd(s, d);
        return 1;
    default:
        for (i = 0; s->choices[i]; i++)
            if (!strcmp(v, s->env_vals ? s->env_vals[i] : s->choices[i])
                    || !strcmp(v, s->choices[i])) {
                *out = i;
                return 1;
            }
        return 0;
    }
}

/* ── the power mode: pacing and spin lead ──────────────────────────── */

static void power_env(double mode, const char **pacing, const char **lead)
{
    int m = (int)mode;
    *pacing = m == 0 ? "spin" : "sleep";
    *lead = m == 1 ? "8" : "0";
}

/* The mode the two knobs make. */
static double power_from(const char *pacing, const char *lead)
{
    double l = lead && *lead ? atof(lead) : 0.0;
    if (!pacing || strcmp(pacing, "sleep"))
        return 0;
    return l > 0.0 ? 1 : 2;
}

/* ── applying ──────────────────────────────────────────────────────── */

static void apply_audio(void)
{
    float g = (float)xbox_SettingValue(XS_AUDIO_VOLUME);
    if (xbox_SettingValue(XS_AUDIO_MUTE) != 0.0
            || (xbox_SettingValue(XS_AUDIO_MUTE_BG) != 0.0 && s_background))
        g = 0.0f;
    xa2_set_gain(g);
}

/* Into the runtime, where it can take the value while running. */
static void apply(xs_id id)
{
    const xs_setting *s = &s_set[id];
    double v = s->value;
    char buf[32];

    /* The cached environment follows, for anything that reads it later. */
    if (s->env != RENV_COUNT) {
        xbox_SettingEnvString(s, v, buf, sizeof buf);
        recomp_env_set(s->env, buf);
    }
    switch (id) {
    case XS_DISPLAY_OVERSCAN:
        nv2a_present_set_overscan(v == 1);
        break;
    case XS_DISPLAY_FIT:
        nv2a_present_set_fit(v == 1);
        break;
    case XS_DISPLAY_FILTER: {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.present_filter = (int)v;
        nv2a_host_opts_set(&o);
        break;
    }
    case XS_DISPLAY_FULLSCREEN: {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.fullscreen = v != 0.0;
        nv2a_host_opts_set(&o);      /* the window follows (overlay_ui.cpp) */
        break;
    }
    case XS_GRAPHICS_ANISO: {
        static const int levels[] = { 1, 2, 4, 8, 16 };
        nv2a_host_set_aniso(levels[(int)v]);
        break;
    }
    case XS_GRAPHICS_AA:
        nv2a_host_set_post_aa((int)v);
        break;
    case XS_GRAPHICS_SHARPEN:
        nv2a_host_set_sharpen((float)v);
        break;
    case XS_AUDIO_VOLUME:
    case XS_AUDIO_MUTE:
    case XS_AUDIO_MUTE_BG:
        apply_audio();
        break;
    case XS_AUDIO_FX_LEVEL:
        mcpx_apu_set_fx_level((float)v);
        break;
    case XS_INPUT_DEADZONE:
        xbox_InputSetDeadzone((int)v);
        break;
    case XS_INPUT_TRIGGER_DEADZONE:
        xbox_InputSetTriggerDeadzone((int)v);
        break;
    case XS_INPUT_RUMBLE:
        xbox_InputSetRumbleGain(v);
        break;
    case XS_POWER_MODE: {
        const char *p, *l;
        power_env(v, &p, &l);
        recomp_env_set(RENV_PRESENT_PACING, p);
        recomp_env_set(RENV_PRESENT_SPIN_LEAD, l);
        xbox_PacingSetMode(v == 0 ? XBOX_PACING_SPIN : XBOX_PACING_SLEEP);
        xbox_PacingSetSpinLead(v == 1 ? (uint64_t)(POWER_LEAD_MS * 1e6) : 0);
        break;
    }
    default:
        break;
    }
}

/* ── the file ──────────────────────────────────────────────────────── */

static const char *section_of(const char *key, char *out, size_t n)
{
    const char *dot = strchr(key, '.');
    size_t l = dot ? (size_t)(dot - key) : strlen(key);
    if (l >= n)
        l = n - 1;
    memcpy(out, key, l);
    out[l] = 0;
    return dot ? dot + 1 : key;
}

static void quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void write_value(FILE *f, const xs_setting *s, double v)
{
    switch (s->type) {
    case XS_BOOL:   fputs(v != 0.0 ? "true" : "false", f); break;
    case XS_INT:    fprintf(f, "%d", (int)v); break;
    case XS_FLOAT:  fprintf(f, "%g", v); break;
    default:        quote(f, s->choices[(int)clampd(s, v)]); break;
    }
}

void xbox_SettingsFlush(void)
{
    char tmp[sizeof s_path + 8], sec[32], last[32] = "";
    FILE *f;
    int i;

    if (!s_dirty || !s_path[0])
        return;
    s_dirty = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, LOGP "cannot write %s\n", tmp);
        return;
    }
    fputs("# Written by the in-game settings overlay (Back+Start, F1).\n"
          "# Only settings you changed are kept; the rest follow the defaults.\n"
          "# A RECOMP_* environment variable overrides what is here.\n", f);
    for (i = 0; i < s_n_keep; i++)
        fprintf(f, "%s = %s\n", s_keep[i].key, s_keep[i].val);
    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = &s_set[i];
        const char *name;
        if (!s->user)
            continue;
        name = section_of(s->key, sec, sizeof sec);
        if (strcmp(sec, last)) {
            fprintf(f, "\n[%s]\n", sec);
            snprintf(last, sizeof last, "%s", sec);
        }
        fprintf(f, "%s = ", name);
        /* A setting the environment overrides keeps what the file said. */
        write_value(f, s, s->locked ? s->filev : s->value);
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, LOGP "cannot write %s\n", tmp);
        return;
    }
#if defined(_WIN32)
    remove(s_path);
#endif
    if (rename(tmp, s_path) != 0)
        fprintf(stderr, LOGP "cannot replace %s\n", s_path);
}

static void keep_unknown(const recomp_cfg *c, const char *key)
{
    char v[192];
    if (s_n_keep == KEEP_MAX || strlen(key) >= sizeof s_keep[0].key)
        return;
    switch (recomp_cfg_type_of(c, key)) {
    case RCFG_STRING: {
        const char *t = recomp_cfg_string(c, key, "");
        size_t n = 0, i;
        v[n++] = '"';
        for (i = 0; t[i] && n + 3 < sizeof v; i++) {
            if (t[i] == '"' || t[i] == '\\')
                v[n++] = '\\';
            v[n++] = t[i];
        }
        v[n++] = '"';
        v[n] = 0;
        break;
    }
    case RCFG_INT:   snprintf(v, sizeof v, "%lld", recomp_cfg_int(c, key, 0)); break;
    case RCFG_FLOAT: snprintf(v, sizeof v, "%.17g", recomp_cfg_float(c, key, 0)); break;
    case RCFG_BOOL:  snprintf(v, sizeof v, "%s", recomp_cfg_bool(c, key, 0) ? "true" : "false"); break;
    default: return;                  /* arrays are not kept */
    }
    snprintf(s_keep[s_n_keep].key, sizeof s_keep[0].key, "%s", key);
    snprintf(s_keep[s_n_keep].val, sizeof s_keep[0].val, "%s", v);
    s_n_keep++;
}

/* Read the file into the settings' values (user = 1). */
static void load_file(void)
{
    char err[512];
    recomp_cfg *c;
    FILE *probe = fopen(s_path, "rb");
    size_t k;
    int i;

    if (!probe) {
        fprintf(stderr, LOGP "%s: no file yet\n", s_path);
        return;
    }
    fclose(probe);
    c = recomp_cfg_load_file(s_path, err, sizeof err);
    if (!c) {
        fprintf(stderr, LOGP "%s (file ignored, the next change replaces it)\n", err);
        return;
    }
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        recomp_cfg_type t = recomp_cfg_type_of(c, s->key);
        const char *want = NULL;
        double v = 0;
        if (t == RCFG_NONE)
            continue;
        switch (s->type) {
        case XS_BOOL:
            if (t == RCFG_BOOL) v = recomp_cfg_bool(c, s->key, 0); else want = "true or false";
            break;
        case XS_INT:
            if (t == RCFG_INT) v = (double)recomp_cfg_int(c, s->key, 0); else want = "an integer";
            break;
        case XS_FLOAT:
            if (t == RCFG_FLOAT || t == RCFG_INT) v = recomp_cfg_float(c, s->key, 0); else want = "a number";
            break;
        default: {
            int ch = t == RCFG_STRING ? recomp_cfg_choice(c, s->key, s->choices, -1) : -1;
            if (ch >= 0) v = ch; else want = "one of its choices";
            break;
        }
        }
        if (want) {
            fprintf(stderr, LOGP "%s:%d: %s should be %s; ignored\n", s_path,
                    recomp_cfg_line(c, s->key), s->key, want);
            continue;
        }
        s->value = s->filev = clampd(s, v);
        s->user = 1;
    }
    for (k = 0; k < recomp_cfg_count(c); k++) {
        const char *key = recomp_cfg_key_at(c, k);
        for (i = 0; i < XS_COUNT; i++)
            if (!strcmp(key, s_set[i].key))
                break;
        if (i == XS_COUNT) {
            fprintf(stderr, LOGP "%s:%d: unknown key %s (kept)\n", s_path,
                    recomp_cfg_line(c, key), key);
            keep_unknown(c, key);
        }
    }
    recomp_cfg_free(c);
}

/* ── start-up ──────────────────────────────────────────────────────── */

static int env_real(recomp_env_id id)
{
    const char *v = getenv(recomp_env_name(id));
    return v && *v;
}

void xbox_SettingsInit(const char *title)
{
    const char *over;
    int i;

    if (s_ready)
        return;
    s_ready = 1;
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        s->value = s->def = s->builtin;
        if (s->env != RENV_COUNT)
            s->locked = env_real(s->env);
    }
    s_power_locked_pacing = env_real(RENV_PRESENT_PACING);
    s_power_locked_lead = env_real(RENV_PRESENT_SPIN_LEAD);
    s_set[XS_POWER_MODE].locked = s_power_locked_pacing || s_power_locked_lead;

    over = getenv("RECOMP_SETTINGS");
    if (over && !strcmp(over, "none")) {
        fprintf(stderr, LOGP "no settings file (RECOMP_SETTINGS=none)\n");
        return;
    }
    if (over && *over) {
        snprintf(s_path, sizeof s_path, "%s", over);
    } else {
        const char *dir = recomp_save_dir(title);
        if (!dir || !*dir)
            return;
        snprintf(s_path, sizeof s_path, "%s/settings.toml", dir);
    }
    load_file();

    /* The file's values into the environment, under what the user set and
     * over what the host defaults (which come after and do not replace). */
    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = &s_set[i];
        char buf[32];
        if (!s->user || s->locked)
            continue;
        if (i == XS_POWER_MODE) {
            const char *p, *l;
            power_env(s->value, &p, &l);
            if (!s_power_locked_pacing) sx_setenv_keep("RECOMP_PRESENT_PACING", p);
            if (!s_power_locked_lead) sx_setenv_keep("RECOMP_PRESENT_SPIN_LEAD", l);
            continue;
        }
        xbox_SettingEnvString(s, s->value, buf, sizeof buf);
        sx_setenv_keep(recomp_env_name(s->env), buf);
    }
    /* recomp_save_dir read the table before these were set. */
    recomp_env_reload();
    fprintf(stderr, LOGP "%s\n", s_path);
}

void xbox_SettingsGameDefault(const char *name, const char *value)
{
    int i;
    if (!name || !value)
        return;
    if (!strcmp(name, "RECOMP_PRESENT_PACING"))
        snprintf(s_game_pacing, sizeof s_game_pacing, "%s", value);
    else if (!strcmp(name, "RECOMP_PRESENT_SPIN_LEAD"))
        snprintf(s_game_lead, sizeof s_game_lead, "%s", value);
    for (i = 0; i < XS_COUNT; i++) {
        double v;
        if (s_set[i].env != RENV_COUNT && !strcmp(name, recomp_env_name(s_set[i].env))
                && parse_env(&s_set[i], value, &v))
            s_set[i].def = v;
    }
}

void xbox_SettingsFinish(void)
{
    int i;

    if (s_finished)
        return;
    s_finished = 1;
    recomp_env_reload();
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        double v;
        if (s->env != RENV_COUNT) {
            const char *e = recomp_env(s->env);
            if (e && parse_env(s, e, &v))
                s->value = v;
            else if (e && *e)
                s->locked = 1;     /* a spelling the overlay cannot show ("linear") */
        }
    }
    {
        /* The power mode from the two knobs as they now stand, and the
         * title's own choice as the default. */
        s_set[XS_POWER_MODE].value = power_from(recomp_env(RENV_PRESENT_PACING),
                                                recomp_env(RENV_PRESENT_SPIN_LEAD));
        s_set[XS_POWER_MODE].def = power_from(
            s_game_pacing[0] ? s_game_pacing : NULL, s_game_lead[0] ? s_game_lead : NULL);
    }
    for (i = 0; i < XS_COUNT; i++) {
        s_set[i].launch = s_set[i].value;
        if (s_set[i].locked)
            fprintf(stderr, LOGP "%s: %s is set in the environment\n", s_set[i].key,
                    s_set[i].env == RENV_COUNT ? "RECOMP_PRESENT_PACING/SPIN_LEAD"
                                               : recomp_env_name(s_set[i].env));
    }
    /* What nothing else reads at start. */
    apply_audio();
    xbox_InputSetRumbleGain(s_set[XS_INPUT_RUMBLE].value);
}

/* ── access ────────────────────────────────────────────────────────── */

const xs_setting *xbox_SettingGet(xs_id id)
{
    return (unsigned)id < XS_COUNT ? &s_set[id] : NULL;
}

double xbox_SettingValue(xs_id id)
{
    return (unsigned)id < XS_COUNT ? s_set[id].value : 0.0;
}

void xbox_SettingSet(xs_id id, double v)
{
    xs_setting *s;
    if ((unsigned)id >= XS_COUNT)
        return;
    s = &s_set[id];
    if (s->locked)
        return;
    v = clampd(s, v);
    if (v == s->value && s->user)
        return;
    s->value = v;
    s->user = 1;
    s_dirty = 1;
    apply(id);
}

void xbox_SettingReset(xs_id id)
{
    xs_setting *s;
    if ((unsigned)id >= XS_COUNT)
        return;
    s = &s_set[id];
    if (s->locked)
        return;
    s->value = s->def;
    if (s->user)
        s_dirty = 1;
    s->user = 0;
    apply(id);
}

void xbox_SettingsResetAll(void)
{
    int i;
    for (i = 0; i < XS_COUNT; i++)
        xbox_SettingReset((xs_id)i);
}

int xbox_SettingsNeedRestart(void)
{
    int i;
    for (i = 0; i < XS_COUNT; i++)
        if ((s_set[i].flags & XS_RESTART) && s_set[i].value != s_set[i].launch)
            return 1;
    return 0;
}

const char *xbox_SettingsPath(void)
{
    return s_path;
}

void xbox_SettingsSetBackground(int bg)
{
    if (s_background == (bg != 0))
        return;
    s_background = bg != 0;
    apply_audio();
}
