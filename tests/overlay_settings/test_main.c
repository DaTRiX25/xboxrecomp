/*
 * overlay_settings test: settings.toml read into the environment under the
 * precedence (real environment > file > the host's defaults), the live apply
 * of a change, the file written back with unknown keys kept and unchanged
 * settings left out. The runtime it applies to is stubbed.
 */
#include "xbox_settings.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"
#include "recomp_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── the runtime, stubbed ──────────────────────────────────────────── */

static int st_deadzone = -2, st_trigger = -2, st_pacing = -2;
static double st_rumble = -2;
static uint64_t st_lead = 99;
static int st_overscan = -2, st_fit = -2;
static float st_gain = -2;
static struct nv2a_host_opts st_opts;

void xbox_InputSetDeadzone(int dz) { st_deadzone = dz; }
void xbox_InputSetTriggerDeadzone(int dz) { st_trigger = dz; }
void xbox_InputSetRumbleStrength(double g) { st_rumble = g; }
static int st_face;
void xbox_InputSetFaceLayoutNintendo(int on) { st_face = on; }
void xbox_PacingSetMode(int m) { st_pacing = m; }
void xbox_PacingSetSpinLead(uint64_t ns) { st_lead = ns; }
void nv2a_present_set_overscan(int c) { st_overscan = c; }
void nv2a_present_set_fit(int f) { st_fit = f; }
void nv2a_host_opts_set(const struct nv2a_host_opts *o) { st_opts = *o; }
const struct nv2a_host_opts *nv2a_host_opts(void) { return &st_opts; }
void xa2_set_gain(float g) { st_gain = g; }
static float st_fx = -2;
void mcpx_apu_set_fx_level(float v) { st_fx = v; }
static int st_aniso = -2, st_aa = -2;
static float st_sharpen = -2;
void nv2a_host_set_post_aa(int a) { st_aa = a; }
void nv2a_host_set_sharpen(float v) { st_sharpen = v; }
void nv2a_host_set_aniso(int n) { st_aniso = n; }
#if !defined(_WIN32)
/* The file writer's one-at-a-time flag (src/platform/win32_compat.c). */
int32_t InterlockedExchange(volatile int32_t *p, int32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
#endif

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void)
{
    const char *path = "overlay_settings_test.toml";
    const xs_setting *s;
    char *text;

    write_file(path,
        "# a user's file\n"
        "mystery.key = 7\n"
        "[display]\n"
        "overscan = \"crop\"\n"
        "filter = 3\n"                 /* the wrong type: ignored */
        "[audio]\n"
        "volume = 0.5\n"               /* the environment sets it too */
        "mute = true\n"
        "[input]\n"
        "deadzone = 3277\n"
        "rumble = 1.8\n"               /* an older slider's value, between two steps */
        "[power]\n"
        "mode = \"battery\"\n"
        "[display]\n"                 /* an older writer's split section */
        "hud_fix = false\n"
        "[graphics]\n"
        "bloom = false\n");            /* a title's effect, on by default: switched off */
    set_env("RECOMP_SETTINGS", path);
    set_env("RECOMP_AUDIO_VOLUME", "0.25");          /* the user's own */
    set_env("RECOMP_AUDIO_MUTE", NULL);
    set_env("RECOMP_PRESENT_OVERSCAN", NULL);
    set_env("RECOMP_PRESENT_PACING", NULL);
    set_env("RECOMP_PRESENT_SPIN_LEAD", NULL);
    set_env("RECOMP_INPUT_RUMBLE_GAIN", NULL);
    set_env("RECOMP_PAD_DEADZONE", NULL);
    set_env("RECOMP_GFX_BLOOM", NULL);
    set_env("RECOMP_GFX_MOTION_BLUR", NULL);

    xbox_SettingsInit("Test");
    CHECK(!strcmp(xbox_SettingsPath(), path));
    /* The file reached the environment, the real environment did not move. */
    CHECK(getenv("RECOMP_PRESENT_OVERSCAN") && !strcmp(getenv("RECOMP_PRESENT_OVERSCAN"), "crop"));
    CHECK(getenv("RECOMP_AUDIO_MUTE") && !strcmp(getenv("RECOMP_AUDIO_MUTE"), "1"));
    CHECK(getenv("RECOMP_AUDIO_VOLUME") && !strcmp(getenv("RECOMP_AUDIO_VOLUME"), "0.25"));
    CHECK(getenv("RECOMP_PAD_DEADZONE") && !strcmp(getenv("RECOMP_PAD_DEADZONE"), "3277"));
    CHECK(getenv("RECOMP_PRESENT_PACING") && !strcmp(getenv("RECOMP_PRESENT_PACING"), "sleep"));
    CHECK(getenv("RECOMP_PRESENT_SPIN_LEAD") && !strcmp(getenv("RECOMP_PRESENT_SPIN_LEAD"), "0"));
    CHECK(!getenv("RECOMP_PRESENT_FILTER"));
    /* The second [display] table was read, not refused (the whole file would
     * have been ignored): its key is the user's own. */
    CHECK(xbox_SettingGet(XS_DISPLAY_HUD_FIX)->user && xbox_SettingValue(XS_DISPLAY_HUD_FIX) == 0);

    /* The host's defaults: setenv without replacing, then told. */
    setenv("RECOMP_PRESENT_OVERSCAN", "crop", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_OVERSCAN", "crop");
    setenv("RECOMP_INPUT_RUMBLE_GAIN", "1.25", 0); xbox_SettingsGameDefault("RECOMP_INPUT_RUMBLE_GAIN", "1.25");
    setenv("RECOMP_PRESENT_PACING", "sleep", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_PACING", "sleep");
    setenv("RECOMP_PRESENT_SPIN_LEAD", "8", 0); xbox_SettingsGameDefault("RECOMP_PRESENT_SPIN_LEAD", "8");
    xbox_SettingsFinish();

    s = xbox_SettingGet(XS_AUDIO_VOLUME);
    CHECK(s->locked && s->value == 0.25 && s->user);
    xbox_SettingSet(XS_AUDIO_VOLUME, 0.9);           /* refused: locked */
    CHECK(xbox_SettingValue(XS_AUDIO_VOLUME) == 0.25);
    CHECK(xbox_SettingValue(XS_AUDIO_MUTE) == 1 && xbox_SettingGet(XS_AUDIO_MUTE)->user);
    CHECK(st_gain == 0.0f);                          /* muted at start */
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1);
    CHECK(xbox_SettingGet(XS_DISPLAY_OVERSCAN)->def == 1);
    CHECK(xbox_SettingValue(XS_DISPLAY_FILTER) == 0 && !xbox_SettingGet(XS_DISPLAY_FILTER)->user);
    CHECK(xbox_SettingValue(XS_INPUT_DEADZONE) == 3277);
    /* The file's rumble strength outranks the host's default (1.25 here) and
     * keeps its number, at the nearest quarter. */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.75 && xbox_SettingGet(XS_INPUT_RUMBLE)->user);
    CHECK(xbox_SettingGet(XS_INPUT_RUMBLE)->def == 1.25);
    CHECK(st_rumble == 1.75);
    CHECK(getenv("RECOMP_INPUT_RUMBLE_GAIN") && !strcmp(getenv("RECOMP_INPUT_RUMBLE_GAIN"), "1.75"));
    /* The power mode: the file said battery (sleep, no spin lead); the
     * host's default is performance (sleep, 8 ms). */
    CHECK(xbox_SettingValue(XS_POWER_MODE) == 2 && xbox_SettingGet(XS_POWER_MODE)->def == 1);
    CHECK(!xbox_SettingsNeedRestart());
    /* The title's own effects (guest patches on the "0"): on unless the
     * file or the overlay says otherwise, and a stored "off" is kept. */
    CHECK(xbox_SettingValue(XS_GRAPHICS_BLOOM) == 0 && xbox_SettingGet(XS_GRAPHICS_BLOOM)->user);
    CHECK(xbox_SettingGet(XS_GRAPHICS_BLOOM)->def == 1);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "0"));
    CHECK(xbox_SettingValue(XS_GRAPHICS_MOTION_BLUR) == 1 && !xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->user);
    {
        /* Untouched, the variable never reads "0": no patch is active. */
        const char *e = recomp_env(RENV_GFX_MOTION_BLUR);
        CHECK(!e || strcmp(e, "0") != 0);
    }
    xbox_SettingSet(XS_GRAPHICS_MOTION_BLUR, 0);
    CHECK(recomp_env(RENV_GFX_MOTION_BLUR) && !strcmp(recomp_env(RENV_GFX_MOTION_BLUR), "0"));
    CHECK(!xbox_SettingsNeedRestart());              /* live */
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 1);
    CHECK(recomp_env(RENV_GFX_BLOOM) && !strcmp(recomp_env(RENV_GFX_BLOOM), "1"));
    xbox_SettingSet(XS_GRAPHICS_BLOOM, 0);

    /* A live change reaches the runtime, the cached environment and the file. */
    xbox_SettingSet(XS_POWER_MODE, 1);
    CHECK(st_pacing == 1 && st_lead == 8000000ull);
    CHECK(recomp_env(RENV_PRESENT_SPIN_LEAD) && !strcmp(recomp_env(RENV_PRESENT_SPIN_LEAD), "8"));
    xbox_SettingSet(XS_INPUT_DEADZONE, 6554);
    CHECK(st_deadzone == 6554);
    xbox_SettingSet(XS_INPUT_TRIGGER_DEADZONE, 20);
    CHECK(st_trigger == 20);
    xbox_SettingSet(XS_DISPLAY_FIT, 1);
    CHECK(st_fit == 1);
    xbox_SettingSet(XS_DISPLAY_OVERSCAN, 0);
    CHECK(st_overscan == 0);
    xbox_SettingSet(XS_DISPLAY_FILTER, 1);
    CHECK(st_opts.present_filter == 1);
    xbox_SettingSet(XS_AUDIO_MUTE, 0);
    CHECK(st_gain == 0.25f);                         /* the environment's volume */
    xbox_SettingSet(XS_AUDIO_MUTE_BG, 1);
    xbox_SettingsSetBackground(1);
    CHECK(st_gain == 0.0f);
    xbox_SettingsSetBackground(0);
    CHECK(st_gain == 0.25f);
    /* Anisotropic filtering: the choice's level, the environment's spelling. */
    xbox_SettingSet(XS_GRAPHICS_ANISO, 3);
    CHECK(st_aniso == 8);
    CHECK(recomp_env(RENV_GFX_ANISO) && !strcmp(recomp_env(RENV_GFX_ANISO), "8"));
    xbox_SettingSet(XS_GRAPHICS_AA, 1);
    xbox_SettingSet(XS_GRAPHICS_SHARPEN, 0.4);
    CHECK(st_aa == 1 && st_sharpen > 0.39f && st_sharpen < 0.41f);
    CHECK(recomp_env(RENV_GFX_AA) && !strcmp(recomp_env(RENV_GFX_AA), "fxaa"));
    /* A restart-flagged setting. */
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 2);
    CHECK(xbox_SettingsNeedRestart());
    xbox_SettingSet(XS_GRAPHICS_RENDER_SCALE, 1);
    CHECK(!xbox_SettingsNeedRestart());
    /* A value is clamped to its range. The rumble strength: 0 (off) to 3,
     * in quarters, and the runtime is given the stepped value. */
    xbox_SettingSet(XS_INPUT_RUMBLE, 9.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 3.0);     /* the range ends at 3 */
    xbox_SettingSet(XS_INPUT_RUMBLE, -1.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 0.0 && st_rumble == 0.0);
    CHECK(xbox_SettingGet(XS_INPUT_RUMBLE)->min == 0.0 && xbox_SettingGet(XS_INPUT_RUMBLE)->max == 3.0
          && xbox_SettingGet(XS_INPUT_RUMBLE)->step == 0.25);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.75);               /* on the step: as it is */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.75 && st_rumble == 1.75);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.3);                /* the old slider's 0.05 steps */
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.25 && st_rumble == 1.25);
    xbox_SettingSet(XS_INPUT_RUMBLE, 1.4);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 1.5);
    xbox_SettingSet(XS_INPUT_RUMBLE, 2.95);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 3.0);
    for (double q = 0.0; q <= 3.0; q += 0.25) {           /* every step is a value */
        xbox_SettingSet(XS_INPUT_RUMBLE, q);
        CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == q);
    }
    {
        char e[32];
        xbox_SettingEnvString(xbox_SettingGet(XS_INPUT_RUMBLE), 1.75, e, sizeof e);
        CHECK(!strcmp(e, "1.75"));
    }
    xbox_SettingSet(XS_INPUT_RUMBLE, 2.0);
    CHECK(xbox_SettingValue(XS_INPUT_RUMBLE) == 2.0);

    xbox_SettingSet(XS_DISPLAY_HUD_FIX, 1);          /* late in the enum, section display */
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text != NULL);
    if (text) {
        /* Each section once, its keys together (hud_fix after the other
         * display keys, before the next section), and the strict reader
         * takes the file back. */
        const char *d = strstr(text, "[display]");
        const char *a = strstr(text, "[audio]");
        const char *h = strstr(text, "hud_fix = true");
        CHECK(d && !strstr(d + 1, "[display]"));
        CHECK(d && h && h > d);
        CHECK(h && a && (h < a || a < d));
        {
            char err[256];
            recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
            CHECK(c != NULL);
            if (c) {
                CHECK(recomp_cfg_bool(c, "display.hud_fix", 0) == 1);
                CHECK(recomp_cfg_choice(c, "display.fit", (const char *const[]){ "letterbox", "fill16x10", NULL }, -1) == 1);
                recomp_cfg_free(c);
            }
        }
        CHECK(strstr(text, "mystery.key = 7"));                  /* kept */
        CHECK(strstr(text, "mode = \"performance\""));
        CHECK(strstr(text, "deadzone = 6554"));
        CHECK(strstr(text, "overscan = \"off\""));
        CHECK(strstr(text, "fit = \"fill16x10\""));
        CHECK(strstr(text, "filter = \"linear\""));
        CHECK(strstr(text, "mute_background = true"));
        CHECK(strstr(text, "aniso = \"8x\""));
        CHECK(strstr(text, "volume = 0.5"));                     /* locked: the file's own */
        CHECK(!strstr(text, "fullscreen"));                      /* never touched */
        {
            /* The effects' "off" is stored, in the one [graphics] table. */
            const char *g = strstr(text, "[graphics]");
            const char *b = strstr(text, "bloom = false");
            const char *m = strstr(text, "motion_blur = false");
            CHECK(g && !strstr(g + 1, "[graphics]"));
            CHECK(g && b && m && b > g && m > g);
        }
    }

    /* Back on: the default, and the file forgets it. */
    xbox_SettingReset(XS_GRAPHICS_MOTION_BLUR);
    CHECK(xbox_SettingValue(XS_GRAPHICS_MOTION_BLUR) == 1 && !xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->user);
    CHECK(recomp_env(RENV_GFX_MOTION_BLUR) && !strcmp(recomp_env(RENV_GFX_MOTION_BLUR), "1"));
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "motion_blur") && strstr(text, "bloom = false"));

    /* Reset goes back to the host's default and forgets the setting. */
    xbox_SettingReset(XS_DISPLAY_OVERSCAN);
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1 && !xbox_SettingGet(XS_DISPLAY_OVERSCAN)->user);
    CHECK(st_overscan == 1);
    xbox_SettingsFlush();
    text = read_file(path);
    CHECK(text && !strstr(text, "overscan"));

    /* The shape an older writer left (a split [display]): the strict reader
     * refuses it, the settings reader (RCFG_REOPEN_TABLES) takes it; a key
     * repeated inside it is still an error. */
    {
        char err[256];
        const char *split = "[display]\nfullscreen = true\n[game]\nspeed_units = \"kmh\"\n[display]\nhud_fix = false\n";
        recomp_cfg *c;
        write_file(path, split);
        CHECK(recomp_cfg_load_file(path, err, sizeof err) == NULL && strstr(err, "defined twice"));
        c = recomp_cfg_load_file_flags(path, RCFG_REOPEN_TABLES, err, sizeof err);
        CHECK(c != NULL);
        if (c) {
            CHECK(recomp_cfg_bool(c, "display.fullscreen", 0) == 1 && recomp_cfg_type_of(c, "display.hud_fix") == RCFG_BOOL);
            recomp_cfg_free(c);
        }
        write_file(path, "[display]\nfullscreen = true\n[display]\nfullscreen = false\n");
        CHECK(recomp_cfg_load_file_flags(path, RCFG_REOPEN_TABLES, err, sizeof err) == NULL);
    }

    /* The face-button layout reaches the input layer as it is set. */
    xbox_SettingSet(XS_INPUT_FACE_LAYOUT, 1);
    CHECK(st_face == 1);
    xbox_SettingReset(XS_INPUT_FACE_LAYOUT);
    CHECK(st_face == 0);

    remove(path);
    fprintf(stderr, "overlay_settings: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
