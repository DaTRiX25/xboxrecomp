/**
 * nv2a_texrep.c - regions of a title's textures replaced at upload.
 * See nv2a_texrep.h.
 */
#include "nv2a_texrep.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LOGP "[TEXREP] "

static const struct nv2a_texrep_tex *s_tex;
static unsigned                      s_n;
static _Atomic int                   s_variant;
static _Atomic uint32_t              s_gen = 1;
static _Atomic uint64_t              s_hashed, s_matched, s_replaced;

static void gen_next(void)
{
    /* Never 0: 0 is "not a registered texture" in a backend's cache. */
    if (atomic_fetch_add_explicit(&s_gen, 1, memory_order_relaxed) == 0xFFFFFFFFu)
        atomic_store_explicit(&s_gen, 1, memory_order_relaxed);
}

void nv2a_texrep_register(const struct nv2a_texrep_tex *tex, unsigned n)
{
    s_tex = n ? tex : NULL;
    s_n = tex ? n : 0;
    gen_next();
}

void nv2a_texrep_set_variant(int v)
{
    if (v < 0)
        v = 0;
    if (atomic_exchange_explicit(&s_variant, v, memory_order_relaxed) != v)
        gen_next();
}

int nv2a_texrep_variant(void)
{
    return atomic_load_explicit(&s_variant, memory_order_relaxed);
}

uint32_t nv2a_texrep_generation(void)
{
    return atomic_load_explicit(&s_gen, memory_order_relaxed);
}

uint64_t nv2a_texrep_hash(const uint8_t *guest, uint32_t color, uint32_t w,
                          uint32_t h, uint32_t pitch)
{
    uint32_t n = nv2a_tex_extent(color, w, h, pitch, 1);
    return n ? nv2a_tex_hash(guest, n, NV2A_TEX_HASH_SEED) : 0;
}

void nv2a_texrep_stats(uint64_t *hashed, uint64_t *matched, uint64_t *replaced)
{
    if (hashed)
        *hashed = atomic_load_explicit(&s_hashed, memory_order_relaxed);
    if (matched)
        *matched = atomic_load_explicit(&s_matched, memory_order_relaxed);
    if (replaced)
        *replaced = atomic_load_explicit(&s_replaced, memory_order_relaxed);
}

/* ── the dump ─────────────────────────────────────────────────────────── */

#define DUMP_SEEN 4096
static uint64_t s_seen[DUMP_SEEN];
static unsigned s_n_seen;

static void tga_write(const char *path, uint32_t w, uint32_t h, const uint32_t *texels)
{
    uint8_t hdr[18] = { 0 };
    FILE *f = fopen(path, "wb");

    if (!f)
        return;
    hdr[2] = 2;                        /* true colour */
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28;                    /* top-left origin, 8 alpha bits */
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(texels, 4, (size_t)w * h, f);   /* A8R8G8B8 words: B G R A bytes */
    fclose(f);
}

static void dump(uint64_t hash, uint32_t color, uint32_t w, uint32_t h, uint32_t levels,
                 const uint32_t *texels)
{
    static const char *dir;
    static int init, mips;
    char path[1200];
    unsigned i;
    uint32_t l;
    FILE *f;

    if (!init) {
        init = 1;
        dir = recomp_env(RENV_TEXREP_DUMP);
        if (dir && !*dir)
            dir = NULL;
        mips = recomp_env(RENV_TEXREP_DUMP_MIPS) != NULL;
        if (dir)
            fprintf(stderr, LOGP "dumping every uploaded texture to %s\n", dir);
    }
    if (!dir || !hash || w > 4096 || h > 4096)
        return;
    for (i = 0; i < s_n_seen; i++)
        if (s_seen[i] == hash)
            return;
    if (s_n_seen < DUMP_SEEN)
        s_seen[s_n_seen++] = hash;
    snprintf(path, sizeof path, "%s/%016llx_%ux%u_f%02x.tga", dir,
             (unsigned long long)hash, w, h, color & 0xFFu);
    tga_write(path, w, h, texels);
    /* texrep_dump_mips: the title's own levels below it too, as decoded
     * (BO-171 compares them with levels made from level 0). */
    for (l = 1; mips && l < levels; l++) {
        texels += (size_t)nv2a_tex_level_dim(w, l - 1) * nv2a_tex_level_dim(h, l - 1);
        snprintf(path, sizeof path, "%s/%016llx_%ux%u_f%02x_m%u.tga", dir,
                 (unsigned long long)hash, w, h, color & 0xFFu, l);
        tga_write(path, nv2a_tex_level_dim(w, l), nv2a_tex_level_dim(h, l), texels);
    }
    snprintf(path, sizeof path, "%s/index.tsv", dir);
    f = fopen(path, "ab");
    if (f) {
        fprintf(f, "%016llx\t%u\t%u\t0x%02x\n", (unsigned long long)hash, w, h,
                color & 0xFFu);
        fclose(f);
    }
}

static int dump_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *d = recomp_env(RENV_TEXREP_DUMP);
        on = d && *d;
    }
    return on;
}

/* ── apply ────────────────────────────────────────────────────────────── */

static uint32_t box4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t o = 0;
    int s;
    for (s = 0; s < 32; s += 8)
        o |= ((((a >> s) & 0xFFu) + ((b >> s) & 0xFFu) + ((c >> s) & 0xFFu)
               + ((d >> s) & 0xFFu) + 2u) >> 2) << s;
    return o;
}

/* Levels 1.. from the one above each, 2x2 box (a level of width or height 1
 * repeats its only row or column). */
static void remip(uint32_t *texels, uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t *src = texels, l;
    for (l = 1; l < levels; l++) {
        uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, x, y;
        uint32_t *dst = src + (size_t)w * h;
        for (y = 0; y < dh; y++) {
            const uint32_t *r0 = src + (size_t)(h > 1 ? 2 * y : 0) * w;
            const uint32_t *r1 = src + (size_t)(h > 1 ? 2 * y + 1 : 0) * w;
            for (x = 0; x < dw; x++) {
                uint32_t x0 = w > 1 ? 2 * x : 0, x1 = w > 1 ? 2 * x + 1 : 0;
                dst[(size_t)y * dw + x] = box4(r0[x0], r0[x1], r1[x0], r1[x1]);
            }
        }
        src = dst;
        w = dw;
        h = dh;
    }
}

uint32_t nv2a_texrep_apply(const uint8_t *guest, uint32_t color, uint32_t w,
                           uint32_t h, uint32_t pitch, uint32_t levels,
                           uint32_t *texels)
{
    const struct nv2a_texrep_tex *tex = s_tex, *t = NULL;
    const struct nv2a_texrep_variant *var;
    unsigned n = s_n, i, cand = 0;
    uint32_t gen;
    uint64_t hash = 0;
    int v;

    for (i = 0; i < n; i++)
        if (tex[i].color == color && tex[i].width == w && tex[i].height == h)
            cand = 1;
    if (!cand && !dump_on())
        return 0;
    hash = nv2a_texrep_hash(guest, color, w, h, pitch);
    if (dump_on())
        dump(hash, color, w, h, levels, texels);
    if (!cand || !hash)
        return 0;
    atomic_fetch_add_explicit(&s_hashed, 1, memory_order_relaxed);
    for (i = 0; i < n && !t; i++)
        if (tex[i].color == color && tex[i].width == w && tex[i].height == h
                && tex[i].hash == hash)
            t = &tex[i];
    if (!t)
        return 0;
    atomic_fetch_add_explicit(&s_matched, 1, memory_order_relaxed);
    /* The generation first: a variant set while this runs leaves the entry
     * with the older generation, and the next bind uploads it again. */
    gen = nv2a_texrep_generation();
    v = nv2a_texrep_variant();
    if (v < 1 || (unsigned)v > t->n_variants)
        return gen;
    var = &t->variants[v - 1];
    if (!var->image || !var->n_rects)
        return gen;
    for (i = 0; i < var->n_rects; i++) {
        struct nv2a_texrep_rect r = var->rects[i];
        uint32_t y;
        if (r.x >= w || r.y >= h)
            continue;
        if ((uint32_t)r.x + r.w > w)
            r.w = (uint16_t)(w - r.x);
        if ((uint32_t)r.y + r.h > h)
            r.h = (uint16_t)(h - r.y);
        for (y = r.y; y < (uint32_t)r.y + r.h; y++)
            memcpy(texels + (size_t)y * w + r.x, var->image + (size_t)y * w + r.x,
                   (size_t)r.w * 4);
    }
    if (levels > 1)
        remip(texels, w, h, levels);
    if (atomic_fetch_add_explicit(&s_replaced, 1, memory_order_relaxed) < 64)
        fprintf(stderr, LOGP "%s (%ux%u, %016llx): variant %d, %u regions\n",
                t->name ? t->name : "texture", w, h, (unsigned long long)hash, v,
                var->n_rects);
    return gen;
}
