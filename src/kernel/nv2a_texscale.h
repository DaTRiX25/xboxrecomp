/**
 * nv2a_texscale.h - a title's 2D art made larger before it is uploaded.
 *
 * PPSSPP's texture scaling, for the 2D art only: a texture the title draws
 * its HUD, menus and fonts with is scaled 2x or 4x on the CPU, once, when a
 * GPU backend uploads it (graphics.ui_scale, RECOMP_GFX_UI_SCALE; off by
 * default). The title samples these with normalised coordinates, so a larger
 * texture needs no other change; the backend keeps the guest size for
 * everything it computes from a size.
 *
 * Which textures: a rule on how a texture is used, not a list (nothing here
 * knows a title's files). nv2a_texscale_draw_is_2d() says whether a draw is
 * a 2D one, nv2a_texscale_tex_ok() whether a texture may be scaled at all;
 * a backend scales a texture when a 2D draw first binds it.
 *
 * The scalers work on premultiplied colour (a transparent texel's colour has
 * no say), read past a texture's edge by repeating the edge texel, and give
 * back straight alpha as the title's blending expects. Fully transparent
 * output texels take the colour of the visible ones beside them, so the
 * GPU's bilinear filter does not mix a stray colour into an outline.
 *
 * Super-xBR is Hyllian's algorithm (MIT, see the .c file); the bicubic is a
 * plain Catmull-Rom for comparison.
 *
 * No OS or graphics headers: shared by the Metal and Vulkan backends.
 */
#ifndef NV2A_TEXSCALE_H
#define NV2A_TEXSCALE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nv2a_texscale_algo {
    NV2A_TEXSCALE_SUPERXBR = 0,
    NV2A_TEXSCALE_BICUBIC  = 1,
    NV2A_TEXSCALE_ALGOS
};

/* The largest texture scaled (texels of the guest's level 0). */
#define NV2A_TEXSCALE_MAX_TEXELS (512u * 512u)

/* src: w x h A8R8G8B8, straight alpha, row-major. dst: (w * factor) x
 * (h * factor), the same layout. factor 2 or 4. Returns 0 (dst untouched) for
 * another factor, an unknown algorithm, a zero size or no memory. The same
 * input gives the same output on every machine built from the same source
 * with the same compiler; plain float arithmetic, no threads. */
int nv2a_texscale(const uint32_t *src, uint32_t w, uint32_t h, unsigned factor,
                  enum nv2a_texscale_algo algo, uint32_t *dst);

/* Levels 1..levels-1 of a texture whose level 0 is w x h at texels, each a
 * 2x2 box of the one above, alpha-weighted (a transparent texel's colour
 * does not darken its neighbours). texels holds every level, level 0 first. */
void nv2a_texscale_mips(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels);

/* ── the setting ─────────────────────────────────────────────────────── */

/* 0 (off), 2 or 4; any other value is taken as off. Any thread; a change
 * moves the generation on. */
void     nv2a_texscale_set_factor(unsigned factor);
unsigned nv2a_texscale_factor(void);
void     nv2a_texscale_set_algo(enum nv2a_texscale_algo algo);
enum nv2a_texscale_algo nv2a_texscale_algo(void);
/* Never 0; changes whenever the factor or the algorithm does. A backend
 * keeps it with a cache entry it scaled and uploads the entry again when it
 * has moved on. */
uint32_t nv2a_texscale_generation(void);

/* ── the rule ────────────────────────────────────────────────────────── */

/* What a backend knows of the draw that binds a texture. */
struct nv2a_texscale_draw {
    int      program;        /* runs a vertex program (not pre-transformed) */
    int      depth_test;     /* depth test enabled */
    int      depth_write;    /* depth test enabled and writing */
    uint32_t target_w, target_h;   /* the colour surface drawn into (guest) */
    uint32_t frame_w, frame_h;     /* the display's frame (guest) */
};

/* A 2D draw: no depth test and no depth write, into the frame's surface
 * (not one of the title's smaller effect targets). */
int nv2a_texscale_draw_is_2d(const struct nv2a_texscale_draw *d);

/* A texture that may be scaled: decoded from guest memory by a backend's
 * ordinary 2D path (the caller never asks for a render target, a cube map or
 * a signed/bump texture), a swizzled or DXT format (a linear one is a video
 * frame or a copy of the screen), one level, and no larger than
 * NV2A_TEXSCALE_MAX_TEXELS. size_from_format: nv2a_tex_size_from_format(). */
int nv2a_texscale_tex_ok(int size_from_format, uint32_t w, uint32_t h,
                         uint32_t levels);

/* Counters for a report and the tests: textures scaled, texels made, and the
 * microseconds spent scaling. note() is for a backend, after it scaled. */
void nv2a_texscale_note(uint32_t w, uint32_t h, unsigned factor, uint64_t usec);
void nv2a_texscale_stats(uint64_t *textures, uint64_t *texels, uint64_t *usec);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEXSCALE_H */
