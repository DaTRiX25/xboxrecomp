/*
 * overlay_ui.cpp - the settings overlay: hotkeys, pause, input routing, the
 * pages and the frame-time graph. See overlay.h.
 *
 * Opening it does three things, on the main thread, in this order: the title
 * stops reading pads and keys (xbox_InputSetBlocked, input_core.c), the title
 * is paused (xbox_HostPause, kernel_pause.h: the GPU thread holds at the next
 * flip, the APU's frame thread idles), and the presenter starts presenting on
 * every turn. Closing undoes them, writes the settings, and keeps the pad
 * neutral until its buttons are let go (the press that closed it is not the
 * title's).
 *
 * The navigation is the overlay's own, not ImGui's: ImGui is the renderer
 * and the mouse. A pad, key or stick event becomes one step on a row (up,
 * down, left, right, activate, back, page, default), with key repeat for a
 * held direction, so what a button does does not depend on ImGui's focus
 * rules. Rows are drawn as ImGui items so the mouse works the same way.
 */
#include "overlay.h"
#include "overlay_internal.h"
#include "xbox_settings.h"
#include "nv2a_texscale.h"
#include "xbox_shader_build.h"
#include "xbox_report.h"
#include "nv2a_pcache.h"
#include "recomp_patch.h"
#include "recomp_env.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"

#include "input_backend.h"
#include "input_map.h"
#include "kernel_pause.h"
#include "present_cadence.h"    /* what reached the glass, for the graph */
extern "C" uint64_t xbox_HostNowNs(void);   /* platform/host_time.h */
#include "apu.h"                /* the speaker test */
#include "apu_bus.h"            /* the Mixer page */
#include "apu_gp.h"             /* is the title's effects image modelled */
#include "xbox_soundtrack.h"    /* the music folder browser's model (xsb_*) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "overlay_font_ext.h"   /* k_overlay_font_ext: the second font's letters */
/* DejaVu Sans Mono beside ProggyForever. A font's size is its ascent plus
 * descent, and of that both have the same x-height (0.47), so the scale is
 * the first font's own (ImGui gives its default 1.015). The baselines differ
 * by 0.02 of the size (ascents 0.797 and 0.777): at the 13-pixel reference
 * size the first font is moved down 0.41, this one 0.15. */
#define OVERLAY_FONT_EXT_SCALE 1.015f
#define OVERLAY_FONT_EXT_DY    0.15f

#define LOGP "[OVERLAY] "

/* ── state ─────────────────────────────────────────────────────────── */

static SDL_Window *s_win;
static bool s_enabled;          /* RECOMP_OVERLAY is not 0 */
static bool s_open;
static float s_alpha;           /* 0..1, the fade */
static bool s_core_ready;
static bool s_vulkan;
static uint32_t s_last_ms;
static bool s_last_input_pad;   /* the hints name pad buttons or keys */

/* Hotkey: Back and Start of one controller. */
static unsigned s_back_mask, s_start_mask;      /* by controller index */
static bool s_combo_fired;

/* Navigation: steps counted by the event handler, taken by the frame. */
enum { D_UP, D_DOWN, D_LEFT, D_RIGHT, D_COUNT };
static bool s_dpad[D_COUNT], s_stick[D_COUNT], s_key[D_COUNT];
static uint32_t s_next_fire[D_COUNT];
static int s_steps[D_COUNT];
static int s_activate, s_back, s_page_delta, s_reset;

/* The page and the row on each. */
enum { P_DISPLAY, P_GRAPHICS, P_AUDIO, P_INPUT, P_POWER, P_ADVANCED, P_COUNT };
static const char *const k_page_name[P_COUNT] = {
    "Display", "Graphics", "Audio", "Input", "Power", "Advanced"
};
static int s_page;
static int s_sel[P_COUNT];
static bool s_scroll_to_sel;

/* Settings written a moment after the last change. */
static bool s_save_pending;
static uint32_t s_save_at;

/* Fullscreen follows the setting when the setting changes. */
static int s_fs_applied = -1;

/* RECOMP_OVERLAY=toggle=<s>[,<s>...]: the overlay opens and closes at those
 * seconds after the window came up, for scripted runs (the pause, the
 * screenshots) with no pad to press. */
#define TOGGLE_MAX 8
static uint32_t s_toggle_ms[TOGGLE_MAX];
static int s_toggle_n, s_toggle_i;
static uint32_t s_t0_ms;
/* RECOMP_OVERLAY=file=<path>: the overlay toggles whenever that file appears
 * (and removes it), for driving it from a script that knows when. */
/* RECOMP_OVERLAY=aa_cycle=<s>: the anti-aliasing setting steps off, FXAA,
 * SMAA, off ... every <s> seconds, through the settings (the live switch
 * under test, with no pad to press). */
static uint32_t s_cycle_ms, s_cycle_next;
static int s_cycle_i;
static char s_trigger[512];
static uint32_t s_trigger_next;

/* The window's present rate for the graph. */
static volatile uint32_t s_presents;

/* ── rows ──────────────────────────────────────────────────────────── */

enum { FMT_PLAIN, FMT_PERCENT, FMT_PERCENT_OF_MAX, FMT_TIMES, FMT_GAIN };

struct Meta {
    const char *label;
    const char *hint;
    double step;                      /* a left/right step */
    int fmt;
    const char *const *names;         /* the choices as shown */
    unsigned disabled;                /* choice bits that cannot be picked */
};

static const char *const n_aspect[]    = { "4:3", "16:9", NULL };
static const char *const n_filter[]    = { "Nearest", "Linear", "Integer", NULL };
static const char *const n_overscan[]  = { "Show all", "Crop (TV safe area)", NULL };
static const char *const n_timing[]    = { "Legacy (when ready)", "Auto (even frames)", NULL };
static const char *const n_fit[]       = { "Letterbox", "Fill 16:10 (crops the sides)", NULL };
static const char *const n_aniso[]     = { "Off", "2x", "4x", "8x", "16x", NULL };
static const char *const n_lod[]       = { "Off", "Auto (from the render scale)", "-0.5", "-1.0", "-1.5", NULL };
static const char *const n_aa[]        = { "Off", "FXAA", "SMAA", NULL };
static const char *const n_speakers[]  = { "Auto", "Stereo", "5.1", NULL };
static const char *const n_lfe_lowpass[] = { "Off", "80 Hz", "100 Hz", "120 Hz", NULL };
#if defined(__ANDROID__)
static const char *const n_vibration[] = { "Auto", "Controller", "This device", "Off", NULL };
#endif
#if defined(__ANDROID__)
static const char *const n_scale[]     = { "1x (640x480)", "1.5x (960x720)", "2x (1280x960)", "3x", "4x", NULL };
#endif
static const char *const n_face[]      = { "Xbox layout", "Nintendo layout", NULL };
static const char *const n_scheme[]    = { "Xbox layout", "PS2 layout", NULL };
static const char *const n_ui_scale[]  = { "Off", "2x", "4x", NULL };
static const char *const n_ui_scale_filter[] = { "Super-xBR", "Bicubic", "Lanczos", "MMPX", NULL };
static const char *const n_glyphs[]    = { "Auto (the controller in use)", "Xbox (original)", "Xbox (modern: LB RB LT RT)", "PlayStation", "Nintendo", "Steam (Deck, Steam Controller)", NULL };
static const char *const n_units[]     = { "mph (as shipped)", "km/h", NULL };
static const char *const n_ultrawide[] = { "Off", "21:9", "32:9", "Window", NULL };
static const char *const n_vehicle_detail[] = { "Original", "Far (2x)", "Farthest (4x)", NULL };
static const char *const n_power[]     = { "Maximum (~14 W on Deck)", "Balanced (~10 W, recommended)", "Battery saver (~8 W)", NULL };
/* Off the Steam Deck the watts are somebody else's: the names alone. */
static const char *const n_power_plain[] = { "Maximum", "Balanced (recommended)", "Battery saver", NULL };
static bool on_steam_deck()
{
    const char *sd = getenv("SteamDeck");
    return sd && sd[0] == '1';
}

static const Meta k_meta[XS_COUNT] = {
    /* XS_DISPLAY_FULLSCREEN */ { "Fullscreen", "Borderless fullscreen on the desktop.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_ASPECT */     { "Aspect ratio", "16:9 makes the game draw widescreen (it reads this at start).", 1, FMT_PLAIN, n_aspect, 0 },
    /* XS_DISPLAY_FILTER */     { "Filter", "How the picture is scaled to the window.", 1, FMT_PLAIN, n_filter, 0 },
    /* XS_DISPLAY_OVERSCAN */   { "Overscan", "Crop the border a TV would have hidden.", 1, FMT_PLAIN, n_overscan, 0 },
    /* XS_DISPLAY_FIT */        { "Present fit", "Fill the window's height on a 16:10 screen.", 1, FMT_PLAIN, n_fit, 0 },
#if defined(__ANDROID__)
    /* XS_GRAPHICS_RENDER_SCALE */ { "Render scale", "Internal resolution, a multiple of the Xbox's 640x480. 1.5x is 960x720.", 1, FMT_PLAIN, n_scale, 0 },
#else
    /* XS_GRAPHICS_RENDER_SCALE */ { "Render scale", "Internal resolution, a multiple of the Xbox's.", 1, FMT_TIMES, NULL, 0 },
#endif
    /* XS_GRAPHICS_ANISO */    { "Anisotropic filtering", "Sharper textures at a slant, the road ahead. Costs GPU time.", 1, FMT_PLAIN, n_aniso, 0 },
    /* XS_GRAPHICS_AA */       { "Anti-aliasing", "Smooths jagged edges after the frame is drawn. FXAA is one cheap pass; SMAA is three, and cleaner.", 1, FMT_PLAIN, n_aa, 0 },
    /* XS_GRAPHICS_SHARPEN */  { "Sharpening", "Contrast-adaptive sharpening (AMD CAS), against the softness of the anti-aliasing and the scale-up.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_VOLUME */       { "Master volume", "The game's sound level.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_MUTE */         { "Mute", "No sound.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_MUTE_BG */      { "Mute in the background", "No sound while the window is not in front.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_SPEAKERS */     { "Speakers", "Auto plays 5.1 when the system's sound output is set to 5.1 (or more), else stereo. The game picks its mix at start.", 1, FMT_PLAIN, n_speakers, 0 },
    /* XS_AUDIO_FX_LEVEL */     { "Surround effects (reverb) level", "5.1 only: the level of the game's effect sends, which play dry here (no effect DSP runs). Tune by ear.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_MIXER */        { "Mixer", "Buses over the game's sounds: gains, sends, LFE, limiter (the Mixer page below). Off: the game's mix as shipped.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_INPUT_DEADZONE */     { "Stick deadzone", "The part of the stick's travel that reads as centre.", 1638.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_TRIGGER_DEADZONE */ { "Trigger deadzone", "The part of a trigger's pull that reads as released.", 13.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_RUMBLE */       { "Rumble strength", "1.0 is the original: the game's rumble as the Xbox pad played it. Higher makes the weaker rumbles stronger (the strongest is full at any setting). 0 turns the rumble off.", 0.25, FMT_GAIN, NULL, 0 },
    /* XS_INPUT_FACE_LAYOUT */  { "Face buttons", "Nintendo layout: for a pad printed with A on the right and B below. A and B, X and Y change places, here and in the game.", 1, FMT_PLAIN, n_face, 0 },
    /* XS_INPUT_CONTROL_SCHEME */ { "Driving controls", "PS2 layout: the PlayStation 2 version's driving controls. Accelerate on the bottom face button, brake on the left one, boost on the right shoulder, look back on the left shoulder. Only while driving: menus are not changed. Changes at once.", 1, FMT_PLAIN, n_scheme, 0 },
    /* XS_POWER_MODE */         { "Power mode", "Maximum busy-waits all the time (steadiest, hottest). Balanced sleeps and spins only the last 8 ms before each frame (steady 60). Battery saver only sleeps (coolest, an occasional small dip).", 1, FMT_PLAIN, n_power, 0 },
    /* XS_ADVANCED_PERF */      { "Performance overlay", "FPS and frame-time graph in the corner. Greetings to the DF team!", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GAME_SPEED_UNITS */   { "Speed units", "Speedometer and distances in km/h and metres instead of mph and miles: the game's own metric display, which the US disc keeps for its other languages. A change shows from the next race. Text stays English.", 1, FMT_PLAIN, n_units, 0 },
    /* XS_DISPLAY_HUD_FIX */    { "Widescreen HUD", "In 16:9 the game stretches its HUD with the picture. On: the HUD keeps its 4:3 shape and stays at the screen edges, by the game's own layout values. A change shows from the next race. Nothing changes in 4:3.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_ULTRAWIDE */  { "Ultrawide", "For a display wider than 16:9, with Aspect at 16:9: the race is drawn for the whole width (more to see at the sides, nothing stretched). Menus, loading screens and videos keep their 16:9 shape in the middle. Window follows the window's or the screen's own shape. A change shows from the next race.", 1, FMT_PLAIN, n_ultrawide, 0 },
    /* XS_GRAPHICS_BLOOM */     { "Bloom", "The game's own soft glow over the picture, strongest while boosting. On is the original look; off skips its three extra render passes. Changes at once.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_MOTION_BLUR */ { "Motion blur", "The game's own blur: the streaks towards the edges of the picture at speed and while boosting. On is the original look; off skips its two extra render passes. Changes at once.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_CRASH_60FPS */ { "60 fps crash cameras", "The game halves its frame rate to 30 in its crash and takedown cameras. On: they run at 60 like the rest of the race. For strong machines: where 60 fps cannot be held, the game itself runs slower while this is on, not just the picture. Leave it off on a Steam Deck and other handhelds. Off is the original. Changes at once.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_SPLIT_60FPS */ { "60 fps split screen", "The game runs two-player split screen at 30 frames a second. On: it runs at 60, with the scene drawn twice a frame. For strong machines: where 60 fps cannot be held, the game itself runs slower while this is on, not just the picture. Leave it off on a Steam Deck and other handhelds. Off is the original. A change shows from the next two-player event.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_SPLIT_MUSIC */  { "Music in split screen", "The game plays no music in two-player split screen. On: EA GAMES TRAX or your custom music plays in two-player Race and Road Rage as in a one-player race. Off is the original. A change shows from the next two-player event.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_LOD_BIAS */  { "Texture sharpness (LOD bias)", "With a render scale above 1x, picks sharper texture levels so the extra pixels show detail. Auto: -0.5 at 2x, -1.0 at 4x. Off is the original. Changes at once.", 1, FMT_PLAIN, n_lod, 0 },
    /* XS_GRAPHICS_DITHER */    { "Dither", "A faint noise in the final scale to the window that hides the stripes in smooth gradients such as the sky. Metal only for now. Off is the original; text and HUD at the exact size are unchanged.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_VEHICLE_DETAIL */ { "Vehicle detail distance", "How far away rival cars and traffic keep their detailed models before the game swaps in simpler ones. Original is the console's distances; Far doubles them, Farthest is four times. Costs GPU time when many vehicles are on screen. Scenery, the track and where traffic appears are not changed. Changes at once.", 1, FMT_PLAIN, n_vehicle_detail, 0 },
    /* XS_INPUT_GLYPHS */       { "Button prompts", "The buttons the game shows in its menus and hints. Auto follows the controller that was last used. Xbox (original) is the game's own picture: white and black buttons, L and R. The game's text and videos still name the Xbox buttons.", 1, FMT_PLAIN, n_glyphs, 0 },
    /* XS_AUDIO_LFE_LOWPASS */  { "Subwoofer low-pass (LFE)", "120 Hz is what the console's Dolby Digital encoder did; Off sends the full-range LFE feed.", 1, FMT_PLAIN, n_lfe_lowpass, 0 },
    /* XS_DISPLAY_PRESENT_TIMING */ { "Frame presentation", "When a finished frame is handed to the display. Legacy: as soon as it is ready. Auto: every frame is aimed at one refresh of the display, so each stays on the screen for the same time, and sooner after it was drawn: on a fixed 60/120/240 Hz display the game's clock follows the display; with variable refresh (VRR, ProMotion) frames are presented at even times. The performance overlay shows the result (the on-glass line).", 1, FMT_PLAIN, n_timing, 0 },
    /* XS_ADVANCED_PERF_STEPS */ { "Graph: simulation steps", "The frame-time graph also marks the frames in which the game ran more than one simulation step (it does when its frame rate drops).", 1, FMT_PLAIN, NULL, 0 },
    /* XS_GRAPHICS_UI_SCALE */  { "Menu and HUD upscaling", "Redraws the game's 2D art (menu text, HUD, icons, logos) at 2 or 4 times its size with an edge-following scaler (Super-xBR), for a render scale above 1x. Off is the original. The 3D world is not touched. Changes at once. The sharper pictures are made in the background and kept on disk, so new art can show unscaled for a moment the first time; uses more texture memory.", 1, FMT_PLAIN, n_ui_scale, 0 },
    /* XS_GRAPHICS_UI_SCALE_FILTER */ { "Upscaling filter", "How the menu and HUD art is enlarged. Super-xBR follows edges: smooth outlines, the best on this game's text and digits. Bicubic and Lanczos are plain enlargements: softer and a little sharper, both keep the stair-steps. MMPX is a pixel-art scaler that keeps exact colours: blocky here, because this art is antialiased. Changes at once; each filter keeps its own pictures on disk.", 1, FMT_PLAIN, n_ui_scale_filter, 0 },
#if defined(__ANDROID__)
    /* XS_INPUT_VIBRATION */    { "Vibration", "Where the rumble goes. Auto: this device's own vibrator while its built-in controls are the ones in use. An external controller does not rumble on Android; This device shakes the device anyway.", 1, FMT_PLAIN, n_vibration, 0 },
#endif
};

/* R_SPEAKERS: the speaker test, six buttons in a row (action: 0 through
 * the game's mixbins and speaker map, 1 straight to the output). */
#if defined(__ANDROID__)
extern "C" const char *nvk_core_describe(void);   /* nv2a_vk_core.c */
#endif
enum RowKind { R_SETTING, R_ACTION, R_INFO, R_SPEAKERS, R_FOLDER };
enum Action { A_RESET_PAGE, A_RESET_ALL, A_REPORT, A_REPORT_SHOT,
              A_HOST0, A_HOST1, A_HOST2, A_HOST3,                  /* xbox_OverlaySetHostRows */
              A_SHADERS, A_SHADERS_LATER, A_SHADERS_NEVER,        /* A_SHADERS on: the build's */
              A_MIXER };                                          /* the Mixer page (overlay_mixer.inc) */

static int s_host_n;
static const char *(*s_host_fn)(int row, int activate);

void xbox_OverlaySetHostRows(int n, const char *(*fn)(int row, int activate))
{
    s_host_fn = fn;
    s_host_n = fn ? (n > XBOX_OVERLAY_HOST_ROWS ? XBOX_OVERLAY_HOST_ROWS : n) : 0;
}

/* ── the bug report (xbox_report.h) ────────────────────────────────── */

static char s_rep_info[160] = "Bug report: a zip file on this device; nothing is sent.";
static char s_rep_done[1400];         /* where the last one went, or why none */

/* ── the port's name and version ───────────────────────────────────── */

static char s_about[200];

/* ── the music folder (xbox_OverlaySetMusicFolder) ─────────────────── */

#define MUSIC_KEY "audio.custom_music_dir"
static const xbox_music_folder_ops *s_mf;
static char s_mf_status[200];
#define MUSIC_NOTES 4
static char s_mf_note[MUSIC_NOTES][200];
static int s_fb_sel;                  /* the browser's row */
static bool s_fb_scroll;

void xbox_OverlaySetMusicFolder(const xbox_music_folder_ops *ops)
{
    s_mf = ops;
}

static const char *path_leaf(const char *p)
{
    const char *e = p + strlen(p), *b;
    while (e > p && (e[-1] == '/' || e[-1] == '\\'))
        e--;
    b = e;
    while (b > p && b[-1] != '/' && b[-1] != '\\')
        b--;
    return b;
}

/* "Off", or the folder's own name. */
static void music_value(char *out, size_t n)
{
    const char *dir = xbox_SettingsText(MUSIC_KEY, NULL);
    if (!dir[0]) {
        snprintf(out, n, "Off");
        return;
    }
    const char *b = path_leaf(dir);
    int len = (int)strcspn(b, "/\\");
    snprintf(out, n, "%.*s", len > 40 ? 40 : len, b);
}

static void browser_count(const xsb_row &r, char *out, size_t n)
{
    out[0] = 0;
    if (r.kind == XSB_OFF || r.kind == XSB_UP)
        return;
    if (r.songs < 0)
        snprintf(out, n, "...");
    else if (r.songs == 0)
        snprintf(out, n, r.more ? "none found yet" : "no music");
    else
        snprintf(out, n, "%d%s song%s", r.songs, r.more ? "+" : "", r.songs == 1 && !r.more ? "" : "s");
}

static void changed();

/* A browser row is picked. */
static void browser_pick(int i)
{
    char dir[1024] = "";
    int sel = 0;
    xsb_result res = xsb_activate(i, dir, sizeof dir, &sel);
    if (res == XSB_MOVED) {
        s_fb_sel = sel;
        s_fb_scroll = true;
    } else if (res == XSB_CHOSEN || res == XSB_TURNED_OFF) {
        if (res == XSB_TURNED_OFF)
            dir[0] = 0;
        xbox_SettingsSetText(MUSIC_KEY, dir);
        changed();
        fprintf(stderr, LOGP "music folder: %s\n", dir[0] ? dir : "off");
        if (s_mf && s_mf->chosen)
            s_mf->chosen(dir);
        xsb_close();
    }
}

void xbox_OverlaySetAbout(const char *text)
{
    snprintf(s_about, sizeof s_about, "%s", text ? text : "");
}

/* ── the shader pre-build (xbox_shader_build.h) ────────────────────── */

static const xbox_shader_build_ops *s_sb;
static bool s_sb_offer;               /* this start's offer is on screen */
static bool s_sb_offer_checked;
static char s_sb_label[200], s_sb_info[200];

void xbox_OverlaySetShaderBuild(const xbox_shader_build_ops *ops)
{
    s_sb = ops;
}

static int sb_state(float *fraction, char *text, unsigned n)
{
    if (text && n)
        text[0] = 0;
    return s_sb && s_sb->poll ? s_sb->poll(fraction, text, n) : XBOX_SHADERS_IDLE;
}

static int page_rows(int page, struct Row *r);

/* The offer's rows come and go: the selection goes (back) to the build's
 * row, never to whatever row slid into its place. */
static void sb_select();

/* The action row's label and the line above it, as the build stands. */
static void sb_texts()
{
    float fr = 0;
    char text[160];
    int st = sb_state(&fr, text, sizeof text);
    switch (st) {
    case XBOX_SHADERS_RUNNING:
        snprintf(s_sb_label, sizeof s_sb_label, "Stop building shaders");
        snprintf(s_sb_info, sizeof s_sb_info, "Building shaders: %d%%  (%s)",
                 (int)(fr * 100.0f + 0.5f), text);
        break;
    case XBOX_SHADERS_DONE:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders again");
        snprintf(s_sb_info, sizeof s_sb_info, "Shaders: built.");
        break;
    case XBOX_SHADERS_FAILED:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders now");
        snprintf(s_sb_info, sizeof s_sb_info, "Shaders: not finished (%.120s).", text);
        break;
    default:
        snprintf(s_sb_label, sizeof s_sb_label, "Build shaders now");
        snprintf(s_sb_info, sizeof s_sb_info, s_sb_offer
                 ? "First start: the shaders are not built yet. Build them now?"
                 : "Shaders: not built; each compiles when it is first drawn.");
        break;
    }
}

struct Row {
    RowKind kind;
    xs_id id;                         /* R_SETTING */
    int action;                       /* R_ACTION */
    const char *text;                 /* R_ACTION label, R_INFO line */
};

/* The Advanced page's three lines on what reaches the glass (below, with
 * the frame-time graph). */
static char s_glass_info[3][160];
static void glass_info();

static int page_rows(int page, Row *r)
{
    int n = 0;
#define SET(i)  r[n++] = Row{ R_SETTING, (i), 0, NULL }
#define ACT(a, t) r[n++] = Row{ R_ACTION, XS_COUNT, (a), (t) }
#define INFO(t) r[n++] = Row{ R_INFO, XS_COUNT, 0, (t) }
#define SPK(a, t) r[n++] = Row{ R_SPEAKERS, XS_COUNT, (a), (t) }
    switch (page) {
    case P_DISPLAY:
        SET(XS_DISPLAY_FULLSCREEN); SET(XS_DISPLAY_ASPECT); SET(XS_DISPLAY_FILTER);
        SET(XS_DISPLAY_OVERSCAN); SET(XS_DISPLAY_FIT);
#if defined(__APPLE__)
        if (!s_vulkan)                 /* the Metal presenter's (BO-115) */
            SET(XS_DISPLAY_PRESENT_TIMING);
#endif
        if (recomp_patch_enabled(xbox_SettingGet(XS_GAME_SPEED_UNITS)->key))
            SET(XS_GAME_SPEED_UNITS);
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_HUD_FIX)->key))
            SET(XS_DISPLAY_HUD_FIX);
        if (recomp_patch_enabled(xbox_SettingGet(XS_DISPLAY_ULTRAWIDE)->key))
            SET(XS_DISPLAY_ULTRAWIDE);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_GRAPHICS:
        SET(XS_GRAPHICS_RENDER_SCALE); SET(XS_GRAPHICS_ANISO);
        SET(XS_GRAPHICS_AA); SET(XS_GRAPHICS_SHARPEN);
        SET(XS_GRAPHICS_LOD_BIAS);
        SET(XS_GRAPHICS_UI_SCALE);
        if (xbox_SettingValue(XS_GRAPHICS_UI_SCALE) != 0)   /* nothing to choose while it is off */
            SET(XS_GRAPHICS_UI_SCALE_FILTER);
#if defined(__APPLE__)          /* only the Metal presenter has the pass so far */
        SET(XS_GRAPHICS_DITHER);
#endif
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_BLOOM)->key))
            SET(XS_GRAPHICS_BLOOM);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_MOTION_BLUR)->key))
            SET(XS_GRAPHICS_MOTION_BLUR);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_CRASH_60FPS)->key))
            SET(XS_GRAPHICS_CRASH_60FPS);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_SPLIT_60FPS)->key))
            SET(XS_GRAPHICS_SPLIT_60FPS);
        if (recomp_patch_enabled(xbox_SettingGet(XS_GRAPHICS_VEHICLE_DETAIL)->key))
            SET(XS_GRAPHICS_VEHICLE_DETAIL);
#if defined(__ANDROID__)
        {
            /* Which driver draws: chosen before the game starts, in the app's
             * second launcher entry (the title's process has loaded it). */
            static char drv[420];
            const char *d = nvk_core_describe();
            if (d && *d) {
                snprintf(drv, sizeof drv, "Graphics driver in use: %s", d);
                INFO(drv);
            }
        }
#endif
        for (int i = 0; i < s_host_n; i++) {
            const char *t = s_host_fn(i, 0);
            if (t && *t)
                ACT(A_HOST0 + i, t);
        }
        if (s_sb) {
            sb_texts();
            INFO(s_sb_info);
            ACT(A_SHADERS, s_sb_label);
            if (s_sb_offer) {
                ACT(A_SHADERS_LATER, "Not now");
                ACT(A_SHADERS_NEVER, "Do not ask again");
            }
        }
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_AUDIO:
        SET(XS_AUDIO_VOLUME); SET(XS_AUDIO_MUTE); SET(XS_AUDIO_MUTE_BG); SET(XS_AUDIO_SPEAKERS);
        /* The dry effect sends' level: only where no model of the title's
         * effects image runs (apu_gp.h); with one they are not dry. */
        if (!apu_gp_wired())
            SET(XS_AUDIO_FX_LEVEL);
        /* audio.lfe_lowpass has no row: the stock 120 Hz is the console's,
         * and the Mixer page has its own LFE low-pass for tuning. */
        if (apu_bus_count() > 0)
            ACT(A_MIXER, "Mixer: buses, sends, LFE, limiter, meters");
        if (recomp_patch_enabled(xbox_SettingGet(XS_AUDIO_SPLIT_MUSIC)->key))
            SET(XS_AUDIO_SPLIT_MUSIC);
        SPK(0, "Speaker test, game mix");
        SPK(1, "Speaker test, direct");
        if (mcpx_apu_title_channels() != 6)
            INFO("The output is stereo: each speaker is folded to left/right.");
        if (s_mf) {
            r[n++] = Row{ R_FOLDER, XS_COUNT, 0, "Custom music folder" };
            s_mf_status[0] = 0;
            if (s_mf->status)
                s_mf->status(s_mf_status, sizeof s_mf_status);
            if (s_mf_status[0])
                INFO(s_mf_status);
            /* More lines of the port's (what a scan left out, by name), as
             * far as the page has room. */
            for (int k = 0; k < MUSIC_NOTES && s_mf->note && n < 15; k++) {
                s_mf_note[k][0] = 0;
                if (!s_mf->note(k, s_mf_note[k], sizeof s_mf_note[k]) || !s_mf_note[k][0])
                    break;
                INFO(s_mf_note[k]);
            }
        }
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_INPUT:
        SET(XS_INPUT_DEADZONE); SET(XS_INPUT_TRIGGER_DEADZONE); SET(XS_INPUT_RUMBLE);
        SET(XS_INPUT_FACE_LAYOUT);
        if (xbox_InputControlSchemeOffered())
            SET(XS_INPUT_CONTROL_SCHEME);
        SET(XS_INPUT_GLYPHS);
#if defined(__ANDROID__)
        SET(XS_INPUT_VIBRATION);
#endif
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_POWER:
#if !defined(__linux__)
        {
            /* The note names the default this build has: the title's own
             * choice (xbox_SettingsGameDefault), Balanced where it takes the
             * Deck's pacing (Windows), the busy-wait where it sets none
             * (macOS). */
            const xs_setting *pm = xbox_SettingGet(XS_POWER_MODE);
            if (xbox_PowerStockHidden(xbox_SettingsHost())) {
                INFO("Tuned on the Steam Deck (Linux). Balanced is the default here too.");
            } else if (pm && (int)pm->def == 1) {
                INFO("Tuned on the Steam Deck (Linux). Here the default is Balanced");
                INFO("too; Maximum is the game's own busy-wait.");
            } else if (pm && (int)pm->def == 2) {
                INFO("Tuned on the Steam Deck (Linux). Here the default is Battery");
                INFO("saver; Maximum is the game's own busy-wait.");
            } else {
                INFO("Tuned on the Steam Deck (Linux). Here the default is Stock, the");
                INFO("game's own busy-wait; the other two sleep between frames.");
            }
        }
#endif
        SET(XS_POWER_MODE);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    default:
        if (s_about[0])
            INFO(s_about);
        SET(XS_ADVANCED_PERF);
#if defined(__APPLE__)
        /* The Metal presenter's (BO-115); the other presenters do not
         * record when frames reach the glass, and their pages are as they
         * were. */
        if (!s_vulkan) {
            SET(XS_ADVANCED_PERF_STEPS);
            /* What reaches the glass, in words (the graph has one line of it). */
            glass_info();
            INFO(s_glass_info[0]);
            INFO(s_glass_info[1]);
            INFO(s_glass_info[2]);
        }
#endif
        if (xbox_BugReportHasMaker()) {
            INFO(s_rep_info);
            ACT(A_REPORT, "Create bug report");
            ACT(A_REPORT_SHOT, "Create bug report with a screenshot");
        }
        ACT(A_RESET_ALL, "Reset all settings to the defaults");
        break;
    }
#undef SET
#undef ACT
#undef INFO
#undef SPK
    return n;
}

static void value_text(xs_id id, char *out, size_t n);

static void test_state(const char *path)
{
    static const char *const kinds[] = { "setting", "action", "info", "speakers", "folder" };
    static const char *const bkinds[] = { "use", "off", "up", "place", "dir" };
    Row r[16];
    int n = page_rows(s_page, r);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "open %d\npage %s\noffer %d\npaused %d\n", s_open ? 1 : 0, k_page_name[s_page],
            s_sb_offer ? 1 : 0, xbox_HostPaused());
    if (xsb_is_open()) {
        /* The folder browser, over the page: where it is and its rows. */
        char where[1024], c[48];
        xsb_row b;
        xsb_where(where, sizeof where);
        fprintf(f, "browser %s\n", where[0] ? where : "(places)");
        for (int i = 0; xsb_row_at(i, &b); i++) {
            browser_count(b, c, sizeof c);
            fprintf(f, "%c %s: %s%s%s\n", i == s_fb_sel ? '>' : ' ', bkinds[b.kind], b.label,
                    c[0] ? " = " : "", c);
        }
        fclose(f);
        return;
    }
    for (int i = 0; i < n; i++) {
        char v[96] = "";
        const char *label = r[i].text;
        if (r[i].kind == R_SETTING) {
            label = k_meta[r[i].id].label;
            value_text(r[i].id, v, sizeof v);
        } else if (r[i].kind == R_FOLDER) {
            music_value(v, sizeof v);
        }
        fprintf(f, "%c %s: %s%s%s\n", s_open && i == s_sel[s_page] ? '>' : ' ', kinds[r[i].kind],
                label ? label : "", v[0] ? " = " : "", v);
    }
    fclose(f);
}

static void sb_select()
{
    Row r[16];
    int n = page_rows(P_GRAPHICS, r);
    for (int i = 0; i < n; i++)
        if (r[i].kind == R_ACTION && r[i].action == A_SHADERS)
            s_sel[P_GRAPHICS] = i;
}

static bool page_has(int page, xs_id id)
{
    Row r[16];
    int n = page_rows(page, r), i;
    for (i = 0; i < n; i++)
        if (r[i].kind == R_SETTING && r[i].id == id)
            return true;
    return false;
}

static bool selectable(const Row &r) { return r.kind != R_INFO; }

/* ── the speaker test ──────────────────────────────────────────────── */

static const char *const k_spk_short[6] = { "FL", "FR", "C", "LFE", "SL", "SR" };
static const char *const k_spk_name[6] = { "Front Left", "Front Right", "Center", "LFE",
                                           "Surround Left", "Surround Right" };
static int s_spk_sel[2];              /* the button picked on each row */

static void spk_play(int row)
{
    mcpx_apu_speaker_test(s_spk_sel[row], row == 0);
}

/* The hint for a speaker row: which mixbins the game-mix row feeds. */
static void spk_hint(int row, char *out, size_t n)
{
    int sp = s_spk_sel[row];
    if (row == 1) {
        snprintf(out, n, "%s: noise straight into that output channel, after the mix. "
                 "If this row and the game-mix row disagree, the speaker map is wrong; "
                 "if both are wrong, the output order is.", k_spk_name[sp]);
        return;
    }
    if (mcpx_apu_title_channels() != 6) {
        snprintf(out, n, "%s: noise into mixbin %d (DirectSound's), through the stereo mix "
                 "(even bins left, odd right: C plays left, LFE right).",
                 k_spk_name[sp], sp);
        return;
    }
    int8_t map[32];
    char bins[128] = "";
    size_t len = 0;
    mcpx_apu_speaker_map_get(map);
    for (int b = 0; b < 32; b++)
        if (map[b] == sp && len < sizeof bins - 8)
            len += snprintf(bins + len, sizeof bins - len, "%s%d", len ? ", " : "", b);
    snprintf(out, n, "%s: noise into the mixbins the speaker map sends there (%s), "
             "mixed as the game's sounds are.", k_spk_name[sp], bins);
}

/* ── value text ────────────────────────────────────────────────────── */

static void value_text(xs_id id, char *out, size_t n)
{
    const xs_setting *s = xbox_SettingGet(id);
    const Meta &m = k_meta[id];
    double v = s->value;

    switch (s->type) {
    case XS_BOOL:
        snprintf(out, n, "%s", v != 0.0 ? "On" : "Off");
        break;
    case XS_CHOICE:
        {
            const char *const *names = m.names == n_power && !on_steam_deck() ? n_power_plain : m.names;
            snprintf(out, n, "%s", names ? names[(int)v] : s->choices[(int)v]);
        }
        break;
    default:
        switch (m.fmt) {
        case FMT_PERCENT:        snprintf(out, n, "%d%%", (int)floor(v * 100.0 + 0.5)); break;
        case FMT_PERCENT_OF_MAX: snprintf(out, n, "%d%%", (int)floor(v / s->max * 100.0 + 0.5)); break;
        case FMT_TIMES:          snprintf(out, n, "%dx", (int)v); break;
        case FMT_GAIN:
            if (v <= 0.0)
                snprintf(out, n, "Off");
            else
                snprintf(out, n, v == 1.0 ? "%.2f (original)" : "%.2f", v);
            break;
        default:                 snprintf(out, n, "%g", v); break;
        }
    }
}

/* ── changing a row ────────────────────────────────────────────────── */

static uint32_t now_ms() { return SDL_GetTicks(); }

static void changed()
{
    s_save_pending = true;
    s_save_at = now_ms() + 600;
}

/* Left/right on a setting (dir -1/+1). */
static void adjust(xs_id id, int dir)
{
    const xs_setting *s = xbox_SettingGet(id);
    const Meta &m = k_meta[id];
    double v = s->value;

    if (s->locked)
        return;
    switch (s->type) {
    case XS_BOOL:
        v = dir > 0 ? 1 : 0;
        break;
    case XS_CHOICE: {
        int n = 0, i = (int)v;
        while (s->choices[n])
            n++;
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            unsigned off = m.disabled;
            if (id == XS_POWER_MODE && xbox_PowerStockHidden(xbox_SettingsHost()))
                off |= 1u;      /* Maximum: not offered here (BO-75) */
            if (!(off & (1u << i)))
                break;
        }
        v = i;
        break;
    }
    default:
        v += dir * m.step;
        break;
    }
    if (v != s->value) {
        xbox_SettingSet(id, v);
        changed();
    }
}

/* Activate: a bool flips, a choice moves on, an action runs. */
static void mx_open();
static bool mx_back();
static void mx_poll();
static void mx_ab_toggle();
static bool s_mx_open;                 /* the Mixer page shows (overlay_mixer.inc) */

static void activate(const Row &r)
{
    if (r.kind == R_ACTION && r.action == A_MIXER) {
        mx_open();
        return;
    }
    if (r.kind == R_SPEAKERS) {
        spk_play(r.action);
        return;
    }
    if (r.kind == R_FOLDER) {
        int locked = 0;
        const char *dir = xbox_SettingsText(MUSIC_KEY, &locked);
        if (!locked) {
            xsb_open(dir);
            /* On the current folder when there is one ("turn off" is above it). */
            s_fb_sel = dir[0] ? 1 : 0;
            s_fb_scroll = true;
        }
        return;
    }
    if (r.kind == R_ACTION && r.action >= A_HOST0 && r.action <= A_HOST3) {
        if (s_host_fn)
            s_host_fn(r.action - A_HOST0, 1);
        return;
    }
    if (r.kind == R_ACTION && (r.action == A_REPORT || r.action == A_REPORT_SHOT)) {
        /* On this thread, with the title paused behind the overlay: a second
         * or so (the frame is the executor's, waited for). The path is shown
         * as the archive writes paths: the home folder as ~. */
        char path[1200] = "";
        int ok = xbox_BugReportMake(r.action == A_REPORT_SHOT, path, sizeof path);
        char *shown = xbox_report_redact(NULL, path, strlen(path), NULL);
        const char *p = shown ? shown : path;
        const char *base = p;
        for (const char *c = p; *c; c++)
            if (*c == '/' || *c == '\\')
                base = c + 1;
        if (ok) {
            snprintf(s_rep_info, sizeof s_rep_info, "Saved: %.100s (path below)", base);
            snprintf(s_rep_done, sizeof s_rep_done, "Saved to %s", p);
        } else {
            snprintf(s_rep_info, sizeof s_rep_info, "Bug report: not made (reason below)");
            snprintf(s_rep_done, sizeof s_rep_done, "No report: %s", p);
        }
        fprintf(stderr, LOGP "bug report: %s\n", s_rep_done);
        free(shown);
        return;
    }
    if (r.kind == R_ACTION && r.action >= A_SHADERS) {
        if (!s_sb)
            return;
        if (r.action == A_SHADERS) {
            if (sb_state(NULL, NULL, 0) == XBOX_SHADERS_RUNNING)
                s_sb->cancel();
            else
                s_sb->start();
            s_sb_offer = false;
            sb_select();
        } else {
            if (r.action == A_SHADERS_NEVER && s_sb->offer_never)
                s_sb->offer_never();
            s_sb_offer = false;
            sb_select();
            s_back++;                 /* answered: close */
        }
        return;
    }
    if (r.kind == R_ACTION) {
        if (r.action == A_RESET_ALL) {
            xbox_SettingsResetAll();
        } else {
            for (int i = 0; i < XS_COUNT; i++)
                if (page_has(s_page, (xs_id)i))
                    xbox_SettingReset((xs_id)i);
        }
        changed();
        return;
    }
    if (r.kind != R_SETTING)
        return;
    const xs_setting *s = xbox_SettingGet(r.id);
    if (s->type == XS_BOOL)
        xbox_SettingSet(r.id, s->value != 0.0 ? 0 : 1), changed();
    else if (s->type == XS_CHOICE)
        adjust(r.id, 1);
}

/* ── open and close ────────────────────────────────────────────────── */

static void clear_nav()
{
    memset(s_dpad, 0, sizeof s_dpad);
    memset(s_stick, 0, sizeof s_stick);
    memset(s_key, 0, sizeof s_key);
    memset(s_steps, 0, sizeof s_steps);
    s_activate = s_back = s_page_delta = s_reset = 0;
}

/* The title is paused while the overlay is open, except on the Mixer page:
 * a mix is set by ear, so there the game keeps running and sounding (its
 * input still held: the car coasts). Called wherever either changes. */
static void pause_sync()
{
    static int applied = -1;
    int want = s_open && !s_mx_open;
    if (want != applied) {
        applied = want;
        xbox_HostPause(want);
        if (s_open)
            fprintf(stderr, LOGP "%s\n", want ? "the title is paused" : "Mixer page: the title runs while it is open");
    }
}

static void set_open(bool on)
{
    if (on == s_open)
        return;
    s_open = on;
    clear_nav();
    if (on) {
        s_scroll_to_sel = true;
        xbox_InputSetBlocked(1);
        pause_sync();
        fprintf(stderr, LOGP "open: the title is paused, its input is held\n");
    } else {
        /* A build beside a running title would cost it frames: it stops
         * with the overlay, and goes on from there the next time. */
        if (s_sb && sb_state(NULL, NULL, 0) == XBOX_SHADERS_RUNNING) {
            s_sb->cancel();
            fprintf(stderr, LOGP "the shader build is stopped with the overlay\n");
        }
        if (s_sb_offer) {
            s_sb_offer = false;
            sb_select();
        }
        mcpx_apu_speaker_test(-1, 0);
        xsb_close();
        pause_sync();
        xbox_InputSetBlocked(0);
        xbox_SettingsFlush();
        s_save_pending = false;
        fprintf(stderr, LOGP "closed\n");
    }
}

/* ── events ────────────────────────────────────────────────────────── */

static void hot_toggle()
{
    set_open(!s_open);
}

/* A direction goes down or up on one of the three sources. A press counts a
 * step at once (a tap shorter than a frame still moves), and a hold repeats
 * from the frame (collect_steps). */
static void dir_set(bool *arr, int d, bool on)
{
    bool was = s_dpad[d] || s_stick[d] || s_key[d];
    arr[d] = on;
    if (on && !was) {
        s_steps[d]++;
        s_next_fire[d] = now_ms() + 350;
    }
}

static void stick_axis(int axis, int value)
{
    /* Hysteresis: on past 16000, off under 8000. */
    bool neg = value < 0;
    int mag = neg ? -value : value;
    int a = axis == SDL_CONTROLLER_AXIS_LEFTX ? 0 : 1;
    int d_neg = a == 0 ? D_LEFT : D_UP, d_pos = a == 0 ? D_RIGHT : D_DOWN;

    if (mag > 16000) {
        dir_set(s_stick, neg ? d_neg : d_pos, true);
        dir_set(s_stick, neg ? d_pos : d_neg, false);
    } else if (mag < 8000) {
        dir_set(s_stick, d_neg, false);
        dir_set(s_stick, d_pos, false);
    }
}

int xbox_OverlayEvent(const SDL_Event *ev)
{
    if (!s_enabled)
        return 0;

    switch (ev->type) {
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
                || ev->window.event == SDL_WINDOWEVENT_MINIMIZED
                || ev->window.event == SDL_WINDOWEVENT_HIDDEN)
            xbox_SettingsSetBackground(1);
        else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED
                 || ev->window.event == SDL_WINDOWEVENT_SHOWN
                 || ev->window.event == SDL_WINDOWEVENT_RESTORED)
            xbox_SettingsSetBackground(0);
        break;

    case SDL_KEYDOWN: {
        SDL_Keycode k = ev->key.keysym.sym;
        if (ev->key.repeat)
            break;
        if (k == SDLK_F1 || (k == SDLK_COMMA && (ev->key.keysym.mod & KMOD_GUI))) {
            hot_toggle();
            return 1;
        }
        if (k == SDLK_F7 && apu_bus_enabled()) {
            mx_ab_toggle();             /* the mixer's A/B, menu open or not */
            return 1;
        }
        if (!s_open)
            break;
        s_last_input_pad = false;
        switch (k) {
        case SDLK_UP:    dir_set(s_key, D_UP, true); break;
        case SDLK_DOWN:  dir_set(s_key, D_DOWN, true); break;
        case SDLK_LEFT:  dir_set(s_key, D_LEFT, true); break;
        case SDLK_RIGHT: dir_set(s_key, D_RIGHT, true); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: s_activate++; break;
        case SDLK_ESCAPE: s_back++; break;
        case SDLK_PAGEUP:
        case SDLK_LEFTBRACKET: s_page_delta--; break;
        case SDLK_PAGEDOWN:
        case SDLK_RIGHTBRACKET: s_page_delta++; break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE: s_reset++; break;
        default: break;
        }
        return 1;
    }
    case SDL_KEYUP:
        if (!s_open)
            break;
        switch (ev->key.keysym.sym) {
        case SDLK_UP:    dir_set(s_key, D_UP, false); break;
        case SDLK_DOWN:  dir_set(s_key, D_DOWN, false); break;
        case SDLK_LEFT:  dir_set(s_key, D_LEFT, false); break;
        case SDLK_RIGHT: dir_set(s_key, D_RIGHT, false); break;
        default: break;
        }
        return 1;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = ev->type == SDL_CONTROLLERBUTTONDOWN;
        unsigned bit = 1u << ((unsigned)ev->cbutton.which & 31u);
        int b = ev->cbutton.button;

        if (b == SDL_CONTROLLER_BUTTON_BACK) {
            if (down) s_back_mask |= bit; else s_back_mask &= ~bit;
        } else if (b == SDL_CONTROLLER_BUTTON_START) {
            if (down) s_start_mask |= bit; else s_start_mask &= ~bit;
        }
        /* Back and Start of one pad together: once per hold. */
        if (down && (s_back_mask & s_start_mask) && !s_combo_fired) {
            s_combo_fired = true;
            hot_toggle();
            return 1;
        }
        if (!(s_back_mask | s_start_mask))
            s_combo_fired = false;
        /* Back held + Y: the mixer's A/B, menu open or not. */
        if (down && b == SDL_CONTROLLER_BUTTON_Y && (s_back_mask & bit) && apu_bus_enabled()) {
            mx_ab_toggle();
            return 1;
        }
        if (!s_open)
            break;
        s_last_input_pad = true;
        /* The Nintendo face layout (Input > Face buttons) holds here too:
         * the button printed A confirms, the one printed B goes back. */
        if (xbox_InputFaceLayoutNintendo()) {
            if (b == SDL_CONTROLLER_BUTTON_A) b = SDL_CONTROLLER_BUTTON_B;
            else if (b == SDL_CONTROLLER_BUTTON_B) b = SDL_CONTROLLER_BUTTON_A;
            else if (b == SDL_CONTROLLER_BUTTON_X) b = SDL_CONTROLLER_BUTTON_Y;
            else if (b == SDL_CONTROLLER_BUTTON_Y) b = SDL_CONTROLLER_BUTTON_X;
        }
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    dir_set(s_dpad, D_UP, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  dir_set(s_dpad, D_DOWN, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dir_set(s_dpad, D_LEFT, down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dir_set(s_dpad, D_RIGHT, down); break;
        case SDL_CONTROLLER_BUTTON_A: if (down) s_activate++; break;
        case SDL_CONTROLLER_BUTTON_B: if (down) s_back++; break;
        case SDL_CONTROLLER_BUTTON_Y: if (down) s_reset++; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  if (down) s_page_delta--; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) s_page_delta++; break;
        default: break;
        }
        return 1;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (!s_open)
            break;
        if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            s_last_input_pad = true;
            stick_axis(ev->caxis.axis, ev->caxis.value);
        }
        return 1;

    default:
        break;
    }

    /* The mouse, and the window's own events, for ImGui while it shows. */
    if (s_open && s_core_ready) {
        ImGui_ImplSDL2_ProcessEvent(ev);
        if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN
                || ev->type == SDL_MOUSEBUTTONUP || ev->type == SDL_MOUSEWHEEL)
            return 1;
    }
    return 0;
}

/* A held direction repeats, after a first delay. */
static void collect_steps()
{
    uint32_t now = now_ms();
    for (int d = 0; d < D_COUNT; d++) {
        bool held = s_dpad[d] || s_stick[d] || s_key[d];
        if (held && (int32_t)(now - s_next_fire[d]) >= 0) {
            s_steps[d]++;
            s_next_fire[d] = now + 70;
        }
    }
}

/* ── RECOMP_OVERLAY_TEST: a test drives the overlay ────────────────── */

extern "C" void xbox_HostWindowShot(const char *path);     /* video_player.h */

/* Each line appended to the file is done once: keys and pad buttons go into
 * the window's event queue as SDL events, so they take the same road as a
 * real press (xbox_OverlayEvent and the input layer after it); a shot is the
 * window as presented; a state dump is the open page's rows, the selected
 * one marked, for a test to assert on. */
static void test_key(SDL_Keycode sym)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
    ev.type = SDL_KEYUP;
    ev.key.state = SDL_RELEASED;
    SDL_PushEvent(&ev);
}

static void test_pad(int button, bool down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.which = 30;            /* no such controller: the overlay only */
    ev.cbutton.button = (Uint8)button;
    ev.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&ev);
}

static void test_state(const char *path);

static void test_poll()
{
    static const char *path;
    static bool checked;
    static long consumed;
    static uint32_t next;
    static const struct { const char *name; SDL_Keycode sym; } keys[] = {
        {"up", SDLK_UP}, {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT},
        {"enter", SDLK_RETURN}, {"esc", SDLK_ESCAPE}, {"f1", SDLK_F1},
        {"pgup", SDLK_PAGEUP}, {"pgdn", SDLK_PAGEDOWN}, {"del", SDLK_DELETE},
    };
    static const struct { const char *name; int b; } pads[] = {
        {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B},
        {"y", SDL_CONTROLLER_BUTTON_Y}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    };
    char line[600];
    FILE *f;

    if (!checked) {
        checked = true;
        path = getenv("RECOMP_OVERLAY_TEST");
        if (path && *path)
            fprintf(stderr, LOGP "driven by %s\n", path);
    }
    if (!path || !*path || (int32_t)(now_ms() - next) < 0)
        return;
    next = now_ms() + 30;
    if (!(f = fopen(path, "rb")))
        return;
    /* One line a turn: the events of one are handled before the next. */
    if (fseek(f, consumed, SEEK_SET) == 0 && fgets(line, sizeof line, f)
            && strchr(line, '\n')) {
        consumed += (long)strlen(line);
        line[strcspn(line, "\r\n")] = 0;
        fprintf(stderr, LOGP "test: %s\n", line);
        if (!strncmp(line, "key ", 4)) {
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (!strcmp(line + 4, keys[i].name))
                    test_key(keys[i].sym);
        } else if (!strcmp(line, "pad back+start")) {
            test_pad(SDL_CONTROLLER_BUTTON_BACK, true);
            test_pad(SDL_CONTROLLER_BUTTON_START, true);
            test_pad(SDL_CONTROLLER_BUTTON_START, false);
            test_pad(SDL_CONTROLLER_BUTTON_BACK, false);
        } else if (!strncmp(line, "pad ", 4)) {
            for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++)
                if (!strcmp(line + 4, pads[i].name)) {
                    test_pad(pads[i].b, true);
                    test_pad(pads[i].b, false);
                }
        } else if (!strncmp(line, "shot ", 5)) {
            xbox_HostWindowShot(line + 5);
        } else if (!strncmp(line, "state ", 6)) {
            test_state(line + 6);
        }
    }
    fclose(f);
}

void xbox_OverlayPoll(void)
{
    if (!s_enabled)
        return;
    test_poll();

    while (s_toggle_i < s_toggle_n && (int32_t)(now_ms() - s_t0_ms - s_toggle_ms[s_toggle_i]) >= 0) {
        s_toggle_i++;
        hot_toggle();
    }

    if (s_cycle_ms && (int32_t)(now_ms() - s_cycle_next) >= 0) {
        s_cycle_next = now_ms() + s_cycle_ms;
        s_cycle_i = (s_cycle_i + 1) % 3;
        xbox_SettingSet(XS_GRAPHICS_AA, s_cycle_i);
    }
    /* The first start's offer, once the window has been up for a moment:
     * the overlay opens on the Graphics page, on the build's row. */
    if (!s_sb_offer_checked && s_win && s_sb && (int32_t)(now_ms() - s_t0_ms - 1500) >= 0) {
        s_sb_offer_checked = true;
        if (!s_open && s_sb->offer && s_sb->offer()) {
            s_sb_offer = true;
            s_page = P_GRAPHICS;
            sb_select();
            set_open(true);
            s_sb_offer = true;        /* set_open(false) clears it; opening does not */
            fprintf(stderr, LOGP "first start: offering the shader build\n");
        }
    }
    if (s_trigger[0] && (int32_t)(now_ms() - s_trigger_next) >= 0) {
        s_trigger_next = now_ms() + 200;
        if (unlink(s_trigger) == 0)
            hot_toggle();
    }

    /* B or Esc: here and not in the frame, so a window that cannot present
     * (minimised) still closes. */
    mx_poll();
    pause_sync();
    if (s_back > 0) {
        s_back = 0;
        if (s_open && s_mx_open) {
            mx_back();
        } else if (s_open && xsb_is_open()) {
            /* In the folder browser: one level up, then back to the page. */
            int sel = 0;
            if (xsb_back(&sel) == XSB_CLOSED)
                xsb_close();
            else {
                s_fb_sel = sel;
                s_fb_scroll = true;
            }
        } else if (s_open)
            set_open(false);
    }
    pause_sync();

    /* The fullscreen setting, when it changes. */
    if (s_win) {
        int fs = xbox_SettingValue(XS_DISPLAY_FULLSCREEN) != 0.0;
        if (s_fs_applied < 0)
            s_fs_applied = fs;
        else if (fs != s_fs_applied) {
            s_fs_applied = fs;
            SDL_SetWindowFullscreen(s_win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
    }
    if (s_save_pending && (int32_t)(now_ms() - s_save_at) >= 0) {
        s_save_pending = false;
        xbox_SettingsFlush();
    }
}

void overlay_fail()
{
    if (s_open)
        set_open(false);
    s_enabled = false;
    xbox_InputSetComboGuard(0);
    fprintf(stderr, LOGP "no overlay (its renderer did not start)\n");
}

int xbox_OverlaySetWindow(SDL_Window *win)
{
    const char *e = getenv("RECOMP_OVERLAY");

    s_win = win;
    s_enabled = !(e && !strcmp(e, "0"));
    if (!s_enabled) {
        fprintf(stderr, LOGP "off (RECOMP_OVERLAY=0)\n");
        return 0;
    }
    xbox_InputSetComboGuard(1);
    s_t0_ms = now_ms();
    if (e && !strncmp(e, "toggle=", 7)) {
        const char *q = e + 7;
        while (*q && s_toggle_n < TOGGLE_MAX) {
            char *end;
            double sec = strtod(q, &end);
            if (end == q)
                break;
            s_toggle_ms[s_toggle_n++] = (uint32_t)(sec * 1000.0);
            q = *end == ',' ? end + 1 : end;
        }
    }
    if (e && !strncmp(e, "aa_cycle=", 9)) {
        s_cycle_ms = (uint32_t)(atof(e + 9) * 1000.0);
        s_cycle_next = now_ms() + s_cycle_ms;
    }
    if (e && !strncmp(e, "file=", 5))
        snprintf(s_trigger, sizeof s_trigger, "%s", e + 5);
    if (e && !strcmp(e, "open"))
        set_open(true);
    return 1;
}

int xbox_OverlayOpen(void)
{
    return s_enabled && s_open;
}

/* The render thread is held for the start-up pre-warm (a key list whose
 * compiled shaders the system no longer has: up to 3 s, once, before the
 * title's first pictures): the window says what it is doing instead of
 * standing black. */
static bool notice_on()
{
    nv2a_pcache_stat st;
    nv2a_pcache_stat_get(&st);
    return st.waiting != 0;
}

/* A pad script is driving a pad (RECOMP_PAD_LIVE / RECOMP_PAD_SCRIPT: a test
 * run) and no controller has taken it back: said on the screen, so a run a
 * script started is not picked up as one's own with the script's buttons
 * still held (BO-96). */
static bool scripted_on()
{
    /* RECOMP_DEBUG=pad_script_note=0: not shown (a scripted run whose
     * window shots are compared). Drawn with the title's frames only: it
     * does not make the window redraw by itself, so a measured run's
     * pacing is what it was. */
    static int show = -1;
    if (show < 0) {
        const char *v = recomp_env(RENV_PAD_SCRIPT_NOTE);
        show = !(v && *v == '0');
    }
    return show && xbox_InputScriptedPads() != 0;
}

int xbox_OverlayNotice(void)
{
    return s_enabled && notice_on();
}

int xbox_OverlayWants(void)
{
    return s_enabled && (s_open || s_alpha > 0.0f || xbox_SettingValue(XS_ADVANCED_PERF) != 0.0
                         || notice_on() || scripted_on());
}

void xbox_OverlayFramePresented(void)
{
    s_presents++;
}

void xbox_OverlayShutdown(void)
{
    if (!s_enabled)
        return;
    if (s_open)
        set_open(false);
    xbox_SettingsFlush();
}

/* ── the colours ───────────────────────────────────────────────────── */

/* Every colour the overlay draws comes from one of these tables; nothing
 * below names a number. "game" is the main menu's own scheme (sampled from
 * its front end: ink-navy panels, steel-blue rules and accent, pale blue
 * text, the title red); "classic" is the earlier neutral grey and orange.
 * RECOMP_OVERLAY_THEME=classic picks the second; there is no setting for it
 * (a compile-time table plus that variable). Colours only. */
struct Rgb { uint8_t r, g, b; };

struct Theme {
    const char *name;
    Rgb scrim;          /* over the game, behind the panel */
    Rgb panel;          /* the settings window */
    Rgb edge;           /* the window's outline; border 0 = none */
    float border;       /* its width in points */
    Rgb rule;           /* thin separators */
    Rgb sel_fill;       /* the selected row's wash (alpha sel_fill_a) */
    float sel_fill_a;
    Rgb sel_bar;        /* the selected row's left bar */
    Rgb tab_on, tab_on_text, tab_text, tab_hint;
    Rgb text;           /* labels */
    Rgb text_hi;        /* the selected row's label and the values */
    Rgb text_dim;       /* locked rows, hints, the key line */
    Rgb text_info;      /* info rows */
    Rgb action;         /* action rows */
    Rgb track, track_off, fill, fill_dim, knob, on_accent;
    Rgb warn;           /* a locked row's reason, the restart note */
    Rgb notice_bg, notice_text;     /* the red scripted-input banner */
    Rgb toast_bg, toast_text;       /* the "Preparing shaders" line */
    Rgb perf_bg, perf_text, perf_text2, perf_good, perf_warn, perf_bad, perf_line;
};

static const Theme k_theme_game = {
    "game",
    {0x00, 0x00, 0x00},             /* scrim: black */
    {0x16, 0x1d, 0x26},             /* panel: the menu body navy */
    {0x5a, 0x99, 0xc0}, 1.0f,       /* edge: the steel-blue rule */
    {0x3f, 0x4d, 0x5d},             /* rule: the grid line */
    {0x5a, 0x99, 0xc0}, 0.24f,      /* selected wash: steel blue */
    {0xc4, 0x1c, 0x1c},             /* selected bar: the title crimson */
    {0x5a, 0x99, 0xc0}, {0x0f, 0x13, 0x19}, {0xac, 0xd4, 0xfc}, {0x82, 0x96, 0xab},
    {0xac, 0xd4, 0xfc},             /* text: the menu entries */
    {0xff, 0xff, 0xff},             /* hi: the lit entry */
    {0x82, 0x96, 0xab},             /* dim: derived, a desaturated step of the entries */
    {0x5a, 0x99, 0xc0},             /* info: the footer blue */
    {0xd4, 0xe4, 0xfc},             /* action */
    {0x2f, 0x39, 0x42}, {0x3f, 0x4d, 0x5d}, {0x5a, 0x99, 0xc0}, {0x4a, 0x5a, 0x6a},
    {0xd4, 0xe4, 0xfc}, {0x0f, 0x13, 0x19},
    {0xff, 0x8a, 0x80},             /* warn: the title red, lifted to read as text */
    {0x94, 0x0c, 0x14}, {0xff, 0xff, 0xff},     /* notice: the title red band */
    {0x11, 0x15, 0x1a}, {0xd4, 0xe4, 0xfc},     /* toast: the header/footer plate */
    {0x0f, 0x13, 0x19}, {0xe6, 0xf0, 0xff}, {0xac, 0xd4, 0xfc},
    {0x5c, 0xb0, 0x1a}, {0xe8, 0xc8, 0x46}, {0xe8, 0x48, 0x40}, {0xd4, 0xe4, 0xfc},
};

static const Theme k_theme_classic = {
    "classic",
    {0, 0, 0},
    {18, 18, 20},
    {0, 0, 0}, 0.0f,
    {51, 51, 53},
    {255, 120, 20}, 0.22f,
    {255, 140, 30},
    {255, 140, 30}, {20, 14, 8}, {190, 190, 198}, {120, 120, 128},
    {240, 240, 244},
    {255, 255, 255},
    {130, 130, 136},
    {160, 160, 168},
    {255, 190, 120},
    {60, 60, 66}, {70, 70, 76}, {255, 140, 30}, {110, 110, 116},
    {250, 250, 252}, {20, 14, 8},
    {240, 170, 90},
    {120, 30, 20}, {255, 240, 230},
    {18, 18, 20}, {235, 235, 240},
    {8, 8, 10}, {255, 255, 255}, {190, 190, 195},
    {90, 210, 120}, {235, 200, 70}, {235, 90, 80}, {255, 255, 255},
};

static const Theme *T = &k_theme_game;

static void theme_pick()
{
    const char *e = getenv("RECOMP_OVERLAY_THEME");
    T = e && !strcmp(e, "classic") ? &k_theme_classic : &k_theme_game;
}

/* ── drawing ───────────────────────────────────────────────────────── */

static ImU32 col(Rgb c, float a = 1.0f)
{
    return IM_COL32(c.r, c.g, c.b, (int)(a * s_alpha * 255.0f));
}

/* Always drawn whole: the perf graph and the notices do not fade. */
static ImU32 solid(Rgb c, float a = 1.0f)
{
    return IM_COL32(c.r, c.g, c.b, (int)(a * 255.0f));
}

static void accent_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f;
    st.WindowBorderSize = T->border;
    st.ScrollbarRounding = 6.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(T->panel.r / 255.0f, T->panel.g / 255.0f, T->panel.b / 255.0f, 0.96f);
    st.Colors[ImGuiCol_Border] = ImVec4(T->edge.r / 255.0f, T->edge.g / 255.0f, T->edge.b / 255.0f, 0.8f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
}

/* The text: Dear ImGui's own vector font (ProggyForever: Basic Latin and
 * Latin-1), which is what the overlay has always drawn with, and behind it a
 * subset of DejaVu Sans Mono for the letters that one lacks: Latin
 * Extended-A and -B, Greek, Cyrillic (overlay_font_ext.h,
 * third_party/dejavu). So a folder named in Hungarian, Polish, Czech,
 * Turkish, Greek or Russian reads as it is written. Every string is UTF-8.
 * A character neither font has (CJK, Arabic, emoji ...) is drawn as a
 * square, U+25A1, not as "?", which could be part of the name.
 *
 * The second font is scaled to the first one's x-height (DejaVu's letters
 * fill more of the em) and set on the same baseline. */
static void overlay_fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    ImFont *font = io.Fonts->AddFontDefaultVector();
    if (!font)
        return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;
    cfg.PixelSnapH = true;
    cfg.ExtraSizeScale = OVERLAY_FONT_EXT_SCALE;
    cfg.GlyphOffset.y = OVERLAY_FONT_EXT_DY;
    snprintf(cfg.Name, sizeof cfg.Name, "DejaVuSansMono subset");
    io.Fonts->AddFontFromMemoryTTF((void *)k_overlay_font_ext, (int)sizeof k_overlay_font_ext,
                                   13.0f, &cfg);
    font->FallbackChar = 0x25A1;
}

bool overlay_core_init(bool vulkan)
{
    if (s_core_ready)
        return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    /* Navigation is the overlay's own (see the top); ImGui is the renderer
     * and the mouse. */
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    overlay_fonts();
    theme_pick();
    accent_style();
    bool ok = vulkan ? ImGui_ImplSDL2_InitForVulkan(s_win) : ImGui_ImplSDL2_InitForMetal(s_win);
    if (!ok) {
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
    s_vulkan = vulkan;
    s_core_ready = true;
    s_last_ms = now_ms();
    return true;
}

/* ── the frame-time graph ──────────────────────────────────────────── */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* What reached the glass (present_cadence.h): looked at once per drawn
 * frame at most, shared by the graph and the Advanced page's lines. */
static pc_report s_glass;
static pc_status s_glass_st;
static pc_sample s_glass_smp[PC_RING];
static int s_glass_n;
static uint32_t s_glass_ms;

static void glass_update()
{
    uint32_t now = now_ms();
    if (s_glass_ms && now - s_glass_ms < 100)
        return;
    s_glass_ms = now ? now : 1;
    s_glass_n = xbox_PresentCadenceSnapshot(s_glass_smp, PC_RING, &s_glass_st);
    pc_analyze(s_glass_smp, s_glass_n, &s_glass_st.display, xbox_HostNowNs(), &s_glass);
}

/* "120 Hz fixed", "48-120 Hz VRR", "display rate unknown". */
static void glass_display_text(char *out, size_t n)
{
    const pc_display &d = s_glass_st.display;
    if (d.refresh_hz <= 0)
        snprintf(out, n, "display rate unknown");
    else if (d.vrr)
        snprintf(out, n, "%.0f-%.0f Hz VRR", d.min_hz, d.refresh_hz);
    else
        snprintf(out, n, "%.4g Hz fixed", d.refresh_hz);
}

static void glass_info()
{
    char disp[48];
    glass_update();
    glass_display_text(disp, sizeof disp);
    snprintf(s_glass_info[0], sizeof s_glass_info[0], "Display: %s. Presentation: %s%s", disp,
             pc_strategy_name(s_glass_st.strategy),
             s_glass_st.strategy == PC_LOCK ? " (the game's clock follows the display)" : "");
    if (!s_glass_st.measured) {
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On-glass times: not available (this graphics backend does not report them).");
        s_glass_info[2][0] = 0;
        return;
    }
    if (!s_glass.available) {
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On-glass times: not available right now (the window is hidden or covered).");
        s_glass_info[2][0] = 0;
        return;
    }
    if (s_glass.expected)
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On glass: a frame every %.2f ms (%.2f refreshes, should be %d); %d uneven of the last %d;"
                 " shown %.1f ms after its vblank (95%%: %.1f).",
                 s_glass.iv_p50_ms, s_glass.iv_p50_ms * s_glass_st.display.refresh_hz / 1000.0,
                 s_glass.expected, s_glass.uneven, s_glass.n_iv, s_glass.lat_p50_ms, s_glass.lat_p95_ms);
    else
        snprintf(s_glass_info[1], sizeof s_glass_info[1],
                 "On glass: a frame every %.2f ms (%.2f to %.2f); %d uneven of the last %d;"
                 " shown %.1f ms after its vblank (95%%: %.1f).",
                 s_glass.iv_p50_ms, s_glass.iv_min_ms, s_glass.iv_max_ms, s_glass.uneven, s_glass.n_iv,
                 s_glass.lat_p50_ms, s_glass.lat_p95_ms);
    snprintf(s_glass_info[2], sizeof s_glass_info[2],
             "Since the start: %u frames skipped (no free drawable), %u late for their refresh%s. Source: %s.",
             s_glass_st.skips, s_glass_st.slips + s_glass_st.lates,
             s_glass.steps_max ? (s_glass.steps_multi ? "; some frames ran 2+ simulation steps"
                                                      : "; 1 simulation step per frame") : "",
             s_glass_st.source);
}

static void draw_perf(ImDrawList *dl, ImVec2 origin, float fs)
{
    uint64_t t[256];
    float iv[255];
    int n = xbox_HostFlipTimes(t, 256), m = 0, i;
    uint64_t newest = 0;
    float fps = 0, last = 0, p50 = 0, p99 = 0, mx = 0;
    static uint32_t pres_prev, pres_ms;
    static float pres_rate;
    uint32_t now = now_ms();

    for (i = 1; i < n; i++)
        if (t[i] && t[i - 1])
            iv[m++] = (float)((double)(t[i] - t[i - 1]) / 1e6);
    for (i = 0; i < n; i++)
        if (t[i] > newest)
            newest = t[i];
    if (m) {
        float srt[255];
        int cnt = 0;
        /* The rate over the last second of flips. */
        for (i = n - 1; i >= 0 && t[i]; i--) {
            if (newest - t[i] > 1000000000ull)
                break;
            cnt++;
        }
        if (cnt > 1 && t[n - cnt] && newest > t[n - cnt])
            fps = (float)((double)(cnt - 1) * 1e9 / (double)(newest - t[n - cnt]));
        last = iv[m - 1];
        memcpy(srt, iv, (size_t)m * sizeof(float));
        qsort(srt, (size_t)m, sizeof(float), cmp_float);
        p50 = srt[m / 2];
        p99 = srt[(int)((m - 1) * 0.99)];
        mx = srt[m - 1];
    }
    if (now - pres_ms >= 1000) {
        uint32_t p = s_presents;
        pres_rate = (float)(p - pres_prev) * 1000.0f / (float)(now - pres_ms);
        pres_prev = p;
        pres_ms = now;
    }
    glass_update();
    const pc_report &g = s_glass;
    /* A presenter that records on-glass times has said so (its status);
     * without one the panel is the two lines and the bars it always was. */
    const bool have_glass = s_glass_st.source[0] != 0;
    const bool steps = have_glass && xbox_SettingValue(XS_ADVANCED_PERF_STEPS) != 0.0;

    /* Three lines of text: the title's flips, what reached the glass, and a
     * warning only when there is something to warn of. */
    char l1[128], l2[128], l3[160], l4[160], disp[48];
    glass_display_text(disp, sizeof disp);
    snprintf(l1, sizeof l1, "%.1f fps   %.1f ms", fps, last);
    snprintf(l2, sizeof l2, "p50 %.1f  p99 %.1f  max %.1f ms   window %.0f/s", p50, p99, mx, pres_rate);
    if (!s_glass_st.measured)
        snprintf(l3, sizeof l3, "on glass: not available (this graphics backend)   %s", disp);
    else if (!g.available)
        snprintf(l3, sizeof l3, "on glass: not available (window hidden?)   %s", disp);
    else if (g.expected)
        snprintf(l3, sizeof l3, "on glass %.1f ms = %.1f refr   %s   uneven %d   lag %.0f ms",
                 g.iv_p50_ms, g.iv_p50_ms * s_glass_st.display.refresh_hz / 1000.0, disp,
                 g.uneven, g.lat_p50_ms);
    else
        snprintf(l3, sizeof l3, "on glass %.1f ms (%.1f-%.1f)   %s   uneven %d   lag %.0f ms",
                 g.iv_p50_ms, g.iv_min_ms, g.iv_max_ms, disp, g.uneven, g.lat_p50_ms);
    l4[0] = 0;
    if (s_glass_st.measured && g.warn_rate)
        snprintf(l4, sizeof l4, "! %.4g Hz is not a multiple of 60: uneven frames",
                 s_glass_st.display.refresh_hz);
    else if (g.warn_uneven)
        snprintf(l4, sizeof l4, "! uneven on the glass: %d of %d frames", g.uneven, g.n_iv);
    else if (steps && g.steps_multi)
        snprintf(l4, sizeof l4, "%d frames ran 2+ simulation steps (dots)", g.steps_multi);

    /* The panel is as wide as its longest line: a fixed width left the stats
     * line running past the backdrop at larger values (a 100+ ms max). */
    const float pad = fs * 0.6f, lh = fs * 1.15f;
    if (!have_glass)
        l4[0] = 0;
    const int lines = !have_glass ? 2 : l4[0] ? 4 : 3;
    const float h = fs * 4.1f + lh * lines;
    float tw = ImGui::CalcTextSize(l1).x;
    const char *const ls[4] = { l1, l2, l3, l4 };
    for (i = 1; i < lines; i++) {
        float w2 = ImGui::CalcTextSize(ls[i]).x;
        if (w2 > tw) tw = w2;
    }
    const float w = tw + 2 * pad > fs * 15.0f ? tw + 2 * pad : fs * 15.0f;
    ImVec2 a = origin, b(origin.x + w, origin.y + h);
    dl->AddRectFilled(a, b, solid(T->perf_bg, 200 / 255.0f), fs * 0.5f);
    dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f), solid(T->perf_text), l1);
    dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f + lh), solid(T->perf_text2), l2);
    if (have_glass)
        dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f + lh * 2),
                    solid(g.warn_uneven || g.warn_rate ? T->perf_warn : T->perf_text2), l3);
    if (l4[0])
        dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f + lh * 3),
                    solid(g.warn_uneven || g.warn_rate ? T->perf_bad : T->perf_text2), l4);

    /* Bars: the last 120 flip intervals against a 50 ms ceiling; the thin
     * line across is one frame at 60 Hz. */
    const float gx = a.x + pad, gy1 = b.y - pad, gh = h - lh * lines - fs * 0.7f - pad, gw = w - 2 * pad;
    const int bars = 120;
    const float bw = gw / bars;
    for (i = 0; i < bars && i < m; i++) {
        float v = iv[m - 1 - i];
        float f = v / 50.0f;
        if (f > 1.0f) f = 1.0f;
        ImU32 c = v <= 17.5f ? solid(T->perf_good)
                : v <= 25.0f ? solid(T->perf_warn) : solid(T->perf_bad);
        float x1 = gx + gw - i * bw;
        dl->AddRectFilled(ImVec2(x1 - bw + 0.5f, gy1 - gh * f), ImVec2(x1, gy1), c);
    }
    float ly = gy1 - gh * (16.667f / 50.0f);
    dl->AddLine(ImVec2(gx, ly), ImVec2(gx + gw, ly), solid(T->perf_line, 90 / 255.0f));

    /* The second trace: the on-glass interval of the same frames, a line
     * over the bars on the same scale. Even frames draw it flat on the 60 Hz
     * line; a 1-3-1-3 repeat is a saw the bars do not have. An uneven
     * interval gets a mark. Nothing is drawn when the system gives no
     * on-glass times: a flat line there would say "even". */
    if (s_glass_st.measured && g.available && g.n_iv > 1) {
        ImU32 lc = solid(T->perf_text), bad = solid(T->perf_bad);
        ImVec2 prev(0, 0);
        bool have = false;
        for (i = 0; i < bars && i < g.n_iv; i++) {
            int k = g.n_iv - 1 - i;
            float v = g.iv_ms[k], f = v / 50.0f;
            if (f > 1.0f) f = 1.0f;
            ImVec2 pt(gx + gw - (i + 0.5f) * bw, gy1 - gh * f);
            if (have)
                dl->AddLine(prev, pt, lc, fs * 0.09f);
            if (g.iv_uneven[k])
                dl->AddCircleFilled(pt, fs * 0.16f, bad);
            prev = pt;
            have = true;
        }
    }
    /* The optional third: frames of more than one simulation step, a dot
     * under the bar (newest sample at the right, as the bars). */
    if (steps && g.steps_max) {
        for (i = 0; i < bars && i < s_glass_n; i++) {
            const pc_sample &sm = s_glass_smp[s_glass_n - 1 - i];
            if (sm.steps > 1)
                dl->AddCircleFilled(ImVec2(gx + gw - (i + 0.5f) * bw, gy1 + pad * 0.35f), fs * 0.13f,
                                    solid(T->perf_warn));
        }
    }
}

/* ── the pages ─────────────────────────────────────────────────────── */

static bool pad_hints() { return s_last_input_pad; }

static void draw_row(const Row &r, int idx, bool sel, float width, float rh, float fs, ImDrawList *dl,
                     bool *clicked, int *click_dir, bool *hovered, double *drag_t)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (r.kind == R_INFO)
        rh *= 0.7f;
    ImGui::PushID(idx);
    ImGui::InvisibleButton("##row", ImVec2(width, rh));
    *hovered = ImGui::IsItemHovered();
    *clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImVec2 mp = ImGui::GetIO().MousePos;
    *click_dir = mp.x > p.x + width * 0.78f ? 1 : -1;
    ImGui::PopID();

    if (sel)
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rh), col(T->sel_fill, T->sel_fill_a), fs * 0.35f);
    if (sel)
        dl->AddRectFilled(p, ImVec2(p.x + fs * 0.22f, p.y + rh), col(T->sel_bar), fs * 0.11f);

    const float ty = p.y + (rh - fs) * 0.5f;
    if (r.kind == R_INFO) {
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(T->text_info), r.text);
        return;
    }
    if (r.kind == R_ACTION) {
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(T->action), r.text);
        return;
    }
    if (r.kind == R_FOLDER) {
        char v[64], t[80];
        int locked = 0;
        xbox_SettingsText(MUSIC_KEY, &locked);
        music_value(v, sizeof v);
        snprintf(t, sizeof t, sel && !locked ? "%s  >" : "%s", v);
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty),
                    locked ? col(T->text_dim) : col(sel ? T->text_hi : T->text), r.text);
        float tw = ImGui::CalcTextSize(t).x;
        dl->AddText(ImVec2(p.x + width - fs * 0.8f - tw, ty),
                    locked ? col(T->text_dim) : col(T->text_hi), t);
        return;
    }
    if (r.kind == R_SPEAKERS) {
        /* Six buttons on the right; the one playing lit, the picked one
         * outlined while the row is selected. A click plays its button. */
        int via = 0, playing = mcpx_apu_speaker_test_playing(&via);
        const float bw = fs * 3.0f, gap = fs * 0.35f, bh = fs * 1.4f;
        float bx = p.x + width - fs * 0.8f - 6 * bw - 5 * gap;
        const float by = p.y + (rh - bh) * 0.5f;
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(sel ? T->text_hi : T->text), r.text);
        for (int k = 0; k < 6; k++, bx += bw + gap) {
            bool lit = playing == k && (via != 0) == (r.action == 0);
            ImVec2 a(bx, by), b(bx + bw, by + bh);
            dl->AddRectFilled(a, b, lit ? col(T->fill) : col(T->track), bh * 0.3f);
            if (sel && s_spk_sel[r.action] == k)
                dl->AddRect(a, b, col(T->text_hi), bh * 0.3f, 0, fs * 0.12f);
            float tw = ImGui::CalcTextSize(k_spk_short[k]).x;
            dl->AddText(ImVec2(bx + (bw - tw) * 0.5f, ty), lit ? col(T->on_accent) : col(T->text),
                        k_spk_short[k]);
            if (*clicked && mp.x >= a.x && mp.x < b.x) {
                s_spk_sel[r.action] = k;
                *click_dir = 0;
            }
        }
        return;
    }

    const xs_setting *s = xbox_SettingGet(r.id);
    const Meta &m = k_meta[r.id];
    char v[96], lab[96];
    bool dim = s->locked;
    value_text(r.id, v, sizeof v);
    if (s->flags & XS_RESTART)
        snprintf(lab, sizeof lab, "%s  (restart)", m.label);
    else
        snprintf(lab, sizeof lab, "%s", m.label);
    dl->AddText(ImVec2(p.x + fs * 0.8f, ty), dim ? col(T->text_dim) : col(sel ? T->text_hi : T->text), lab);

    const float vx1 = p.x + width - fs * 0.8f;
    if (s->type == XS_INT || s->type == XS_FLOAT) {
        /* A bar and the number. */
        const float tw = ImGui::CalcTextSize(v).x;
        const float bw = fs * 9.0f, bh = fs * 0.5f;
        const float bx1 = vx1 - fs * 4.4f, bx0 = bx1 - bw, by = p.y + (rh - bh) * 0.5f;
        double t = (s->value - s->min) / (s->max - s->min);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + bh), col(T->track), bh * 0.5f);
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + bw * (float)t, by + bh),
                          dim ? col(T->fill_dim) : col(T->fill), bh * 0.5f);
        dl->AddText(ImVec2(vx1 - tw, ty), dim ? col(T->text_dim) : col(T->text_hi), v);
        if (drag_t && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive()) {
            double f = (ImGui::GetIO().MousePos.x - bx0) / bw;
            *drag_t = f < 0 ? 0 : f > 1 ? 1 : f;
        }
    } else if (s->type == XS_CHOICE) {
        char t[128];
        snprintf(t, sizeof t, sel && !dim ? "<  %s  >" : "%s", v);
        float tw = ImGui::CalcTextSize(t).x;
        dl->AddText(ImVec2(vx1 - tw, ty), dim ? col(T->text_dim) : col(T->text_hi), t);
    } else {
        const float pw = fs * 3.0f, ph = fs * 1.4f;
        const float px = vx1 - pw, py = p.y + (rh - ph) * 0.5f;
        bool on = s->value != 0.0;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph),
                          on && !dim ? col(T->fill) : col(T->track_off), ph * 0.5f);
        float kx = on ? px + pw - ph * 0.5f : px + ph * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, py + ph * 0.5f), ph * 0.38f, col(T->knob));
        const char *txt = on ? "On" : "Off";
        float tw = ImGui::CalcTextSize(txt).x;
        dl->AddText(ImVec2(px - tw - fs * 0.6f, ty), dim ? col(T->text_dim) : col(T->text_hi), txt);
    }
}

/* The folder browser, in the place of a page's rows: where it is, the rows
 * (a place or folder with the music found in and below it), the keys. */
static void build_browser(float fs, ImDrawList *dl)
{
    ImGuiIO &io = ImGui::GetIO();
    int n = xsb_rows();

    collect_steps();
    if (s_fb_sel >= n) s_fb_sel = n - 1;
    if (s_fb_sel < 0) s_fb_sel = 0;
    while (s_steps[D_UP]-- > 0)   { s_fb_sel = (s_fb_sel + n - 1) % (n ? n : 1); s_fb_scroll = true; }
    while (s_steps[D_DOWN]-- > 0) { s_fb_sel = (s_fb_sel + 1) % (n ? n : 1); s_fb_scroll = true; }
    memset(s_steps, 0, sizeof s_steps);
    s_reset = 0;
    s_page_delta = 0;
    if (s_activate > 0) {
        s_activate = 0;
        browser_pick(s_fb_sel);
        if (!xsb_is_open()) {
            /* Chosen: nothing more is drawn this frame, and the header left
             * the cursor moved with no item after it, which ImGui reports. */
            ImGui::Dummy(ImVec2(0, 0));
            return;
        }
        n = xsb_rows();
    }

    /* Where: the folder, cut from the left when it is too long. */
    char where[1024];
    xsb_where(where, sizeof where);
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        const char *t = where[0] ? where : "Custom music folder: choose where to look";
        char cut[1100];
        if (ImGui::CalcTextSize(t).x > wrap_w) {
            const char *q = t;
            do {
                q++;
                snprintf(cut, sizeof cut, "...%s", q);
            } while (*q && ImGui::CalcTextSize(cut).x > wrap_w);
            t = cut;
        }
        dl->AddText(ImVec2(p.x, p.y), col(T->text_hi), t);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + fs * 1.6f));
    }

    const float footer = fs * 0.5f + fs + fs * 2.9f;
    const float rh = fs * 2.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("folders", ImVec2(avail.x, avail.y - footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav);
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x - fs * 0.5f;
    int picked = -1;
    xsb_row r;
    for (int i = 0; xsb_row_at(i, &r); i++) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = i == s_fb_sel;
        ImGui::PushID(i);
        ImGui::InvisibleButton("##dir", ImVec2(rw, rh));
        if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            s_fb_sel = i;
            s_last_input_pad = false;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            picked = i;
        ImGui::PopID();
        if (!ImGui::IsRectVisible(p, ImVec2(p.x + rw, p.y + rh)) && !(sel && s_fb_scroll))
            continue;
        if (sel) {
            cdl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), col(T->sel_fill, T->sel_fill_a), fs * 0.35f);
            cdl->AddRectFilled(p, ImVec2(p.x + fs * 0.22f, p.y + rh), col(T->sel_bar), fs * 0.11f);
        }
        const float ty = p.y + (rh - fs) * 0.5f;
        bool act = r.kind == XSB_USE || r.kind == XSB_OFF;
        char c[48];
        browser_count(r, c, sizeof c);
        float cw = ImGui::CalcTextSize(c).x;
        cdl->PushClipRect(p, ImVec2(p.x + rw - cw - fs * 1.6f, p.y + rh), true);
        cdl->AddText(ImVec2(p.x + fs * 0.8f, ty),
                     act ? col(T->action) : col(sel ? T->text_hi : T->text), r.label);
        cdl->PopClipRect();
        if (c[0])
            cdl->AddText(ImVec2(p.x + rw - fs * 0.8f - cw, ty),
                         r.songs > 0 ? col(T->text_hi) : col(T->text_dim), c);
        if (sel && s_fb_scroll) {
            ImGui::SetScrollHereY(0.5f);
            s_fb_scroll = false;
        }
    }
    ImGui::EndChild();
    if (picked >= 0) {
        s_fb_sel = picked;
        browser_pick(picked);
    }

    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y + fs * 0.2f), ImVec2(p.x + avail.x, p.y + fs * 0.2f), col(T->rule));
        float y = p.y + fs * 0.5f;
        dl->AddText(ImVec2(p.x, y), col(T->text_dim),
                    "Every folder with music in it becomes a soundtrack in the game's own music menu.");
        y += fs + fs * 0.3f + fs * 1.3f;
        const char *keys = pad_hints()
            ? "D-pad  select      A  open / choose      B  back"
            : "Arrows  select      Enter  open / choose      Esc  back";
        dl->AddText(ImVec2(p.x, y), col(T->text_dim), keys);
    }
}

#include "overlay_mixer.inc"

static void build_pages(float fs)
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const ImVec2 disp = io.DisplaySize;

    /* Dim the game. */
    bg->AddRectFilled(ImVec2(0, 0), disp, col(T->scrim, 0.55f));

    float pw = fs * 44.0f, ph = fs * 28.0f;
    if (s_mx_open) {                    /* the Mixer page: a longer list, wider rows */
        pw = fs * 54.0f;
        ph = fs * 38.0f;
    }
    if (pw > disp.x * 0.94f) pw = disp.x * 0.94f;
    if (ph > disp.y * 0.90f) ph = disp.y * 0.90f;
    ImGui::SetNextWindowPos(ImVec2((disp.x - pw) * 0.5f, (disp.y - ph) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs, fs * 0.8f));
    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Header: the pages. */
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        float x = p.x;
        const float th = fs * 1.8f;
        for (int i = 0; i < P_COUNT; i++) {
            float tw = ImGui::CalcTextSize(k_page_name[i]).x + fs * 1.4f;
            ImGui::SetCursorScreenPos(ImVec2(x, p.y));
            ImGui::PushID(i);
            ImGui::InvisibleButton("##tab", ImVec2(tw, th));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                s_page = i;
                s_scroll_to_sel = true;
                s_mx_open = false;
            }
            ImGui::PopID();
            bool on = i == s_page;
            if (on)
                dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + tw, p.y + th), col(T->tab_on), fs * 0.35f);
            dl->AddText(ImVec2(x + fs * 0.7f, p.y + (th - fs) * 0.5f),
                        on ? col(T->tab_on_text) : col(T->tab_text), k_page_name[i]);
            x += tw + fs * 0.25f;
        }
        const char *hint = pad_hints() ? "LB / RB" : "PgUp / PgDn";
        float hw = ImGui::CalcTextSize(hint).x;
        dl->AddText(ImVec2(p.x + pw - fs * 2.0f - hw, p.y + (th - fs) * 0.5f), col(T->tab_hint), hint);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + fs * 0.5f));
    }

    if (xsb_is_open()) {
        build_browser(fs, dl);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    if (s_mx_open) {
        build_mixer(fs, dl);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    /* The rows. */
    Row rows[16];
    int n = page_rows(s_page, rows);
    int &sel = s_sel[s_page];
    if (sel < 0 || sel >= n || !selectable(rows[sel])) {
        sel = 0;
        while (sel < n && !selectable(rows[sel]))
            sel++;
    }

    /* Steps from the pad and keys. */
    collect_steps();
    auto move = [&](int dir) {
        for (int k = 0; k < n; k++) {
            sel = (sel + dir + n) % n;
            if (selectable(rows[sel]))
                break;
        }
        s_scroll_to_sel = true;
    };
    while (s_steps[D_UP]-- > 0) move(-1);
    while (s_steps[D_DOWN]-- > 0) move(1);
    s_steps[D_UP] = s_steps[D_DOWN] = 0;
    if (rows[sel].kind == R_SETTING) {
        while (s_steps[D_LEFT]-- > 0) adjust(rows[sel].id, -1);
        while (s_steps[D_RIGHT]-- > 0) adjust(rows[sel].id, 1);
    } else if (rows[sel].kind == R_SPEAKERS) {
        int &k = s_spk_sel[rows[sel].action];
        while (s_steps[D_LEFT]-- > 0) k = (k + 5) % 6;
        while (s_steps[D_RIGHT]-- > 0) k = (k + 1) % 6;
    }
    s_steps[D_LEFT] = s_steps[D_RIGHT] = 0;
    while (s_activate-- > 0) activate(rows[sel]);
    s_activate = 0;
    while (s_reset-- > 0)
        if (rows[sel].kind == R_SETTING) {
            xbox_SettingReset(rows[sel].id);
            changed();
        }
    s_reset = 0;
    /* The footer: the selected row's hint (wrapped), a restart note, the keys. */
    char hint[1800] = "";
    bool hint_locked = false;
    if (rows[sel].kind == R_SETTING) {
        const xs_setting *s = xbox_SettingGet(rows[sel].id);
        hint_locked = s->locked;
        if (s->locked) {
            if (s->env == RENV_COUNT)
                snprintf(hint, sizeof hint, "Set by the environment (RECOMP_PRESENT_PACING / RECOMP_PRESENT_SPIN_LEAD); unset it to change it here.");
            else
                snprintf(hint, sizeof hint, "Set by the environment (%s); unset it to change it here.", recomp_env_name(s->env));
        } else {
            if (rows[sel].id == XS_POWER_MODE && xbox_PowerStockHidden(xbox_SettingsHost()))
                snprintf(hint, sizeof hint, "Balanced sleeps and spins only the last 8 ms before each frame (steady"
                         " 60, recommended). Battery saver only sleeps (coolest, an occasional small dip)."
                         " The busy-wait mode is not offered here: it measured no steadier and costs more"
                         " power (diagnosis: RECOMP_PRESENT_PACING=spin).");
            else
                snprintf(hint, sizeof hint, "%s", k_meta[rows[sel].id].hint);
            if (rows[sel].id == XS_GRAPHICS_UI_SCALE || rows[sel].id == XS_GRAPHICS_UI_SCALE_FILTER) {
                /* The scaled pictures are made in the background and kept on
                 * disk: say so while that is going on. */
                unsigned done = 0, total = 0;
                nv2a_texscale_progress(&done, &total);
                if (xbox_SettingValue(XS_GRAPHICS_UI_SCALE) != 0 && done < total) {
                    char whole[sizeof hint];
                    snprintf(whole, sizeof whole, "Preparing sharper menus and HUD... %u of %u. %s",
                             done, total, hint);
                    snprintf(hint, sizeof hint, "%s", whole);
                }
            }
        }
    } else if (rows[sel].kind == R_SPEAKERS) {
        spk_hint(rows[sel].action, hint, sizeof hint);
    } else if (rows[sel].kind == R_FOLDER) {
        int locked = 0;
        xbox_SettingsText(MUSIC_KEY, &locked);
        hint_locked = locked != 0;
        if (locked)
            snprintf(hint, sizeof hint, "Set by the environment (RECOMP_AUDIO_MUSIC_DIR or --music-dir);"
                     " unset it to change it here.");
        else
#if defined(__ANDROID__)
            /* The app does not ask for the audio permission yet (the game
             * repository's docs/custom-music.md, "Android"), so there is
             * nothing the player could grant: say that it is not there. */
            snprintf(hint, sizeof hint, "Your own music in the game's music menu is not available on"
                     " Android yet: this app cannot read the device's music folders. It works in"
                     " the Windows, macOS and Linux versions.");
#else
            snprintf(hint, sizeof hint, "A folder of your own music (mp3, flac, ogg, wav) for the game's"
                     " own music menu: every folder with music in it becomes a soundtrack there."
                     " A opens a folder list.");
#endif
    } else if (rows[sel].kind == R_ACTION
               && (rows[sel].action == A_REPORT || rows[sel].action == A_REPORT_SHOT)) {
        snprintf(hint, sizeof hint, "%s%sA zip in the reports folder beside the settings: the logs of"
                 " this run and the last ones, crash reports, the settings, a system summary%s."
                 " No saves, no game data; your home folder is written ~. Attach it to an issue.",
                 s_rep_done, s_rep_done[0] ? ". " : "",
                 rows[sel].action == A_REPORT_SHOT ? ", the frame behind this menu" : "");
    } else if (rows[sel].kind == R_ACTION && rows[sel].action == A_SHADERS && s_sb && s_sb->cost) {
        snprintf(hint, sizeof hint, "%s Paused meanwhile; closing the settings stops it"
                 " (it goes on from there next time).", s_sb->cost);
    } else if (rows[sel].kind == R_ACTION && rows[sel].action == A_SHADERS_LATER) {
        snprintf(hint, sizeof hint, "Asks again at the next start. It is also here, on the Graphics page, at any time.");
    } else if (rows[sel].kind == R_ACTION && rows[sel].action == A_SHADERS_NEVER) {
        snprintf(hint, sizeof hint, "No question at start any more. \"Build shaders now\" stays here on the Graphics page.");
    }
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float hint_h = hint[0] ? ImGui::CalcTextSize(hint, nullptr, false, wrap_w).y : fs;
    const float footer = fs * 0.5f + hint_h + fs * 2.9f;
    const float rh = fs * 2.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("rows", ImVec2(avail.x, avail.y - footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav);
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x - fs * 0.5f;
    for (int i = 0; i < n; i++) {
        bool clicked, hov;
        int cdir;
        double drag = -1;
        ImVec2 y0 = ImGui::GetCursorScreenPos();
        draw_row(rows[i], i, i == sel, rw, rh, fs, cdl, &clicked, &cdir, &hov, &drag);
        if (hov && selectable(rows[i]) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            sel = i;
            s_last_input_pad = false;
        }
        if (rows[i].kind == R_SETTING) {
            const xs_setting *s = xbox_SettingGet(rows[i].id);
            if (drag >= 0 && !s->locked) {
                double v = s->min + drag * (s->max - s->min);
                if (s->type == XS_INT)
                    v = floor(v + 0.5);
                else
                    v = floor(v / k_meta[rows[i].id].step + 0.5) * k_meta[rows[i].id].step;
                sel = i;
                xbox_SettingSet(rows[i].id, v);
                changed();
            } else if (clicked && s->type != XS_INT && s->type != XS_FLOAT) {
                sel = i;
                if (s->type == XS_BOOL)
                    activate(rows[i]);
                else
                    adjust(rows[i].id, cdir);
            } else if (clicked) {
                sel = i;
            }
        } else if (clicked && (rows[i].kind == R_ACTION || rows[i].kind == R_FOLDER)) {
            sel = i;
            activate(rows[i]);
        } else if (clicked && rows[i].kind == R_SPEAKERS) {
            sel = i;
            if (cdir == 0)              /* on a button */
                activate(rows[i]);
        }
        if (i == sel && s_scroll_to_sel) {
            ImGui::SetScrollHereY(0.5f);
            s_scroll_to_sel = false;
        }
        (void)y0;
    }
    ImGui::EndChild();

    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y + fs * 0.2f), ImVec2(p.x + avail.x, p.y + fs * 0.2f), col(T->rule));
        float y = p.y + fs * 0.5f;
        if (hint[0])
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x, y), hint_locked ? col(T->warn) : col(T->text_dim),
                        hint, nullptr, wrap_w);
        y += hint_h + fs * 0.3f;
        if (xbox_SettingsNeedRestart())
            dl->AddText(ImVec2(p.x, y), col(T->warn), "Restart the game for the changes marked (restart) to apply.");
        y += fs * 1.3f;
        const char *keys = pad_hints()
            ? "D-pad  select / change      A  toggle      Y  default      B  close"
            : "Arrows  select / change      Enter  toggle      Del  default      Esc  close";
        dl->AddText(ImVec2(p.x, y), col(T->text_dim), keys);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    /* A page change shows from the next frame. */
    if (s_page_delta) {
        s_page = (s_page + (s_page_delta > 0 ? 1 : -1) + P_COUNT) % P_COUNT;
        s_page_delta = 0;
        s_scroll_to_sel = true;
    }
}

bool overlay_begin_frame()
{
    if (!s_core_ready)
        return false;
    uint32_t now = now_ms();
    float dt = (float)(now - s_last_ms) / 1000.0f;
    s_last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    s_alpha += (s_open ? 1.0f : -1.0f) * dt / 0.14f;
    s_alpha = s_alpha < 0 ? 0 : s_alpha > 1 ? 1 : s_alpha;
    bool perf = xbox_SettingValue(XS_ADVANCED_PERF) != 0.0;
    bool note = notice_on(), scripted = scripted_on();
    if (!s_open && s_alpha <= 0.0f && !perf && !note && !scripted)
        return false;

    ImGuiIO &io = ImGui::GetIO();
    /* One text size for the window: 17 points at 720 high, up to 2.2x. */
    float scale = io.DisplaySize.y / 720.0f;
    scale = scale < 1.0f ? 1.0f : scale > 2.2f ? 2.2f : scale;
    ImGui::GetStyle().FontSizeBase = 17.0f * scale;

    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    const float fs = ImGui::GetFontSize();

    if (perf) {
        draw_perf(ImGui::GetForegroundDrawList(), ImVec2(fs * 0.6f, fs * 0.6f), fs);
    }
    if (note) {
        /* Centred, low on the screen; the dots move so it is seen to live. */
        static const char *const dots[4] = { "", ".", "..", "..." };
        char text[64];
        snprintf(text, sizeof text, "Preparing shaders%s", dots[(now / 300u) & 3u]);
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float tw = ImGui::CalcTextSize("Preparing shaders...").x;
        const ImVec2 p((disp.x - tw) * 0.5f, disp.y * 0.72f);
        dl->AddRectFilled(ImVec2(p.x - fs, p.y - fs * 0.5f), ImVec2(p.x + tw + fs, p.y + fs * 1.5f),
                          solid(T->toast_bg, 235 / 255.0f), fs * 0.5f);
        dl->AddText(p, solid(T->toast_text), text);
    }
    if (scripted) {
        /* Top centre, out of the HUD's way. */
        static const char text[] = "Test script is driving the controller - press a button to take over";
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float tw = ImGui::CalcTextSize(text).x;
        const ImVec2 p((disp.x - tw) * 0.5f, fs * 1.2f);
        dl->AddRectFilled(ImVec2(p.x - fs, p.y - fs * 0.5f), ImVec2(p.x + tw + fs, p.y + fs * 1.5f),
                          solid(T->notice_bg, 235 / 255.0f), fs * 0.5f);
        dl->AddText(p, solid(T->notice_text), text);
    }
    if (s_open || s_alpha > 0.0f) {
        /* A closing overlay still draws, fading. */
        build_pages(fs);
    }
    return true;
}

ImDrawData *overlay_end_frame()
{
    ImGui::Render();
    return ImGui::GetDrawData();
}
