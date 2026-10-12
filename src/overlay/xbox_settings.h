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
    XS_AUDIO_MIXER,          /* the host mixer (apu_bus.h); its mix is in mixer.toml */
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
    XS_GRAPHICS_CRASH_60FPS, /* another: the title's 30 Hz phases at 60, off as shipped */
    XS_GRAPHICS_SPLIT_60FPS, /* another: the title's 30 Hz split screen at 60, off as shipped */
    XS_AUDIO_SPLIT_MUSIC,    /* another: the title's music in split screen, off as shipped */
    XS_GRAPHICS_LOD_BIAS,    /* extra mip LOD bias: off, auto (from the render scale), or a value */
    XS_GRAPHICS_DITHER,      /* noise dither in the presenter's scale to the window (Metal) */
    XS_GRAPHICS_VEHICLE_DETAIL, /* an opt-in guest patch: the title's vehicle LOD distances */
    XS_INPUT_GLYPHS,         /* auto / xbox / xbox_modern / playstation / playstation5 / nintendo / steam / positional (pad_glyphs.h) */
    XS_AUDIO_LFE_LOWPASS,    /* the LFE feed's low-pass: off / 80 / 100 / 120 Hz (apu_gp.h) */
    XS_DISPLAY_PRESENT_TIMING, /* legacy / auto: when frames are presented (present_cadence.h) */
    XS_ADVANCED_PERF_STEPS,  /* the graph marks frames of more than one simulation step */
    XS_ADVANCED_DIAG_LOG,    /* diagnostic logging: the print gate (platform/xr_log.h) */
    XS_GRAPHICS_UI_SCALE,    /* the title's 2D art scaled before upload: off, 2x, 4x (nv2a_texscale.h) */
    XS_INTERFACE_MENU_ENTRY, /* the overlay's entry in the title's own Settings menu, on by default */
    XS_AUDIO_CUSTOM_LEVEL,   /* the user's own music against the title's: off / quieter / match / louder (apu_level.h) */
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

/* A text setting (audio.custom_music_dir: a folder): what the file holds, or
 * the environment's when it sets the variable (*locked, may be NULL). ""
 * when unset or unknown. Set: remembered for the file (an empty value
 * removes the key); the runtime reads it at the next start. */
/* Always UTF-8: the file is; on Windows the environment's value is taken
 * from the wide environment and converted (the narrow one is in the ANSI
 * code page), so a folder with any letters in its name opens. A reader of
 * such a setting uses this, not getenv. */
const char *xbox_SettingsText(const char *key, int *locked);
void        xbox_SettingsSetText(const char *key, const char *value);

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

/* The "Check for updates" row of the Advanced page (xbox_update.h): the
 * GitHub repository the port's releases are published in ("owner/name") and
 * the running version ("0.1.0"). Without this call, or on a build that
 * cannot make the request (Android), there is no row. The request is made
 * only when the row is pressed. */
void xbox_OverlaySetUpdateCheck(const char *repo, const char *version);

/* Ask for the overlay to open, from any thread (a host hook on the title's
 * thread: a menu entry of the game's own). It opens on the main loop's next
 * turn, as the hotkey opens it. 0 when there is no overlay to open (not
 * built, or switched off). */
int xbox_OverlayRequestOpen(void);

/* Cheats, for the session only: rows on the Advanced page, each switched with
 * A (on asks once more, showing `warning`). While one is on the title's saves
 * go to a copy of the save (recomp_save_sandbox_begin); when the last goes
 * off the player keeps that progress or throws it away. get and set run on
 * the main thread while the overlay is open (the title is paused). Up to 4;
 * call once, after the save root is known (the kernel's paths are set up). */
typedef struct xbox_cheat {
    const char *name;
    const char *warning;
    int  (*get)(void);
    void (*set)(int on);
} xbox_cheat;
void xbox_OverlaySetCheats(const xbox_cheat *list, int n);
/* Whether there is an overlay to open (built and not switched off). */
int xbox_OverlayAvailable(void);

/* The "Custom music folder" row of the overlay's Audio page, for a port
 * whose title plays the user's music (xbox_soundtrack.h); none until this is
 * called. The row opens a folder browser inside the overlay (a pad is
 * enough); the folder picked there is stored as audio.custom_music_dir and
 * handed to `chosen` ("" for "turn off"), on the overlay's thread. `status`
 * fills the line under the row: what is in use, a scan's progress, when a
 * change shows ("" for no line). */
typedef struct xbox_music_folder_ops {
    void (*status)(char *out, unsigned n);
    void (*chosen)(const char *dir);
    /* May be NULL. Further lines under the status, i = 0..3 (files a scan
     * left out, by name and reason): 1 and the text, or 0 for no more. */
    int  (*note)(int i, char *out, unsigned n);
} xbox_music_folder_ops;
void xbox_OverlaySetMusicFolder(const xbox_music_folder_ops *ops);

/* The "save data" rows of the overlay's Advanced page (recomp_save_dir.h's
 * export and import), for a port that names its title's save: "Export save
 * to home folder" copies UDATA/<title id> and TDATA/<title id> of the save
 * root to a dated folder in `export_dir` and shows the full path; "Import
 * save from home folder" takes the newest export there, "Import save from
 * another folder..." one picked in the folder browser. An import is checked,
 * staged, and done at the next start by the port's call of
 * recomp_save_import_apply (the save in use is set aside first); meanwhile
 * the rows offer "Restart now" and "Cancel the import". `note` is a line
 * for the export's text file (may be NULL). The strings are copied. */
typedef struct xbox_save_io {
    const char *save_dir;
    const char *title_id;
    const char *export_dir;
    const char *note;
} xbox_save_io;
void xbox_OverlaySetSaveIO(const xbox_save_io *io);

/* Restart (recomp_restart.h). The overlay offers "Restart game now" on a
 * page while one is wanted: a setting flagged XS_RESTART in the table
 * differs from its value at launch (xbox_SettingsNeedRestart: the table is
 * the only list), or the port says so here with its reason, a sentence for
 * the row's note (NULL or "": no longer). The Advanced page always has
 * "Restart game". Nothing restarts unless the player picks the row. */
void xbox_OverlayWantRestart(const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_SETTINGS_H */
