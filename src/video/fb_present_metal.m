/**
 * The SDL window's CAMetalLayer present (macOS): the zero-copy half of
 * fb_present_sdl.c, used when the Metal backend draws (RECOMP_PB_BACKEND=metal,
 * not RECOMP_DEBUG=metal_present=readback).
 *
 * The window has no SDL_Renderer then. fb_present_sdl.c keeps the window, the
 * event loop and the three-slot hand-off; this file owns what the slots point
 * at and how a slot reaches the screen:
 *
 *   device, queue  made here at window creation, before the guest starts; the
 *                  Metal backend adopts both at its lazy init
 *                  (xbox_FramebufferWindowMetal), so a slot texture takes the
 *                  backend's blit and a present pass on the same queue runs
 *                  after the blit it shows (commit order, tracked hazards).
 *   slots          three BGRA8 textures, frame-sized (render.scale's host
 *                  size). The backend blits its present target into the back
 *                  slot on the GPU; a guest-bytes frame is uploaded into it.
 *   present        on the main thread: the layer's next drawable, cleared to
 *                  black, the slot drawn into nv2a_present_rect's rectangle
 *                  with one full-screen triangle, nearest or linear as the
 *                  SDL path filters (and as D3D11's scaling blit does).
 *   shot           RECOMP_WINDOW_SHOT: the drawable read back after the pass,
 *                  framebufferOnly being off only when shots are asked for.
 *
 * Manual retain/release, like nv2a_pb_metal.m. Every entry point is wrapped in
 * an autorelease pool: the main loop and the ack thread have none of their own.
 */
#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend_common.h"
#include "platform/host_time.h"
#include "recomp_env.h"
#include "fb_present_metal.h"
#include "latency_trace.h"
#include "post_shaders.h"
#include "smaa.h"
#if defined(XBOXRECOMP_HAVE_OVERLAY)
#include "overlay.h"
#include "overlay_metal.h"
#endif
#include "host_time.h"
#include <mach/mach_time.h>

#include "kernel/kernel_pacing.h"
#include "kernel/present_cadence.h"
#include <os/lock.h>
#include <stdatomic.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#if __has_feature(objc_arc)
#error "fb_present_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

static id<MTLDevice>               s_dev;
/* BO-110 (RECOMP_DEBUG=hdr): an EDR layer, RGBA16Float in extended sRGB, so
 * the game's values pass 1.0 as they are (gamma-encoded, as the title blends
 * them); the slots and the post passes are half float too. Everything at or
 * below 1.0 is written as it is: SDR content keeps its look at the system's
 * paper white. fs_hdr_comp puts the scene's light (the frame where it
 * equals the backend's scene snapshot) through a shoulder towards the peak
 * and the HUD at its own white, live from the settings. */
static int                         s_hdr_win;    /* the layer is EDR now */
static id<MTLRenderPipelineState>  s_hp_blit, s_hp_fxaa, s_hp_cas, s_hp_comp;   /* half-float variants */
static id<MTLTexture>              s_scene[3];   /* per slot: the scene snapshot (nv2a_pb_metal.m) */
static int                         s_scene_ok[3];
static id<MTLTexture>              s_hp_c;       /* the composite, frame size */
#define PP_FMT (s_hdr_win ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm)
static id<MTLCommandQueue>         s_queue;
static id<MTLRenderPipelineState>  s_pso;
static id<MTLRenderPipelineState>  s_pso_fxaa, s_pso_cas;   /* post passes */
static id<MTLTexture>              s_pp[2];                  /* their targets */
/* SMAA (smaa.h): made at the first use, its textures only while it is on. */
static id<MTLRenderPipelineState>  s_pso_sm[3];              /* edges, weights, blend */
static int                         s_smaa_state;             /* 0 untried, 1 ready, -1 failed */
static id<MTLTexture>              s_sm_tex[2];              /* edges RG8, weights RGBA8 */
static id<MTLTexture>              s_sm_lut[2];              /* area RG8, search R8 */
static id<MTLSamplerState>         s_smp[2];   /* nearest, linear */
static SDL_MetalView               s_view;
static CAMetalLayer               *s_layer;
static int                         s_unified;
static id<MTLTexture>              s_slot[3];
/* The last present pass that sampled each slot: an upload (a CPU write) into
 * a slot waits for it; a GPU blit needs no wait (tracked hazards). Written by
 * the main thread while the slot is front, read by the ack thread once the
 * hand-off's lock has made it back. */
static id<MTLCommandBuffer>        s_slot_cb[3];
static uint32_t                   *s_conv;     /* R5G6B5 -> BGRA8 rows */
static size_t                      s_conv_cap;

/* ---- the display's clock ------------------------------------------------
 *
 * RECOMP_VBLANK_SYNC=display: a CVDisplayLink on the window's display
 * reports every refresh to the vblank timer (kernel_pacing.c), which then
 * keeps the emulated vblank in phase with the display. RECOMP_TRACE=
 * pacing=all: each drawable's presentedTime, the time it reached the screen,
 * is logged as a host present. Both are mach_absolute_time stamps, moved
 * onto xbox_HostNowNs's clock by the offset between the two read now. */
static CVDisplayLinkRef s_link;
static int s_trace_presented;

static uint64_t mach_to_host_ns(uint64_t mach_ns)
{
    static mach_timebase_info_data_t tb;
    uint64_t host = xbox_HostNowNs(), mnow;
    if (!tb.denom)
        mach_timebase_info(&tb);
    mnow = mach_absolute_time() * tb.numer / tb.denom;
    return mnow >= mach_ns ? host - (mnow - mach_ns) : host + (mach_ns - mnow);
}

static uint64_t mach_ticks_ns(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *out, CVOptionFlags in_flags,
                        CVOptionFlags *out_flags, void *ctx)
{
    uint64_t period = 0;
    (void)link; (void)in_flags; (void)out_flags; (void)ctx;
    /* The output stamp: the refresh the next frame will be shown at, on
     * the display's grid (inNow is only when the callback ran). Its period
     * from the stamp's video time scale. */
    (void)now;
    if ((out->flags & kCVTimeStampVideoRefreshPeriodValid) && out->videoTimeScale)
        period = (uint64_t)out->videoRefreshPeriod * 1000000000ull
                 / (uint64_t)out->videoTimeScale;
    if (out->flags & kCVTimeStampHostTimeValid)
        xbox_VblankDisplayRefresh(mach_to_host_ns(mach_ticks_ns(out->hostTime)), period);
    return kCVReturnSuccess;
}

/* The display the window is on (its NSScreen's number), else the main one
 * (win NULL: a headless run, for measuring the lock without a window). */
void fbm_vblank_link(SDL_Window *win)
{
    CGDirectDisplayID id = CGMainDisplayID();
    SDL_SysWMinfo wm;
    if (s_link)
        return;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA) {
        NSScreen *scr = [wm.info.cocoa.window screen];
        NSNumber *n = scr ? [[scr deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
        if (n)
            id = (CGDirectDisplayID)[n unsignedIntValue];
    }
    if (CVDisplayLinkCreateWithCGDisplay(id, &s_link) != kCVReturnSuccess) {
        s_link = NULL;
        fprintf(stderr, "[PRESENT] vblank sync: no CVDisplayLink; the vblank keeps"
                " its own 60 Hz timer\n");
        return;
    }
    CVDisplayLinkSetOutputCallback(s_link, link_cb, NULL);
    CVDisplayLinkStart(s_link);
    fprintf(stderr, "[PRESENT] vblank sync: the emulated vblank follows display %u"
            " (CVDisplayLink, RECOMP_VBLANK_SYNC=display)\n", (unsigned)id);
}
#pragma clang diagnostic pop

/* ---- present timing (present_cadence.h, BO-115) ----------------------------
 *
 * display.present_timing = legacy: the layer has two drawables, a frame is
 * presented as soon as it is picked up (presentDrawable:), and nextDrawable
 * waits when both drawables are in use. Measured in a window on a 120 Hz
 * display that wait is a whole frame once it starts (a drawable is free
 * again only when the next one replaces it on the glass), and the frame is
 * on the glass about three frames after its vblank.
 *
 * auto: three drawables and never a wait: a frame is presented only while at
 * most one earlier present has not reached the glass yet, else it is skipped
 * (counted; the title's timing is not touched either way). Each frame is
 * aimed with presentDrawable:atTime: by the pacer: on a fixed-rate display
 * at one refresh of the display's grid, which the on-glass times define and
 * the emulated vblank is locked to; on a variable-refresh display at the
 * frame's vblank plus a fixed lead, the vblank on its own 60.000 Hz timer.
 * The display is looked at again twice a second (the window moved, the mode
 * changed, the setting changed).
 *
 * Every drawable's presentedTime is recorded in both modes: the overlay's
 * graph and RECOMP_TRACE=glass show what reached the glass. */
static pc_pacer        s_pacer;
void xbox_PacingResetHook(void (*fn)(void));
static pc_display      s_disp;
static int             s_strategy = -1;         /* PC_*; -1 before the first look */
static os_unfair_lock  s_pc_lock = OS_UNFAIR_LOCK_INIT;
static atomic_int      s_inflight;              /* presents not on the glass yet */
static atomic_uint     s_skips;
static unsigned        s_poll;
static char            s_disp_name[48];
static int             s_disp_fullscreen;
static uint64_t        s_last_commit_ns;        /* main thread */
static unsigned        s_no_drawable;
static FILE           *s_glass_f;
static int             s_glass_tried;

static double env_ms(recomp_env_id id, double def, double lo, double hi)
{
    const char *e = recomp_env(id);
    double v = e && *e ? atof(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

/* A time on xbox_HostNowNs's clock as presentDrawable:atTime: wants it:
 * seconds of mach_absolute_time. */
static CFTimeInterval host_ns_to_mach_s(uint64_t host_ns)
{
    uint64_t host = xbox_HostNowNs(), mnow = mach_ticks_ns(mach_absolute_time());
    return host_ns >= host ? (double)(mnow + (host_ns - host)) / 1e9
                           : (double)(mnow - (host - host_ns)) / 1e9;
}

/* The display the window is on: its rate, and whether it is variable. */
static void display_probe(SDL_Window *win, pc_display *d, char *name, size_t namen, int *fs)
{
    NSScreen *scr = nil;
    NSNumber *num;
    CGDirectDisplayID id;
    CGDisplayModeRef m;
    double mode_hz = 0, mn = 0, mx = 0, fps = 0;
    SDL_SysWMinfo wm;

    memset(d, 0, sizeof *d);
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
        scr = [wm.info.cocoa.window screen];
    if (!scr)
        scr = [NSScreen mainScreen];
    num = scr ? [[scr deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
    id = num ? (CGDirectDisplayID)[num unsignedIntValue] : CGMainDisplayID();
    m = CGDisplayCopyDisplayMode(id);
    if (m) {
        mode_hz = CGDisplayModeGetRefreshRate(m);
        CGDisplayModeRelease(m);
    }
    if (scr) {
        if (@available(macOS 12.0, *)) {
            mn = scr.minimumRefreshInterval;
            mx = scr.maximumRefreshInterval;
            fps = (double)scr.maximumFramesPerSecond;
        }
    }
    d->builtin = CGDisplayIsBuiltin(id) ? 1 : 0;
    d->refresh_hz = mn > 0 ? 1.0 / mn : mode_hz > 0 ? mode_hz : fps;
    d->min_hz = mx > 0 ? 1.0 / mx : d->refresh_hz;
    /* A variable display states a range of refresh intervals (an external
     * adaptive-sync display with the option on, a ProMotion panel). */
    d->vrr = mn > 0 && mx > mn * 1.02;
    if (name && namen) {
        const char *n = NULL;
        if (scr) {
            if (@available(macOS 10.15, *))
                n = [[scr localizedName] UTF8String];
        }
        snprintf(name, namen, "%s", n ? n : "display");
    }
    if (fs)
        *fs = win && (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

/* 1 while some part of the window can be seen (not covered by other
 * windows, not on another Space): only then does a drawable reach the glass
 * and report when. */
int fbm_visible(SDL_Window *win)
{
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
        return ([wm.info.cocoa.window occlusionState] & NSWindowOcclusionStateVisible) != 0;
    return 1;
}

static void timing_status(void)
{
    pc_status st;
    memset(&st, 0, sizeof st);
    os_unfair_lock_lock(&s_pc_lock);
    st.display = s_disp;
    st.strategy = s_strategy;
    st.depth = s_pacer.depth;
    st.phase_ms = s_pacer.phase_ns / 1e6;
    st.lead_ms = s_pacer.lead_ns / 1e6;
    st.slips = s_pacer.slips_all;
    st.lates = s_pacer.lates_all;
    os_unfair_lock_unlock(&s_pc_lock);
    st.measured = 1;
    st.skips = atomic_load(&s_skips);
    snprintf(st.source, sizeof st.source, "Metal presentedTime");
    xbox_PresentCadenceStatus(&st);
}

/* The pacing watchdog's reset (BO-211), on the executor: the depth and the
 * lead back to the setting's, the grid relearnt. */
static void timing_pacing_reset(void)
{
    os_unfair_lock_lock(&s_pc_lock);
    pc_pacer_relearn(&s_pacer);
    os_unfair_lock_unlock(&s_pc_lock);
}

/* Twice a second, on the main thread: the display, the setting, the
 * strategy; what follows from a change. */
static void timing_poll(SDL_Window *win)
{
    static int hooked;
    if (!hooked) {
        hooked = 1;
        xbox_PacingResetHook(timing_pacing_reset);
    }
    pc_display d;
    char name[sizeof s_disp_name];
    int fs = 0, strategy, changed;

    if (s_strategy >= 0 && s_poll++ % 30)
        return;
    display_probe(win, &d, name, sizeof name, &fs);
    strategy = pc_pick(&d, pc_setting(recomp_env(RENV_PRESENT_TIMING)));
    changed = strategy != s_strategy || d.vrr != s_disp.vrr
              || (int)(d.refresh_hz * 100) != (int)(s_disp.refresh_hz * 100)
              || strcmp(name, s_disp_name) || fs != s_disp_fullscreen;
    if (!changed) {
        timing_status();
        return;
    }
    os_unfair_lock_lock(&s_pc_lock);
    s_disp = d;
    s_strategy = strategy;
    pc_pacer_init(&s_pacer, strategy, &d,
                  (uint64_t)(env_ms(RENV_PRESENT_PHASE, 5.0, 0.0, 15.0) * 1e6),
                  (uint64_t)(env_ms(RENV_PRESENT_LEAD, 12.0, 0.0, 33.0) * 1e6),
                  (int)env_ms(RENV_PRESENT_DEPTH, 1.0, 0.0, PC_DEPTH_MAX));
    os_unfair_lock_unlock(&s_pc_lock);
    snprintf(s_disp_name, sizeof s_disp_name, "%s", name);
    s_disp_fullscreen = fs;
    {
        const char *e = recomp_env(RENV_PRESENT_DRAWABLES);
        int n = strategy != PC_LEGACY ? 3 : (e && atoi(e) == 3) ? 3 : 2;
        if ((int)s_layer.maximumDrawableCount != n)
            s_layer.maximumDrawableCount = n;
    }
    /* The vblank follows the display only where there is a grid to follow;
     * the on-glass times are its clock then, not a CVDisplayLink. */
    if (strategy == PC_LOCK && s_link) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
#pragma clang diagnostic pop
        s_link = NULL;
    }
    if (strategy != PC_LEGACY || !s_link)
        xbox_VblankDisplayLockAuto(strategy == PC_LOCK);
    fprintf(stderr, "[PRESENT] timing: %s on \"%s\" (%s%.2f Hz%s, %s): ",
            pc_strategy_name(strategy), s_disp_name, d.builtin ? "built-in, " : "",
            d.refresh_hz, d.vrr ? ", variable refresh" : ", fixed",
            fs ? "fullscreen" : "windowed");
    if (d.vrr)
        fprintf(stderr, "down to %.1f Hz; ", d.min_hz);
    switch (strategy) {
    case PC_LOCK:
        fprintf(stderr, "the vblank %.1f ms before a display refresh, each frame"
                " shown %d refresh(es) after it, 3 drawables\n",
                s_pacer.phase_ns / 1e6, s_pacer.depth);
        break;
    case PC_TIMED:
    case PC_TIMED_ODD:
        fprintf(stderr, "the vblank on its own 60 Hz timer, each frame presented"
                " %.1f ms after it, 3 drawables%s\n", s_pacer.lead_ns / 1e6,
                strategy == PC_TIMED_ODD ? "; this rate is not a multiple of 60:"
                " frames cannot all be shown for the same time" : "");
        break;
    default:
        fprintf(stderr, "present when ready, %d drawables\n",
                (int)s_layer.maximumDrawableCount);
        break;
    }
    timing_status();
}

/* RECOMP_DEBUG=glass_measure=0: no on-glass handler in legacy (an A/B of
 * what the measurement itself costs). */
static int glass_measure_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_GLASS_MEASURE);
        on = !(e && *e == '0');
    }
    return on;
}

/* RECOMP_TRACE=glass=<file>. */
static FILE *glass_file(void)
{
    if (!s_glass_tried) {
        const char *path = recomp_env(RENV_GLASS_TRACE);
        s_glass_tried = 1;
        if (path && *path && strcmp(path, "1")) {
            s_glass_f = fopen(path, "w");
            if (s_glass_f)
                fprintf(s_glass_f, "flip,vblank_ns,commit_ns,at_ns,target_ns,glass_ns,late,skipped,"
                        "steps,strategy,depth,refresh_hz,vrr\n");
            else
                fprintf(stderr, "[PRESENT] RECOMP_TRACE=glass: cannot write %s\n", path);
        }
    }
    return s_glass_f;
}

/* One presented (or skipped) game frame: the ring, the pacer, the vblank's
 * lock, the trace. Any thread. */
static void glass_note(uint32_t flip, uint64_t vblank_ns, uint64_t commit_ns,
                       const pc_aim *aim, uint64_t glass_ns, int skipped)
{
    pc_sample smp;
    uint64_t report = 0, period = 0;
    int strategy, depth, raised = 0;
    double hz;
    int vrr;
    FILE *f;

    memset(&smp, 0, sizeof smp);
    smp.flip = flip;
    smp.vblank_ns = vblank_ns;
    smp.glass_ns = glass_ns;
    smp.skipped = skipped ? 1 : 0;
    smp.steps = (uint8_t)xbox_PresentCadenceStepsOf(flip);
    xbox_PresentCadenceNote(&smp);
    os_unfair_lock_lock(&s_pc_lock);
    if (!skipped && glass_ns) {
        raised = pc_pacer_glass(&s_pacer, aim, glass_ns);
        report = pc_pacer_lock_report(&s_pacer, glass_ns);
        period = s_pacer.period_ns;
    }
    strategy = s_strategy;
    depth = s_pacer.depth;
    hz = s_disp.refresh_hz;
    vrr = s_disp.vrr;
    os_unfair_lock_unlock(&s_pc_lock);
    if (report)
        xbox_VblankDisplayRefresh(report, period);
    if (raised > 0)
        fprintf(stderr, "[PRESENT] timing: frames reached the glass late; now %s\n",
                strategy == PC_LOCK ? "one refresh more between a frame's vblank and its refresh"
                                    : "2 ms more between a frame's vblank and its present");
    else if (raised < 0)
        fprintf(stderr, "[PRESENT] timing: %d s of frames on time; now %s\n",
                PC_DECAY_FRAMES / 60,
                strategy == PC_LOCK ? "one refresh less between a frame's vblank and its refresh"
                                    : "2 ms less between a frame's vblank and its present");
    if ((f = glass_file()) != NULL)
        fprintf(f, "%u,%llu,%llu,%llu,%llu,%llu,%d,%d,%u,%d,%d,%.3f,%d\n", flip,
                (unsigned long long)vblank_ns, (unsigned long long)commit_ns,
                (unsigned long long)aim->at_ns, (unsigned long long)aim->target_ns,
                (unsigned long long)glass_ns, aim->late, skipped, smp.steps, strategy, depth,
                hz, vrr);
}

static const char s_blit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VO { float4 p [[position]]; float2 t; };\n"
    "vertex VO vs_blit(uint i [[vertex_id]]) {\n"
    "    VO o;\n"
    "    o.t = float2((i << 1) & 2, i & 2);\n"
    "    o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    /* uv: the shown part of the slot (nv2a_present_layout), offset and size. */
    "fragment float4 fs_blit(VO i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]], constant float4 &uv [[buffer(0)]],\n"
    "                        constant float &dither [[buffer(1)]]) {\n"
    "    float3 c = t.sample(s, uv.xy + i.t * uv.zw).rgb;\n"
    /* RECOMP_GFX_DITHER: interleaved gradient noise (Jimenez, 2014) at the
     * window pixel, scaled to just under +-0.5 LSB of the 8-bit target, so
     * a texel that is already an exact 8-bit value (1:1 text, flat colour)
     * rounds back to itself and only a filtered, in-between value moves. */
    "    if (dither > 0.0) {\n"
    "        float n = fract(52.9829189 * fract(dot(floor(i.p.xy), float2(0.06711056, 0.00583715))));\n"
    "        c += (n - 0.5) * (0.98 / 255.0);\n"
    "    }\n"
    "    return float4(c, 1);\n"
    "}\n"
    /* BO-110: the same scale into an extended-sRGB EDR layer. hr.x is the
     * peak in paper whites (the display's headroom, or the user's peak if
     * lower). Values at or below 1.0 pass as they are; above, the brightest
     * channel in linear light goes through a shoulder with slope 1 at 1.0
     * that approaches the peak, and the others keep their ratio to it (hue
     * kept), then back to extended sRGB. */
    "float3 hdr_lin(float3 c) {\n"
    "    return select(c / 12.92, pow((max(c, 0.0) + 0.055) / 1.055, 2.4), c > 0.04045);\n"
    "}\n"
    "float3 hdr_enc(float3 l) {\n"
    "    return select(l * 12.92, 1.055 * pow(max(l, 0.0), 1.0 / 2.4) - 0.055, l > 0.0031308);\n"
    "}\n"
    /* The composite, at the frame's size: f the frame, g the scene
     * snapshot (hp.w 1 when there is one). A pixel where the frame is the
     * snapshot is scene: linear, a shoulder on its brightest channel (slope
     * 1 at paper white, towards the peak; hue kept), times paper white. One
     * that differs was drawn over by the HUD or a menu: clamped to 1.0, times
     * the HUD's white. hp: paper, HUD and peak in EDR units (1.0 = the
     * system's SDR white), has scene. Out: extended sRGB. */
    "fragment float4 fs_hdr_comp(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                            texture2d<float> sc [[texture(1)]], sampler s [[sampler(0)]],\n"
    "                            constant float4 *hq [[buffer(0)]]) {\n"
    /* hq[1]: x the shoulder's softness, y the black level in EDR units (BO-195) */
    "    float4 hp = hq[0], hb = hq[1];\n"
    "    float3 f = t.sample(s, vo.t).rgb, l;\n"
    "    bool scene = false;\n"
    "    if (hp.w > 0.5) {\n"
    "        float3 g = sc.sample(s, vo.t).rgb, d = abs(f - g);\n"
    "        scene = max(d.r, max(d.g, d.b)) < 0.002;\n"
    "    }\n"
    "    if (!scene) {\n"
    "        l = hdr_lin(saturate(f)) * hp.y;\n"
    "    } else {\n"
    "        l = hdr_lin(max(f, 0.0));\n"
    "        float mx = max(l.r, max(l.g, l.b)), m = max(hp.z / hp.x, 1.0);\n"
    "        if (mx > 1.0) {\n"
    "            float y = 1.0;\n"
    "            if (m > 1.0001) {\n"
    "                float k = m - 1.0, e = 1.0 + k * (1.0 - exp(-(mx - 1.0) / k));\n"
    "                float r = 1.0 + k * (mx - 1.0) / ((mx - 1.0) + k);\n"
    "                y = mix(e, r, hb.x * clamp(k - 0.5, 0.0, 1.0) * smoothstep(2.5, 4.0, mx));\n"
    "            }\n"
    "            l *= y / mx;\n"
    "        }\n"
    "        l *= hp.x;\n"
    "    }\n"
    "    l = hb.y + (1.0 - hb.y / hp.z) * l;\n"
    "    return float4(hdr_enc(l), 1);\n"
    "}\n"
    /* The post passes (post_shaders.h): the frame at its own size, one pixel
     * to one texel, so a bilinear fetch at an offset of whole texels is that
     * texel. */
    "#define FETCH(p) t.sample(s, (p)).rgb\n"
    "#define RETURN(c) return float4((c), 1.0)\n"
    "fragment float4 fs_fxaa(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                        sampler s [[sampler(0)]]) {\n"
    "    float2 uv = vo.t;\n"
    "    float2 rcp = 1.0 / float2(t.get_width(), t.get_height());\n"
    POST_FXAA_BODY
    "}\n"
    "fragment float4 fs_cas(VO vo [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                       sampler s [[sampler(0)]], constant float &sharp [[buffer(0)]]) {\n"
    "    float2 uv = vo.t;\n"
    "    float2 rcp = 1.0 / float2(t.get_width(), t.get_height());\n"
    POST_CAS_BODY
    "}\n";

/* BO-110: the window's display, for the HDR settings: how far past its SDR
 * white it can go at most and now (EDR headroom; 1.0 none). */
static void hdr_display(SDL_Window *win)
{
    NSScreen *scr = [NSScreen mainScreen];
    SDL_SysWMinfo wm;
    float pot = 1.0f, now = 1.0f;
    SDL_VERSION(&wm.version);
    if (win && SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA
            && wm.info.cocoa.window.screen)
        scr = wm.info.cocoa.window.screen;
    if (@available(macOS 10.15, *)) {
        pot = (float)scr.maximumPotentialExtendedDynamicRangeColorComponentValue;
        now = (float)scr.maximumExtendedDynamicRangeColorComponentValue;
    }
    nv2a_host_hdr_display(pot, now);
}

/* BO-110: the layer EDR (RGBA16Float, extended sRGB: the encoding the
 * game's values and the overlay's are in, 1.0 the system's SDR white, past
 * 1.0 brighter) or as before. Live: the slots and the post targets follow
 * the format at their next use. */
static void hdr_layer(int on)
{
    CGColorSpaceRef cs;
    if (on && (!s_hp_blit || !s_hp_comp))
        on = 0;
    if (on == s_hdr_win)
        return;
    s_hdr_win = on;
    cs = CGColorSpaceCreateWithName(on ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
    s_layer.pixelFormat = on ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    s_layer.wantsExtendedDynamicRangeContent = on ? YES : NO;
    s_layer.colorspace = cs;
    CGColorSpaceRelease(cs);
    /* SMAA's last pass writes the frame's format: made again. */
    for (int k = 0; k < 3; k++) {
        [s_pso_sm[k] release];
        s_pso_sm[k] = nil;
    }
    s_smaa_state = 0;
    fprintf(stderr, "[PRESENT] HDR %s: %s layer\n", on ? "on" : "off",
            on ? "EDR (RGBA16Float, extended sRGB)" : "SDR (BGRA8, sRGB)");
}

/* BO-110: the peak calibration patch for the overlay (nv2a_host_hdr_patch):
 * a sun-like disc at symbol_nits on a background far past any display's
 * peak, so the disc disappears once it reaches what the display can show.
 * Extended sRGB values, as the overlay draws into the EDR layer. */
static id<MTLTexture> s_patch;
static float          s_patch_nits = -1.0f;
static void *hdr_patch(float nits)
{
    enum { PW = 128, PH = 64 };
    if (!s_hdr_win)
        return NULL;
    if (!s_patch) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:PW height:PH mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_patch = [s_dev newTextureWithDescriptor:td];
        if (!s_patch)
            return NULL;
    }
    if (nits != s_patch_nits) {
        static __fp16 px[PH][PW][4];
        float bg = 1.055f * powf(100.0f, 1.0f / 2.4f) - 0.055f;          /* EDR 100: clipped by any display */
        float l = nits / 100.0f;
        float fg = l > 0.0031308f ? 1.055f * powf(l, 1.0f / 2.4f) - 0.055f : l * 12.92f;
        for (int y = 0; y < PH; y++)
            for (int x = 0; x < PW; x++) {
                float dx = x - PW * 0.5f + 0.5f, dy = y - PH * 0.5f + 0.5f;
                float d = sqrtf(dx * dx + dy * dy);
                float v = d < PH * 0.22f ? fg : bg;
                px[y][x][0] = px[y][x][1] = px[y][x][2] = (__fp16)v;
                px[y][x][3] = (__fp16)1.0f;
            }
        [s_patch replaceRegion:MTLRegionMake2D(0, 0, PW, PH) mipmapLevel:0 withBytes:px
                   bytesPerRow:PW * 8];
        s_patch_nits = nits;
    }
    return (void *)s_patch;
}

static int fail(char *err, size_t n, const char *what)
{
    snprintf(err, n, "%s", what);
    return 0;
}

int fbm_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn)
{
    NSError *e = nil;
    id<MTLLibrary> lib;
    MTLCompileOptions *opts;
    MTLRenderPipelineDescriptor *pd;
    MTLSamplerDescriptor *sd;
    int w = 0, h = 0, k;

    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev)
            return fail(err, errn, "no Metal device");
        s_queue = [s_dev newCommandQueue];
        if (!s_queue)
            return fail(err, errn, "no command queue");
        s_unified = [s_dev hasUnifiedMemory] ? 1 : 0;
        opts = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_blit_msl]
                                  options:opts error:&e];
        [opts release];
        if (!lib)
            return fail(err, errn, "present shader failed to compile");
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_blit"] autorelease];
        /* BO-110: the half-float set first (an EDR layer, its passes). */
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        s_hp_blit = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_hdr_comp"] autorelease];
        s_hp_comp = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_fxaa"] autorelease];
        s_hp_fxaa = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_cas"] autorelease];
        s_hp_cas = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!s_hp_blit || !s_hp_comp)
            XR_ERROR( "[PRESENT] HDR passes unavailable: %s\n",
                    e ? [[e localizedDescription] UTF8String] : "?");
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_blit"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (s_pso) {
            /* The post passes; a failure leaves them off (logged), the
             * present itself unaffected. */
            pd.fragmentFunction = [[lib newFunctionWithName:@"fs_fxaa"] autorelease];
            s_pso_fxaa = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!s_pso_fxaa)
                XR_ERROR( "[PRESENT] FXAA pass unavailable: %s\n",
                        e ? [[e localizedDescription] UTF8String] : "?");
            pd.fragmentFunction = [[lib newFunctionWithName:@"fs_cas"] autorelease];
            s_pso_cas = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!s_pso_cas)
                XR_ERROR( "[PRESENT] CAS pass unavailable: %s\n",
                        e ? [[e localizedDescription] UTF8String] : "?");
        }
        [pd release];
        [lib release];
        if (!s_pso)
            return fail(err, errn, "present pipeline failed");
        for (k = 0; k < 2; k++) {
            sd = [[MTLSamplerDescriptor alloc] init];
            sd.minFilter = sd.magFilter = k ? MTLSamplerMinMagFilterLinear
                                             : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            s_smp[k] = [s_dev newSamplerStateWithDescriptor:sd];
            [sd release];
        }

        s_view = SDL_Metal_CreateView(win);
        if (!s_view)
            return fail(err, errn, SDL_GetError());
        s_layer = (CAMetalLayer *)SDL_Metal_GetLayer(s_view);
        if (!s_layer)
            return fail(err, errn, "SDL_Metal_GetLayer returned NULL");
        [s_layer retain];
        s_layer.device = s_dev;
        s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* The SDL renderer this replaces draws SDR content as sRGB. */
        {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            s_layer.colorspace = cs;
            CGColorSpaceRelease(cs);
        }
        hdr_display(win);           /* BO-110: what the display can show */
        nv2a_host_set_hdr_patch_fn(hdr_patch);
        hdr_layer(nv2a_host_hdr_active());
        s_layer.framebufferOnly = shots ? NO : YES;
        s_layer.displaySyncEnabled = vsync ? YES : NO;
        /* Two drawables: at most one frame queued ahead of the display
         * (the BO-74 latency measurement's recommendation, with display
         * sync on). RECOMP_PRESENT_DRAWABLES=3 gives one more of slack. */
        {
            const char *e = recomp_env(RENV_PRESENT_DRAWABLES);
            s_layer.maximumDrawableCount = (e && atoi(e) == 3) ? 3 : 2;
            if (e)
                fprintf(stderr, "[PRESENT] CAMetalLayer: %d drawables, display sync %s\n",
                        (int)s_layer.maximumDrawableCount, vsync ? "on" : "off");
        }
        s_layer.allowsNextDrawableTimeout = YES;
        SDL_Metal_GetDrawableSize(win, &w, &h);
        if (w > 0 && h > 0)
            s_layer.drawableSize = CGSizeMake(w, h);
        if (recomp_env_on(RENV_WINDOW_FLOAT)) {
            SDL_SysWMinfo wm;
            SDL_VERSION(&wm.version);
            if (SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_COCOA)
                [wm.info.cocoa.window setLevel:NSFloatingWindowLevel];
        }
        if (xbox_VblankDisplayLockOn())
            fbm_vblank_link(win);
        {
            const char *t = recomp_env(RENV_PACING_TRACE);
            s_trace_presented = t && !strcmp(t, "all");
        }
    }
    return 1;
}

void fbm_destroy(void)
{
    int k;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (s_link) {
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
        s_link = NULL;
    }
#pragma clang diagnostic pop
    @autoreleasepool {
        for (k = 0; k < 3; k++) {
            if (s_slot_cb[k])
                [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
            [s_slot[k] release];
            s_slot[k] = nil;
        }
        [s_layer release];
        s_layer = nil;
        if (s_view)
            SDL_Metal_DestroyView(s_view);
        s_view = NULL;
        [s_pso release];
        s_pso = nil;
        [s_pso_fxaa release];
        [s_pso_cas release];
        s_pso_fxaa = s_pso_cas = nil;
        [s_pp[0] release];
        [s_pp[1] release];
        s_pp[0] = s_pp[1] = nil;
        for (k = 0; k < 3; k++) {
            [s_pso_sm[k] release];
            s_pso_sm[k] = nil;
        }
        for (k = 0; k < 2; k++) {
            [s_sm_tex[k] release];
            [s_sm_lut[k] release];
            s_sm_tex[k] = s_sm_lut[k] = nil;
        }
        [s_smp[0] release];
        [s_smp[1] release];
        s_smp[0] = s_smp[1] = nil;
        /* The device and queue stay: the backend holds its own references
         * and the process is about to exit. On the fallback path (setup
         * failed, the window goes back to the SDL renderer) that leaks the
         * device and queue made here for the run: the backend has not
         * started yet and creates its own. */
    }
}

void fbm_get(void **device, void **queue)
{
    *device = (void *)s_dev;
    *queue = (void *)s_queue;
}

/* Slot k at w x h, made or re-made at that size. Releasing the old texture
 * while a present pass reads it is fine: a committed command buffer retains
 * what it uses. */
void *fbm_slot(int k, uint32_t w, uint32_t h)
{
    @autoreleasepool {
        if (!s_slot[k] || [s_slot[k] width] != w || [s_slot[k] height] != h
                || [s_slot[k] pixelFormat] != PP_FMT) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:PP_FMT
                                             width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            /* Shared: an upload needs replaceRegion, which a private texture
             * refuses, and a blit destination gains nothing from private.
             * Managed on a discrete GPU, as the backend's targets. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
            [s_slot[k] release];
            s_slot[k] = [s_dev newTextureWithDescriptor:td];
        }
    }
    return (void *)s_slot[k];
}

/* BO-110: slot k's scene snapshot at w x h (half float), and whether this
 * frame has one. */
void *fbm_scene_slot(int k, uint32_t w, uint32_t h, int valid)
{
    s_scene_ok[k] = 0;
    if (!valid || !s_hdr_win)
        return NULL;
    @autoreleasepool {
        if (!s_scene[k] || [s_scene[k] width] != w || [s_scene[k] height] != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                             width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
            [s_scene[k] release];
            s_scene[k] = [s_dev newTextureWithDescriptor:td];
        }
    }
    s_scene_ok[k] = s_scene[k] != nil;
    return (void *)s_scene[k];
}

/* A guest-bytes frame (bpp 4 = X8R8G8B8, which is BGRA8's byte order;
 * bpp 2 = R5G6B5) into slot k. */
int fbm_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
               uint32_t bpp)
{
    id<MTLTexture> t;
    uint32_t x, y;

    @autoreleasepool {
        if (s_slot_cb[k]) {
            [s_slot_cb[k] waitUntilCompleted];
            [s_slot_cb[k] release];
            s_slot_cb[k] = nil;
        }
        t = (id<MTLTexture>)fbm_slot(k, w, h);
        if (!t)
            return 0;
        s_scene_ok[k] = 0;
        if (s_hdr_win) {
            /* BO-110: a half-float slot; the guest's bytes converted. */
            uint16_t *row = (uint16_t *)malloc((size_t)w * 8u);
            for (y = 0; row && y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint32_t v;
                    __fp16 c[4];
                    if (bpp == 4) {
                        memcpy(&v, src + (size_t)y * pitch + x * 4u, 4);
                    } else {
                        uint16_t q;
                        memcpy(&q, src + (size_t)y * pitch + x * 2u, 2);
                        v = 0xFF000000u | ((uint32_t)((q >> 11) & 0x1F) << 19)
                          | ((uint32_t)((q >> 5) & 0x3F) << 10) | ((uint32_t)(q & 0x1F) << 3);
                    }
                    c[0] = (__fp16)(((v >> 16) & 0xFF) / 255.0f);
                    c[1] = (__fp16)(((v >> 8) & 0xFF) / 255.0f);
                    c[2] = (__fp16)((v & 0xFF) / 255.0f);
                    c[3] = (__fp16)1.0f;
                    memcpy(row + x * 4u, c, 8);
                }
                [t replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:row
                     bytesPerRow:w * 8u];
            }
            free(row);
            return row != NULL;
        }
        if (bpp == 4) {
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:src
                 bytesPerRow:pitch];
        } else {
            if (s_conv_cap < w) {
                uint32_t *n = (uint32_t *)realloc(s_conv, (size_t)w * 4);
                if (!n)
                    return 0;
                s_conv = n;
                s_conv_cap = w;
            }
            for (y = 0; y < h; y++) {
                const uint16_t *r = (const uint16_t *)(src + (size_t)y * pitch);
                for (x = 0; x < w; x++) {
                    uint32_t v = r[x];
                    uint32_t b = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2,
                             rr = ((v >> 11) & 0x1F) << 3;
                    s_conv[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
                }
                [t replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:s_conv
                     bytesPerRow:w * 4];
            }
        }
    }
    return 1;
}

static void save_shot(id<MTLBuffer> buf, uint32_t w, uint32_t h, const char *path,
                      uint32_t flip)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 24,
                                                       SDL_PIXELFORMAT_BGR24);
    const uint8_t *px = (const uint8_t *)[buf contents];
    uint32_t x, y;

    if (!surf)
        return;
    for (y = 0; y < h; y++) {
        uint8_t *o = (uint8_t *)surf->pixels + (size_t)y * surf->pitch;
        const uint8_t *in = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            /* BGRA in the drawable; BGR24 is the bytes B, G, R. */
            o[x * 3 + 0] = in[x * 4 + 0];
            o[x * 3 + 1] = in[x * 4 + 1];
            o[x * 3 + 2] = in[x * 4 + 2];
        }
    }
    if (SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, "[PRESENT] window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

/* A post pass's target at w x h, made or re-made at that size. */
static id<MTLTexture> target_texture(id<MTLTexture> *slot, MTLPixelFormat fmt, NSUInteger w,
                                     NSUInteger h)
{
    if (!*slot || [*slot width] != w || [*slot height] != h || [*slot pixelFormat] != fmt) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        [*slot release];
        *slot = [s_dev newTextureWithDescriptor:td];
    }
    return *slot;
}

/* RECOMP_TRACE=post_gpu: the GPU time of each pass, from its own command
 * buffer (the passes are committed one by one while it is on). */
enum { PT_FXAA, PT_CAS, PT_SM_EDGES, PT_SM_WEIGHTS, PT_SM_BLEND, PT_SCALE, PT_N };
static const char *const s_pt_name[PT_N] = { "fxaa", "cas", "smaa edges", "smaa weights",
                                             "smaa blend", "scale+overlay" };
static double s_pt_sum[PT_N];
static unsigned s_pt_n[PT_N], s_pt_presents;
static int s_pt_on = -1;

static int pt_on(void)
{
    if (s_pt_on < 0)
        s_pt_on = recomp_env(RENV_POST_GPU) != NULL;
    return s_pt_on;
}

/* The command buffer's GPU time under label `what`; every 300 scales a line. */
static void pt_watch(id<MTLCommandBuffer> cb, int what)
{
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        @synchronized ([NSNull null]) {
            int i;
            s_pt_sum[what] += c.GPUEndTime - c.GPUStartTime;
            s_pt_n[what]++;
            if (what == PT_SCALE && ++s_pt_presents % 300 == 0) {
                fprintf(stderr, "[PRESENT] post GPU ms (avg of the last 300):");
                for (i = 0; i < PT_N; i++) {
                    if (s_pt_n[i])
                        fprintf(stderr, " %s %.3f", s_pt_name[i],
                                s_pt_sum[i] * 1000.0 / (double)s_pt_n[i]);
                    s_pt_sum[i] = 0;
                    s_pt_n[i] = 0;
                }
                fprintf(stderr, "\n");
            }
        }
    }];
}

/* One full-frame pass: `src` (up to three textures, all sampled bilinear,
 * binding k with sampler k) through pso into dst, which is first cleared to
 * zero when `clear` (SMAA's edge pass discards where there is no edge).
 * `bytes` is the fragment function's constant at buffer 0. With the trace on
 * the pass is committed on its own and *cbp moves to a new buffer. */
static void post_pass(id<MTLCommandBuffer> *cbp, int what, id<MTLRenderPipelineState> pso,
                      id<MTLTexture> dst, int clear, id<MTLTexture> const *src, int nsrc,
                      const void *bytes, NSUInteger blen)
{
    MTLRenderPassDescriptor *pr = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> pe;
    int i;

    pr.colorAttachments[0].texture = dst;
    pr.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
    pr.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pr.colorAttachments[0].storeAction = MTLStoreActionStore;
    pe = [*cbp renderCommandEncoderWithDescriptor:pr];
    [pe setRenderPipelineState:pso];
    for (i = 0; i < nsrc; i++) {
        [pe setFragmentTexture:src[i] atIndex:(NSUInteger)i];
        [pe setFragmentSamplerState:s_smp[1] atIndex:(NSUInteger)i];
    }
    if (bytes)
        [pe setFragmentBytes:bytes length:blen atIndex:0];
    [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [pe endEncoding];
    if (pt_on()) {
        pt_watch(*cbp, what);
        [*cbp commit];
        *cbp = [s_queue commandBuffer];
    }
}

/* SMAA's pipelines and lookup textures, at the first use. 1 when it can run. */
static int smaa_init(void)
{
    const MTLPixelFormat fmt[3] = { MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm, PP_FMT };
    static const char *const fn[3] = { "fs_smaa_edges", "fs_smaa_weights", "fs_smaa_blend" };
    NSError *e = nil;
    char *src;
    id<MTLLibrary> lib;
    int k;

    if (s_smaa_state)
        return s_smaa_state > 0;
    s_smaa_state = -1;
    src = smaa_msl_source();
    if (!src) {
        XR_ERROR( "[PRESENT] SMAA unavailable: its shader source did not adapt\n");
        return 0;
    }
    lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:nil error:&e];
    free(src);
    if (!lib) {
        XR_ERROR( "[PRESENT] SMAA unavailable: %s\n",
                e ? [[e localizedDescription] UTF8String] : "shader compile failed");
        return 0;
    }
    for (k = 0; k < 3; k++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_smaa"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:[NSString stringWithUTF8String:fn[k]]]
                               autorelease];
        pd.colorAttachments[0].pixelFormat = fmt[k];
        s_pso_sm[k] = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        [pd release];
        if (!s_pso_sm[k]) {
            XR_ERROR( "[PRESENT] SMAA unavailable: pipeline %s: %s\n", fn[k],
                    e ? [[e localizedDescription] UTF8String] : "?");
            [lib release];
            return 0;
        }
    }
    [lib release];
    s_smaa_state = 1;
    fprintf(stderr, "[PRESENT] SMAA ready (preset high)\n");
    return 1;
}

/* SMAA's textures at w x h (the lookup tables once), or all released. */
static int smaa_textures(NSUInteger w, NSUInteger h)
{
    if (!target_texture(&s_sm_tex[0], MTLPixelFormatRG8Unorm, w, h)
            || !target_texture(&s_sm_tex[1], MTLPixelFormatRGBA8Unorm, w, h))
        return 0;
    if (!s_sm_lut[0]) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                         width:SMAA_AREA_W height:SMAA_AREA_H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_sm_lut[0] = [s_dev newTextureWithDescriptor:td];
        [s_sm_lut[0] replaceRegion:MTLRegionMake2D(0, 0, SMAA_AREA_W, SMAA_AREA_H) mipmapLevel:0
                         withBytes:smaa_area_bytes bytesPerRow:SMAA_AREA_W * 2];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                width:SMAA_SEARCH_W
                                                               height:SMAA_SEARCH_H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_sm_lut[1] = [s_dev newTextureWithDescriptor:td];
        [s_sm_lut[1] replaceRegion:MTLRegionMake2D(0, 0, SMAA_SEARCH_W, SMAA_SEARCH_H)
                       mipmapLevel:0 withBytes:smaa_search_bytes bytesPerRow:SMAA_SEARCH_W];
    }
    return s_sm_lut[0] && s_sm_lut[1];
}

static void smaa_release(void)
{
    int k;
    for (k = 0; k < 2; k++) {
        [s_sm_tex[k] release];
        [s_sm_lut[k] release];
        s_sm_tex[k] = s_sm_lut[k] = nil;
    }
}

/* The passes asked for (RECOMP_GFX_AA, RECOMP_GFX_SHARPEN) over `src`, the
 * slot: FXAA, or SMAA's three, then CAS. Returns what the scale to the
 * window samples. What a setting not asked for needs is released. */
static id<MTLTexture> post_chain(id<MTLCommandBuffer> *cbp, id<MTLTexture> src)
{
    int aa = nv2a_host_post_aa();
    float sharp = nv2a_host_sharpen();
    NSUInteger sw = [src width], sh = [src height];
    static int warmed;
    if (!warmed) {
        /* BO-208: SMAA's pipelines are built at the first present, not at
         * the moment the player picks SMAA (FXAA and CAS are made at start). */
        warmed = 1;
        (void)smaa_init();
    }
    id<MTLRenderPipelineState> pfx = s_hdr_win ? s_hp_fxaa : s_pso_fxaa;
    id<MTLRenderPipelineState> pcas = s_hdr_win ? s_hp_cas : s_pso_cas;
    int fx = aa == NV2A_AA_FXAA && pfx;
    int sm = aa == NV2A_AA_SMAA && smaa_init() && smaa_textures(sw, sh);
    int cas = sharp > 0.001f && pcas;
    float metrics[4] = { 1.0f / (float)sw, 1.0f / (float)sh, (float)sw, (float)sh };
    static int last_aa = -1;
    static float last_sharp = -1.0f;

    if (aa != last_aa || sharp != last_sharp) {
        last_aa = aa;
        last_sharp = sharp;
        fprintf(stderr, "[PRESENT] post passes: AA %s, sharpen %.2f (%lux%lu)\n",
                aa == NV2A_AA_FXAA ? "FXAA" : aa == NV2A_AA_SMAA ? (sm ? "SMAA" : "SMAA (unavailable)")
                : "off", sharp, (unsigned long)sw, (unsigned long)sh);
    }
    if (!sm)
        smaa_release();
    if (!fx && !sm && s_pp[0]) {
        [s_pp[0] release];
        s_pp[0] = nil;
    }
    if (!cas && s_pp[1]) {
        [s_pp[1] release];
        s_pp[1] = nil;
    }
    if (fx) {
        id<MTLTexture> dst = target_texture(&s_pp[0], PP_FMT, sw, sh);
        if (dst) {
            post_pass(cbp, PT_FXAA, pfx, dst, 0, &src, 1, NULL, 0);
            src = dst;
        }
    } else if (sm) {
        id<MTLTexture> dst = target_texture(&s_pp[0], PP_FMT, sw, sh);
        if (dst) {
            id<MTLTexture> in2[3];
            in2[0] = src;
            post_pass(cbp, PT_SM_EDGES, s_pso_sm[0], s_sm_tex[0], 1, in2, 1, metrics, sizeof metrics);
            in2[0] = s_sm_tex[0];
            in2[1] = s_sm_lut[0];
            in2[2] = s_sm_lut[1];
            post_pass(cbp, PT_SM_WEIGHTS, s_pso_sm[1], s_sm_tex[1], 1, in2, 3, metrics, sizeof metrics);
            in2[0] = src;
            in2[1] = s_sm_tex[1];
            post_pass(cbp, PT_SM_BLEND, s_pso_sm[2], dst, 0, in2, 2, metrics, sizeof metrics);
            src = dst;
        }
    }
    if (cas) {
        id<MTLTexture> dst = target_texture(&s_pp[1], PP_FMT, sw, sh);
        if (dst) {
            post_pass(cbp, PT_CAS, pcas, dst, 0, &src, 1, &sharp, sizeof sharp);
            src = dst;
        }
    }
    return src;
}

/* Slot k (fw x fh) into the window. 0 when no drawable came (timeout, display
 * asleep, window hidden): the caller keeps the frame and tries again. */
int fbm_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                const char *shot_path, uint32_t flip, uint64_t vblank_ns)
{
    int ok = 0;

    @autoreleasepool {
        id<CAMetalDrawable> d;
        id<MTLTexture> src;
        id<MTLCommandBuffer> cb;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor *rp;
        id<MTLBuffer> shot = nil;
        struct nv2a_rect r;
        int whole, dw = 0, dh = 0;
        CGSize cur = s_layer.drawableSize;

        /* Checked every present, not only on resize events: a move between
         * displays of another scale need not arrive as SIZE_CHANGED. */
        SDL_Metal_GetDrawableSize(win, &dw, &dh);
        if (dw > 0 && dh > 0 && ((int)cur.width != dw || (int)cur.height != dh))
            s_layer.drawableSize = CGSizeMake(dw, dh);
        uint64_t t_acq0 = xbox_LatOn() ? xbox_HostNowNs() : 0;
        pc_aim aim;
        int timed;

        memset(&aim, 0, sizeof aim);
        timing_poll(win);
        timed = s_strategy != PC_LEGACY;
        /* Never wait for a drawable: with two presents still on their way
         * to the glass the third drawable is the one on it. A new game
         * frame is skipped then (and said so); a redraw is tried again. */
        if (timed && !shot_path && atomic_load(&s_inflight) >= 2) {
            /* Presents the system has said nothing about for a tenth of a
             * second (the window went behind another, the display slept)
             * are not on their way any more. */
            if (xbox_HostNowNs() - s_last_commit_ns > 100000000ull) {
                atomic_store(&s_inflight, 0);
                goto take;
            }
            if (!vblank_ns)
                return 0;
            atomic_fetch_add(&s_skips, 1);
            glass_note(flip, vblank_ns, xbox_HostNowNs(), &aim, 0, 1);
            return 2;
        }
    take:
        d = [s_layer nextDrawable];
        if (!d) {
            if (!s_no_drawable++)
                fprintf(stderr, "[PRESENT] no drawable (the window is hidden, behind"
                        " another, or the display sleeps): presents wait\n");
            return 0;
        }
        if (s_no_drawable) {
            fprintf(stderr, "[PRESENT] drawables again after %u tries\n", s_no_drawable);
            s_no_drawable = 0;
        }
        if (t_acq0) {
            char b[32];
            snprintf(b, sizeof b, "%llu", (unsigned long long)xbox_HostNowNs());
            xbox_LatRow('W', flip, t_acq0, b);
        }
        dw = (int)[d.texture width];
        dh = (int)[d.texture height];
        cb = [s_queue commandBuffer];
        /* The post passes (post_shaders.h), at the frame's own size and
         * before the scale to the window; the overlay below is drawn after
         * them, so it is not filtered. */
        /* BO-110: HDR follows the setting live; the display's headroom is
         * read every present (macOS changes it with the brightness). */
        hdr_display(win);
        hdr_layer(nv2a_host_hdr_active());
        if (s_hdr_win && s_slot[k] && [s_slot[k] pixelFormat] != MTLPixelFormatRGBA16Float)
            fw = fh = 0;               /* a slot from before the switch: black once */
        src = s_slot[k];
        if (s_hdr_win && src && fw && fh) {
            /* The composite at the frame's size, then the post passes on it. */
            float pk, pw, hw;
            id<MTLTexture> in2[2];
            id<MTLTexture> dst = target_texture(&s_hp_c, MTLPixelFormatRGBA16Float,
                                                [src width], [src height]);
            nv2a_host_hdr_levels(&pk, &pw, &hw);
            float sf, bl;
            nv2a_host_hdr_shape(&sf, &bl);
            float hp[8] = { pw / 100.0f, hw / 100.0f, pk / 100.0f,
                            s_scene_ok[k] && s_scene[k] ? 1.0f : 0.0f,
                            sf, bl / 100.0f, 0.0f, 0.0f };
            if (dst) {
                in2[0] = src;
                in2[1] = hp[3] > 0.5f ? s_scene[k] : src;
                post_pass(&cb, PT_SCALE, s_hp_comp, dst, 0, in2, 2, hp, sizeof hp);
                src = dst;
            }
        }
        if (src && fw && fh)
            src = post_chain(&cb, src);
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (s_slot[k] && fw && fh) {
            struct nv2a_rect sr;
            nv2a_present_frame(flip);
            whole = nv2a_present_layout(fw, fh, (uint32_t)dw, (uint32_t)dh, filter, &sr, &r);
            if (r.w > 0 && r.h > 0) {
                float uv[4] = { (float)sr.x / (float)fw, (float)sr.y / (float)fh,
                                (float)sr.w / (float)fw, (float)sr.h / (float)fh };
                MTLViewport vp = { r.x, r.y, r.w, r.h, 0.0, 1.0 };
                int lin = filter == NV2A_PRESENT_LINEAR
                          || (filter == NV2A_PRESENT_INTEGER && !whole);
                [enc setRenderPipelineState:s_hdr_win ? s_hp_blit : s_pso];
                [enc setViewport:vp];
                [enc setFragmentTexture:src atIndex:0];
                [enc setFragmentSamplerState:s_smp[lin] atIndex:0];
                float dither = nv2a_host_dither() ? 1.0f : 0.0f;
                [enc setFragmentBytes:uv length:sizeof uv atIndex:0];
                if (s_hdr_win)
                    dither = 0.0f;      /* BO-110: the half-float layer needs none */
                [enc setFragmentBytes:&dither length:sizeof dither atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
        }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
        /* The settings overlay, over the game's image and under the present
         * (overlay.h): more of the same encoder. */
        if (xbox_OverlayWants())
            xbox_OverlayMetalDraw((void *)s_dev, (void *)cb, (void *)rp, (void *)enc);
#endif
        [enc endEncoding];
        if (shot_path && !s_hdr_win) {
            shot = [s_dev newBufferWithLength:(NSUInteger)dw * dh * 4
                                      options:MTLResourceStorageModeShared];
            if (shot) {
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake((NSUInteger)dw, (NSUInteger)dh, 1)
                          toBuffer:shot destinationOffset:0
                 destinationBytesPerRow:(NSUInteger)dw * 4
               destinationBytesPerImage:(NSUInteger)dw * dh * 4];
                [b endEncoding];
            }
        }
        if (s_trace_presented)
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                CFTimeInterval t = dr.presentedTime;   /* 0: never shown */
                if (t > 0)
                    xbox_PacingPresent(mach_to_host_ns((uint64_t)(t * 1e9)));
            }];
        /* Aimed here, with the pass encoded: what is left before the
         * commit is the same few microseconds every frame. */
        if (timed && vblank_ns) {
            uint64_t now = xbox_HostNowNs();
            os_unfair_lock_lock(&s_pc_lock);
            pc_pacer_aim(&s_pacer, vblank_ns, now, &aim);
            os_unfair_lock_unlock(&s_pc_lock);
            if (aim.at_ns > now + 100000000ull)   /* never park a frame */
                aim.at_ns = 0;
        }
        /* When it reached the glass: the pacer's feedback and the graph's
         * trace (a redraw of the same frame only gives its drawable back). */
        if (timed || glass_measure_on()) {
            uint64_t vb = vblank_ns, commit_ns = xbox_HostNowNs();
            uint32_t fl2 = flip;
            pc_aim aim2 = aim;
            atomic_fetch_add(&s_inflight, 1);
            s_last_commit_ns = commit_ns;
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                CFTimeInterval t = dr.presentedTime;   /* 0: never shown */
                if (atomic_fetch_sub(&s_inflight, 1) <= 0)
                    atomic_store(&s_inflight, 0);       /* reset while it was out */
                if (vb)
                    glass_note(fl2, vb, commit_ns, &aim2,
                               t > 0 ? mach_to_host_ns((uint64_t)(t * 1e9)) : 0, 0);
            }];
        }
        if (aim.at_ns)
            [cb presentDrawable:d atTime:host_ns_to_mach_s(aim.at_ns)];
        else
            [cb presentDrawable:d];
        if (t_acq0) {
            /* presentedTime: host seconds (mach_absolute_time) when the
             * drawable reached the glass, 0 when it never did. */
            uint32_t fl = flip;
            [d addPresentedHandler:^(id<MTLDrawable> dr) {
                static mach_timebase_info_data_t tb;
                double ps = dr.presentedTime;
                uint64_t now = xbox_HostNowNs(), mach_ns, on = 0;
                char b[32];
                if (!tb.denom)
                    mach_timebase_info(&tb);
                mach_ns = mach_absolute_time() * tb.numer / tb.denom;
                if (ps > 0)
                    on = now - mach_ns + (uint64_t)(ps * 1e9);
                snprintf(b, sizeof b, "%llu", (unsigned long long)on);
                xbox_LatRow('D', fl, now, b);
            }];
        }
        if (pt_on())
            pt_watch(cb, PT_SCALE);
        [cb commit];
        if (t_acq0)
            xbox_LatRow('P', flip, 0, NULL);
        [s_slot_cb[k] release];
        s_slot_cb[k] = [cb retain];
        if (shot) {
            [cb waitUntilCompleted];
            save_shot(shot, (uint32_t)dw, (uint32_t)dh, shot_path, flip);
            [shot release];
        }
        ok = 1;
    }
    return ok;
}

#endif /* __APPLE__ */
