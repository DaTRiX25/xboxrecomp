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
    XS_AUDIO_VOLUME,
    XS_AUDIO_MUTE,
    XS_AUDIO_MUTE_BG,
    XS_AUDIO_SPEAKERS,
    XS_INPUT_DEADZONE,
    XS_INPUT_TRIGGER_DEADZONE,
    XS_INPUT_RUMBLE,
    XS_POWER_MODE,
    XS_ADVANCED_PERF,
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

/* The window is in the background: audio.mute_background silences it. */
void xbox_SettingsSetBackground(int bg);

/* The environment spelling of a value, for the overlay's hints. */
void xbox_SettingEnvString(const xs_setting *s, double v, char *out, unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_SETTINGS_H */
