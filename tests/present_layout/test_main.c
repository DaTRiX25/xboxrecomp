/* nv2a_present_layout: what a window shows of a frame, and where; and the
 * visible-columns helpers a host lays a HUD out by.
 *
 * golden.inc is the output of the loop in same_as_golden at the commit
 * before these helpers took an aspect: three aspects x overscan off/crop x
 * fit off/fill16x10 x three filters x four frame sizes x nine outputs.
 * Every one must give the same rectangles. */
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct row { int aspect, crop, fill, filter; uint32_t fw, fh, dw, dh; int whole; int32_t s[4], d[4]; };
static const struct row k_golden[] = {
#include "golden.inc"
};

static void mode(int aspect, int crop, int fill)
{
    struct nv2a_host_opts h = *nv2a_host_opts();
    h.present_aspect = aspect;
    nv2a_host_opts_set(&h);
    nv2a_present_set_overscan(crop);
    nv2a_present_set_fit(fill);
}

static int same_as_golden(void)
{
    size_t i, n = sizeof k_golden / sizeof k_golden[0];
    for (i = 0; i < n; i++) {
        const struct row *g = &k_golden[i];
        struct nv2a_rect s, d;
        int whole;
        mode(g->aspect, g->crop, g->fill);
        whole = nv2a_present_layout(g->fw, g->fh, g->dw, g->dh, g->filter, &s, &d);
        if (whole != g->whole || s.x != g->s[0] || s.y != g->s[1] || s.w != g->s[2] || s.h != g->s[3]
            || d.x != g->d[0] || d.y != g->d[1] || d.w != g->d[2] || d.h != g->d[3]) {
            printf("FAIL: row %u differs from the golden table\n", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/* The columns of a 640x480 frame an output shows, in the configured mode
 * and for a 4:3 picture in the same output. */
static int visible(int aspect, int crop, int fill, uint32_t dw, uint32_t dh,
                   double want0, double want1, double ref0, double ref1)
{
    struct nv2a_rect s, d;
    double x0, x1, r0, r1;
    mode(aspect, crop, fill);
    nv2a_present_layout(640, 480, dw, dh, NV2A_PRESENT_LINEAR, &s, &d);   /* a present */
    nv2a_present_visible_x(640, 480, &x0, &x1);
    nv2a_present_visible_x_at(640, 480, NV2A_ASPECT_4_3, &r0, &r1);
    if (x0 != want0 || x1 != want1 || r0 != ref0 || r1 != ref1) {
        printf("visible: aspect %d crop %d fill %d %ux%u: %.0f..%.0f (4:3: %.0f..%.0f)\n",
               aspect, crop, fill, dw, dh, x0, x1, r0, r1);
        return 0;
    }
    return 1;
}

int main(void)
{
    double x0, x1;

    CHECK(sizeof k_golden / sizeof k_golden[0] == 1296);

    /* Before any present and without a window: the overscan crop alone. */
    mode(NV2A_ASPECT_16_9, 1, 1);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    /* A window, not yet presented to: its shape counts (the Deck's panel). */
    nv2a_present_note_output(1280, 800);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 51.0 && x1 == 589.0);

    CHECK(same_as_golden());

    /* 4:3, overscan crop: 21 columns a side, in any output. */
    CHECK(visible(NV2A_ASPECT_4_3, 1, 0, 1440, 1080, 21, 619, 21, 619));
    CHECK(visible(NV2A_ASPECT_FRAME, 1, 0, 1280, 800, 21, 619, 21, 619));
    /* 16:9, overscan crop, a 16:9 output: the same columns. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 0, 1920, 1080, 21, 619, 21, 619));
    /* No crop: the whole frame. */
    CHECK(visible(NV2A_ASPECT_16_9, 0, 0, 1920, 1080, 0, 640, 0, 640));
    /* 16:9 with the 16:10 fill on the Deck's 1280x800: the picture fills the
     * height and loses 30 more columns a side; a 4:3 picture there loses
     * none, which is the margin a HUD fix has to keep. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 1280, 800, 51, 589, 21, 619));
    CHECK(visible(NV2A_ASPECT_16_9, 0, 1, 1280, 800, 32, 608, 0, 640));
    /* The fill in a 16:9 output crops nothing. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 1920, 1080, 21, 619, 21, 619));
    /* 4:3 with the fill on the Deck: pillarboxed, nothing cropped. */
    CHECK(visible(NV2A_ASPECT_4_3, 1, 1, 1280, 800, 21, 619, 21, 619));
    /* A wide output (21:9, 2560x1080) with a 16:9 picture: bars, no crop. */
    CHECK(visible(NV2A_ASPECT_16_9, 1, 0, 2560, 1080, 21, 619, 21, 619));
    CHECK(visible(NV2A_ASPECT_16_9, 1, 1, 2560, 1080, 21, 619, 21, 619));
    /* A present's size replaces the noted one; a later note does not. */
    nv2a_present_note_output(640, 480);
    mode(NV2A_ASPECT_16_9, 1, 1);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);       /* the 2560x1080 present still stands */

    CHECK(same_as_golden());
    printf("present_layout: ok (%u golden rows, visible columns)\n",
           (unsigned)(sizeof k_golden / sizeof k_golden[0]));
    return 0;
}
