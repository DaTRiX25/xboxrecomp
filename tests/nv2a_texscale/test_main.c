/* nv2a_texscale: the 2D art scalers and the rule that picks what they scale,
 * on synthetic textures. No GPU. */
#include "nv2a_texscale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks, s_fail;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define W 16
#define H 12
#define A(c) ((c) >> 24)
#define R(c) (((c) >> 16) & 0xFFu)
#define G(c) (((c) >> 8) & 0xFFu)
#define B(c) ((c) & 0xFFu)

static int near(uint32_t a, uint32_t b, int tol)
{
    int s;
    for (s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > tol || d < -tol)
            return 0;
    }
    return 1;
}

/* An opaque white block (4..11, 3..8) on fully transparent texels of colour
 * `under`. */
static void block(uint32_t *t, uint32_t under)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = (x >= 4 && x < 12 && y >= 3 && y < 9) ? 0xFFFFFFFFu
                                                               : (under & 0x00FFFFFFu);
}

static void scaler(enum nv2a_texscale_algo algo, unsigned f)
{
    static uint32_t src[W * H], src2[W * H], d1[W * H * 16], d2[W * H * 16];
    const int OW = W * (int)f, OH = H * (int)f;
    int x, y, i, bad;

    /* One colour in, the same colour out, everywhere (edges included). */
    for (i = 0; i < W * H; i++)
        src[i] = 0xFF3C7AC8u;
    memset(d1, 0, sizeof d1);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += d1[i] != 0xFF3C7AC8u;
    CHECK(bad == 0);
    /* ... and a half-transparent one. */
    for (i = 0; i < W * H; i++)
        src[i] = 0x80FF8000u;
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += !near(d1[i], 0x80FF8000u, 1);
    CHECK(bad == 0);

    /* Alpha edges: white on transparent black. No visible texel is darker
     * than white (no fringe), the alpha stays within the block's reach, and
     * the transparent texels beside the block are white under their zero
     * alpha, so the GPU's bilinear filter has nothing dark to mix in. */
    block(src, 0x000000u);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    {
        int dark = 0, far_alpha = 0, inside = 0, bled = 0, kept = 0;
        for (y = 0; y < OH; y++) {
            for (x = 0; x < OW; x++) {
                uint32_t c = d1[y * OW + x];
                int sx = x / (int)f, sy = y / (int)f;
                if (A(c) && (R(c) < 250 || G(c) < 250 || B(c) < 250))
                    dark++;
                /* more than one source texel away from the block: nothing */
                if ((sx < 3 || sx > 12 || sy < 2 || sy > 9) && A(c))
                    far_alpha++;
                /* well inside the block: opaque */
                if (sx >= 6 && sx < 10 && sy >= 5 && sy < 7 && c != 0xFFFFFFFFu)
                    inside++;
                if (!A(c)) {
                    int nb = 0, dx, dy;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++)
                            if (x + dx >= 0 && x + dx < OW && y + dy >= 0 && y + dy < OH)
                                nb += A(d1[(y + dy) * OW + x + dx]) != 0;
                    if (nb && (c & 0x00FFFFFFu) != 0x00FFFFFFu)
                        bled++;
                    if (!nb && c != 0)
                        kept++;       /* the source's own colour, here black */
                }
            }
        }
        CHECK(dark == 0);
        CHECK(far_alpha == 0);
        CHECK(inside == 0);
        CHECK(bled == 0);
        CHECK(kept == 0);
        /* The block's coverage is kept: the alpha sums to the block's area. */
        {
            unsigned long sum = 0;
            for (i = 0; i < OW * OH; i++)
                sum += A(d1[i]);
            CHECK(sum > 255ul * 48 * f * f * 95 / 100 && sum < 255ul * 48 * f * f * 105 / 100);
        }
    }
    /* What colour the transparent texels have has no say in what is seen. */
    block(src2, 0xFF00FFu);
    CHECK(nv2a_texscale(src2, W, H, f, algo, d2) == 1);
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += A(d1[i]) != A(d2[i]) || (A(d1[i]) && d1[i] != d2[i]);
    CHECK(bad == 0);
    /* ... and far from anything visible they keep it (alpha 0). */
    CHECK(d2[0] == 0x00FF00FFu && d2[OW * OH - 1] == 0x00FF00FFu);

    /* Placement: the block is symmetric about the texture's middle both
     * ways, so the result is too. A scaler that leaves the picture a part of
     * a texel off its place fails this. */
    for (bad = 0, y = 0; y < OH; y++)
        for (x = 0; x < OW; x++)
            bad += !near(d1[y * OW + x], d1[y * OW + (OW - 1 - x)], 1)
                || !near(d1[y * OW + x], d1[(OH - 1 - y) * OW + x], 1);
    CHECK(bad == 0);

    /* Borders: an opaque red left column and nothing else. The right edge
     * stays empty (no wrap), the left edge is red to the texture's edge
     * (the edge texel repeats past it, it does not fade). */
    for (i = 0; i < W * H; i++)
        src[i] = (i % W) == 0 ? 0xFFFF0000u : 0u;
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, y = 0; y < OH; y++) {
        bad += A(d1[y * OW + OW - 1]) != 0;
        bad += d1[y * OW] != 0xFFFF0000u;
    }
    CHECK(bad == 0);

    /* A ramp stays a ramp: each source texel's block averages to the texel
     * (the result lies over the source, and nothing rings). */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            src[y * W + x] = 0xFF000000u | (uint32_t)(x * 16) << 16 | (uint32_t)(x * 16) << 8
                           | (uint32_t)(x * 16);
    CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
    for (bad = 0, x = 2; x < W - 2; x++) {
        int sum = 0, k;
        for (k = 0; k < (int)f; k++)
            sum += (int)B(d1[(OH / 2) * OW + x * (int)f + k]);
        sum = (sum + (int)f / 2) / (int)f;
        bad += sum < x * 16 - 2 || sum > x * 16 + 2;
    }
    CHECK(bad == 0);
    for (bad = 0, x = 1; x < OW; x++)
        bad += B(d1[(OH / 2) * OW + x]) < B(d1[(OH / 2) * OW + x - 1]);
    CHECK(bad == 0);                                   /* monotonic */
    for (bad = 0, i = 0; i < OW * OH; i++)
        bad += A(d1[i]) != 255;
    CHECK(bad == 0);                                   /* opaque stays opaque */

    /* Determinism: a noisy picture twice. */
    {
        uint32_t seed = 12345u;
        for (i = 0; i < W * H; i++) {
            seed = seed * 1664525u + 1013904223u;
            src[i] = seed;
        }
        CHECK(nv2a_texscale(src, W, H, f, algo, d1) == 1);
        CHECK(nv2a_texscale(src, W, H, f, algo, d2) == 1);
        CHECK(!memcmp(d1, d2, (size_t)OW * OH * 4));
        /* a visible texel's colour never exceeds what its neighbourhood has:
         * spot check that nothing is out of range (to8 clamps; alpha 0 texels
         * carry no alpha) */
        for (bad = 0, i = 0; i < OW * OH; i++)
            bad += A(d1[i]) == 0 && (d1[i] >> 24) != 0;
        CHECK(bad == 0);
    }
    /* A 1x1 and a 1-wide texture do not read outside themselves. */
    {
        uint32_t one = 0xC0102030u, o[16], col[5] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF000000u,
                                                      0xFFFFFFFFu, 0xFF000000u }, oc[5 * 16];
        CHECK(nv2a_texscale(&one, 1, 1, f, algo, o) == 1);
        for (bad = 0, i = 0; i < (int)(f * f); i++)
            bad += !near(o[i], one, 1);
        CHECK(bad == 0);
        CHECK(nv2a_texscale(col, 1, 5, f, algo, oc) == 1);
        for (bad = 0, y = 0; y < 5 * (int)f; y++)
            for (x = 1; x < (int)f; x++)
                bad += oc[y * (int)f + x] != oc[y * (int)f];
        CHECK(bad == 0);
    }
}

int main(void)
{
    static uint32_t src[W * H], dst[W * H * 16];
    unsigned i;

    /* What is not scaled: dst untouched. */
    memset(dst, 0x5A, sizeof dst);
    CHECK(nv2a_texscale(src, W, H, 3, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, W, H, 1, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, 0, H, 2, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    CHECK(nv2a_texscale(src, W, H, 2, NV2A_TEXSCALE_ALGOS, dst) == 0);
    CHECK(nv2a_texscale(NULL, W, H, 2, NV2A_TEXSCALE_SUPERXBR, dst) == 0);
    for (i = 0; i < W * H * 16; i++)
        if (dst[i] != 0x5A5A5A5Au)
            break;
    CHECK(i == W * H * 16);

    scaler(NV2A_TEXSCALE_SUPERXBR, 2);
    scaler(NV2A_TEXSCALE_SUPERXBR, 4);
    scaler(NV2A_TEXSCALE_BICUBIC, 2);
    scaler(NV2A_TEXSCALE_BICUBIC, 4);

    /* Super-xBR follows an edge: a diagonal step, black over white, comes
     * out as a straight diagonal (each row is the row above moved one texel
     * along). The bicubic leaves the source's stair on it. */
    {
        static uint32_t a[16 * 16], x4[64 * 64], b4[64 * 64];
        int x, y, step_x = 0, step_b = 0;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                a[y * 16 + x] = x > y ? 0xFFFFFFFFu : 0xFF000000u;
        CHECK(nv2a_texscale(a, 16, 16, 4, NV2A_TEXSCALE_SUPERXBR, x4) == 1);
        CHECK(nv2a_texscale(a, 16, 16, 4, NV2A_TEXSCALE_BICUBIC, b4) == 1);
        for (y = 12; y < 50; y++)
            for (x = 12; x < 50; x++) {
                step_x += !near(x4[y * 64 + x], x4[(y + 1) * 64 + x + 1], 12);
                step_b += !near(b4[y * 64 + x], b4[(y + 1) * 64 + x + 1], 12);
            }
        CHECK(step_x == 0);
        CHECK(step_b > 0);
        /* and the two sides are still black and white */
        CHECK(x4[40 * 64 + 8] == 0xFF000000u && x4[8 * 64 + 40] == 0xFFFFFFFFu);
    }

    /* The mip chain: 2x2 boxes weighted by alpha. */
    {
        uint32_t t[16 + 4 + 1];
        for (i = 0; i < 16; i++)
            t[i] = 0;
        t[0] = 0xFFFFFFFFu;              /* one white texel in a clear block */
        t[2] = 0xFF204060u; t[3] = 0xFF204060u; t[6] = 0xFF204060u; t[7] = 0xFF204060u;
        t[8] = 0x00112233u; t[9] = 0x00112233u; t[12] = 0x00112233u; t[13] = 0x00112233u;
        nv2a_texscale_mips(t, 4, 4, 3);
        CHECK(t[16] == 0x40FFFFFFu);     /* a quarter covered, still white */
        CHECK(t[17] == 0xFF204060u);
        CHECK(t[18] == 0x00112233u);     /* nothing visible: the colours' mean */
        CHECK(t[19] == 0);
        CHECK(A(t[20]) == (0x40 + 0xFF + 2) / 4);
    }

    /* The rule: a 2D draw. */
    {
        struct nv2a_texscale_draw d = { 0, 0, 0, 640, 480, 640, 480 };
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.program = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);       /* a program or not: no matter */
        d.frame_h = 464;                                /* the presented extent's crop */
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.frame_h = 480; d.target_h = 464;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 1);
        d.target_h = 480; d.depth_test = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* the world, particles, flares */
        d.depth_write = 1;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);
        d.depth_test = 0;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);
        d.depth_write = 0; d.target_w = 320; d.target_h = 240;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* an effect target */
        d.target_w = 128; d.target_h = 128;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* a reflection face */
        d.target_w = 640; d.target_h = 480; d.frame_w = d.frame_h = 0;
        CHECK(nv2a_texscale_draw_is_2d(&d) == 0);       /* before the first flip */
    }
    /* The rule: a texture. */
    CHECK(nv2a_texscale_tex_ok(1, 256, 256, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 512, 512, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 1024, 128, 1) == 1);
    CHECK(nv2a_texscale_tex_ok(1, 1024, 512, 1) == 0);  /* too large */
    CHECK(nv2a_texscale_tex_ok(1, 256, 256, 9) == 0);   /* mipmapped: the world's */
    CHECK(nv2a_texscale_tex_ok(0, 640, 352, 1) == 0);   /* linear: a video frame */
    CHECK(nv2a_texscale_tex_ok(1, 0, 16, 1) == 0);

    /* The setting: off unless asked; 2 and 4 only; a change moves the
     * generation on, the same value does not. */
    {
        uint32_t g0;
        uint64_t n, texels, usec;
        CHECK(nv2a_texscale_factor() == 0);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_SUPERXBR);
        g0 = nv2a_texscale_generation();
        CHECK(g0 != 0);
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_factor() == 2 && nv2a_texscale_generation() != g0);
        g0 = nv2a_texscale_generation();
        nv2a_texscale_set_factor(2);
        CHECK(nv2a_texscale_generation() == g0);
        nv2a_texscale_set_factor(4);
        CHECK(nv2a_texscale_factor() == 4);
        nv2a_texscale_set_factor(3);
        CHECK(nv2a_texscale_factor() == 0);
        g0 = nv2a_texscale_generation();
        nv2a_texscale_set_algo(NV2A_TEXSCALE_BICUBIC);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_BICUBIC && nv2a_texscale_generation() != g0);
        nv2a_texscale_set_algo(NV2A_TEXSCALE_ALGOS);
        CHECK(nv2a_texscale_algo() == NV2A_TEXSCALE_SUPERXBR);
        nv2a_texscale_stats(&n, &texels, &usec);
        CHECK(n == 0 && texels == 0);
        nv2a_texscale_note(128, 64, 4, 1500);
        nv2a_texscale_stats(&n, &texels, &usec);
        CHECK(n == 1 && texels == 128u * 64u * 16u && usec == 1500);
    }

    printf("nv2a_texscale: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
