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

static void vertex(float x, float y, uint32_t argb)
{
    M(0x1818, f2u(x));
    M(0x1818, f2u(y));
    M(0x1818, f2u(0.0f));
    M(0x1818, f2u(1.0f));
    M(0x1818, argb);
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

        M(0x0210, buf[(f + 1) & 1]);           /* next buffer, as the XDK does */
        M(0x0130, 0);                          /* FLIP_STALL */
        Sleep(16);
    }
    fprintf(stderr, "[backend-smoke] done\n");
    return 0;
}
