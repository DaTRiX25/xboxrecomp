/**
 * d3d11_backend_smoke: the pushbuffer executor, drawing through D3D11.
 *
 * Sends nv2a_pb_exec_method() what a title's pushbuffer would: a 640x480
 * surface, a colour clear, one pre-transformed quad as INLINE_ARRAY (float4
 * position in pixels + D3DCOLOR), then the flip, double-buffered the way the
 * XDK does it (color_offset moves to the other buffer before FLIP_STALL).
 * Then textured quads (a swizzled checker, a linear YUY2 ramp) and a quad
 * through four register-combiner stages with the title's final combiner,
 * and three overlapping quads with depth tests.
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

/* An untextured vertex at depth z, in depth units (0..2^24-1 for Z24S8). */
static void vertex_z(float x, float y, float z, uint32_t argb)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(z));
    M(0x1818, f2u(1.0f));
    M(0x1818, argb);
}

/* Guest memory for the texture: the runtime's memory offset is 0 here, so a
 * guest address is a host address, and it has to fit in 32 bits. */
#define TEX_GUEST 0x10000000u
#define OFFSCREEN 0x01800000u   /* a 320x240 render target */
#define P8_GUEST  (TEX_GUEST + 0x8000u)
#define PAL_GUEST (TEX_GUEST + 0x9000u)
#define TEX_LOG2  6                            /* 64x64 */

/* A swizzled A8R8G8B8 texture: 8x8-texel checker, orange and blue, with a
 * white top row and a green left column so orientation shows. */
static int make_texture(void)
{
    uint32_t n = 1u << TEX_LOG2, x, y;
    uint32_t *p = (uint32_t *)VirtualAlloc((void *)(uintptr_t)TEX_GUEST,
                                           0x10000, MEM_RESERVE | MEM_COMMIT,
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
    /* A swizzled P8 texture, 16x16, at P8_GUEST, and its palette at
     * PAL_GUEST: 4x4-texel diagonal bands through entries 0..3 (red,
     * yellow, cyan, white); the other 252 entries dark grey. */
    {
        uint8_t *i8 = (uint8_t *)(uintptr_t)P8_GUEST;
        uint32_t *pal = (uint32_t *)(uintptr_t)PAL_GUEST;
        static const uint32_t band[4] = {0xFFFF0000u, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFFFFFFu};
        for (x = 0; x < 256; x++)
            pal[x] = x < 4 ? band[x] : 0xFF404040u;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                i8[swizzle_offset(x, y, 16, 16)] = (uint8_t)(((x >> 2) + (y >> 2)) & 3);
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
        M(0x0214, 0x02400000u);                /* SURFACE_ZETA_OFFSET     */
        M(0x1D90, clear);                      /* COLOR_CLEAR_VALUE       */
        M(0x1D8C, 0xFFFFFF00u);                /* ZSTENCIL_CLEAR_VALUE: far */
        M(0x1D94, 0xF1);                       /* CLEAR_SURFACE: colour + Z */

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

        /* Register combiners: the title's control word and final combiner
         * (ctl 0x11104 and fcw 0x130C0300/0x00001C80 from the title-screen
         * capture: four stages, mux on R0.a's MSB, per-stage constants,
         * final = fog.a * R0 + (1 - fog.a) * fog, alpha R0.a, V1+R0 clamped)
         * with four stages of our own that exercise them:
         *   0: R0 = T0 * V0 (rgb and alpha)
         *   1: R1 = mux(R0 * 1, C0[1] * 1): R0.a >= 0.5 picks C0[1]
         *   2: R0 = R1 * C1[2], output x2 (C1[2] = 0.5 so the colour holds)
         *   3: R0 = R0 * 1
         * The quad's diffuse alpha goes 0 -> 1 left to right, so the left
         * half shows the checker and the right half magenta. */
        M(0x1B00, TEX_GUEST);
        M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                  | (1u << 4) | (2u << 0));
        M(0x1B08, 0x00000101u);
        M(0x1E70, 1);                          /* SHADER_STAGE_PROGRAM: T0 2D */
        {
            static const uint32_t cicw[4] = {0x08040000u, 0x0C200120u, 0x0D020000u, 0x0C200000u};
            static const uint32_t cocw[4] = {0x000000C0u, 0x00004D00u, 0x000100C0u, 0x000000C0u};
            static const uint32_t aicw[4] = {0x18140000u, 0x1C200000u, 0x1D200000u, 0x1C200000u};
            static const uint32_t aocw[4] = {0x000000C0u, 0x00000D00u, 0x000000C0u, 0x000000C0u};
            static const float uv[4][2] = {{0, 0}, {4, 0}, {4, 1}, {0, 1}};
            int s;
            for (s = 0; s < 4; s++) {
                M(0x0AC0 + 4 * s, cicw[s]);    /* COMBINER_COLOR_ICW */
                M(0x1E40 + 4 * s, cocw[s]);    /* COMBINER_COLOR_OCW */
                M(0x0260 + 4 * s, aicw[s]);    /* COMBINER_ALPHA_ICW */
                M(0x0AA0 + 4 * s, aocw[s]);    /* COMBINER_ALPHA_OCW */
                M(0x0A60 + 4 * s, s == 1 ? 0xFFFF00FFu : 0xFF000000u); /* FACTOR0 */
                M(0x0A80 + 4 * s, s == 2 ? 0x80808080u : 0xFF000000u); /* FACTOR1 */
            }
            M(0x02A8, 0xFF203040u);            /* FOG_COLOR */
            M(0x0288, 0x130C0300u);            /* SPECULAR_FOG_CW0 */
            M(0x028C, 0x00001C80u);            /* SPECULAR_FOG_CW1 */
            M(0x1E60, 0x00011104u);            /* COMBINER_CONTROL */
            M(0x17FC, 8);
            vertex_uv(20.0f,  380.0f, 0x00FFFFFFu, uv[0]);
            vertex_uv(420.0f, 380.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(420.0f, 470.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(20.0f,  470.0f, 0x00FFFFFFu, uv[3]);
            M(0x17FC, 0);
            M(0x1E60, 0);                      /* combiners off: 0 stages */
        }

        M(0x1760 + 4 * 9, 0x02u);              /* attr9 off again */
        M(0x1B0C, 0);                          /* stage 0 off */

        /* Depth: LESS with writes on. The near green quad goes first, then
         * a far red one overlapping it, which must stay behind: green wins
         * where they overlap. Then a blue quad between them in depth with
         * writes off and GREATER: it shows only over the green one. */
        M(0x1E70, 0);                          /* no texture stages */
        M(0x030C, 1);                          /* DEPTH_TEST_ENABLE */
        M(0x0354, 0x201);                      /* DEPTH_FUNC: LESS */
        M(0x035C, 1);                          /* DEPTH_MASK: write */
        M(0x17FC, 8);
        vertex_z(440.0f, 370.0f, 4194304.0f, 0xFF00C000u);   /* z 0.25 */
        vertex_z(560.0f, 370.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(560.0f, 450.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(440.0f, 450.0f, 4194304.0f, 0xFF00C000u);
        vertex_z(500.0f, 390.0f, 12582912.0f, 0xFFE00000u);  /* z 0.75 */
        vertex_z(620.0f, 390.0f, 12582912.0f, 0xFFE00000u);
        vertex_z(620.0f, 470.0f, 12582912.0f, 0xFFE00000u);
        vertex_z(500.0f, 470.0f, 12582912.0f, 0xFFE00000u);
        M(0x17FC, 0);
        M(0x0354, 0x204);                      /* DEPTH_FUNC: GREATER */
        M(0x035C, 0);                          /* no writes */
        M(0x17FC, 8);
        vertex_z(470.0f, 380.0f, 8388608.0f, 0xFF2040FFu);   /* z 0.5 */
        vertex_z(600.0f, 380.0f, 8388608.0f, 0xFF2040FFu);
        vertex_z(600.0f, 420.0f, 8388608.0f, 0xFF2040FFu);
        vertex_z(470.0f, 420.0f, 8388608.0f, 0xFF2040FFu);
        M(0x17FC, 0);
        M(0x030C, 0);                          /* depth test off */
        M(0x1E70, 1);                          /* stage 0 2D again */

        /* Render to texture. The top-left inset samples the offscreen
         * surface (as a linear A8R8G8B8 texture at its address, texel
         * coordinates) that the previous frame drew below: a backend that
         * decodes guest memory instead shows whatever is there. */
        M(0x1B00, OFFSCREEN);
        M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0)); /* LU_IMAGE_A8R8G8B8 */
        M(0x1B08, 0x00000303u);
        M(0x1B0C, 0x40000000u);
        M(0x1B10, (320u * 4u) << 16);
        M(0x1B1C, (320u << 16) | 240u);
        M(0x1760 + 4 * 9, 0x22u);
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {320, 0}, {320, 240}, {0, 240}};
            vertex_uv(20.0f,  20.0f,  0xFFFFFFFFu, uv[0]);
            vertex_uv(180.0f, 20.0f,  0xFFFFFFFFu, uv[1]);
            vertex_uv(180.0f, 140.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(20.0f,  140.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);

        /* A palettized (P8) texture, and next to it one in a format with no
         * decoder (0x2A, a swizzled depth format), which must come out
         * magenta and be logged once. */
        {
            static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            int q;
            for (q = 0; q < 2; q++) {
                float x0 = q ? 300.0f : 200.0f;
                M(0x1B00, P8_GUEST);
                M(0x1B04, ((q ? 0x2Au : 0x0Bu) << 8) | (4u << 20) | (4u << 24)
                          | (1u << 4) | (2u << 0));
                M(0x1B08, 0x00000101u);
                M(0x1B20, PAL_GUEST);          /* TEXTURE_PALETTE: 256 entries */
                M(0x17FC, 8);
                vertex_uv(x0,         20.0f,  0xFFFFFFFFu, uv[0]);
                vertex_uv(x0 + 80.0f, 20.0f,  0xFFFFFFFFu, uv[1]);
                vertex_uv(x0 + 80.0f, 100.0f, 0xFFFFFFFFu, uv[2]);
                vertex_uv(x0,         100.0f, 0xFFFFFFFFu, uv[3]);
                M(0x17FC, 0);
            }
        }
        /* A vertex-program batch: mov oPos, v0; mov oD0, v3; mov oT0, v9,
         * hand-encoded, with the checker texture. It draws the same as a
         * pass-through quad would, and gives RECOMP_WHITE_TRACE a program
         * batch to trace (stream output and the CPU interpreter must agree). */
        {
            static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            static const uint32_t outs[3][2] = {{0, 0}, {3, 3}, {9, 9}}; /* {o, v} */
            int k;
            M(0x1E9C, 0);                      /* PROGRAM_LOAD: slot 0 */
            for (k = 0; k < 3; k++) {
                M(0x0B00 + 16 * k + 0, 0);
                M(0x0B00 + 16 * k + 4, (1u << 21) | (outs[k][1] << 9) | 0x1Bu);
                M(0x0B00 + 16 * k + 8, 2u << 26);          /* A = v */
                M(0x0B00 + 16 * k + 12, (0xFu << 12) | (1u << 11)
                                        | (outs[k][0] << 3) | (k == 2 ? 1u : 0u));
            }
            M(0x1EA0, 0);                      /* PROGRAM_START */
            M(0x1E94, 2);                      /* EXECUTION_MODE: program */
            M(0x1B00, TEX_GUEST);
            M(0x1B04, (0x06u << 8) | (TEX_LOG2 << 20) | (TEX_LOG2 << 24)
                      | (1u << 4) | (2u << 0));
            M(0x1B08, 0x00000101u);
            M(0x17FC, 8);
            vertex_uv(300.0f, 110.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(380.0f, 110.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(380.0f, 190.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(300.0f, 190.0f, 0xFFFFFFFFu, uv[3]);
            M(0x17FC, 0);
            M(0x1E94, 0);                      /* fixed function again */
        }
        /* A post pass reading the surface it draws into (linear A8R8G8B8 at
         * the colour offset, texel coordinates): a quarter-size copy of the
         * frame so far, bottom right. Bound straight from guest memory, it
         * is black. */
        M(0x1B00, buf[f & 1]);
        M(0x1B04, (0x12u << 8) | (1u << 4) | (2u << 0));
        M(0x1B08, 0x00000303u);
        M(0x1B0C, 0x40000000u);
        M(0x1B10, (640u * 4u) << 16);
        M(0x1B1C, (640u << 16) | 480u);
        M(0x1760 + 4 * 9, 0x22u);
        M(0x17FC, 8);
        {
            static const float uv[4][2] = {{0, 0}, {640, 0}, {640, 480}, {0, 480}};
            vertex_uv(460.0f, 360.0f, 0xFFFFFFFFu, uv[0]);
            vertex_uv(620.0f, 360.0f, 0xFFFFFFFFu, uv[1]);
            vertex_uv(620.0f, 480.0f, 0xFFFFFFFFu, uv[2]);
            vertex_uv(460.0f, 480.0f, 0xFFFFFFFFu, uv[3]);
        }
        M(0x17FC, 0);
        M(0x1760 + 4 * 9, 0x02u);
        M(0x1B0C, 0);

        /* ... and the offscreen surface itself, drawn last in the frame the
         * way a title renders a glow or shadow target: the flip must still
         * present the 640x480 back buffer, not this. A dark teal clear with
         * an orange quad sliding across it. */
        M(0x0200, 320u << 16);
        M(0x0204, 240u << 16);
        M(0x020C, 320u * 4u);
        M(0x0210, OFFSCREEN);
        M(0x1D90, 0xFF004040u);
        M(0x1D94, 0xF0);                       /* colour only */
        M(0x17FC, 8);
        vertex(20.0f + 200.0f * t, 60.0f,  0xFFFF8000u);
        vertex(120.0f + 200.0f * t, 60.0f, 0xFFFF8000u);
        vertex(120.0f + 200.0f * t, 180.0f, 0xFFFF8000u);
        vertex(20.0f + 200.0f * t, 180.0f, 0xFFFF8000u);
        M(0x17FC, 0);

        M(0x0210, buf[(f + 1) & 1]);           /* next buffer, as the XDK does */
        M(0x0130, 0);                          /* FLIP_STALL */
        Sleep(16);
    }
    fprintf(stderr, "[backend-smoke] done\n");
    return 0;
}
