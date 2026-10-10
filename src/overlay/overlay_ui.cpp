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

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"

#include "input_backend.h"
#include "kernel_pause.h"
#include "apu.h"                /* the speaker test */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static const char *const n_fit[]       = { "Letterbox", "Fill 16:10 (crops the sides)", NULL };
static const char *const n_aniso[]     = { "Off", "2x", "4x", "8x", "16x", NULL };
static const char *const n_aa[]        = { "Off", "FXAA", "SMAA (coming soon)", NULL };
static const char *const n_speakers[]  = { "Auto", "Stereo", "5.1", NULL };
static const char *const n_power[]     = { "Maximum (~14 W on Deck)", "Balanced (~10 W, recommended)", "Battery saver (~8 W)", NULL };

static const Meta k_meta[XS_COUNT] = {
    /* XS_DISPLAY_FULLSCREEN */ { "Fullscreen", "Borderless fullscreen on the desktop.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_DISPLAY_ASPECT */     { "Aspect ratio", "16:9 makes the game draw widescreen (it reads this at start).", 1, FMT_PLAIN, n_aspect, 0 },
    /* XS_DISPLAY_FILTER */     { "Filter", "How the picture is scaled to the window.", 1, FMT_PLAIN, n_filter, 0 },
    /* XS_DISPLAY_OVERSCAN */   { "Overscan", "Crop the border a TV would have hidden.", 1, FMT_PLAIN, n_overscan, 0 },
    /* XS_DISPLAY_FIT */        { "Present fit", "Fill the window's height on a 16:10 screen.", 1, FMT_PLAIN, n_fit, 0 },
    /* XS_GRAPHICS_RENDER_SCALE */ { "Render scale", "Internal resolution, a multiple of the Xbox's.", 1, FMT_TIMES, NULL, 0 },
    /* XS_GRAPHICS_ANISO */    { "Anisotropic filtering", "Sharper textures at a slant, the road ahead. Costs GPU time.", 1, FMT_PLAIN, n_aniso, 0 },
    /* XS_GRAPHICS_AA */       { "Anti-aliasing", "Smooths jagged edges after the frame is drawn. FXAA is one cheap pass.", 1, FMT_PLAIN, n_aa, 1u << 2 },
    /* XS_GRAPHICS_SHARPEN */  { "Sharpening", "Contrast-adaptive sharpening (AMD CAS), against the softness of the anti-aliasing and the scale-up.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_VOLUME */       { "Master volume", "The game's sound level.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_AUDIO_MUTE */         { "Mute", "No sound.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_MUTE_BG */      { "Mute in the background", "No sound while the window is not in front.", 1, FMT_PLAIN, NULL, 0 },
    /* XS_AUDIO_SPEAKERS */     { "Speakers", "Auto plays 5.1 when the system's sound output is set to 5.1 (or more), else stereo. The game picks its mix at start.", 1, FMT_PLAIN, n_speakers, 0 },
    /* XS_AUDIO_FX_LEVEL */     { "Surround effects (reverb) level", "5.1 only: the level of the game's effect sends, which play dry here (no effect DSP runs). Tune by ear.", 0.05, FMT_PERCENT, NULL, 0 },
    /* XS_INPUT_DEADZONE */     { "Stick deadzone", "The part of the stick's travel that reads as centre.", 1638.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_TRIGGER_DEADZONE */ { "Trigger deadzone", "The part of a trigger's pull that reads as released.", 13.0, FMT_PERCENT_OF_MAX, NULL, 0 },
    /* XS_INPUT_RUMBLE */       { "Rumble strength", "0 turns the rumble off.", 0.05, FMT_GAIN, NULL, 0 },
    /* XS_POWER_MODE */         { "Power mode", "Maximum busy-waits all the time (steadiest, hottest). Balanced sleeps and spins only the last 8 ms before each frame (steady 60). Battery saver only sleeps (coolest, an occasional small dip). Watts measured on a Steam Deck.", 1, FMT_PLAIN, n_power, 0 },
    /* XS_ADVANCED_PERF */      { "Performance overlay", "FPS and frame-time graph in the corner. Greetings to the DF team!", 1, FMT_PLAIN, NULL, 0 },
};

/* R_SPEAKERS: the speaker test, six buttons in a row (action: 0 through
 * the game's mixbins and speaker map, 1 straight to the output). */
enum RowKind { R_SETTING, R_ACTION, R_INFO, R_SPEAKERS };
enum Action { A_RESET_PAGE, A_RESET_ALL };

struct Row {
    RowKind kind;
    xs_id id;                         /* R_SETTING */
    int action;                       /* R_ACTION */
    const char *text;                 /* R_ACTION label, R_INFO line */
};

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
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_GRAPHICS:
        SET(XS_GRAPHICS_RENDER_SCALE); SET(XS_GRAPHICS_ANISO);
        SET(XS_GRAPHICS_AA); SET(XS_GRAPHICS_SHARPEN);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_AUDIO:
        SET(XS_AUDIO_VOLUME); SET(XS_AUDIO_MUTE); SET(XS_AUDIO_MUTE_BG); SET(XS_AUDIO_SPEAKERS);
        SET(XS_AUDIO_FX_LEVEL);
        SPK(0, "Speaker test, game mix");
        SPK(1, "Speaker test, direct");
        if (mcpx_apu_title_channels() != 6)
            INFO("The output is stereo: each speaker is folded to left/right.");
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_INPUT:
        SET(XS_INPUT_DEADZONE); SET(XS_INPUT_TRIGGER_DEADZONE); SET(XS_INPUT_RUMBLE);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    case P_POWER:
#if !defined(__linux__)
        INFO("Tuned on the Steam Deck (Linux). Here the default is Stock, the");
        INFO("game's own busy-wait; the other two sleep between frames.");
#endif
        SET(XS_POWER_MODE);
        ACT(A_RESET_PAGE, "Reset this page to the defaults");
        break;
    default:
        SET(XS_ADVANCED_PERF);
        ACT(A_RESET_ALL, "Reset all settings to the defaults");
        break;
    }
#undef SET
#undef ACT
#undef INFO
#undef SPK
    return n;
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
        snprintf(out, n, "%s", m.names ? m.names[(int)v] : s->choices[(int)v]);
        break;
    default:
        switch (m.fmt) {
        case FMT_PERCENT:        snprintf(out, n, "%d%%", (int)floor(v * 100.0 + 0.5)); break;
        case FMT_PERCENT_OF_MAX: snprintf(out, n, "%d%%", (int)floor(v / s->max * 100.0 + 0.5)); break;
        case FMT_TIMES:          snprintf(out, n, "%dx", (int)v); break;
        case FMT_GAIN:           snprintf(out, n, v <= 0.0 ? "Off" : "%.2fx", v); break;
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
            if (!(m.disabled & (1u << i)))
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
static void activate(const Row &r)
{
    if (r.kind == R_SPEAKERS) {
        spk_play(r.action);
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

static void set_open(bool on)
{
    if (on == s_open)
        return;
    s_open = on;
    clear_nav();
    if (on) {
        s_scroll_to_sel = true;
        xbox_InputSetBlocked(1);
        xbox_HostPause(1);
        fprintf(stderr, LOGP "open: the title is paused, its input is held\n");
    } else {
        mcpx_apu_speaker_test(-1, 0);
        xbox_HostPause(0);
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
        if (!s_open)
            break;
        s_last_input_pad = true;
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

void xbox_OverlayPoll(void)
{
    if (!s_enabled)
        return;

    while (s_toggle_i < s_toggle_n && (int32_t)(now_ms() - s_t0_ms - s_toggle_ms[s_toggle_i]) >= 0) {
        s_toggle_i++;
        hot_toggle();
    }

    if (s_trigger[0] && (int32_t)(now_ms() - s_trigger_next) >= 0) {
        s_trigger_next = now_ms() + 200;
        if (unlink(s_trigger) == 0)
            hot_toggle();
    }

    /* B or Esc: here and not in the frame, so a window that cannot present
     * (minimised) still closes. */
    if (s_back > 0) {
        s_back = 0;
        if (s_open)
            set_open(false);
    }

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

int xbox_OverlayWants(void)
{
    return s_enabled && (s_open || s_alpha > 0.0f || xbox_SettingValue(XS_ADVANCED_PERF) != 0.0);
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

/* ── drawing ───────────────────────────────────────────────────────── */

static ImU32 col(int r, int g, int b, float a = 1.0f)
{
    return IM_COL32(r, g, b, (int)(a * s_alpha * 255.0f));
}

static void accent_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f;
    st.WindowBorderSize = 0.0f;
    st.ScrollbarRounding = 6.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.96f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
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
        double sum = 0;
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
        (void)sum;
    }
    if (now - pres_ms >= 1000) {
        uint32_t p = s_presents;
        pres_rate = (float)(p - pres_prev) * 1000.0f / (float)(now - pres_ms);
        pres_prev = p;
        pres_ms = now;
    }

    const float w = fs * 15.0f, h = fs * 6.4f, pad = fs * 0.6f;
    ImVec2 a = origin, b(origin.x + w, origin.y + h);
    dl->AddRectFilled(a, b, IM_COL32(8, 8, 10, 200), fs * 0.5f);
    char buf[128];
    snprintf(buf, sizeof buf, "%.1f fps   %.1f ms", fps, last);
    dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f), IM_COL32(255, 255, 255, 255), buf);
    snprintf(buf, sizeof buf, "p50 %.1f  p99 %.1f  max %.1f ms   window %.0f/s", p50, p99, mx, pres_rate);
    dl->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f + fs * 1.15f), IM_COL32(190, 190, 195, 255), buf);

    /* Bars: the last 120 flip intervals against a 50 ms ceiling; the line
     * is one frame at 60 Hz. */
    const float gx = a.x + pad, gy1 = b.y - pad, gh = h - fs * 3.0f - pad, gw = w - 2 * pad;
    const int bars = 120;
    const float bw = gw / bars;
    for (i = 0; i < bars && i < m; i++) {
        float v = iv[m - 1 - i];
        float f = v / 50.0f;
        if (f > 1.0f) f = 1.0f;
        ImU32 c = v <= 17.5f ? IM_COL32(90, 210, 120, 255)
                : v <= 25.0f ? IM_COL32(235, 200, 70, 255) : IM_COL32(235, 90, 80, 255);
        float x1 = gx + gw - i * bw;
        dl->AddRectFilled(ImVec2(x1 - bw + 0.5f, gy1 - gh * f), ImVec2(x1, gy1), c);
    }
    float ly = gy1 - gh * (16.667f / 50.0f);
    dl->AddLine(ImVec2(gx, ly), ImVec2(gx + gw, ly), IM_COL32(255, 255, 255, 90));
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
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rh), col(255, 120, 20, 0.22f), fs * 0.35f);
    if (sel)
        dl->AddRectFilled(p, ImVec2(p.x + fs * 0.22f, p.y + rh), col(255, 140, 30), fs * 0.11f);

    const float ty = p.y + (rh - fs) * 0.5f;
    if (r.kind == R_INFO) {
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(160, 160, 168), r.text);
        return;
    }
    if (r.kind == R_ACTION) {
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(255, 190, 120), r.text);
        return;
    }
    if (r.kind == R_SPEAKERS) {
        /* Six buttons on the right; the one playing lit, the picked one
         * outlined while the row is selected. A click plays its button. */
        int via = 0, playing = mcpx_apu_speaker_test_playing(&via);
        const float bw = fs * 3.0f, gap = fs * 0.35f, bh = fs * 1.4f;
        float bx = p.x + width - fs * 0.8f - 6 * bw - 5 * gap;
        const float by = p.y + (rh - bh) * 0.5f;
        dl->AddText(ImVec2(p.x + fs * 0.8f, ty), col(240, 240, 244), r.text);
        for (int k = 0; k < 6; k++, bx += bw + gap) {
            bool lit = playing == k && (via != 0) == (r.action == 0);
            ImVec2 a(bx, by), b(bx + bw, by + bh);
            dl->AddRectFilled(a, b, lit ? col(255, 140, 30) : col(60, 60, 66), bh * 0.3f);
            if (sel && s_spk_sel[r.action] == k)
                dl->AddRect(a, b, col(255, 255, 255), bh * 0.3f, 0, fs * 0.12f);
            float tw = ImGui::CalcTextSize(k_spk_short[k]).x;
            dl->AddText(ImVec2(bx + (bw - tw) * 0.5f, ty), lit ? col(20, 14, 8) : col(240, 240, 244),
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
    dl->AddText(ImVec2(p.x + fs * 0.8f, ty), dim ? col(130, 130, 136) : col(240, 240, 244), lab);

    const float vx1 = p.x + width - fs * 0.8f;
    if (s->type == XS_INT || s->type == XS_FLOAT) {
        /* A bar and the number. */
        const float tw = ImGui::CalcTextSize(v).x;
        const float bw = fs * 9.0f, bh = fs * 0.5f;
        const float bx1 = vx1 - fs * 4.4f, bx0 = bx1 - bw, by = p.y + (rh - bh) * 0.5f;
        double t = (s->value - s->min) / (s->max - s->min);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + bh), col(60, 60, 66), bh * 0.5f);
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + bw * (float)t, by + bh),
                          dim ? col(110, 110, 116) : col(255, 140, 30), bh * 0.5f);
        dl->AddText(ImVec2(vx1 - tw, ty), dim ? col(130, 130, 136) : col(255, 255, 255), v);
        if (drag_t && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive()) {
            double f = (ImGui::GetIO().MousePos.x - bx0) / bw;
            *drag_t = f < 0 ? 0 : f > 1 ? 1 : f;
        }
    } else if (s->type == XS_CHOICE) {
        char t[128];
        snprintf(t, sizeof t, sel && !dim ? "<  %s  >" : "%s", v);
        float tw = ImGui::CalcTextSize(t).x;
        dl->AddText(ImVec2(vx1 - tw, ty), dim ? col(130, 130, 136) : col(255, 255, 255), t);
    } else {
        const float pw = fs * 3.0f, ph = fs * 1.4f;
        const float px = vx1 - pw, py = p.y + (rh - ph) * 0.5f;
        bool on = s->value != 0.0;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph),
                          on && !dim ? col(255, 140, 30) : col(70, 70, 76), ph * 0.5f);
        float kx = on ? px + pw - ph * 0.5f : px + ph * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, py + ph * 0.5f), ph * 0.38f, col(250, 250, 252));
        const char *txt = on ? "On" : "Off";
        float tw = ImGui::CalcTextSize(txt).x;
        dl->AddText(ImVec2(px - tw - fs * 0.6f, ty), dim ? col(130, 130, 136) : col(255, 255, 255), txt);
    }
}

static void build_pages(float fs)
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const ImVec2 disp = io.DisplaySize;

    /* Dim the game. */
    bg->AddRectFilled(ImVec2(0, 0), disp, col(0, 0, 0, 0.55f));

    float pw = fs * 44.0f, ph = fs * 28.0f;
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
            }
            ImGui::PopID();
            bool on = i == s_page;
            if (on)
                dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + tw, p.y + th), col(255, 140, 30), fs * 0.35f);
            dl->AddText(ImVec2(x + fs * 0.7f, p.y + (th - fs) * 0.5f),
                        on ? col(20, 14, 8) : col(190, 190, 198), k_page_name[i]);
            x += tw + fs * 0.25f;
        }
        const char *hint = pad_hints() ? "LB / RB" : "PgUp / PgDn";
        float hw = ImGui::CalcTextSize(hint).x;
        dl->AddText(ImVec2(p.x + pw - fs * 2.0f - hw, p.y + (th - fs) * 0.5f), col(120, 120, 128), hint);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + fs * 0.5f));
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
    char hint[320] = "";
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
            snprintf(hint, sizeof hint, "%s", k_meta[rows[sel].id].hint);
        }
    } else if (rows[sel].kind == R_SPEAKERS) {
        spk_hint(rows[sel].action, hint, sizeof hint);
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
        } else if (clicked && rows[i].kind == R_ACTION) {
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
        dl->AddLine(ImVec2(p.x, p.y + fs * 0.2f), ImVec2(p.x + avail.x, p.y + fs * 0.2f), col(255, 255, 255, 0.14f));
        float y = p.y + fs * 0.5f;
        if (hint[0])
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x, y), hint_locked ? col(240, 170, 90) : col(180, 180, 188),
                        hint, nullptr, wrap_w);
        y += hint_h + fs * 0.3f;
        if (xbox_SettingsNeedRestart())
            dl->AddText(ImVec2(p.x, y), col(240, 200, 90), "Restart the game for the changes marked (restart) to apply.");
        y += fs * 1.3f;
        const char *keys = pad_hints()
            ? "D-pad  select / change      A  toggle      Y  default      B  close"
            : "Arrows  select / change      Enter  toggle      Del  default      Esc  close";
        dl->AddText(ImVec2(p.x, y), col(110, 110, 118), keys);
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
    if (!s_open && s_alpha <= 0.0f && !perf)
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
