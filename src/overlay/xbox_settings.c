/*
 * xbox_settings.c - the overlay's settings and settings.toml; see xbox_settings.h.
 */
#include "xbox_settings.h"

#include "recomp_cfg.h"
#include "recomp_save_dir.h"
#include "input_backend.h"
#include "input_map.h"
#include "pad_glyphs.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"
#include "nv2a_texscale.h"
#include "apu_xaudio2.h"
#include "apu.h"                /* mcpx_apu_set_fx_level */
#include "apu_bus.h"

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
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
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
#if defined(__ANDROID__)
static const char *const k_scale[]    = { "1", "1.5", "2", "3", "4", NULL };
#endif
static const char *const k_aniso[]    = { "off", "2x", "4x", "8x", "16x", NULL };
static const char *const k_aniso_env[] = { "1", "2", "4", "8", "16", NULL };
static const char *const k_ui_scale[] = { "off", "2x", "4x", NULL };
static const char *const k_lod[]      = { "off", "auto", "-0.5", "-1", "-1.5", NULL };
static const char *const k_lod_env[]  = { "0", "auto", "-0.5", "-1", "-1.5", NULL };
static const char *const k_aa[]       = { "off", "fxaa", "smaa", NULL };
static const char *const k_units[]    = { "mph", "kmh", NULL };
static const char *const k_face[]     = { "xbox", "nintendo", NULL };
static const char *const k_scheme[]   = { "xbox", "ps2", NULL };
static const char *const k_glyphs[]   = { "auto", "xbox", "xbox_modern", "playstation", "playstation5", "nintendo", "steam", "positional", "keyboard", NULL };
_Static_assert(sizeof k_glyphs / sizeof k_glyphs[0] == XBOX_GLYPHS_MODE_COUNT + 1, "one choice per glyph mode, in pad_glyphs.h order");
#if defined(__ANDROID__)
static const char *const k_vibration[] = { "auto", "controller", "device", "off", NULL };
#endif
static const char *const k_ultrawide[] = { "off", "21:9", "32:9", "window", NULL };
static const char *const k_vehicle_detail[] = { "original", "far", "farthest", NULL };
static const char *const k_power[]    = { "stock", "performance", "battery", NULL };
static const char *const k_lfe_lowpass[] = { "off", "80", "100", "120", NULL };
static const char *const k_timing[]   = { "legacy", "auto", NULL };
static const char *const k_custom_level[] = { "off", "quieter", "match", "louder", NULL };

/* Designated, so the state half of the struct starts zero. */
#define SETTING(k, t, f, e, c, ev, mn, mx, b) \
    { .key = (k), .type = (t), .flags = (f), .env = (e), .choices = (c), \
      .env_vals = (ev), .min = (mn), .max = (mx), .builtin = (b) }
#define SETTING_STEP(k, t, f, e, mn, mx, b, st) \
    { .key = (k), .type = (t), .flags = (f), .env = (e), \
      .min = (mn), .max = (mx), .builtin = (b), .step = (st) }
static xs_setting s_set[XS_COUNT] = {
    [XS_DISPLAY_FULLSCREEN] = SETTING("display.fullscreen", XS_BOOL, 0, RENV_PRESENT_FULLSCREEN, NULL, NULL, 0, 0, 0),
    [XS_DISPLAY_ASPECT]     = SETTING("display.aspect", XS_CHOICE, XS_RESTART, RENV_DISPLAY_WIDESCREEN, k_aspects, k_aspect_env, 0, 0, 0),
    [XS_DISPLAY_FILTER]     = SETTING("display.filter", XS_CHOICE, 0, RENV_PRESENT_FILTER, k_filters, NULL, 0, 0, 0),
    [XS_DISPLAY_OVERSCAN]   = SETTING("display.overscan", XS_CHOICE, 0, RENV_PRESENT_OVERSCAN, k_overscan, NULL, 0, 0, 0),
    [XS_DISPLAY_FIT]        = SETTING("display.fit", XS_CHOICE, 0, RENV_PRESENT_FIT, k_fits, NULL, 0, 0, 0),
#if defined(__ANDROID__)     /* with 1.5 (the Vulkan backend's half step): a choice there */
    [XS_GRAPHICS_RENDER_SCALE] = SETTING("graphics.render_scale", XS_CHOICE, XS_RESTART, RENV_RENDER_SCALE, k_scale, k_scale, 0, 0, 0),
#else
    [XS_GRAPHICS_RENDER_SCALE] = SETTING("graphics.render_scale", XS_INT, XS_RESTART, RENV_RENDER_SCALE, NULL, NULL, 1, 4, 1),
#endif
    [XS_GRAPHICS_ANISO]     = SETTING("graphics.aniso", XS_CHOICE, 0, RENV_GFX_ANISO, k_aniso, k_aniso_env, 0, 0, 0),
    [XS_GRAPHICS_AA]        = SETTING("graphics.aa", XS_CHOICE, 0, RENV_GFX_AA, k_aa, NULL, 0, 0, 0),
    [XS_GRAPHICS_SHARPEN]   = SETTING("graphics.sharpen", XS_FLOAT, 0, RENV_GFX_SHARPEN, NULL, NULL, 0, 1, 0),
    [XS_AUDIO_VOLUME]       = SETTING("audio.volume", XS_FLOAT, 0, RENV_AUDIO_VOLUME, NULL, NULL, 0, 1, 1),
    [XS_AUDIO_MUTE]         = SETTING("audio.mute", XS_BOOL, 0, RENV_AUDIO_MUTE, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_MUTE_BG]      = SETTING("audio.mute_background", XS_BOOL, 0, RENV_AUDIO_MUTE_BG, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_SPEAKERS]     = SETTING("audio.speakers", XS_CHOICE, XS_RESTART, RENV_AUDIO_SPEAKERS, k_speakers, NULL, 0, 0, 0),
    [XS_AUDIO_FX_LEVEL]     = SETTING("audio.fx_level", XS_FLOAT, 0, RENV_AUDIO_FX_LEVEL, NULL, NULL, 0, 1, 0.25),
    [XS_AUDIO_MIXER]        = SETTING("audio.mixer", XS_BOOL, 0, RENV_AUDIO_MIXER, NULL, NULL, 0, 0, 0),
    [XS_INPUT_DEADZONE]     = SETTING("input.deadzone", XS_INT, 0, RENV_PAD_DEADZONE, NULL, NULL, 0, 32767, 0),
    [XS_INPUT_TRIGGER_DEADZONE] = SETTING("input.trigger_deadzone", XS_INT, 0, RENV_PAD_TRIGGER_DEADZONE, NULL, NULL, 0, 254, 0),
    /* 0..3 in quarters: the rumble curve's strength, motor = (speed /
     * peak) ^ (1 / strength) (input_core.c, rumble_curve.h). 1 is the
     * console's own, 0 off; a weak motor (a handheld's own vibrator) wants
     * more than a pad's. */
    [XS_INPUT_RUMBLE]       = SETTING_STEP("input.rumble", XS_FLOAT, 0, RENV_INPUT_RUMBLE_GAIN, 0, 3, 1, 0.25),
    [XS_INPUT_FACE_LAYOUT]  = SETTING("input.face_layout", XS_CHOICE, 0, RENV_INPUT_FACE_LAYOUT, k_face, NULL, 0, 0, 0),
    [XS_INPUT_CONTROL_SCHEME] = SETTING("input.control_scheme", XS_CHOICE, 0, RENV_INPUT_CONTROL_SCHEME, k_scheme, NULL, 0, 0, 0),
    [XS_POWER_MODE]         = SETTING("power.mode", XS_CHOICE, XS_VIRTUAL, RENV_COUNT, k_power, NULL, 0, 0, 0),
    [XS_ADVANCED_PERF]      = SETTING("advanced.perf_overlay", XS_BOOL, 0, RENV_OVERLAY_PERF, NULL, NULL, 0, 0, 0),
    [XS_GAME_SPEED_UNITS]   = SETTING("game.speed_units", XS_CHOICE, 0, RENV_SPEED_UNITS, k_units, NULL, 0, 0, 0),
    [XS_DISPLAY_HUD_FIX]    = SETTING("display.hud_fix", XS_BOOL, 0, RENV_GFX_HUD_FIX, NULL, NULL, 0, 0, 0),
    [XS_DISPLAY_ULTRAWIDE]  = SETTING("display.ultrawide", XS_CHOICE, 0, RENV_DISPLAY_ULTRAWIDE, k_ultrawide, NULL, 0, 0, 0),
    /* The title's own effects, on as shipped: the patch is the "0". */
    [XS_GRAPHICS_BLOOM]     = SETTING("graphics.bloom", XS_BOOL, 0, RENV_GFX_BLOOM, NULL, NULL, 0, 0, 1),
    [XS_GRAPHICS_MOTION_BLUR] = SETTING("graphics.motion_blur", XS_BOOL, 0, RENV_GFX_MOTION_BLUR, NULL, NULL, 0, 0, 1),
    /* The title's crash and takedown cameras at 60 instead of its own 30: the
     * patch is the "1". */
    [XS_GRAPHICS_CRASH_60FPS] = SETTING("graphics.crash_60fps", XS_BOOL, 0, RENV_GFX_CRASH_60FPS, NULL, NULL, 0, 0, 0),
    /* Split screen at 60 instead of the title's 30, and with the title's
     * music, which it keeps silent there: the patch is the "1" of each. */
    [XS_GRAPHICS_SPLIT_60FPS] = SETTING("graphics.split_60fps", XS_BOOL, 0, RENV_GFX_SPLIT_60FPS, NULL, NULL, 0, 0, 0),
    [XS_AUDIO_SPLIT_MUSIC]  = SETTING("audio.split_music", XS_BOOL, 0, RENV_AUDIO_SPLIT_MUSIC, NULL, NULL, 0, 0, 0),
    /* Both off as shipped: today's picture, bit for bit. */
    [XS_GRAPHICS_LOD_BIAS]  = SETTING("graphics.lod_bias", XS_CHOICE, 0, RENV_GFX_LOD_BIAS, k_lod, k_lod_env, 0, 0, 0),
    [XS_GRAPHICS_DITHER]    = SETTING("graphics.dither", XS_BOOL, 0, RENV_GFX_DITHER, NULL, NULL, 0, 0, 0),
    /* The title's vehicle level-of-detail distances: the patch is anything
     * but "original". */
    [XS_GRAPHICS_VEHICLE_DETAIL] = SETTING("graphics.vehicle_detail", XS_CHOICE, 0, RENV_GFX_VEHICLE_DETAIL, k_vehicle_detail, NULL, 0, 0, 0),
    /* The title's button prompts for the pad in use (pad_glyphs.h). */
    [XS_INPUT_GLYPHS]       = SETTING("input.glyphs", XS_CHOICE, 0, RENV_INPUT_GLYPHS, k_glyphs, NULL, 0, 0, 0),
    /* The LFE feed's low-pass where the title's effects image is modelled
     * (apu_gp.h): 120 Hz is what the console's AC-3 encoder did after the
     * GP stage; off is the GP stage's own full-range feed. Live. */
    [XS_AUDIO_LFE_LOWPASS]  = SETTING("audio.lfe_lowpass", XS_CHOICE, 0, RENV_AUDIO_LFE_LOWPASS, k_lfe_lowpass, NULL, 0, 0, 3),
    /* When frames are presented (present_cadence.h, BO-115): legacy is
     * "when ready", as before; auto aims each frame at one refresh. Taken
     * while running (the presenter looks twice a second). */
    [XS_DISPLAY_PRESENT_TIMING] = SETTING("display.present_timing", XS_CHOICE, 0, RENV_PRESENT_TIMING, k_timing, NULL, 0, 0, 0),
    [XS_ADVANCED_PERF_STEPS] = SETTING("advanced.perf_steps", XS_BOOL, 0, RENV_OVERLAY_PERF_STEPS, NULL, NULL, 0, 0, 0),
    /* The print gate (xr_log.h, BO-125). The built-in value is the build's:
     * on in a developer build, off in a release (set at load). Live. */
    [XS_ADVANCED_DIAG_LOG]  = SETTING("advanced.diagnostic_log", XS_BOOL, 0, RENV_LOG_DIAG, NULL, NULL, 0, 0, 1),
    /* Off as shipped: the title's own textures, bit for bit. */
    [XS_GRAPHICS_UI_SCALE]  = SETTING("graphics.ui_scale", XS_CHOICE, 0, RENV_GFX_UI_SCALE, k_ui_scale, NULL, 0, 0, 0),
    /* On as shipped by us: the title's Settings menu lists TAKEDAWN SETTINGS,
     * which opens this overlay (BO-133). Read when the menu is opened. */
    [XS_INTERFACE_MENU_ENTRY] = SETTING("interface.menu_entry", XS_BOOL, 0, RENV_MENU_ENTRY, NULL, NULL, 0, 0, 1),
    /* The custom soundtrack brought to the level of the title's own music
     * (apu_level.h, BO-132); off is the title's sends as they are, bit for
     * bit. Match as shipped. Live. */
    [XS_AUDIO_CUSTOM_LEVEL] = SETTING("audio.custom_music_level", XS_CHOICE, 0, RENV_AUDIO_CUSTOM_LEVEL, k_custom_level, NULL, 0, 0, 2),
#if defined(__ANDROID__)
    [XS_INPUT_VIBRATION]    = SETTING("input.vibration", XS_CHOICE, XS_RESTART, RENV_ANDROID_VIBRATION, k_vibration, NULL, 0, 0, 0),
#endif
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

/* Text settings: a key whose value is a string (a folder), not one of the
 * stepped settings above. Read from the file into its variable like them,
 * written back as it is; a change shows after a restart. */
static struct {
    const char *key;
    recomp_env_id env;
    char val[1024];
    int user, locked;
} s_text[] = {
    { "audio.custom_music_dir", RENV_AUDIO_MUSIC_DIR, "", 0, 0 },
};
#define TEXT_COUNT ((int)(sizeof s_text / sizeof s_text[0]))

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
    /* A stepped value from anywhere (the file, a variable, the overlay) is
     * the nearest step: a file's 1.3 from a finer slider reads as 1.25. */
    if (s->type == XS_FLOAT && s->step > 0.0) {
        v = floor(v / s->step + 0.5) * s->step;
        if (v > s->max) v = s->max;
    }
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

/* Where Maximum ("stock") is not offered (BO-75, a stand-in decision to be
 * confirmed: the Deck and Windows numbers show Balanced as steady and cooler;
 * macOS is unmeasured and keeps all three). Revert: set an entry to 0. */
static const unsigned char k_stock_hidden[] = {
    [XS_HOST_MACOS]   = 0,
    [XS_HOST_WINDOWS] = 1,
    [XS_HOST_ANDROID] = 1,
    [XS_HOST_LINUX]   = 1,
};

static xs_host s_host =
#if defined(__ANDROID__)
    XS_HOST_ANDROID;
#elif defined(_WIN32)
    XS_HOST_WINDOWS;
#elif defined(__linux__)
    XS_HOST_LINUX;
#else
    XS_HOST_MACOS;
#endif

xs_host xbox_SettingsHost(void) { return s_host; }
void xbox_SettingsSetHost(xs_host h) { s_host = h; }

int xbox_PowerStockHidden(xs_host h)
{
    return (unsigned)h < sizeof k_stock_hidden && k_stock_hidden[h];
}

double xbox_PowerModeAtLoad(xs_host h, double stored)
{
    return (int)stored == 0 && xbox_PowerStockHidden(h) ? 1 : stored;
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
    case XS_GRAPHICS_LOD_BIAS: {
        static const float bias[] = { 0.0f, 0.0f, -0.5f, -1.0f, -1.5f };
        int i = (int)v;
        nv2a_host_set_lod_bias(i == 0 ? NV2A_LOD_OFF : i == 1 ? NV2A_LOD_AUTO : NV2A_LOD_VALUE,
                               bias[i]);
        break;
    }
    case XS_GRAPHICS_DITHER:
        nv2a_host_set_dither(v != 0.0);
        break;
    case XS_GRAPHICS_UI_SCALE:
        /* Held off (Android for now): a value stored in the file stays in
         * the file and does nothing; set_factor says so once. */
        nv2a_texscale_set_factor((int)v == 1 ? 2u : (int)v == 2 ? 4u : 0u);
        break;
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
    case XS_AUDIO_MIXER:
        apu_bus_enable(v != 0.0);
        break;
    case XS_ADVANCED_DIAG_LOG: {
        int was = g_xr_log_diag, now = xr_log_set_diag(v != 0.0);
        if (was != now)
            XR_NOTICE("[LOG] diagnostic logging %s\n", now ? "on" : "off");
        break;
    }
    case XS_AUDIO_CUSTOM_LEVEL:
        mcpx_apu_set_custom_level((int)v);
        break;
    case XS_AUDIO_LFE_LOWPASS:
        mcpx_apu_set_lfe_lowpass(atoi(k_lfe_lowpass[(int)v]));
        break;
    case XS_INPUT_DEADZONE:
        xbox_InputSetDeadzone((int)v);
        break;
    case XS_INPUT_TRIGGER_DEADZONE:
        xbox_InputSetTriggerDeadzone((int)v);
        break;
    case XS_INPUT_RUMBLE:
        xbox_InputSetRumbleStrength(v);
        break;
    case XS_INPUT_FACE_LAYOUT:
        xbox_InputSetFaceLayoutNintendo(v != 0.0);
        xbox_InputGlyphsRefresh();     /* the Nintendo prompts follow it */
        break;
    case XS_INPUT_GLYPHS:
        xbox_InputSetGlyphMode((int)v);
        break;
    case XS_INPUT_CONTROL_SCHEME:
        xbox_InputSetControlScheme((int)v);
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

/* Is the section of setting i already written by an earlier user setting? */
static int section_written_before(int i)
{
    char a[32], b[32];
    int j;
    section_of(s_set[i].key, a, sizeof a);
    for (j = 0; j < i; j++) {
        if (!s_set[j].user)
            continue;
        section_of(s_set[j].key, b, sizeof b);
        if (!strcmp(a, b))
            return 1;
    }
    return 0;
}

/* The text settings of section `sec`, under its header (in_section), or,
 * with sec NULL, those whose section no stepped setting opened, each under a
 * header of its own: a key and its [table] are written once (TOML). */
static void write_texts(FILE *f, const char *sec, int in_section)
{
    int t, i;

    for (t = 0; t < TEXT_COUNT; t++) {
        char ts[32], sj[32];
        const char *name;
        int opened = 0;

        if (!s_text[t].user || !s_text[t].val[0])
            continue;
        name = section_of(s_text[t].key, ts, sizeof ts);
        for (i = 0; i < XS_COUNT; i++) {
            if (!s_set[i].user)
                continue;
            section_of(s_set[i].key, sj, sizeof sj);
            if (!strcmp(ts, sj))
                opened = 1;
        }
        if (in_section ? strcmp(ts, sec) != 0 : opened)
            continue;
        if (!in_section)
            fprintf(f, "\n[%s]\n", ts);
        fprintf(f, "%s = ", name);
        quote(f, s_text[t].val);
        fputc('\n', f);
    }
}

void xbox_SettingsFlush(void)
{
    char tmp[sizeof s_path + 8], sec[32];
    FILE *f;
    int i;

    /* Also called from the Android life-cycle callback, on another thread
     * than the overlay's: one writer at a time, the other's change stays
     * dirty for the next call. */
    static volatile LONG writing;
    if (!s_dirty || !s_path[0])
        return;
    if (InterlockedExchange(&writing, 1))
        return;
    s_dirty = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    f = fopen(tmp, "wb");
    if (!f) {
        XR_ERROR( LOGP "cannot write %s\n", tmp);
        InterlockedExchange(&writing, 0);
        return;
    }
    fputs("# Written by the in-game settings overlay (Back+Start, F1).\n"
          "# Only settings you changed are kept; the rest follow the defaults.\n"
          "# A RECOMP_* environment variable overrides what is here.\n", f);
    for (i = 0; i < s_n_keep; i++)
        fprintf(f, "%s = %s\n", s_keep[i].key, s_keep[i].val);
    /* One [table] per section, whatever order the enum keeps its keys in: a
     * second header of the same name is an error for the reader (and for
     * TOML). A section is written at its first key, with all of its keys. */
    for (i = 0; i < XS_COUNT; i++) {
        int j;
        if (!s_set[i].user || section_written_before(i))
            continue;
        section_of(s_set[i].key, sec, sizeof sec);
        fprintf(f, "\n[%s]\n", sec);
        for (j = i; j < XS_COUNT; j++) {
            const xs_setting *s = &s_set[j];
            char sj[32];
            const char *name;
            if (!s->user)
                continue;
            name = section_of(s->key, sj, sizeof sj);
            if (strcmp(sec, sj))
                continue;
            fprintf(f, "%s = ", name);
            /* A setting the environment overrides keeps what the file said. */
            write_value(f, s, s->locked ? s->filev : s->value);
            fputc('\n', f);
        }
        write_texts(f, sec, 1);
    }
    write_texts(f, NULL, 0);
    if (fclose(f) != 0) {
        XR_ERROR( LOGP "cannot write %s\n", tmp);
        InterlockedExchange(&writing, 0);
        return;
    }
#if defined(_WIN32)
    remove(s_path);
#endif
    if (rename(tmp, s_path) != 0)
        XR_ERROR( LOGP "cannot replace %s\n", s_path);
    else
        fprintf(stderr, LOGP "saved\n");
    InterlockedExchange(&writing, 0);
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
        XR_NOTICE( LOGP "%s: no file yet\n", s_path);
        return;
    }
    fclose(probe);
    /* Reopening a [table] is accepted: the writer used to split a section in
     * two when its keys were not next to each other in the enum. */
    c = recomp_cfg_load_file_flags(s_path, RCFG_REOPEN_TABLES, err, sizeof err);
    if (!c) {
        XR_ERROR( LOGP "%s (file ignored, the next change replaces it)\n", err);
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
            int t;
            for (t = 0; t < TEXT_COUNT; t++)
                if (!strcmp(key, s_text[t].key))
                    break;
            if (t < TEXT_COUNT) {
                if (recomp_cfg_type_of(c, key) == RCFG_STRING) {
                    snprintf(s_text[t].val, sizeof s_text[t].val, "%s",
                             recomp_cfg_string(c, key, ""));
                    s_text[t].user = 1;
                } else {
                    fprintf(stderr, LOGP "%s:%d: %s should be a string; ignored\n", s_path,
                            recomp_cfg_line(c, key), key);
                }
                continue;
            }
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
    /* Diagnostic logging defaults to what this build does (xr_log.h). */
    s_set[XS_ADVANCED_DIAG_LOG].builtin = xr_log_default();
    for (i = 0; i < XS_COUNT; i++) {
        xs_setting *s = &s_set[i];
        s->value = s->def = s->builtin;
        if (s->env != RENV_COUNT)
            s->locked = env_real(s->env);
    }
    for (i = 0; i < TEXT_COUNT; i++)
        s_text[i].locked = env_real(s_text[i].env);
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
            power_env(xbox_PowerModeAtLoad(s_host, s->value), &p, &l);
            if (!s_power_locked_pacing) sx_setenv_keep("RECOMP_PRESENT_PACING", p);
            if (!s_power_locked_lead) sx_setenv_keep("RECOMP_PRESENT_SPIN_LEAD", l);
            continue;
        }
        xbox_SettingEnvString(s, s->value, buf, sizeof buf);
        sx_setenv_keep(recomp_env_name(s->env), buf);
    }
    for (i = 0; i < TEXT_COUNT; i++)
        if (s_text[i].user && s_text[i].val[0] && !s_text[i].locked)
            sx_setenv_keep(recomp_env_name(s_text[i].env), s_text[i].val);
    /* recomp_save_dir read the table before these were set. */
    recomp_env_reload();
    XR_NOTICE( LOGP "%s\n", s_path);
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
                s->locked = 1;     /* a spelling the overlay cannot show */
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
            XR_NOTICE( LOGP "%s: %s is set in the environment\n", s_set[i].key,
                    s_set[i].env == RENV_COUNT ? "RECOMP_PRESENT_PACING/SPIN_LEAD"
                                               : recomp_env_name(s_set[i].env));
    }
    /* What nothing else reads at start. */
    apply_audio();
    xbox_InputSetRumbleStrength(s_set[XS_INPUT_RUMBLE].value);
    xbox_InputSetFaceLayoutNintendo(s_set[XS_INPUT_FACE_LAYOUT].value != 0.0);
    xbox_InputSetControlScheme((int)s_set[XS_INPUT_CONTROL_SCHEME].value);
    xbox_InputSetGlyphMode((int)s_set[XS_INPUT_GLYPHS].value);
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

const char *xbox_SettingsText(const char *key, int *locked)
{
    int i;
    for (i = 0; i < TEXT_COUNT; i++)
        if (!strcmp(key, s_text[i].key)) {
            if (locked)
                *locked = s_text[i].locked;
            if (s_text[i].locked) {
#if defined(_WIN32)
                /* The user's own variable (or an argument put there): the
                 * narrow copy is in the ANSI code page; the wide one is
                 * right for any name. */
                static char utf8[1024];
                wchar_t wname[64];
                const wchar_t *w;
                MultiByteToWideChar(CP_ACP, 0, recomp_env_name(s_text[i].env), -1, wname, 64);
                w = _wgetenv(wname);
                if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8, NULL, NULL) > 0)
                    return utf8;
                return "";
#else
                const char *v = getenv(recomp_env_name(s_text[i].env));
                return v ? v : "";
#endif
            }
            return s_text[i].val;
        }
    if (locked)
        *locked = 0;
    return "";
}

void xbox_SettingsSetText(const char *key, const char *value)
{
    int i;
    for (i = 0; i < TEXT_COUNT; i++)
        if (!strcmp(key, s_text[i].key)) {
            if (s_text[i].locked || !strcmp(s_text[i].val, value ? value : ""))
                return;
            snprintf(s_text[i].val, sizeof s_text[i].val, "%s", value ? value : "");
            s_text[i].user = 1;
            s_dirty = 1;
            return;
        }
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
