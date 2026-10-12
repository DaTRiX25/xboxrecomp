/**
 * nv2a_texrep.h - regions of a title's textures replaced at upload.
 *
 * The idea is Dolphin's Dynamic Input Textures: a texture of the title is
 * known by the hash of its bytes, and before a GPU backend uploads it, images
 * the host program brings are copied over rectangles of it. The first use is
 * the title's button prompts drawn for the pad in the player's hands
 * (input.glyphs: the title's own Xbox discs, or PlayStation, Nintendo, Steam
 * Deck ones).
 *
 * The title registers its textures once, at start (nv2a_texrep_register):
 * each with the hash of its level-0 guest bytes (nv2a_texrep_hash), its
 * format and size, and for each variant an image of the texture's own size
 * and the rectangles of it that go over the title's texels. Variant 0 is the
 * title's own picture: nothing is replaced and the upload is what it was.
 *
 * A backend calls nv2a_texrep_apply() between its decode (nv2a_tex_decode,
 * A8R8G8B8) and its upload. A texture of another format or size than every
 * registered one costs a few integer compares and is not hashed; a candidate
 * is hashed once per upload (the backends upload a texture when it is first
 * drawn and when its bytes change, not per frame). The backend keeps the
 * generation apply() returned with its cache entry and uploads the texture
 * again when nv2a_texrep_generation() has moved on: that is how a change of
 * variant reaches a texture that is already on the GPU.
 *
 * No OS or graphics headers: shared by the Metal, Vulkan and D3D11 backends.
 */
#ifndef NV2A_TEXREP_H
#define NV2A_TEXREP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nv2a_texrep_rect {
    uint16_t x, y, w, h;
};

/* One variant's picture of one texture. image: width x height A8R8G8B8,
 * row-major (the decode's layout); NULL: this variant keeps the title's
 * texels. Only the rectangles are copied, all four channels. */
struct nv2a_texrep_variant {
    const uint32_t                *image;
    const struct nv2a_texrep_rect *rects;
    unsigned                       n_rects;
};

struct nv2a_texrep_tex {
    const char *name;                 /* for the log */
    uint64_t    hash;                 /* nv2a_texrep_hash of the title's bytes */
    uint32_t    color;                /* the NV2A colour format (0x0F: DXT45) */
    uint32_t    width, height;
    /* variants[v - 1] is variant v (v >= 1); a variant past n_variants keeps
     * the title's texels. */
    const struct nv2a_texrep_variant *variants;
    unsigned    n_variants;
};

/* The textures, kept by pointer (static tables). Replaces an earlier call;
 * (NULL, 0) forgets them. Call before the first draw, or under the title's
 * own care that no backend is uploading. */
void nv2a_texrep_register(const struct nv2a_texrep_tex *tex, unsigned n);

/* The variant in use (0: the title's own). Any thread; a change moves the
 * generation on. */
void     nv2a_texrep_set_variant(int v);
int      nv2a_texrep_variant(void);
/* Never 0; changes whenever what apply() would do changes. */
uint32_t nv2a_texrep_generation(void);

/* The hash a registered texture is known by: its level-0 guest bytes
 * (nv2a_tex_extent of one level), whatever number of levels a backend
 * samples. 0 for a format with no extent. */
uint64_t nv2a_texrep_hash(const uint8_t *guest, uint32_t color, uint32_t w,
                          uint32_t h, uint32_t pitch);

/* texels: the decoded texture, nv2a_tex_texels(w, h, levels) entries, level 0
 * first. If (color, w, h) and the hash of guest name a registered texture:
 * the variant's rectangles are copied over level 0, the further levels are
 * made again from it (2x2 box), and the generation is returned (also for
 * variant 0, which copies nothing: the entry must be uploaded again when the
 * variant changes). 0: not a registered texture, texels untouched. */
uint32_t nv2a_texrep_apply(const uint8_t *guest, uint32_t color, uint32_t w,
                           uint32_t h, uint32_t pitch, uint32_t levels,
                           uint32_t *texels);

/* Counters, for a report and the tests: candidates hashed, textures matched,
 * textures a variant changed. */
void nv2a_texrep_stats(uint64_t *hashed, uint64_t *matched, uint64_t *replaced);

/* RECOMP_DEBUG=texrep_dump=<dir> (read by apply): every texture a backend
 * uploads, once per content, as <dir>/<hash>_<w>x<h>_f<color>.tga (level 0,
 * A8R8G8B8 as decoded, before any replacement), with one line per file in
 * <dir>/index.tsv. The hash is nv2a_texrep_hash, the one to register. A
 * debug aid for finding a title's textures: the files are the title's own
 * pictures and stay on the machine that made them. With it on every upload
 * is hashed. */

#ifdef __cplusplus
}
#endif

#endif /* NV2A_TEXREP_H */
