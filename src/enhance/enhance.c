/*
 * enhance.c - see enhance.h. Reads the toolkit's keys once and stores them
 * as nv2a_host_opts; the per-frame paths only ever read that struct.
 */
#include "enhance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_cfg.h"
#include "kernel.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"

#define LOGP "[ENHANCE] "
/* Android: 3. A phone's panel is about 1080 rows, which 3x (1440) already
 * passes; 4x is only heat. */
#if defined(__ANDROID__)
#define SCALE_MAX 3
#else
#define SCALE_MAX 4
#endif

static const char *const s_filters[] = { "nearest", "linear", "integer", NULL };
/* Indexes match XBOX_PACING_SPIN / XBOX_PACING_SLEEP. */
static const char *const s_pacing[] = { "spin", "sleep", NULL };
/* present.aspect; "auto" is resolved below, the rest index
 * enum nv2a_present_aspect + 1. */
static const char *const s_aspects[] = { "auto", "frame", "4:3", "16:9", NULL };
/* The dashboard's HDTV modes, each with those below it (the dashboard has a
 * box per mode; a set that skips one is not offered here). */
static const char *const s_hdtv[] = { "off", "480p", "720p", "1080i", NULL };
static const ULONG s_hdtv_bits[] = {
    0,
    XC_VIDEO_AV_HDTV_480p,
    XC_VIDEO_AV_HDTV_480p | XC_VIDEO_AV_HDTV_720p,
    XC_VIDEO_AV_HDTV_480p | XC_VIDEO_AV_HDTV_720p | XC_VIDEO_AV_HDTV_1080i,
};

int xbox_enhance_init(const char *root, const char *title)
{
    struct nv2a_host_opts o;
    const char *aspect;
    long long scale;
    int rc, pacing, wide, hdtv, pa;
    double lead;
    ULONG xc_video;

    rc = enhance_cfg_init(root, title) < 0 ? -1 : 0;
    memset(&o, 0, sizeof o);

    scale = enhance_cfg_int("render.scale", 1);
    if (scale < 1 || scale > SCALE_MAX) {
        long long c = scale < 1 ? 1 : SCALE_MAX;
        fprintf(stderr, LOGP "render.scale=%lld out of range 1..%d; using %lld\n",
                scale, SCALE_MAX, c);
        scale = c;
    }
    o.render_scale = (unsigned)scale;
    /* Indexes match enum nv2a_present_filter. */
    o.present_filter = enhance_cfg_choice("present.filter", s_filters, NV2A_PRESENT_NEAREST);
    o.fullscreen = enhance_cfg_bool("present.fullscreen", 0);
    /* How lowered guest spin waits wait (kernel_pacing.h): only titles
     * generated with --spin-waits have any. Set before the guest starts. */
    pacing = enhance_cfg_choice("present.pacing", s_pacing, XBOX_PACING_SPIN);
    xbox_PacingSetMode(pacing);
    /* Milliseconds of the flip's vblank hold spun, not slept, at sleep
     * pacing: what keeps an APU's CPU clock up (kernel_pacing.c). */
    lead = enhance_cfg_float("present.spin_lead", 0.0);
    if (lead < 0 || lead > 20) {
        fprintf(stderr, LOGP "present.spin_lead=%g out of range 0..20 ms; using 0\n", lead);
        lead = 0;
    }
    xbox_PacingSetSpinLead((uint64_t)(lead * 1e6));

    /* The dashboard's video settings, as the kernel reports them to the
     * title (XC_VIDEO, HalBootSMCVideoMode; kernel.h). What the title does
     * with them is its own: Burnout 3 renders 16:9 anamorphic into its
     * 640x480 buffer and asks D3D for 480p. Stock is neither. Before the
     * kernel bridge starts, which copies the SMC mode into guest memory. */
    wide = enhance_cfg_bool("display.widescreen", 0);
    hdtv = enhance_cfg_choice("display.hdtv", s_hdtv, 0);
    xc_video = (wide ? XC_VIDEO_AV_WIDESCREEN : 0) | s_hdtv_bits[hdtv];
    xbox_SetVideoSettings(xc_video, hdtv ? SMC_VIDEO_MODE_HDTV : SMC_VIDEO_MODE_SCART);

    /* The window's frame shape. A title in its widescreen mode draws 16:9
     * into the same 4:3-sized buffer (anamorphic; the TV stretched it), so
     * auto shows 16:9 when display.widescreen tells it to, and the frame's
     * own shape otherwise (stock). A title that ignores the setting stays
     * 4:3 with present.aspect = "frame". */
    pa = enhance_cfg_choice("present.aspect", s_aspects, 0);
    o.present_aspect = pa == 0 ? (wide ? NV2A_ASPECT_16_9 : NV2A_ASPECT_FRAME) : pa - 1;

    /* Hor+ is a later slice; a file or env that asks for it must not be
     * ignored silently. */
    aspect = enhance_cfg_string("display.aspect", "4:3");
    if (strcmp(aspect, "4:3") != 0) {
        fprintf(stderr, LOGP "display.aspect=%s not implemented yet (hor+ is a"
                " later slice); using 4:3\n", aspect);
        aspect = "4:3";
    }

    /* Where the GPU backends keep their pipeline-key list (nv2a_pcache.h):
     * unset is the per-user cache directory, "none" no disk cache. */
    {
        const char *sc = enhance_cfg_string("render.shader_cache", NULL);
        o.shader_cache = sc ? strdup(sc) : NULL;
    }

    nv2a_host_opts_set(&o);
    fprintf(stderr, LOGP "render.scale=%u present.filter=%s present.fullscreen=%d"
            " present.pacing=%s (display.aspect=%s)\n", o.render_scale,
            s_filters[o.present_filter], o.fullscreen, s_pacing[pacing], aspect);
    fprintf(stderr, LOGP "display.widescreen=%d display.hdtv=%s (XC_VIDEO=0x%08lX)"
            " present.aspect=%s (%s)\n", wide, s_hdtv[hdtv], (unsigned long)xc_video,
            s_aspects[pa], s_aspects[o.present_aspect + 1]);
    if (lead > 0)
        fprintf(stderr, LOGP "present.spin_lead=%g ms%s\n", lead,
                pacing == XBOX_PACING_SLEEP ? "" : " (no effect: pacing is spin)");
    if (o.shader_cache)
        fprintf(stderr, LOGP "render.shader_cache=%s\n", o.shader_cache);
    /* No enhance_cfg_report_unused() here: the title reads its own keys
     * (fps.mode) after this, and calls it then. */
    return rc;
}
