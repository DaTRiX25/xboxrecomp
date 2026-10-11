/*
 * overlay.h - the in-game settings overlay (Dear ImGui), as the window's
 * presenter sees it.
 *
 * The presenter (src/video/fb_present_sdl.c and its Metal and Vulkan halves)
 * owns the window, its one event loop and the present, all on the process
 * main thread, and so does the overlay: ImGui is used from that thread only.
 *
 *   events    every SDL event of the main loop goes to xbox_OverlayEvent
 *             first. It watches for the hotkeys (Back+Start on a pad, F1,
 *             Cmd+, on macOS); Esc or B closes. While it is open the title
 *             reads no pad or key (xbox_InputSetBlocked) and is paused
 *             (xbox_HostPause); see overlay_ui.cpp.
 *   drawing   after the game's image is in the drawable and before it is
 *             presented, the presenter calls its backend's draw
 *             (overlay_metal.h, overlay_vulkan.h), when xbox_OverlayWants.
 *   backends  one source file per graphics API, each a few calls around the
 *             official imgui_impl_* renderer; D3D11 adds one more
 *             (overlay_internal.h lists what a backend provides).
 *
 * RECOMP_OVERLAY=0 turns the whole thing off; =open starts it open (for
 * screenshots). The settings it edits are xbox_settings.h.
 */
#ifndef XBOX_OVERLAY_H
#define XBOX_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once, after the window exists. 0 when the overlay is off. */
int  xbox_OverlaySetWindow(SDL_Window *win);

/* Every event of the main loop, before anything else uses it. Returns 1 when
 * the overlay is open and took it. */
int  xbox_OverlayEvent(const SDL_Event *ev);

/* Once per turn of the main loop: held directions repeat, a saved change
 * reaches the file, the window follows the fullscreen setting. */
void xbox_OverlayPoll(void);

/* Open, fading, or showing the frame-time graph: the presenter draws it, and
 * while it is open (xbox_OverlayOpen) it presents on every turn, not only on
 * a new game frame. */
int  xbox_OverlayWants(void);
int  xbox_OverlayOpen(void);
/* A line of text is up without the overlay being open ("Preparing shaders":
 * the render thread waits for the start-up pre-warm, nv2a_pcache.h, and no
 * game frame comes meanwhile): the presenter presents on every turn so that
 * it shows, over the last frame or over black. */
int  xbox_OverlayNotice(void);

/* The window showed a new frame (the graph counts presents). */
void xbox_OverlayFramePresented(void);

/* The loop is ending: the title is let go, the settings written. */
void xbox_OverlayShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_OVERLAY_H */
