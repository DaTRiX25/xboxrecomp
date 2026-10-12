/*
 * xbox_settings.h - the settings the in-game overlay edits, and settings.toml.
 *
 * Every setting is one of the runtime's RECOMP_* knobs (recomp_env.h) with a
 * key in the settings file, a type, a range and, where the runtime can take
 * it while running, a live apply. The precedence, high to low:
 *
 *   1. the environment: a RECOMP_* variable the user set; the overlay shows
 *      the setting locked
 *   2. settings.toml in the save directory (recomp_save_dir.h), or the file
 *      RECOMP_SETTINGS names; "none" reads and writes no file
 *   3. the title's defaults (the host's env_default calls, passed to
 *      xbox_SettingsGameDefault), then the toolkit's
 *
 * The file is applied by setting the variables, so every reader of the
 * environment (the backends, the window, the enhancements layer, which then
 * ranks the file over enhance.toml) sees it without knowing it exists.
 * Order in a host's main:
 *
 *   xbox_SettingsInit("Burnout3");       // first: before the defaults
 *   env_default(...) + xbox_SettingsGameDefault(name, value) for each
 *   xbox_SettingsFinish();               // after the last default
 *
 * Only settings the user changed are written; the rest follow the defaults.
 * TOML, not INI: the enhancements layer already reads it (recomp_cfg.h), the
 * file has typed values and sections, and enhance.toml is the same shape.
 * The overlay rewrites the whole file; comments in it are not kept.
 *
 * Values are doubles here: a bool is 0/1, a choice its index.
 */
#ifndef XBOX_OVERLAY_SETTINGS_H
#define XBOX_OVERLAY_SETTINGS_H

#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { XS_BOOL, XS_INT, XS_FLOAT, XS_CHOICE } xs_type;

enum {
    XS_RESTART = 1,       /* read at start: a change shows after a restart */
    XS_VIRTUAL = 2        /* several knobs (the Power page's mode) */
};

typedef enum {
    XS_DISPLAY_FULLSCREEN,
    XS_DISPLAY_ASPECT,
    XS_DISPLAY_FILTER,
    XS_DISPLAY_OVERSCAN,
    XS_DISPLAY_FIT,
    XS_GRAPHICS_RENDER_SCALE,
    XS_GRAPHICS_ANISO,
    XS_GRAPHICS_AA,
    XS_GRAPHICS_SHARPEN,
    XS_AUDIO_VOLUME,
    XS_AUDIO_MUTE,
    XS_AUDIO_MUTE_BG,
    XS_AUDIO_SPEAKERS,
    XS_AUDIO_FX_LEVEL,
    XS_INPUT_DEADZONE,
    XS_INPUT_TRIGGER_DEADZONE,
    XS_INPUT_RUMBLE,
    XS_INPUT_FACE_LAYOUT,    /* xbox / nintendo (A-B and X-Y mirrored) */
    XS_INPUT_CONTROL_SCHEME, /* xbox / ps2: the driving controls, where the title offers them */
    XS_POWER_MODE,
    XS_ADVANCED_PERF,
    XS_GAME_SPEED_UNITS,     /* an opt-in guest patch (recomp_patch.h) */
    XS_DISPLAY_HUD_FIX,      /* another: the HUD's shape in 16:9 */
    XS_DISPLAY_ULTRAWIDE,    /* another: the 3D wider than 16:9 */
    XS_GRAPHICS_BLOOM,       /* another: the title's own bloom, on as shipped */
    XS_GRAPHICS_MOTION_BLUR, /* another: the title's own motion blur, on as shipped */
    XS_GRAPHICS_LOD_BIAS,    /* extra mip LOD bias: off, auto (from the render scale), or a value */
    XS_GRAPHICS_DITHER,      /* noise dither in the presenter's scale to the window (Metal) */
    XS_GRAPHICS_VEHICLE_DETAIL, /* an opt-in guest patch: the title's vehicle LOD distances */
    XS_INPUT_GLYPHS,         /* auto / xbox / xbox_modern / playstation / nintendo / steam (pad_glyphs.h) */
#if defined(__ANDROID__)
    XS_INPUT_VIBRATION,      /* pad motors or the device's vibrator (android_vibration.c) */
#endif
    XS_COUNT
} xs_id;

typedef struct xs_setting {
    const char *key;                  /* "display.overscan" */
    xs_type type;
    unsigned flags;
    recomp_env_id env;                /* RENV_COUNT: none (XS_VIRTUAL) */
    const char *const *choices;       /* XS_CHOICE: NULL-terminated */
    const char *const *env_vals;      /* the knob's spelling per choice, or NULL */
    double min, max;                  /* INT and FLOAT */
    double builtin;                   /* the toolkit's default */
    double step;                      /* FLOAT: values are multiples of it; 0: any */
    /* state */
    double value;                     /* now */
    double filev;                     /* what the file said (kept while locked) */
    double def;                       /* with the title's defaults */
    double launch;                    /* when Finish ran: restart compares */
    int user;                         /* set by the file or the overlay */
    int locked;                       /* the environment sets it */
} xs_setting;

/* The save directory's settings.toml (or RECOMP_SETTINGS), read, and the
 * variables it sets applied. `title` names the save directory
 * (recomp_save_dir). Before any recomp_env lookup that matters: it ends by
 * re-reading the cached environment. */
void xbox_SettingsInit(const char *title);
/* A default the host gives a variable (its env_default); the setting's
 * "reset" value. Call after xbox_SettingsInit. */
void xbox_SettingsGameDefault(const char *env_name, const char *value);
/* After the host's last default: the settings take the values the
 * environment now holds, and are applied. */
void xbox_SettingsFinish(void);

const xs_setting *xbox_SettingGet(xs_id id);
double xbox_SettingValue(xs_id id);
/* Set, apply live where it can be, remember it for the file. No effect on a
 * locked setting. */
void xbox_SettingSet(xs_id id, double v);
/* Back to the default; the file forgets it. */
void xbox_SettingReset(xs_id id);
void xbox_SettingsResetAll(void);
/* A restart-flagged setting differs from the value at launch. */
int  xbox_SettingsNeedRestart(void);
/* Write settings.toml if anything changed since the last write. */
void xbox_SettingsFlush(void);
/* The file in use, or "" (none). */
const char *xbox_SettingsPath(void);

/* The power mode's "stock" (Maximum, the busy-wait) is offered on some hosts
 * only (BO-75: on the Deck and Windows it measured no steadier than Balanced
 * and costs 2.7 W / stutters). Where it is hidden the overlay steps over it
 * and a stored "stock" is read as "performance" at load (the file itself is
 * left as it is until the next save). The spin pacing stays reachable with
 * RECOMP_PRESENT_PACING=spin. The choice is one table in xbox_settings.c. */
typedef enum { XS_HOST_MACOS, XS_HOST_WINDOWS, XS_HOST_ANDROID, XS_HOST_LINUX } xs_host;
xs_host xbox_SettingsHost(void);               /* the host this was built for */
void    xbox_SettingsSetHost(xs_host h);       /* tests */
int     xbox_PowerStockHidden(xs_host h);
/* The power mode (0 stock, 1 performance, 2 battery) a stored value means on h. */
double  xbox_PowerModeAtLoad(xs_host h, double stored);

/* The window is in the background: audio.mute_background silences it. */
void xbox_SettingsSetBackground(int bg);

/* The bug report's parts that come from here (xbox_report.h): settings.toml
 * attached, the settings with their sources, the shader cache's counts.
 * Once, at start (xbox_settings_report.c). */
void xbox_SettingsReportRegister(void);

/* The environment spelling of a value, for the overlay's hints. */
void xbox_SettingEnvString(const xs_setting *s, double v, char *out, unsigned n);

/* The line at the top of the overlay's Advanced page: the port's name and
 * version (NULL or "": none). Here and not in overlay.h, which needs SDL. */
void xbox_OverlaySetAbout(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_SETTINGS_H */
