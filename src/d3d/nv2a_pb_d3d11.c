/**
 * A D3D11 backend for the pushbuffer executor (struct nv2a_pb_backend).
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's pushbuffer into
 * the state in nv2a_pb_state.h and calls four hooks; this file answers them
 * with D3D11 instead of the CPU rasteriser:
 *
 *   on_clear  -> ClearRenderTargetView on the surface at color_offset
 *   on_draw   -> the batch's vertex program, translated to HLSL by d3d8_vsh.c
 *                and compiled with D3DCompile (Wine's d3dcompiler_47 under
 *                Proton), or a pass-through for pre-transformed batches
 *   on_flip   -> copy the surface last drawn into the swap chain, Present
 *
 * Opt-in: RECOMP_PB_BACKEND=d3d11 (see nv2a_pb_d3d11_register_from_env).
 *
 * Not yet: textures, register combiners (the pixel shader is oD0), depth,
 * the clear rectangle (whole surface), 16-bit surfaces, guest readback of
 * the rendered surface (the title never sees D3D11's pixels).
 *
 * Everything runs on the NV2A ack thread: the device, the window and its
 * message pump are created and used there only.
 *
 * Off Windows this file compiles to nothing.
 */
#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_vsh.h"
#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"   /* xbox_GetMemoryOffset */

#define LOGP "[d3d11] "

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR,
                                     const D3D_SHADER_MACRO *, ID3DInclude *,
                                     LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);

/* ---- device ------------------------------------------------------------ */

static int                     s_failed;        /* init failed: draw nothing */
static HWND                    s_hwnd;
static ID3D11Device           *s_dev;
static ID3D11DeviceContext    *s_ctx;
static IDXGISwapChain         *s_swap;
static ID3D11Texture2D        *s_back;
static UINT                    s_back_w, s_back_h;
static compile_fn              s_compile;

static ID3D11Buffer           *s_cb_consts;     /* b1: c[192]            */
static ID3D11Buffer           *s_cb_vp;         /* b2: vp_scale, vp_off  */
static ID3D11Buffer           *s_vb;            /* ring of float4 streams */
static UINT                    s_vb_size, s_vb_pos;
static ID3D11Buffer           *s_ib;
static UINT                    s_ib_size, s_ib_pos;
static ID3D11RasterizerState  *s_rs;
static ID3D11DepthStencilState *s_ds_off;
static ID3D11PixelShader      *s_ps;

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped;

/* ---- render targets: one per guest surface ------------------------------ */

#define RT_MAX 8
typedef struct {
    uint32_t                offset;     /* color_offset, as the title set it */
    UINT                    w, h;
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
    uint64_t                last_use;
} RenderTarget;

static RenderTarget s_rt[RT_MAX];
static RenderTarget *s_drawn;   /* what the next flip presents */
static uint64_t      s_tick;

/* ---- shaders --------------------------------------------------------------- */

#define VS_MAX 128
typedef struct {
    uint32_t            hash;     /* 0 = pass-through (no program) */
    int                 used;
    int                 bad;      /* failed to compile: skip its batches */
    uint16_t            inputs;   /* ATTR slots the shader reads */
    ID3D11VertexShader *vs;
    ID3D11InputLayout  *layout;
} VsEntry;

static VsEntry s_vs[VS_MAX];

#define BLEND_MAX 32
typedef struct {
    uint32_t          key[3];
    ID3D11BlendState *bs;
} BlendEntry;
static BlendEntry s_blend[BLEND_MAX];
static int        s_blend_n;

/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) {
        /* Closing the window ends the run, like the CPU path's window. */
        ExitProcess(0);
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int compile(const char *what, const char *src, const char *target,
                   ID3DBlob **code)
{
    ID3DBlob *err = NULL;
    HRESULT hr = s_compile(src, strlen(src), what, NULL, NULL, "main", target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, LOGP "compile %s failed hr=0x%08lX\n%s\n", what,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (getenv("RECOMP_D3D11_VERBOSE"))
            fprintf(stderr, "--- source ---\n%s\n--- end ---\n", src);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

static ID3D11Buffer *make_buffer(UINT size, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &b)))
        return NULL;
    return b;
}

/* The VS output struct d3d8_vsh.c emits. A pixel shader that declares the
 * same struct links with any of its vertex shaders. */
#define VS_OUT_DECL \
    "struct VS_OUT {\n" \
    "    float4 oPos : SV_POSITION;\n" \
    "    float4 oD0  : COLOR0;\n" \
    "    float4 oD1  : COLOR1;\n" \
    "    float4 oT0  : TEXCOORD0;\n" \
    "    float4 oT1  : TEXCOORD1;\n" \
    "    float4 oT2  : TEXCOORD2;\n" \
    "    float4 oT3  : TEXCOORD3;\n" \
    "    float  oFog : FOG;\n" \
    "    float  oPts : PSIZE;\n" \
    "    float4 oB0  : TEXCOORD4;\n" \
    "    float4 oB1  : TEXCOORD5;\n" \
    "};\n"

static const char s_ps_src[] =
    VS_OUT_DECL
    "float4 main(VS_OUT i) : SV_Target { return i.oD0; }\n";

/* Batches without a vertex program: attribute 0 is already in surface pixels
 * (x, y, z, rhw); 3 is the diffuse colour, 9 the first texture coordinate. */
static const char s_vs_passthrough_src[] =
    "cbuffer VSH_Viewport : register(b2) { float4 vp_scale; float4 vp_off; };\n"
    "struct VS_IN { float4 v0 : ATTR0; float4 v3 : ATTR3; float4 v9 : ATTR9; };\n"
    VS_OUT_DECL
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o = (VS_OUT)0;\n"
    "    o.oPos = float4(i.v0.xy * vp_scale.xy + vp_off.xy,\n"
    "                    saturate(i.v0.z * vp_scale.z), 1.0);\n"
    "    o.oD0 = i.v3; o.oD1 = float4(0,0,0,1); o.oT0 = i.v9;\n"
    "    return o;\n"
    "}\n";

static int init(void)
{
    static int tried;
    HMODULE dll;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    WNDCLASSA wc;
    RECT r = {0, 0, 640, 480};

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    dll = LoadLibraryA("d3dcompiler_47.dll");
    if (dll)
        s_compile = (compile_fn)(void (*)(void))GetProcAddress(dll, "D3DCompile");
    if (!s_compile) {
        fprintf(stderr, LOGP "no D3DCompile in d3dcompiler_47.dll\n");
        return 0;
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RecompNv2aD3D11";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA(wc.lpszClassName, "recomp (NV2A via D3D11)",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!s_hwnd) {
        fprintf(stderr, LOGP "CreateWindow failed (%lu)\n", GetLastError());
        return 0;
    }

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = s_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd,
                                       &s_swap, &s_dev, &fl, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, LOGP "D3D11CreateDeviceAndSwapChain hr=0x%08lX\n",
                (unsigned long)hr);
        return 0;
    }
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&s_back);
    s_back_w = 640;
    s_back_h = 480;

    s_cb_consts = make_buffer(NV2A_VSH_CONSTANTS * 16, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_vp     = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER);
    s_vb_size   = 8u << 20;
    s_vb        = make_buffer(s_vb_size, D3D11_BIND_VERTEX_BUFFER);
    s_ib_size   = 1u << 20;
    s_ib        = make_buffer(s_ib_size, D3D11_BIND_INDEX_BUFFER);
    if (!s_cb_consts || !s_cb_vp || !s_vb || !s_ib) {
        fprintf(stderr, LOGP "buffer creation failed\n");
        return 0;
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        /* No depth buffer yet, and NV2A depth is not clipped the way D3D's
         * is: keep everything the title sends. */
        rd.DepthClipEnable = FALSE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds_off);
    }
    {
        ID3DBlob *code = NULL;
        if (!compile("nv2a_ps_diffuse", s_ps_src, "ps_5_0", &code))
            return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL,
                                       &s_ps);
        ID3D10Blob_Release(code);
    }

    fprintf(stderr, LOGP "device up: feature level 0x%X, swap chain 640x480\n",
            (unsigned)fl);
    s_failed = 0;
    return 1;
}

/* ---- surfaces ------------------------------------------------------------- */

static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    UINT w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    int i;

    if (!g->color_offset || !w || !h)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            rt->last_use = s_tick;
            return rt;
        }
        if (!rt->tex && !free_rt)
            free_rt = rt;
        if (rt->last_use < lru->last_use)
            lru = rt;
    }
    if (!free_rt) {
        free_rt = lru;
        if (s_drawn == free_rt)
            s_drawn = NULL;
        ID3D11RenderTargetView_Release(free_rt->rtv);
        ID3D11Texture2D_Release(free_rt->tex);
        memset(free_rt, 0, sizeof *free_rt);
    }
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &free_rt->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(
                    s_dev, (ID3D11Resource *)free_rt->tex, NULL, &free_rt->rtv))) {
            ID3D11Texture2D_Release(free_rt->tex);
            free_rt->tex = NULL;
            return NULL;
        }
    }
    free_rt->offset = g->color_offset;
    free_rt->w = w;
    free_rt->h = h;
    free_rt->last_use = s_tick;
    fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
            g->color_offset, w, h);
    return free_rt;
}

static void d3d11_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    float c[4];

    if (!(param & 0xF0) || !init())
        return;
    rt = surface();
    if (!rt)
        return;
    c[0] = (float)((g->clear_color >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((g->clear_color >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(g->clear_color & 0xFF) / 255.0f;
    c[3] = (float)(g->clear_color >> 24) / 255.0f;
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
    /* A clear is drawing too: the flip presents this surface. (The CPU
     * backend records this in drawn_offset; other backends cannot.) */
    s_drawn = rt;
    s_clears++;
}

/* ---- vertices -------------------------------------------------------------- */

/* One attribute of one vertex as float4, decoded the way the CPU path's
 * fetch_attr does. 0 if the stream has no data for it. */
static int fetch(const struct nv2a_pb_draw *d, const VertexAttr *a,
                 uint32_t index, float out[4])
{
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (d->inline_data) {
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > d->inline_bytes)
            return 0;
        p = (const uint8_t *)d->inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = (const uint8_t *)xbox_GetMemoryOffset() + a->offset
          + (size_t)index * a->stride;
    }
    switch (a->type) {
    case NV2A_VTX_D3DCOLOR:
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV2A_VTX_FLOAT:
        for (i = 0; i < a->size && i < 4; i++)
            memcpy(&out[i], p + 4 * i, 4);
        return 1;
    case NV2A_VTX_UBYTE:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case NV2A_VTX_S1:
        for (i = 0; i < a->size && i < 4; i++) {
            int16_t s;
            float f;
            memcpy(&s, p + 2 * i, 2);
            f = (float)s / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case NV2A_VTX_S32K:
        for (i = 0; i < a->size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (float)s;
        }
        return 1;
    case NV2A_VTX_CMP: {
        uint32_t w;
        int32_t x, y, z;
        memcpy(&w, p, 4);
        x = (int32_t)(w << 21) >> 21;
        y = (int32_t)(w << 10) >> 21;
        z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static void *ring_map(ID3D11Buffer *b, UINT size, UINT *pos, UINT need,
                      UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at = (*pos + align - 1) / align * align;

    if (need > size)
        return NULL;
    if (at + need > size) {
        at = 0;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0, how, 0, &m)))
        return NULL;
    *pos = at + need;
    return (uint8_t *)m.pData + at;
}

/* Triangle, line and point lists from any NV097 primitive. Returns the
 * index count (into out, rebased by -base) and the topology. */
static uint32_t convert_indices(const struct nv2a_pb_draw *d, uint32_t base,
                                uint32_t *out, D3D11_PRIMITIVE_TOPOLOGY *topo)
{
    const uint16_t *v = d->idx;
    uint32_t n = d->idx_count, k = 0, i;

#define PUT(x) (out[k++] = (uint32_t)(x) - base)
    switch (d->prim) {
    case NV_PRIM_POINTS:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        for (i = 0; i < n; i++) PUT(v[i]);
        break;
    case NV_PRIM_LINES:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (i = 0; i + 1 < n; i += 2) { PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (i = 0; i + 1 < n; i++) { PUT(v[i]); PUT(v[i + 1]); }
        if (d->prim == NV_PRIM_LINE_LOOP && n > 2) { PUT(v[n - 1]); PUT(v[0]); }
        break;
    case NV_PRIM_TRIANGLES:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 0; i + 2 < n; i += 3) { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) { PUT(v[i + 1]); PUT(v[i]); PUT(v[i + 2]); }
            else       { PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]); }
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 1; i + 1 < n; i++) { PUT(v[0]); PUT(v[i]); PUT(v[i + 1]); }
        break;
    case NV_PRIM_QUADS:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 0; i + 3 < n; i += 4) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 2]);
            PUT(v[i]); PUT(v[i + 2]); PUT(v[i + 3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 0; i + 3 < n; i += 2) {
            PUT(v[i]); PUT(v[i + 1]); PUT(v[i + 3]);
            PUT(v[i]); PUT(v[i + 3]); PUT(v[i + 2]);
        }
        break;
    default:
        return 0;
    }
#undef PUT
    return k;
}

/* ---- shaders --------------------------------------------------------------- */

static uint32_t program_hash(const struct nv2a_pb_vsh *v)
{
    uint32_t h = 2166136261u, pc, k;
    for (pc = v->start; pc < NV2A_VSH_SLOTS; pc++) {
        for (k = 0; k < 4; k++) {
            h ^= v->prog[pc][k];
            h *= 16777619u;
        }
        if (v->prog[pc][3] & 1u)      /* FINAL */
            break;
    }
    return h ? h : 1;
}

static VsEntry *vertex_shader(int program)
{
    static NV2AVshProgram parsed;
    static char hlsl[65536];
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash = program ? program_hash(v) : 0;
    D3D11_INPUT_ELEMENT_DESC el[NV2A_VS_MAX_INPUTS];
    VsEntry *e = NULL;
    ID3DBlob *code = NULL;
    const char *src;
    char name[32];
    int i, n;

    for (i = 0; i < VS_MAX; i++) {
        if (s_vs[i].used && s_vs[i].hash == hash)
            return s_vs[i].bad ? NULL : &s_vs[i];
        if (!s_vs[i].used && !e)
            e = &s_vs[i];
    }
    if (!e)
        return NULL;    /* full; a title with this many programs wants LRU */
    e->used = 1;
    e->hash = hash;
    e->bad = 1;

    if (program) {
        d3d8_vsh_parse((const DWORD *)&v->prog[v->start][0],
                       NV2A_VSH_SLOTS - (int)v->start, &parsed);
        if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      hlsl, sizeof hlsl) <= 0) {
            fprintf(stderr, LOGP "program %08X: no HLSL\n", hash);
            return NULL;
        }
        src = hlsl;
        e->inputs = parsed.inputs_read;
        snprintf(name, sizeof name, "nv2a_vsh_%08X", hash);
    } else {
        src = s_vs_passthrough_src;
        e->inputs = (1u << 0) | (1u << 3) | (1u << 9);
        snprintf(name, sizeof name, "nv2a_vsh_passthrough");
    }
    if (!compile(name, src, "vs_5_0", &code))
        return NULL;
    if (FAILED(ID3D11Device_CreateVertexShader(
                s_dev, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &e->vs))) {
        ID3D10Blob_Release(code);
        return NULL;
    }
    /* Every stream is uploaded as float4 for now, one input slot each. The
     * native formats (d3d8_vsh_nv2a_input_layout) would save the expansion;
     * that is the next step once the picture is right. */
    for (i = 0, n = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(e->inputs & (1u << i)))
            continue;
        memset(&el[n], 0, sizeof el[n]);
        el[n].SemanticName = "ATTR";
        el[n].SemanticIndex = (UINT)i;
        el[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        el[n].InputSlot = (UINT)i;
        el[n].AlignedByteOffset = 0;
        el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        n++;
    }
    if (n && FAILED(ID3D11Device_CreateInputLayout(
                s_dev, el, (UINT)n, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), &e->layout))) {
        fprintf(stderr, LOGP "%s: CreateInputLayout failed\n", name);
        ID3D10Blob_Release(code);
        return NULL;
    }
    ID3D10Blob_Release(code);
    fprintf(stderr, LOGP "compiled %s (inputs 0x%04X)\n", name, e->inputs);
    e->bad = 0;
    return e;
}

/* ---- blending -------------------------------------------------------------- */

static D3D11_BLEND blend_factor(uint32_t gl, int alpha)
{
    switch (gl) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static ID3D11BlendState *blend_state(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t key[3];
    D3D11_BLEND_DESC bd;
    ID3D11BlendState *bs = NULL;
    int i;

    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    for (i = 0; i < s_blend_n; i++)
        if (!memcmp(s_blend[i].key, key, sizeof key))
            return s_blend[i].bs;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = key[0];
    bd.RenderTarget[0].SrcBlend = blend_factor(key[1], 0);
    bd.RenderTarget[0].DestBlend = blend_factor(key[2], 0);
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = blend_factor(key[1], 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_factor(key[2], 1);
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!key[0]) {
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    }
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &bs)))
        return NULL;
    if (s_blend_n < BLEND_MAX) {
        memcpy(s_blend[s_blend_n].key, key, sizeof key);
        s_blend[s_blend_n++].bs = bs;
    }
    return bs;
}

/* ---- draw ------------------------------------------------------------------ */

static void d3d11_on_draw(const struct nv2a_pb_draw *d)
{
    static uint32_t idx[NV_MAX_INDICES * 3];
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *vbs[NV2A_VS_MAX_INPUTS];
    UINT strides[NV2A_VS_MAX_INPUTS], offs[NV2A_VS_MAX_INPUTS];
    RenderTarget *rt;
    VsEntry *vs;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k;
    void *p;

    if (!init())
        return;
    rt = surface();
    vs = vertex_shader(d->program);
    if (!rt || !vs || !d->idx_count) {
        s_draws_skipped++;
        return;
    }
    for (i = 0; i < d->idx_count; i++) {
        if (d->idx[i] < lo) lo = d->idx[i];
        if (d->idx[i] > hi) hi = d->idx[i];
    }
    count = hi - lo + 1;
    nidx = convert_indices(d, lo, idx, &topo);
    if (!nidx) {
        s_draws_skipped++;
        return;
    }

    /* Streams: each one the shader reads, as float4 per vertex lo..hi, or one
     * float4 at stride 0 (the current SET_VERTEX_DATA value) if disabled. */
    memset(vbs, 0, sizeof vbs);
    memset(strides, 0, sizeof strides);
    memset(offs, 0, sizeof offs);
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        int on = a->size && a->stride;
        UINT bytes = on ? count * 16u : 16u;
        float *f;

        if (!(vs->inputs & (1u << i)))
            continue;
        f = (float *)ring_map(s_vb, s_vb_size, &s_vb_pos, bytes, 16);
        if (!f) {
            s_draws_skipped++;
            return;
        }
        offs[i] = s_vb_pos - bytes;
        if (on) {
            for (k = 0; k < count; k++) {
                if (!fetch(d, a, lo + k, f + 4 * k))
                    memcpy(f + 4 * k, v->inl[i], 16);
            }
            strides[i] = 16;
        } else {
            memcpy(f, v->inl[i], 16);
            strides[i] = 0;
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        vbs[i] = s_vb;
    }
    p = ring_map(s_ib, s_ib_size, &s_ib_pos, nidx * 4u, 4);
    if (!p) {
        s_draws_skipped++;
        return;
    }
    memcpy(p, idx, nidx * 4u);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);

    /* Constants: the program's c[192] at b1 and the pixels->NDC mapping. */
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (d->program && SUCCEEDED(ID3D11DeviceContext_Map(
                s_ctx, (ID3D11Resource *)s_cb_consts, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, v->c, sizeof v->c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_consts, 0);
        }
        if (SUCCEEDED(ID3D11DeviceContext_Map(
                s_ctx, (ID3D11Resource *)s_cb_vp, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float zmax = ((g->format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
            float c[8];
            c[0] = 2.0f / (float)rt->w;
            c[1] = -2.0f / (float)rt->h;
            c[2] = 1.0f / zmax;
            c[3] = 0.0f;
            c[4] = -1.0f;
            c[5] = 1.0f;
            c[6] = 0.0f;
            c[7] = 0.0f;
            memcpy(m.pData, c, sizeof c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_vp, 0);
        }
    }

    {
        D3D11_VIEWPORT vp;
        ID3D11Buffer *cbs[3];
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.Width = (float)rt->w;
        vp.Height = (float)rt->h;
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        cbs[0] = NULL;
        cbs[1] = s_cb_consts;
        cbs[2] = s_cb_vp;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, NULL);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(), NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, vs->layout);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, NV2A_VS_MAX_INPUTS,
                                               vbs, strides, offs);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT,
                                             s_ib_pos - nidx * 4u);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
        ID3D11DeviceContext_VSSetShader(s_ctx, vs->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, cbs);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
    }
    s_drawn = rt;
    s_draws++;
}

/* ---- flip ------------------------------------------------------------------ */

/* RECOMP_D3D11_DUMP=<prefix>: every 60th presented frame as <prefix>NNNN.bmp,
 * read back from the swap chain's buffer. Proof of what reached the screen. */
static void dump_frame(void)
{
    static const char *prefix;
    static int checked;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    FILE *f;
    UINT y;

    if (!checked) {
        checked = 1;
        prefix = getenv("RECOMP_D3D11_DUMP");
    }
    if (!prefix || (s_presents % 60) != 1)
        return;
    ID3D11Texture2D_GetDesc(s_back, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st,
                                     (ID3D11Resource *)s_back);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        snprintf(path, sizeof path, "%s%04u.bmp", prefix,
                 (unsigned)(s_presents / 60));
        f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh;
            BITMAPINFOHEADER ih;
            memset(&fh, 0, sizeof fh);
            memset(&ih, 0, sizeof ih);
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + td.Width * td.Height * 4;
            ih.biSize = sizeof ih;
            ih.biWidth = (LONG)td.Width;
            ih.biHeight = -(LONG)td.Height;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            for (y = 0; y < td.Height; y++)
                fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                       td.Width * 4, 1, f);
            fclose(f);
            fprintf(stderr, LOGP "dumped %s\n", path);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void d3d11_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    HRESULT hr;

    if (!init())
        return;
    pump();
    if (s_drawn) {
        if (s_drawn->w == s_back_w && s_drawn->h == s_back_h) {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_back,
                                             (ID3D11Resource *)s_drawn->tex);
        } else {
            D3D11_BOX b;
            b.left = 0;
            b.top = 0;
            b.front = 0;
            b.right = s_drawn->w < s_back_w ? s_drawn->w : s_back_w;
            b.bottom = s_drawn->h < s_back_h ? s_drawn->h : s_back_h;
            b.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(
                s_ctx, (ID3D11Resource *)s_back, 0, 0, 0, 0,
                (ID3D11Resource *)s_drawn->tex, 0, &b);
        }
    }
    s_presents++;
    dump_frame();
    hr = IDXGISwapChain_Present(s_swap, 0, 0);
    if (FAILED(hr) && s_presents < 4)
        fprintf(stderr, LOGP "Present hr=0x%08lX\n", (unsigned long)hr);
    /* FLIP_DISCARD unbinds the back buffer; the next frame rebinds its own
     * render target anyway. */
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (walker said 0x%08X),"
                " %llu clears, %llu draws, %llu skipped\n",
                (unsigned long long)s_presents,
                s_drawn ? s_drawn->offset : 0, surface_offset,
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_skipped);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_d3d11_backend = {
    d3d11_on_clear,
    NULL,               /* on_zclear: no depth buffer yet */
    d3d11_on_draw,
    d3d11_on_flip,
};

const struct nv2a_pb_backend *nv2a_pb_d3d11_backend(void)
{
    return &s_d3d11_backend;
}

/* RECOMP_PB_BACKEND=d3d11 routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_d3d11_register_from_env(void)
{
    const char *b = getenv("RECOMP_PB_BACKEND");
    if (b && !_stricmp(b, "d3d11")) {
        nv2a_pb_set_backend(&s_d3d11_backend);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=d3d11: executor draws through D3D11\n");
    }
}

#endif /* _WIN32 */
