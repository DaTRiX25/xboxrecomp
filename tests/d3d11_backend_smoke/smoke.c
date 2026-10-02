/**
 * d3d11_backend_smoke: the pushbuffer executor, drawing through D3D11.
 *
 * Sends nv2a_pb_exec_method() what a title's pushbuffer would: a 640x480
 * surface, a colour clear, one pre-transformed quad as INLINE_ARRAY (float4
 * position in pixels + D3DCOLOR), then the flip, double-buffered the way the
 * XDK does it (color_offset moves to the other buffer before FLIP_STALL).
 * With RECOMP_PB_BACKEND=d3d11 that reaches ClearRenderTargetView, a draw
 * and Present in src/d3d/nv2a_pb_d3d11.c.
 *
 * Usage (under Proton: umu-run d3d11_backend_smoke.exe [frames]):
 *   RECOMP_STDIO_LOG=<file>      stderr goes here (Proton drops stdio)
 *   RECOMP_D3D11_DUMP=<prefix>   presented frames 1, 61, ... as BMPs
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_swizzle.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* The kernel library expects these from the title's generated code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

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

static void vertex(float x, float y, uint32_t argb)
{
    vertex_uv(x, y, argb, NULL);
}

/* Guest memory for the texture: the runtime's memory offset is 0 here, so a
 * guest address is a host address, and it has to fit in 32 bits. */
#define TEX_GUEST 0x10000000u
#define TEX_LOG2  6                            /* 64x64 */

/* A swizzled A8R8G8B8 texture: 8x8-texel checker, orange and blue, with a
 * white top row and a green left column so orientation shows. */
static int make_texture(void)
{
    uint32_t n = 1u << TEX_LOG2, x, y;
    uint32_t *p = (uint32_t *)VirtualAlloc((void *)(uintptr_t)TEX_GUEST,
                                           n * n * 4 + 128 * 32, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
    if (!p)
        return 0;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            uint32_t c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFF2040FFu : 0xFFFF8000u;
            if (y == 0) c = 0xFFFFFFFFu;
            if (x == 0) c = 0xFF00FF00u;
            p[swizzle_offset(x, y, n, n)] = c;
        }
    /* A linear YUY2 image (the movie texture's format), 64x32 at pitch 128,
     * right after it: luma ramps left to right, Cr top to bottom, so it
     * goes from black to white with red growing downwards. */
    {
        uint8_t *y8 = (uint8_t *)p + n * n * 4;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x += 2) {
                uint8_t *g = y8 + y * 128 + x * 2;
                g[0] = (uint8_t)(16 + x * 219 / 63);           /* Y0 */
                g[1] = 128;                                     /* Cb */
                g[2] = (uint8_t)(16 + (x + 1) * 219 / 63);     /* Y1 */
                g[3] = (uint8_t)(128 + y * 3);                  /* Cr */
            }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *log = getenv("RECOMP_STDIO_LOG");
    int frames = argc > 1 ? atoi(argv[1]) : 180;
    uint32_t buf[2] = {0x01000000u, 0x0112C000u};
    int f, a;

    if (log)
        freopen(log, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!getenv("RECOMP_PB_BACKEND"))
        _putenv("RECOMP_PB_BACKEND=d3d11");
    fprintf(stderr, "[backend-smoke] %d frames\n", frames);
    if (!make_texture())
        fprintf(stderr, "[backend-smoke] no texture memory at 0x%08X\n", TEX_GUEST);

    for (f = 0; f < frames; f++) {
        float t = (float)(f % 120) / 120.0f;
        float x = 80.0f + 320.0f * t, y = 120.0f;
        uint32_t clear = 0xFF000000u | ((uint32_t)(255 * t) << 16) | 0x30u;

        M(0x0200, 640u << 16);                 /* SURFACE_CLIP_HORIZONTAL */
        M(0x0204, 480u << 16);                 /* SURFACE_CLIP_VERTICAL   */
        M(0x0208, 0x00000128u);                /* SURFACE_FORMAT          */
        M(0x020C, 640u * 4u);                  /* SURFACE_PITCH           */
        M(0x0210, buf[f & 1]);                 /* SURFACE_COLOR_OFFSET    */
        M(0x1D90, clear);                      /* COLOR_CLEAR_VALUE       */
        M(0x1D94, 0xF0);                       /* CLEAR_SURFACE: colour   */

        for (a = 0; a < 16; a++)               /* VERTEX_DATA_ARRAY_FORMAT */
            M(0x1760 + 4 * a, a == 0 ? 0x42u : a == 3 ? 0x40u : 0x02u);
        M(0x17FC, 8);                          /* BEGIN_END: QUADS */
        vertex(x,          y,          0xFFFF0000u);
        vertex(x + 160.0f, y,          0xFF00FF00u);
        vertex(x + 160.0f, y + 240.0f, 0xFF0000FFu);
        vertex(x,          y + 240.0f, 0xFFFFFFFFu);
        M(0x17FC, 0);                          /* BEGIN_END: end */

        /* Stage 0: the swizzled texture; a textured quad on the right,
         * diffuse white so the texels show unmodified. */
        M(0x1B00, TEX_GUEST);                  /* TEXTURE_OFFSET(0)  */
        M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                  | (1u << 4) | (2u << 0));    /* FORMAT: A8R8G8B8, 2D, 64x64 */
        M(0x1B08, 0x00000101u);                /* ADDRESS: wrap u, wrap v */
        M(0x1B0C, 0x40000000u);                /* CONTROL0: enable */
        M(0x1760 + 4 * 9, 0x22u);              /* attr9: float2 */
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
            vertex_uv(440.0f, 40.0f,  0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 40.0f,  0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 220.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(440.0f, 220.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);

        /* The YUY2 image, linear: size from IMAGE_RECT, pitch from CONTROL1,
         * and texture coordinates in texels. */
        M(0x1B00, TEX_GUEST + (4u << (2 * TEX_LOG2)));
        M(0x1B04, (0x24u << 8) | (1u << 4) | (2u << 0)); /* LC_CR8YB8CB8YA8 */
        M(0x1B08, 0x00000303u);                /* clamp */
        M(0x1B10, 128u << 16);                 /* CONTROL1: pitch */
        M(0x1B1C, (64u << 16) | 32u);          /* IMAGE_RECT */
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {64, 0}, {64, 32}, {0, 32}};
            vertex_uv(440.0f, 260.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 260.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 350.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(440.0f, 350.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);
        M(0x1760 + 4 * 9, 0x02u);              /* attr9 off again */
        M(0x1B0C, 0);                          /* stage 0 off */

        M(0x0210, buf[(f + 1) & 1]);           /* next buffer, as the XDK does */
        M(0x0130, 0);                          /* FLIP_STALL */
        Sleep(16);
    }
    fprintf(stderr, "[backend-smoke] done\n");
    return 0;
}
