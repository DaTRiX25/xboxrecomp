/*
 * enhance.c - see enhance.h. Reads the toolkit's keys once and stores them
 * as nv2a_host_opts; the per-frame paths only ever read that struct.
 */
#include "enhance.h"

#include <stdio.h>
#include <string.h>

#include "enhance_cfg.h"
#include "nv2a_backend_common.h"

#define LOGP "[ENHANCE] "
#define SCALE_MAX 4

static const char *const s_filters[] = { "nearest", "linear", "integer", NULL };

int xbox_enhance_init(const char *root, const char *title)
{
    struct nv2a_host_opts o;
    const char *aspect;
    long long scale;
    int rc;

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

    /* Hor+ is a later slice; a file or env that asks for it must not be
     * ignored silently. */
    aspect = enhance_cfg_string("display.aspect", "4:3");
    if (strcmp(aspect, "4:3") != 0) {
        fprintf(stderr, LOGP "display.aspect=%s not implemented yet (hor+ is a"
                " later slice); using 4:3\n", aspect);
        aspect = "4:3";
    }

    nv2a_host_opts_set(&o);
    fprintf(stderr, LOGP "render.scale=%u present.filter=%s present.fullscreen=%d"
            " (display.aspect=%s)\n", o.render_scale, s_filters[o.present_filter],
            o.fullscreen, aspect);
    enhance_cfg_report_unused();
    return rc;
}
