/**
 * nv2a_backend_smoke: a GPU render backend against the CPU rasteriser, on a
 * POSIX host, frame by frame.
 *
 * The POSIX, backend-neutral successor of tests/d3d11_backend_smoke for the
 * scenes a backend draws first. Each scene is a hand-written method stream
 * (surface, clear, quads, FLIP_STALL) sent to nv2a_pb_exec_method() twice:
 * once with the CPU rasteriser, once with the backend under test. Both leave
 * the presented frame in guest memory -- the CPU path draws there, a GPU
 * backend writes its present surface back at the flip -- so the comparison
 * is the same bytes RECOMP_FB_DUMP and the SDL2 window read.
 *
 * Scenes (M1 of render-gpu-backend §4):
 *   clear_quad   colour clear + a solid quad (pre-transformed, INLINE_ARRAY)
 *   checker      + a swizzled A8R8G8B8 checker, stage 0, wrap addressing
 *   yuy2         + a linear YUY2 ramp (the movie texture's format), clamp
 *   blend        + an alpha-blended quad over the checker (SRC_ALPHA,
 *                  ONE_MINUS_SRC_ALPHA)
 *
 * Pass: per scene, an exact match. Every scene is screen-space with nearest
 * sampling, so the two paths compute the same bytes; the golden-limit
 * statistics (scripts/golden.py: MAE per channel, share off by more than 8)
 * are printed alongside. The presented surface must also be the one drawn.
 *
 * Backend under test: RECOMP_SMOKE_BACKEND, default "metal" (the only one
 * that writes back on POSIX). RECOMP_SMOKE_DUMP=<prefix> writes
 * <prefix><scene>_{cpu,gpu}.bmp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "d3d8_swizzle.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
#ifdef __APPLE__
const struct nv2a_pb_backend *nv2a_pb_metal_backend(void);
#endif

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define W 640u
#define H 480u
#define BUF0      0x01000000u
#define BUF1      0x0112C000u
#define TEX_GUEST 0x02800000u   /* 64x64 swizzled A8R8G8B8, then YUY2 64x32 */
#define TEX_LOG2  6

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* Inline vertex: attr0 float4 position (pixels), attr3 D3DCOLOR, and with
 * `uv`, attr9 float2 texture coordinate. */
static void vertex_uv(float x, float y, uint32_t argb, const float *uv)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(0.0f));
    M(0x1818, f2u(1.0f));
    M(0x1818, argb);
    if (uv) {
        M(0x1818, f2u(uv[0]));
        M(0x1818, f2u(uv[1]));
    }
}

static void quad_uv(float x0, float y0, float x1, float y1, const uint32_t c[4],
                    const float uv[4][2])
{
    M(0x17FC, 8);                          /* BEGIN_END: QUADS */
    vertex_uv(x0, y0, c[0], uv ? uv[0] : NULL);
    vertex_uv(x1, y0, c[1], uv ? uv[1] : NULL);
    vertex_uv(x1, y1, c[2], uv ? uv[2] : NULL);
    vertex_uv(x0, y1, c[3], uv ? uv[3] : NULL);
    M(0x17FC, 0);
}

/* The swizzled checker (8x8-texel orange/blue squares, white top row, green
 * left column so orientation shows) and the YUY2 ramp after it: luma left
 * to right, Cr top to bottom. The same images as d3d11_backend_smoke. */
static void make_textures(void)
{
    uint32_t n = 1u << TEX_LOG2, x, y;
    uint32_t *p = (uint32_t *)guest(TEX_GUEST);
    uint8_t *y8 = guest(TEX_GUEST) + n * n * 4;

    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            uint32_t c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFF2040FFu : 0xFFFF8000u;
            if (y == 0) c = 0xFFFFFFFFu;
            if (x == 0) c = 0xFF00FF00u;
            p[swizzle_offset(x, y, n, n)] = c;
        }
    for (y = 0; y < 32; y++)
        for (x = 0; x < 64; x += 2) {
            uint8_t *g = y8 + y * 128 + x * 2;
            g[0] = (uint8_t)(16 + x * 219 / 63);
            g[1] = 128;
            g[2] = (uint8_t)(16 + (x + 1) * 219 / 63);
            g[3] = (uint8_t)(128 + y * 3);
        }
}

enum { SC_CLEAR_QUAD, SC_CHECKER, SC_YUY2, SC_BLEND, SC_COUNT };
static const char *const s_scene_name[SC_COUNT] = {"clear_quad", "checker", "yuy2", "blend"};

static void stage0_checker(void)
{
    M(0x1B00, TEX_GUEST);                  /* TEXTURE_OFFSET(0) */
    M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
              | (1u << 4) | (2u << 0));    /* A8R8G8B8, 2D, 64x64 */
    M(0x1B08, 0x00000101u);                /* wrap u, wrap v */
    M(0x1B0C, 0x40000000u);                /* CONTROL0: enable */
}

/* One scene into the surface at `buf`, then the flip. */
static void scene(int sc, uint32_t buf, uint32_t other)
{
    /* One colour per quad: the CPU rasteriser's screen-space path shades
     * flat (the first vertex's colour), so a Gouraud quad would measure the
     * reference, not the backend. */
    static const uint32_t solid[4] = {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u};
    static const uint32_t white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    int a;

    M(0x0200, W << 16);                    /* SURFACE_CLIP_HORIZONTAL */
    M(0x0204, H << 16);                    /* SURFACE_CLIP_VERTICAL   */
    M(0x0208, 0x00000128u);                /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    M(0x020C, W * 4u);                     /* SURFACE_PITCH           */
    M(0x0210, buf);                        /* SURFACE_COLOR_OFFSET    */
    M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
    M(0x1D90, 0xFF603010u);                /* COLOR_CLEAR_VALUE       */
    M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */
    M(0x0304, 0);                          /* BLEND_ENABLE off        */
    M(0x1B0C, 0);                          /* stage 0 off */
    for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
        M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);

    quad_uv(80.0f, 120.0f, 240.0f, 360.0f, solid, NULL);
    /* Fractional edges: a half-pixel offset in either rasteriser (pixel
     * centres at .5, the top-left rule) moves a whole row or column here. */
    {
        static const uint32_t green[4] = {0xFF00C040u, 0xFF00C040u, 0xFF00C040u, 0xFF00C040u};
        quad_uv(80.3f, 380.5f, 239.6f, 460.4f, green, NULL);
    }

    if (sc >= SC_CHECKER) {
        static const float uv[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
        stage0_checker();
        M(0x1760 + 4 * 9, 0x22u);          /* attr9: float2 */
        quad_uv(440.0f, 40.0f, 620.0f, 220.0f, white, uv);
    }
    if (sc >= SC_YUY2) {
        static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 32}, {0, 32}};
        M(0x1B00, TEX_GUEST + (4u << (2 * TEX_LOG2)));
        M(0x1B04, (0x24u << 8) | (1u << 4) | (2u << 0)); /* LC_CR8YB8CB8YA8 */
        M(0x1B08, 0x00000303u);            /* clamp */
        M(0x1B0C, 0x40000000u);
        M(0x1B10, 128u << 16);             /* CONTROL1: pitch */
        M(0x1B1C, (64u << 16) | 32u);      /* IMAGE_RECT */
        M(0x1760 + 4 * 9, 0x22u);
        quad_uv(440.0f, 260.0f, 620.0f, 350.0f, white, uv);
    }
    if (sc >= SC_BLEND) {
        /* A half-transparent quad (alpha 0x80) across the solid quad and
         * the background; untextured. */
        static const uint32_t c[4] = {0x8000FFFFu, 0x8000FFFFu, 0x8000FFFFu, 0x8000FFFFu};
        M(0x1B0C, 0);
        M(0x1760 + 4 * 9, 0x02u);
        M(0x0304, 1);                      /* BLEND_ENABLE */
        M(0x0344, 0x0302);                 /* SFACTOR: SRC_ALPHA */
        M(0x0348, 0x0303);                 /* DFACTOR: ONE_MINUS_SRC_ALPHA */
        M(0x0350, 0x8006);                 /* EQUATION: ADD */
        quad_uv(160.0f, 200.0f, 400.0f, 300.0f, c, NULL);
        M(0x0304, 0);
    }
    M(0x1760 + 4 * 9, 0x02u);
    M(0x1B0C, 0);

    M(0x0210, other);                      /* next buffer, as the XDK does */
    M(0x0130, 0);                          /* FLIP_STALL */
}

static void write_bmp(const char *path, const uint32_t *px)
{
    uint32_t row = W * 3, filesz = 54 + row * H, y, x;
    uint8_t hdr[54];
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    { uint32_t w = W, h = H; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); }
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = H; y-- > 0; )
        for (x = 0; x < W; x++) {
            uint32_t v = px[y * W + x];
            uint8_t bgr[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

/* Run scene sc with backend b; its frame into out (W*H X8R8G8B8). The
 * surface is filled with a pattern first, so a backend that writes nothing
 * back cannot pass on the CPU path's leftovers. */
static int run(const struct nv2a_pb_backend *b, int sc, uint32_t *out, int k)
{
    uint32_t buf = (k & 1) ? BUF1 : BUF0, other = (k & 1) ? BUF0 : BUF1, i;
    uint32_t *g = (uint32_t *)guest(buf);
    const struct nv2a_pb_present *p;

    for (i = 0; i < W * H; i++)
        g[i] = 0xFFFF00FFu;
    nv2a_pb_set_backend(b);
    scene(sc, buf, other);
    p = nv2a_pb_present_state();
    if (p->offset != buf) {
        fprintf(stderr, "[backend-smoke] %s: flip presented 0x%08X, drew 0x%08X\n",
                s_scene_name[sc], p->offset, buf);
        return 0;
    }
    memcpy(out, g, W * H * 4);
    return 1;
}

static int compare(const char *name, const uint32_t *a, const uint32_t *b)
{
    const int tol = 8;
    uint64_t sum[3] = {0, 0, 0};
    uint32_t bad = 0, i, maxd[3] = {0, 0, 0};
    double mae[3], bad_frac;
    int c, ok;

    for (i = 0; i < W * H; i++) {
        int off = 0;
        for (c = 0; c < 3; c++) {
            int d = (int)((a[i] >> (8 * c)) & 0xFF) - (int)((b[i] >> (8 * c)) & 0xFF);
            if (d < 0) d = -d;
            sum[c] += (uint64_t)d;
            if ((uint32_t)d > maxd[c]) maxd[c] = (uint32_t)d;
            if (d > tol) off = 1;
        }
        bad += (uint32_t)off;
    }
    for (c = 0; c < 3; c++)
        mae[c] = (double)sum[c] / (double)(W * H);
    bad_frac = (double)bad / (double)(W * H);
    ok = maxd[0] == 0 && maxd[1] == 0 && maxd[2] == 0;   /* exact */
    fprintf(stderr, "[backend-smoke] %-10s %s: mae b/g/r %.3f/%.3f/%.3f max %u/%u/%u"
            " bad %.4f%% at tol %d (exact match required)\n", name,
            ok ? "PASS" : "FAIL", mae[0], mae[1], mae[2], maxd[0], maxd[1], maxd[2],
            100.0 * bad_frac, tol);
    return ok;
}

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

int main(void)
{
    static uint8_t xbe[0x1000];
    const char *which = getenv("RECOMP_SMOKE_BACKEND");
    const char *dump = getenv("RECOMP_SMOKE_DUMP");
    const struct nv2a_pb_backend *gpu = NULL;
    uint32_t *cpu_px, *gpu_px;
    int sc, k = 0, failures = 0;

    setvbuf(stderr, NULL, _IONBF, 0);
    if (!which)
        which = "metal";
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[backend-smoke] FAIL: guest memory init\n");
        return 1;
    }
#ifdef __APPLE__
    if (!strcasecmp(which, "metal"))
        gpu = nv2a_pb_metal_backend();
#endif
    if (!gpu) {
        fprintf(stderr, "[backend-smoke] no backend '%s' on this host: skipped\n", which);
        return 77;
    }
    cpu_px = (uint32_t *)malloc(W * H * 4);
    gpu_px = (uint32_t *)malloc(W * H * 4);
    if (!cpu_px || !gpu_px)
        return 1;
    make_textures();

    for (sc = 0; sc < SC_COUNT; sc++) {
        char path[512];
        if (!run(nv2a_pb_cpu_backend(), sc, cpu_px, k++)
                || !run(gpu, sc, gpu_px, k++)) {
            failures++;
            continue;
        }
        if (dump) {
            snprintf(path, sizeof path, "%s%s_cpu.bmp", dump, s_scene_name[sc]);
            write_bmp(path, cpu_px);
            snprintf(path, sizeof path, "%s%s_gpu.bmp", dump, s_scene_name[sc]);
            write_bmp(path, gpu_px);
        }
        if (!compare(s_scene_name[sc], cpu_px, gpu_px))
            failures++;
    }
    nv2a_pb_set_backend(NULL);
    if (failures) {
        fprintf(stderr, "[backend-smoke] FAIL: %d of %d scenes\n", failures, SC_COUNT);
        return 1;
    }
    fprintf(stderr, "[backend-smoke] done: %d scenes identical on both paths\n", SC_COUNT);
    return 0;
}
