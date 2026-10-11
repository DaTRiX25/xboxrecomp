/* nv2a_present_layout: what a window shows of a frame, and where.
 *
 * golden.inc is the output of the loop below at the commit before the
 * per-frame aspect (nv2a_present_frame_aspect_fn) was added: three aspects x
 * overscan off/crop x fit off/fill16x10 x three filters x four frame sizes x
 * nine outputs. Every existing mode must give the same rectangles, with no
 * aspect function, with one that answers 0, and after one was used and
 * removed. Then the per-frame aspect: a wide frame fills a wide output, a
 * frame without one is the configured shape in the middle. */
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct row { int aspect, crop, fill, filter; uint32_t fw, fh, dw, dh; int whole; int32_t s[4], d[4]; };
static const struct row k_golden[] = {
#include "golden.inc"
};

static double s_answer;
static uint32_t s_asked;
static double aspect_of(uint32_t flip) { s_asked = flip; return flip == 7 ? s_answer : 0.0; }

static int same_as_golden(const char *what)
{
    size_t i, n = sizeof k_golden / sizeof k_golden[0];
    for (i = 0; i < n; i++) {
        const struct row *g = &k_golden[i];
        struct nv2a_host_opts h = *nv2a_host_opts();
        struct nv2a_rect s, d;
        int whole;
        h.present_aspect = g->aspect;
        nv2a_host_opts_set(&h);
        nv2a_present_set_overscan(g->crop);
        nv2a_present_set_fit(g->fill);
        whole = nv2a_present_layout(g->fw, g->fh, g->dw, g->dh, g->filter, &s, &d);
        if (whole != g->whole || s.x != g->s[0] || s.y != g->s[1] || s.w != g->s[2] || s.h != g->s[3]
            || d.x != g->d[0] || d.y != g->d[1] || d.w != g->d[2] || d.h != g->d[3]) {
            printf("FAIL %s: row %u differs from the golden table\n", what, (unsigned)i);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    struct nv2a_host_opts h;
    struct nv2a_rect s, d;
    double x0, x1;

    CHECK(sizeof k_golden / sizeof k_golden[0] == 1296);
    CHECK(same_as_golden("no aspect function"));

    /* A function that answers 0 for the frame: nothing changes. */
    nv2a_present_frame_aspect_fn(aspect_of);
    nv2a_present_frame(3);
    CHECK(s_asked == 3);
    CHECK(same_as_golden("aspect function answering 0"));

    /* 16:9 configured, the overscan crop, a 21:9 output (2560x1080). */
    h = *nv2a_host_opts();
    h.present_aspect = NV2A_ASPECT_16_9;
    nv2a_host_opts_set(&h);
    nv2a_present_set_overscan(1);
    nv2a_present_set_fit(0);
    /* A frame of the configured shape: 16:9 in the middle, bars at the sides. */
    nv2a_present_frame(3);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.x == 21 && s.y == 16 && s.w == 598 && s.h == 448);
    CHECK(d.x == 320 && d.y == 0 && d.w == 1920 && d.h == 1080);
    /* The wide frame (flip 7, 64:27): the whole output, the same crop. */
    s_answer = 64.0 / 27.0;
    nv2a_present_frame(7);
    nv2a_present_layout(640, 480, 2560, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.x == 21 && s.y == 16 && s.w == 598 && s.h == 448);
    CHECK(d.x == 0 && d.y == 0 && d.w == 2560 && d.h == 1080);
    /* The wide frame in a 16:9 output: letterboxed, never cropped. */
    nv2a_present_layout(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(s.w == 598 && d.x == 0 && d.w == 1920 && d.h == 810 && d.y == 135);
    /* 32:9 in a 32:9 output. */
    s_answer = 32.0 / 9.0;
    nv2a_present_frame(7);
    nv2a_present_layout(640, 480, 3840, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.x == 0 && d.y == 0 && d.w == 3840 && d.h == 1080);
    /* The next frame is an ordinary one again. */
    nv2a_present_frame(8);
    nv2a_present_layout(640, 480, 3840, 1080, NV2A_PRESENT_LINEAR, &s, &d);
    CHECK(d.x == 960 && d.w == 1920 && d.h == 1080);
    /* The visible columns do not depend on the frame's aspect here. */
    nv2a_present_frame(7);
    nv2a_present_visible_x(640, 480, &x0, &x1);
    CHECK(x0 == 21.0 && x1 == 619.0);
    CHECK(nv2a_present_output_aspect() > 3.55 && nv2a_present_output_aspect() < 3.56);

    /* The window's opening shape is its own knob; the present width follows
     * the frame (an ordinary one here), not the window. */
    nv2a_present_frame(8);
    CHECK(nv2a_present_window_width(640, 480) == nv2a_present_width(640, 480));
    nv2a_present_window_aspect(64.0 / 27.0);
    CHECK(nv2a_present_window_width(640, 480) == 1138 && nv2a_present_width(640, 480) == 853);
    nv2a_present_window_aspect(0.0);

    /* The function removed: every mode as recorded. */
    nv2a_present_frame_aspect_fn(NULL);
    CHECK(same_as_golden("aspect function removed"));
    printf("present_layout: ok (%u golden rows x3)\n", (unsigned)(sizeof k_golden / sizeof k_golden[0]));
    return 0;
}
