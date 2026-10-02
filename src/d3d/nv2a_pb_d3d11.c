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
 * Depth: a D32_FLOAT buffer per zeta offset, cleared by on_zclear.
 *
 * Not yet: stencil, the clear rectangle (whole surface), 16-bit surfaces, guest readback of
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
#include "d3d8_combiners.h"
#include "d3d8_swizzle.h"
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
static ID3D11Buffer           *s_cb_ps;         /* PS b0: PsConsts       */
static ID3D11Buffer           *s_vb;            /* ring of float4 streams */
static UINT                    s_vb_size, s_vb_pos;
static ID3D11Buffer           *s_ib;
static UINT                    s_ib_size, s_ib_pos;
static ID3D11RasterizerState  *s_rs;
static ID3D11DepthStencilState *s_ds_off;
static ID3D11PixelShader      *s_ps;

/* The pixel shaders' constants (PS b0). The layout is NV2APSConstants
 * (d3d8_combiners.h) plus the per-stage texture-coordinate scale and mode. */
typedef struct {
    float    c0[8][4], c1[8][4], fc0[4], fc1[4], fog_color[4];
    float    alpha_ref;
    uint32_t alpha_func, alpha_test_enable, fog_enable;
    uint32_t alpha_only[4];
    float    tex_scale[4][4];   /* xy: texcoord multiplier (1/size for linear) */
    uint32_t tex_mode[4];       /* stage texture-shader mode, 0 = none */
} PsConsts;

#define PS_CB_DECL \
    "cbuffer CombinerCB : register(b0) {\n" \
    "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n" \
    "    float alpha_ref; uint alpha_func; uint alpha_test_enable; uint fog_enable;\n" \
    "    uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n" \
    "};\n"

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped;
static uint64_t s_tex_uploads, s_tex_rehash, s_tex_changed;
static uint64_t s_vb_native, s_vb_expanded;  /* vertex bytes copied / made */
static uint64_t s_draws_prog, s_draws_tex;   /* with a vertex program / a texture */

/* ---- render targets: one per guest surface ------------------------------ */

#define RT_MAX 8
typedef struct {
    uint32_t                offset;     /* color_offset, as the title set it */
    uint32_t                addr;       /* ... resolved to a guest address */
    UINT                    w, h;
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;      /* for render-to-texture, made lazily */
    uint64_t                last_use;
    uint64_t                last_draw, last_clear;  /* s_seq at the last one */
    int                     flipped;    /* the walker named it at a flip */
} RenderTarget;

static RenderTarget s_rt[RT_MAX];
static RenderTarget *s_drawn;   /* what the last flip presented */
static RenderTarget *s_cur;     /* the target of the batch being drawn */
static uint64_t      s_tick;
static uint64_t      s_seq, s_flip_seq;  /* draw/clear order; s_seq at the last flip */
static uint64_t      s_rtt_binds;        /* textures served from a render target */

/* ---- shaders --------------------------------------------------------------- */

#define VS_MAX 128
typedef struct {
    uint32_t            hash;     /* 0 = pass-through (no program) */
    int                 used;
    int                 bad;      /* failed to compile: skip its batches */
    uint16_t            inputs;   /* ATTR slots the shader reads */
    ID3D11VertexShader *vs;
    ID3DBlob           *code;     /* for input layouts made later */
    struct {
        uint8_t            fmt[16];  /* DXGI_FORMAT per ATTR slot, 0 unread */
        ID3D11InputLayout *il;
    } lay[8];                     /* one per combination of stream formats */
    int                 lay_next;
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

/* Batches without register combiners: the CPU path's MODULATE, oD0 * T0
 * when stage 0 is on, then the alpha test (D3DCMP numbering, on the 8-bit
 * values the way the CPU compares them). */
static const char s_ps_src[] =
    VS_OUT_DECL
    PS_CB_DECL
    "Texture2D tex0 : register(t0);\n"
    "SamplerState samp0 : register(s0);\n"
    "float4 main(VS_OUT i) : SV_Target {\n"
    "    float4 c = i.oD0;\n"
    "    if (tex_mode.x == 4u) {\n"
    "        c *= saturate(i.oT0);\n"
    "    } else if (tex_mode.x != 0u) {\n"
    "        float2 uv = i.oT0.xy;\n"
    "        if (tex_mode.x == 1u && i.oT0.w != 0.0 && i.oT0.w != 1.0) uv /= i.oT0.w;\n"
    "        c *= tex0.Sample(samp0, uv * tex_scale[0].xy);\n"
    "    }\n"
    "    if (alpha_test_enable != 0u) {\n"
    "        uint a = (uint)(saturate(c.a) * 255.0 + 0.5), r = (uint)(alpha_ref * 255.0 + 0.5);\n"
    "        bool ok = true;\n"
    "        if      (alpha_func == 1u) ok = false;\n"
    "        else if (alpha_func == 2u) ok = a <  r;\n"
    "        else if (alpha_func == 3u) ok = a == r;\n"
    "        else if (alpha_func == 4u) ok = a <= r;\n"
    "        else if (alpha_func == 5u) ok = a >  r;\n"
    "        else if (alpha_func == 6u) ok = a != r;\n"
    "        else if (alpha_func == 7u) ok = a >= r;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    return c;\n"
    "}\n";

/* Batches without a vertex program: attribute 0 is already in surface pixels
 * (x, y, z, rhw); 3 and 4 are the diffuse and specular colours, 9..12 the
 * texture coordinates. */
static const char s_vs_passthrough_src[] =
    "cbuffer VSH_Viewport : register(b2) { float4 vp_scale; float4 vp_off; };\n"
    "struct VS_IN { float4 v0 : ATTR0; float4 v3 : ATTR3; float4 v4 : ATTR4;\n"
    "               float4 v9 : ATTR9; float4 v10 : ATTR10; float4 v11 : ATTR11;\n"
    "               float4 v12 : ATTR12; };\n"
    VS_OUT_DECL
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o = (VS_OUT)0;\n"
    "    o.oPos = float4(i.v0.xy * vp_scale.xy + vp_off.xy,\n"
    "                    saturate(i.v0.z * vp_scale.z), 1.0);\n"
    "    o.oD0 = i.v3; o.oD1 = i.v4;\n"
    "    o.oT0 = i.v9; o.oT1 = i.v10; o.oT2 = i.v11; o.oT3 = i.v12;\n"
    "    return o;\n"
    "}\n";

static void create_black_texture(void);

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
    s_cb_ps     = make_buffer(sizeof(PsConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_vb_size   = 8u << 20;
    s_vb        = make_buffer(s_vb_size, D3D11_BIND_VERTEX_BUFFER);
    s_ib_size   = 1u << 20;
    s_ib        = make_buffer(s_ib_size, D3D11_BIND_INDEX_BUFFER);
    if (!s_cb_consts || !s_cb_vp || !s_cb_ps || !s_vb || !s_ib) {
        fprintf(stderr, LOGP "buffer creation failed\n");
        return 0;
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        /* The vertex programs hand D3D11 real clip-space positions (see the
         * oPos epilogue in d3d8_vsh.c: the title's divide by w is undone, w
         * kept), so the rasteriser clips triangles with vertices behind the
         * eye against the near plane instead of dropping them, as the CPU
         * path has to. Depth clipping stays on for that; NV2A depth runs
         * 0..zmax, which vp_scale.z maps onto D3D's 0..1. */
        rd.DepthClipEnable = TRUE;
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

    create_black_texture();
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
        if (free_rt->srv)
            ID3D11ShaderResourceView_Release(free_rt->srv);
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
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->last_use = s_tick;
    fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
            g->color_offset, w, h);
    return free_rt;
}

/* ---- depth ------------------------------------------------------------------ */

/* One depth buffer per zeta offset and surface size, D32_FLOAT so the test
 * compares the same normalised z (depth units / zmax) the CPU path stores in
 * its float buffer. Stencil is not emulated. */
#define DT_MAX 4
typedef struct {
    uint32_t                zeta;
    UINT                    w, h;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t                last_use;
} DepthTarget;

static DepthTarget s_dt[DT_MAX];
static ID3D11DepthStencilState *s_dss[8][2];   /* [func - 0x200][write] */
static uint64_t s_zclears, s_draws_z;

static DepthTarget *depth_target(UINT w, UINT h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    D3D11_TEXTURE2D_DESC td;
    int i;

    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (d->tex && d->zeta == zeta && d->w == w && d->h == h) {
            d->last_use = s_tick;
            return d;
        }
        if (!d->tex && !e)
            e = d;
        if (d->last_use < lru->last_use)
            lru = d;
    }
    if (!e) {
        e = lru;
        ID3D11DepthStencilView_Release(e->dsv);
        ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
    }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
        return NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)e->tex,
                                                   NULL, &e->dsv))) {
        ID3D11Texture2D_Release(e->tex);
        e->tex = NULL;
        return NULL;
    }
    /* The CPU path starts its buffer at the far plane. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, e->dsv, D3D11_CLEAR_DEPTH,
                                              1.0f, 0);
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* SET_DEPTH_FUNC takes GL's codes, 0x200 NEVER .. 0x207 ALWAYS, in the order
 * D3D11_COMPARISON_* numbers them from 1. Anything else tests as ALWAYS, as
 * on the CPU. */
static ID3D11DepthStencilState *depth_state(uint32_t func, int write)
{
    uint32_t f = (func >= 0x200 && func <= 0x207) ? func - 0x200 : 7;
    ID3D11DepthStencilState **slot = &s_dss[f][write ? 1 : 0];
    if (!*slot) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL
                                  : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = (D3D11_COMPARISON_FUNC)(f + 1);
        if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, slot)))
            return s_ds_off;
    }
    return *slot;
}

/* CLEAR_SURFACE with the Z bit: the clear value scaled as the CPU path
 * scales it, by the zeta format's range. */
static void d3d11_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;
    float z;

    if (!(param & 1u) || !init())
        return;
    rt = surface();
    if (!rt)
        return;
    dt = depth_target(rt->w, rt->h);
    if (!dt)
        return;
    z = ((g->format >> 4) & 0xF) == 1
      ? (float)(v->zclear & 0xFFFFu) / 65535.0f
      : (float)(v->zclear >> 8) / 16777215.0f;
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, dt->dsv, D3D11_CLEAR_DEPTH,
                                              z, 0);
    s_zclears++;
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
    rt->last_clear = ++s_seq;
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

/* The DXGI format a stream can be read in as it is, so its bytes are copied
 * rather than expanded to float4 (the shader's float4 input gets the same
 * values fetch() would make, missing components (0,0,0,1)). 0 for the types
 * and sizes that have none: S32K (integers read as floats), CMP, and
 * three-component bytes and shorts. *bytes: the size of one element. */
static DXGI_FORMAT native_format(const VertexAttr *a, UINT *bytes)
{
    static const DXGI_FORMAT f32[5] = {0, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT};
    static const DXGI_FORMAT u8[5] = {0, DXGI_FORMAT_R8_UNORM,
        DXGI_FORMAT_R8G8_UNORM, 0, DXGI_FORMAT_R8G8B8A8_UNORM};
    static const DXGI_FORMAT s16[5] = {0, DXGI_FORMAT_R16_SNORM,
        DXGI_FORMAT_R16G16_SNORM, 0, DXGI_FORMAT_R16G16B16A16_SNORM};

    if (a->size < 1 || a->size > 4)
        return 0;
    switch (a->type) {
    case NV2A_VTX_D3DCOLOR: *bytes = 4;           return DXGI_FORMAT_B8G8R8A8_UNORM;
    case NV2A_VTX_FLOAT:    *bytes = 4 * a->size; return f32[a->size];
    case NV2A_VTX_UBYTE:    *bytes = a->size;     return u8[a->size];
    case NV2A_VTX_S1:       *bytes = 2 * a->size; return s16[a->size];
    default:                return 0;
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
    VsEntry *e = NULL;
    ID3DBlob *code = NULL;
    const char *src;
    char name[32];
    int i;

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
        e->inputs = (1u << 0) | (1u << 3) | (1u << 4) | (0xFu << 9);
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
    e->code = code;     /* input layouts come per draw: input_layout() */
    fprintf(stderr, LOGP "compiled %s (inputs 0x%04X)\n", name, e->inputs);
    e->bad = 0;
    return e;
}

/* The input layout for a shader and the formats its streams arrive in this
 * batch (one input slot per ATTR), made the first time that pair is seen.
 * NULL if the shader reads nothing. */
static ID3D11InputLayout *input_layout(VsEntry *e, const uint8_t fmt[16])
{
    D3D11_INPUT_ELEMENT_DESC el[NV2A_VS_MAX_INPUTS];
    int i, n, k;

    for (k = 0; k < 8; k++)
        if (e->lay[k].il && !memcmp(e->lay[k].fmt, fmt, 16))
            return e->lay[k].il;
    for (i = 0, n = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(e->inputs & (1u << i)))
            continue;
        memset(&el[n], 0, sizeof el[n]);
        el[n].SemanticName = "ATTR";
        el[n].SemanticIndex = (UINT)i;
        el[n].Format = (DXGI_FORMAT)fmt[i];
        el[n].InputSlot = (UINT)i;
        el[n].AlignedByteOffset = 0;
        el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        n++;
    }
    if (!n)
        return NULL;
    k = e->lay_next;
    e->lay_next = (k + 1) % 8;
    if (e->lay[k].il)
        ID3D11InputLayout_Release(e->lay[k].il);
    e->lay[k].il = NULL;
    memcpy(e->lay[k].fmt, fmt, 16);
    if (FAILED(ID3D11Device_CreateInputLayout(
                s_dev, el, (UINT)n, ID3D10Blob_GetBufferPointer(e->code),
                ID3D10Blob_GetBufferSize(e->code), &e->lay[k].il))) {
        fprintf(stderr, LOGP "vs %08X: CreateInputLayout failed\n", e->hash);
        e->lay[k].il = NULL;
    }
    return e->lay[k].il;
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

/* ---- textures -------------------------------------------------------------
 *
 * Four stages, decoded from the texture registers the title last wrote, the
 * way the CPU path's vsh_build_stages does: offset, format, size (from the
 * format word for swizzled and DXT, from IMAGE_RECT for linear), pitch,
 * wrap modes, enable, and a texture-shader mode from SET_SHADER_STAGE_PROGRAM.
 *
 * Each texture is decoded to A8R8G8B8 on the CPU -- the same per-format
 * arithmetic as the CPU sampler, so both paths see the same texels -- and
 * uploaded as B8G8R8A8_UNORM (the same bytes). Cache key and invalidation
 * follow nv2a_pb_state.h: offset + format + width + height + pitch; on a new
 * flip generation the whole texture is hashed again and re-uploaded if it
 * changed (the letterboxed movie changes only in the middle).
 *
 * ponytail: point sampling (the CPU path's nearest texel); no mipmaps; DXT is
 * decoded rather than uploaded as BC1-3; a texture rewritten between two
 * batches of one flip is not seen. */

typedef struct {
    uint32_t offset, color, width, height, pitch, addr_u, addr_v;
    uint32_t palette, pal_len;  /* P8: resolved palette address, entries */
    int      valid;
    int      mode;      /* 0 none, 1 2D, 4 pass-through, other: sampled as 2D */
} Stage;

static Stage s_stage[4];

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 512   /* a demo frame binds ~100; 64 thrashed */
typedef struct {
    uint32_t offset, color, width, height, pitch, palette;
    uint32_t gen, used;
    uint64_t hash;
    int      live;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
} TexEntry;

static TexEntry  s_tc[TEX_CACHE];
static uint32_t  s_tc_clock;
static uint32_t *s_texels;
static size_t    s_texels_cap;
static ID3D11ShaderResourceView *s_black_srv;   /* too large: (0,0,0,1) */
static ID3D11ShaderResourceView *s_magenta_srv; /* format not decoded */
static const uint32_t *s_pal;                   /* P8 palette, while decoding */
static uint32_t        s_pal_len;

static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void build_stages(void)
{
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int n;

    for (n = 0; n < 4; n++) {
        const uint32_t *r = &regs[(0x40u * n) / 4];
        const uint8_t  *set = &sets[(0x40u * n) / 4];
        Stage *t = &s_stage[n];
        uint32_t fmt = r[0x04 / 4], ctl0 = r[0x0C / 4];
        int on = (ctl0 >> 30) & 1;
        int mode;

        memset(t, 0, sizeof *t);
        t->offset = r[0] ? nv2a_pb_dma_resolve(r[0]) : 0;
        t->color = (fmt >> 8) & 0xFF;
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((fmt >> 20) & 0xF);
            t->height = 1u << ((fmt >> 24) & 0xF);
        } else {
            t->width  = r[0x1C / 4] >> 16;
            t->height = r[0x1C / 4] & 0xFFFF;
        }
        /* CONTROL1's pitch means nothing to a swizzled or DXT texture,
         * and left in the cache key a stale one splits it in two. */
        t->pitch  = tex_size_from_format(t->color) ? 0 : r[0x10 / 4] >> 16;
        t->addr_u =  r[0x08 / 4]       & 0xF;
        t->addr_v = (r[0x08 / 4] >> 8) & 0xF;
        /* P8: SET_TEXTURE_PALETTE (0x1B20): offset in bits 6-31, length
         * 256 >> bits 2-3, entries A8R8G8B8. */
        if (t->color == 0x0B) {
            uint32_t pal = r[0x20 / 4];
            t->palette = (pal & ~0x3Fu) ? nv2a_pb_dma_resolve(pal & ~0x3Fu) : 0;
            t->pal_len = 256u >> ((pal >> 2) & 3u);
        }
        t->valid = t->offset && t->width && t->height
                && (tex_size_from_format(t->color) || t->pitch)
                && (t->color != 0x0B || t->palette);
        if (n == 0 && !set[0x0C / 4])
            on = 1;                        /* never configured: as the CPU path */
        if (v->shader_set)
            mode = (int)((v->shader_prog >> (5 * n)) & 0x1F);
        else
            mode = n == 0 ? 1 : 0;
        if (mode && mode != 4 && (!on || !t->valid)) {
            /* The program asks for a texture this stage cannot give: it
             * reads 0 now. Said once per stage, mode and format, since a
             * world drawn in flat vertex colour starts exactly here. */
            static uint8_t said[4][32][32];
            uint8_t bit = (uint8_t)(1u << (t->color & 7));
            if (!(said[n][mode & 31][(t->color >> 3) & 31] & bit)) {
                said[n][mode & 31][(t->color >> 3) & 31] |= bit;
                fprintf(stderr, LOGP "\x1b[1;35mstage %d: mode %d with no texture"
                        " (enabled %d, offset 0x%08X, format 0x%02X, %ux%u, pitch %u)"
                        "\x1b[0m\n", n, mode, on, t->offset, t->color,
                        t->width, t->height, t->pitch);
            }
        }
        if (mode != 4 && (!on || !t->valid))
            mode = 0;
        if (mode == 5)
            mode = 0;   /* CLIP_PLANE: no texture, the register reads 0
                         * (ponytail: its per-pixel discard is not done) */
        t->mode = mode;
    }
}

/* The linear format with the same texel encoding as a swizzled one. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;
    case 0x02: return 0x10;
    case 0x03: return 0x1C;
    case 0x04: return 0x1D;
    case 0x05: return 0x11;
    case 0x06: return 0x12;
    case 0x07: return 0x1E;
    case 0x19: return 0x1F;
    case 0x01: return 0x1B;     /* AY8 */
    case 0x1A: return 0x20;     /* A8Y8 */
    case 0x27: return 0x37;     /* R6G5B5 */
    case 0x32: return 0x35;     /* Y16 */
    case 0x38: return 0x3D;     /* R5G5B5A1 */
    case 0x39: return 0x3E;     /* R4G4B4A4 */
    case 0x3A: return 0x3F;     /* A8B8G8R8 */
    case 0x3B: return 0x40;     /* B8G8R8A8 */
    case 0x3C: return 0x41;     /* R8G8B8A8 */
    default:   return fmt;      /* already linear, P8, or not decoded */
    }
}

static uint32_t texel_bytes(uint32_t lin)
{
    switch (lin) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x1C: case 0x11: case 0x1D: case 0x24: case 0x25:
    case 0x20: case 0x37: case 0x35: case 0x3D: case 0x3E: return 2;
    case 0x13: case 0x1F: case 0x1B: case 0x0B: return 1;
    default: return 0;
    }
}

/* Bytes of guest memory the texture occupies; 0 if it cannot be decoded. */
static uint32_t tex_extent(const Stage *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color), tb;
    if (!t->valid || (uint64_t)t->width * t->height > TEX_MAX_TEXELS)
        return 0;
    if (bb)
        return ((t->width + 3u) / 4u) * ((t->height + 3u) / 4u) * bb;
    if (d3d8_format_is_swizzled(t->color)) {
        tb = texel_bytes(linear_twin(t->color));
        return tb ? t->width * t->height * tb : 0;
    }
    tb = texel_bytes(t->color);
    if (!tb || !t->pitch)
        return 0;
    return t->pitch * (t->height - 1u) + t->width * tb;
}

static uint64_t tex_hash(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint64_t l[4] = { h, h ^ 1u, h ^ 2u, h ^ 3u };
    uint32_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        l[0] = (l[0] ^ w[0]) * 0x100000001B3ull;
        l[1] = (l[1] ^ w[1]) * 0x100000001B3ull;
        l[2] = (l[2] ^ w[2]) * 0x100000001B3ull;
        l[3] = (l[3] ^ w[3]) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        l[0] = (l[0] ^ p[i]) * 0x100000001B3ull;
    return (l[0] ^ (l[1] << 1) ^ (l[2] << 2) ^ (l[3] << 3)) * 0x100000001B3ull;
}

static uint8_t clamp255(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

/* One texel as A8R8G8B8, from texel index u of the row at p (linear
 * formats; for swizzled ones p is the image base and u the Morton index). */
static uint32_t decode_texel(uint32_t fmt, const uint8_t *p, uint32_t u)
{
    uint32_t t;
    switch (fmt) {
    case 0x12: return ((const uint32_t *)p)[u];
    case 0x1E: return ((const uint32_t *)p)[u] | 0xFF000000u;
    case 0x3F:
        t = ((const uint32_t *)p)[u];
        return (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
    case 0x40:
        t = ((const uint32_t *)p)[u];
        return ((t & 0xFFu) << 24) | (((t >> 8) & 0xFFu) << 16)
             | (((t >> 16) & 0xFFu) << 8) | ((t >> 24) & 0xFFu);
    case 0x41:
        t = ((const uint32_t *)p)[u];
        return ((t & 0xFFu) << 24) | (t >> 8);
    case 0x10:
        t = ((const uint16_t *)p)[u];
        return ((t & 0x8000u) ? 0xFF000000u : 0u)
             | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
             |  d3d8_expand_channel(t & 0x1F, 5);
    case 0x1C:
        t = ((const uint16_t *)p)[u];
        return 0xFF000000u
             | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
             |  d3d8_expand_channel(t & 0x1F, 5);
    case 0x11:
        t = ((const uint16_t *)p)[u];
        return 0xFF000000u
             | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((t >> 5) & 0x3F, 6) << 8)
             |  d3d8_expand_channel(t & 0x1F, 5);
    case 0x1D:
        t = ((const uint16_t *)p)[u];
        return (d3d8_expand_channel((t >> 12) & 0xF, 4) << 24)
             | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 16)
             | (d3d8_expand_channel((t >> 4) & 0xF, 4) << 8)
             |  d3d8_expand_channel(t & 0xF, 4);
    case 0x13:
        t = p[u];
        return 0xFF000000u | (t << 16) | (t << 8) | t;
    case 0x1F:
        return ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
    case 0x1B:                              /* AY8: Y in all four */
        return (uint32_t)p[u] * 0x01010101u;
    case 0x20:                              /* A8Y8: Y low byte, A high */
        t = ((const uint16_t *)p)[u];
        return ((t >> 8) << 24) | (t & 0xFFu) * 0x010101u;
    case 0x35:                              /* Y16: top byte */
        t = ((const uint16_t *)p)[u] >> 8;
        return 0xFF000000u | t * 0x010101u;
    case 0x37:                              /* R6G5B5 */
        t = ((const uint16_t *)p)[u];
        return 0xFF000000u
             | (d3d8_expand_channel((t >> 10) & 0x3F, 6) << 16)
             | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
             |  d3d8_expand_channel(t & 0x1F, 5);
    case 0x3D:                              /* R5G5B5A1 */
        t = ((const uint16_t *)p)[u];
        return ((t & 1u) ? 0xFF000000u : 0u)
             | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
             | (d3d8_expand_channel((t >> 6) & 0x1F, 5) << 8)
             |  d3d8_expand_channel((t >> 1) & 0x1F, 5);
    case 0x3E:                              /* R4G4B4A4 */
        t = ((const uint16_t *)p)[u];
        return (d3d8_expand_channel(t & 0xF, 4) << 24)
             | (d3d8_expand_channel((t >> 12) & 0xF, 4) << 16)
             | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 8)
             |  d3d8_expand_channel((t >> 4) & 0xF, 4);
    case 0x0B:                              /* P8, through the palette */
        t = p[u];
        return t < s_pal_len ? s_pal[t] : 0xFFFF00FFu;
    case 0x24:                              /* YUY2 */
    case 0x25: {                            /* UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        return 0xFF000000u
             | ((uint32_t)clamp255((298 * c + 409 * cv + 128) >> 8) << 16)
             | ((uint32_t)clamp255((298 * c - 100 * cu - 208 * cv + 128) >> 8) << 8)
             |  (uint32_t)clamp255((298 * c + 516 * cu + 128) >> 8);
    }
    default:
        return 0;
    }
}

/* The whole texture into s_texels (width x height, A8R8G8B8). */
static int decode_texture(const Stage *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    size_t n = (size_t)t->width * t->height;
    uint32_t x, y, fmt = t->color;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n;
    }
    if (d3d8_format_dxt_block_bytes(fmt)) {
        for (y = 0; y < t->height; y++)
            for (x = 0; x < t->width; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, t->width, &argb);
                s_texels[(size_t)y * t->width + x] = argb;
            }
        return 1;
    }
    if (fmt == 0x0B) {
        s_pal = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset()
                                   + t->palette);
        s_pal_len = t->pal_len;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        uint32_t mx, my;
        fmt = linear_twin(fmt);
        xbox_swizzle_masks(t->width, t->height, &mx, &my);
        for (y = 0; y < t->height; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < t->width; x++)
                s_texels[(size_t)y * t->width + x] =
                    decode_texel(fmt, mem, swizzle_deposit(x, mx) | oy);
        }
        return 1;
    }
    for (y = 0; y < t->height; y++) {
        const uint8_t *row = mem + (size_t)y * t->pitch;
        for (x = 0; x < t->width; x++)
            s_texels[(size_t)y * t->width + x] = decode_texel(fmt, row, x);
    }
    return 1;
}

static int tex_upload(TexEntry *e, const Stage *t)
{
    if (!decode_texture(t))
        return 0;
    if (!e->tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
            return 0;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv))) {
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            return 0;
        }
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0,
                                          NULL, s_texels, t->width * 4, 0);
    s_tex_uploads++;
    return 1;
}

static void tex_free(TexEntry *e)
{
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    memset(e, 0, sizeof *e);
}

/* The texels' hash, and for P8 the palette's folded in: a palette swap is a
 * new texture. */
static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = tex_hash(mem + t->offset, bytes);
    if (t->color == 0x0B)
        h ^= tex_hash(mem + t->palette, t->pal_len * 4u) * 31u;
    return h;
}

/* The view for a stage's texture, decoded and uploaded if it is new or its
 * bytes changed since it was last checked (once per flip). */
static ID3D11ShaderResourceView *tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = tex_extent(t), i;
    TexEntry *e = NULL, *victim = &s_tc[0];

    if (!bytes) {
        /* A format with no decoder is magenta, and said once, loudly: white
         * or black passes for a texture that decoded to that. */
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black_srv;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta_srv ? s_magenta_srv : s_black_srv;
    }
    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry *c = &s_tc[i];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette) {
            e = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    if (e) {
        if (e->gen != g->flips) {
            uint64_t h = tex_hash_pal(mem, t, bytes);
            s_tex_rehash++;
            e->gen = g->flips;
            if (h != e->hash) {
                e->hash = h;
                s_tex_changed++;
                if (!tex_upload(e, t))
                    return s_black_srv;
            }
        }
    } else {
        e = victim;
        tex_free(e);
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        if (!tex_upload(e, t)) {
            tex_free(e);
            return s_black_srv;
        }
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    return e->srv;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp. */
static D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *sampler(uint32_t u, uint32_t v)
{
    static ID3D11SamplerState *cache[16][16];
    D3D11_SAMPLER_DESC sd;
    u &= 0xF;
    v &= 0xF;
    if (cache[u][v])
        return cache[u][v];
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = address_mode(u);
    sd.AddressV = address_mode(v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &cache[u][v]);
    return cache[u][v];
}

static ID3D11ShaderResourceView *solid_texture(uint32_t argb)
{
    ID3D11ShaderResourceView *v = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &argb;
    sd.SysMemPitch = 4;
    sd.SysMemSlicePitch = 4;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v);
        ID3D11Texture2D_Release(t);
    }
    return v;
}

static void create_black_texture(void)
{
    static const uint32_t black = 0xFF000000u;
    s_magenta_srv = solid_texture(0xFFFF00FFu);
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &black;
    sd.SysMemPitch = 4;
    sd.SysMemSlicePitch = 4;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL,
                                              &s_black_srv);
        ID3D11Texture2D_Release(t);
    }
}

/* ---- pixel stage --------------------------------------------------------- */

/* Register-combiner pixel shaders, generated by d3d8_combiners.c from the
 * combiner registers and keyed by a hash of the parsed state (constants are
 * not part of it: they go in the constant buffer). */
#define RC_MAX 64
typedef struct {
    uint32_t           hash;
    int                used, bad;
    uint64_t           last_use;
    NV2ACombinerState  st;
    ID3D11PixelShader *ps;
} RcEntry;
static RcEntry  s_rc[RC_MAX];
static uint64_t s_rc_compiles;

static uint32_t rc_hash(const NV2ACombinerState *st)
{
    const uint8_t *p = (const uint8_t *)st;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < sizeof *st; i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h ? h : 1;
}

/* NULL when the shader cannot be built: the caller falls back to s_ps. */
static ID3D11PixelShader *combiner_ps(const NV2ACombinerState *st)
{
    static char src[32768];
    uint32_t h = rc_hash(st);
    RcEntry *e = NULL, *lru = &s_rc[0];
    ID3DBlob *code = NULL;
    int i;

    for (i = 0; i < RC_MAX; i++) {
        RcEntry *c = &s_rc[i];
        if (c->used && c->hash == h && !memcmp(&c->st, st, sizeof *st)) {
            c->last_use = s_tick;
            return c->bad ? NULL : c->ps;
        }
        if (!c->used) {
            if (!e) e = c;
        } else if (c->last_use < lru->last_use) {
            lru = c;
        }
    }
    if (!e) {
        e = lru;
        if (e->ps) ID3D11PixelShader_Release(e->ps);
    }
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->hash = h;
    e->st = *st;
    e->last_use = s_tick;
    s_rc_compiles++;
    if (d3d8_combiners_generate_hlsl(st, src, (int)sizeof src) < 0
     || !compile("nv2a_ps_combiner", src, "ps_5_0", &code)) {
        fprintf(stderr, LOGP "combiner shader %08X failed (%d stages)\n",
                (unsigned)h, st->num_stages);
        e->bad = 1;
        return NULL;
    }
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev,
                   ID3D10Blob_GetBufferPointer(code),
                   ID3D10Blob_GetBufferSize(code), NULL, &e->ps)))
        e->bad = 1;
    ID3D10Blob_Release(code);
    if (getenv("RECOMP_D3D11_VERBOSE"))
        fprintf(stderr, LOGP "combiner shader %08X: %d stages\n%s\n",
                (unsigned)h, st->num_stages, src);
    return e->bad ? NULL : e->ps;
}

static void argb_to_float4(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* Render to texture: a stage whose texture lies where a surface this backend
 * renders into starts samples that surface. The guest's copy of those bytes
 * is stale (nothing here writes guest memory), so decoding it would show
 * whatever was there before -- or nothing at all. A 32-bit texture at the
 * surface's address only, and never the surface being drawn into. */
static RenderTarget *rt_texture(const Stage *t)
{
    int i;

    if (!t->offset || d3d8_format_dxt_block_bytes(t->color)
            || texel_bytes(linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || rt->addr != t->offset || rt == s_cur)
            continue;
        if (!rt->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv)))
            return NULL;
        return rt;
    }
    return NULL;
}

/* Stages, textures, samplers and PS constants for the batch about to be
 * drawn. Returns the pixel shader to use. */
static ID3D11PixelShader *setup_pixel(void)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE m;
    PsConsts pc;
    int n, textured = 0;

    build_stages();
    memset(&pc, 0, sizeof pc);
    for (n = 0; n < 4; n++) {
        const Stage *t = &s_stage[n];
        int scaled = tex_size_from_format(t->color);
        RenderTarget *src = (t->mode && t->mode != 4) ? rt_texture(t) : NULL;
        pc.tex_mode[n] = (uint32_t)t->mode;
        pc.tex_scale[n][0] = scaled || !t->width ? 1.0f : 1.0f / (float)t->width;
        pc.tex_scale[n][1] = scaled || !t->height ? 1.0f : 1.0f / (float)t->height;
        if (src) {
            /* Linear coordinates are texels of the surface as drawn. */
            if (!scaled) {
                pc.tex_scale[n][0] = 1.0f / (float)src->w;
                pc.tex_scale[n][1] = 1.0f / (float)src->h;
            }
            srv[n] = src->srv;
            s_rtt_binds++;
            textured = 1;
        } else if (t->mode && t->mode != 4) {
            srv[n] = tex_bind(t);
            textured = 1;
        } else {
            srv[n] = s_black_srv;
        }
        smp[n] = sampler(t->addr_u, t->addr_v);
    }
    for (n = 0; n < 8; n++) {
        argb_to_float4(v->rc_f0[n], pc.c0[n]);
        argb_to_float4(v->rc_f1[n], pc.c1[n]);
    }
    argb_to_float4(v->rc_sf[0], pc.fc0);
    argb_to_float4(v->rc_sf[1], pc.fc1);
    argb_to_float4(v->fog_color, pc.fog_color);
    {   /* the CPU path reads the fog colour register as RGBA, alpha 1 */
        float t = pc.fog_color[0];
        pc.fog_color[0] = pc.fog_color[2];
        pc.fog_color[2] = t;
        pc.fog_color[3] = 1.0f;
    }
    pc.alpha_ref = (float)v->atest_ref / 255.0f;
    pc.alpha_test_enable = v->atest_enable ? 1u : 0u;
    pc.alpha_func = (v->atest_func >= 0x200 && v->atest_func <= 0x207)
                  ? v->atest_func - 0x200 + 1 : 8;          /* GL -> D3DCMP */
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_ps, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_ps, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_ps);
    if (textured)
        s_draws_tex++;

    if (v->rc_set && (v->rc_ctl & 0xFF)) {
        NV2ACombinerState st;
        ID3D11PixelShader *ps;
        uint32_t modes = 0;
        for (n = 0; n < 4; n++) {
            /* Only 2D textures are bound: other sampling modes read 2D at
             * (s, t), as the CPU path does. */
            uint32_t m = (uint32_t)s_stage[n].mode;
            if (m && m != 4) m = 1;
            modes |= m << (5 * n);
        }
        d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw,
                                 v->rc_aocw, v->rc_ctl, v->rc_fcw0,
                                 v->rc_fcw1, modes, &st);
        ps = combiner_ps(&st);
        if (ps)
            return ps;
    }
    return s_ps;
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
    uint8_t fmts[16];
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

    /* Streams: each one the shader reads. One in a format the input
     * assembler reads natively is copied as it is: vertices lo..hi of its
     * buffer in one block, shared by every stream interleaved in it (same
     * stride, within one stride of each other), bound at their own offsets.
     * The rest go as float4 per vertex, or one float4 at stride 0 (the
     * current SET_VERTEX_DATA value) if disabled. */
    memset(vbs, 0, sizeof vbs);
    memset(strides, 0, sizeof strides);
    memset(offs, 0, sizeof offs);
    memset(fmts, 0, sizeof fmts);
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        int on = a->size && a->stride;
        UINT bytes = on ? count * 16u : 16u, esz;
        float *f;

        if (!(vs->inputs & (1u << i)) || fmts[i])
            continue;
        if (on && (d->inline_data || a->offset) && !(a->stride & 3)
                && native_format(a, &esz)) {
            uint32_t lo_off = a->offset, hi_end = a->offset + esz, group = 0, j;
            const uint8_t *base = d->inline_data
                ? (const uint8_t *)d->inline_buf
                : (const uint8_t *)xbox_GetMemoryOffset();
            for (j = i; j < NV2A_VS_MAX_INPUTS; j++) {
                const VertexAttr *b = &d->attr[j];
                UINT bsz;
                if (!(vs->inputs & (1u << j)) || fmts[j] || !b->size
                        || b->stride != a->stride || !native_format(b, &bsz)
                        || (!d->inline_data && !b->offset)
                        || (b->offset > a->offset ? b->offset - a->offset
                                                  : a->offset - b->offset) >= a->stride)
                    continue;
                group |= 1u << j;
                if (b->offset < lo_off) lo_off = b->offset;
                if (b->offset + bsz > hi_end) hi_end = b->offset + bsz;
            }
            bytes = (count - 1u) * a->stride + (hi_end - lo_off);
            if (!d->inline_data
                    || (size_t)lo_off + (size_t)lo * a->stride + bytes <= d->inline_bytes) {
                uint8_t *dst = (uint8_t *)ring_map(s_vb, s_vb_size, &s_vb_pos, bytes, 16);
                if (!dst) {
                    s_draws_skipped++;
                    return;
                }
                memcpy(dst, base + lo_off + (size_t)lo * a->stride, bytes);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
                for (j = i; j < NV2A_VS_MAX_INPUTS; j++) {
                    if (!(group & (1u << j)))
                        continue;
                    fmts[j] = (uint8_t)native_format(&d->attr[j], &esz);
                    offs[j] = s_vb_pos - bytes + (d->attr[j].offset - lo_off);
                    strides[j] = a->stride;
                    vbs[j] = s_vb;
                }
                s_vb_native += bytes;
                continue;
            }
            bytes = count * 16u;
        }
        fmts[i] = (uint8_t)DXGI_FORMAT_R32G32B32A32_FLOAT;
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
        s_vb_expanded += bytes;
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
        ID3D11DepthStencilView *dsv = NULL;
        ID3D11DepthStencilState *dss = s_ds_off;
        if (v->depth_enable) {
            DepthTarget *dt = depth_target(rt->w, rt->h);
            if (dt) {
                dsv = dt->dsv;
                dss = depth_state(v->depth_func, v->depth_mask);
                s_draws_z++;
            }
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, 0);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(), NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, input_layout(vs, fmts));
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, NV2A_VS_MAX_INPUTS,
                                               vbs, strides, offs);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT,
                                             s_ib_pos - nidx * 4u);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
        ID3D11DeviceContext_VSSetShader(s_ctx, vs->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, cbs);
        s_cur = rt;
        ID3D11DeviceContext_PSSetShader(s_ctx, setup_pixel(), NULL, 0);
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
    }
    rt->last_draw = ++s_seq;
    s_cur = NULL;
    s_draws++;
    if (d->program)
        s_draws_prog++;
}

/* ---- flip ------------------------------------------------------------------ */

/* RECOMP_D3D11_DUMP=<prefix>: every 60th presented frame as <prefix>NNNN.bmp,
 * read back from the swap chain's buffer. Proof of what reached the screen.
 * The prefix is prepended to the file name as is; one ending in / or \ is a
 * directory, and the files in it are named frame_NNNN.bmp. */
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
        size_t n = strlen(prefix);
        int dir = n && (prefix[n - 1] == '/' || prefix[n - 1] == '\\');
        snprintf(path, sizeof path, "%s%s%04u.bmp", prefix, dir ? "frame_" : "",
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

/* The surface a flip puts on screen.
 *
 * The walker hands over drawn_offset, else color_offset. drawn_offset is the
 * CPU backend's bookkeeping, so here it is color_offset: by FLIP_STALL the
 * XDK has already pointed that at the next back buffer. And the last surface
 * drawn into is not the answer either -- a title renders glow, shadow and
 * reflection targets too, sometimes last.
 *
 * What is used instead: the swap-chain buffers are the surfaces the walker
 * names at flips (both of them, after two), plus anything as large as the
 * largest surface (for the first flip, before the second buffer has been
 * named). Of those, the one drawn into most recently since the last flip;
 * if none was, the one cleared most recently; if none was either, the
 * frame on screen stays. */
static RenderTarget *present_target(uint32_t surface_offset)
{
    RenderTarget *best = NULL;
    UINT max_area = 0;
    int i, pass;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex)
            continue;
        if (surface_offset && rt->offset == surface_offset)
            rt->flipped = 1;
        if (rt->w * rt->h > max_area)
            max_area = rt->w * rt->h;
    }
    for (pass = 0; pass < 2 && !best; pass++) {
        uint64_t top = s_flip_seq;
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            uint64_t when;
            if (!rt->tex || !(rt->flipped || rt->w * rt->h == max_area))
                continue;
            when = pass == 0 ? rt->last_draw : rt->last_clear;
            if (when > top) {
                top = when;
                best = rt;
            }
        }
    }
    s_flip_seq = s_seq;
    return best ? best : s_drawn;
}

/* RECOMP_FLIP_LOG: a line per flip, just before the walker's "[GPU] flip"
 * one, with what this backend did since the last. The walker's own "batches"
 * counts the CPU rasteriser's batches, so under this backend it reads 0;
 * "batches" here is the same quantity for D3D11 (3D frames have them, movie
 * frames do not); "vb" is vertex bytes copied as they are / expanded to
 * float4; the flip number is this backend's present count. */
static void flip_log(uint32_t surface_offset)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_tex, p_rtt, p_clears, p_up, p_chg,
                    p_re, p_vn, p_ve, p_skip;
    if (on < 0)
        on = getenv("RECOMP_FLIP_LOG") != NULL;
    if (!on)
        return;
    fprintf(stderr, "[D3D11] flip %llu batches %llu prog %llu tex %llu rtt %llu"
            " skipped %llu clears %llu uploads %llu changed %llu rehash %llu"
            " vb %llu/%llu bytes present 0x%08X walker 0x%08X\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_rtt_binds - p_rtt),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_vb_native - p_vn),
            (unsigned long long)(s_vb_expanded - p_ve),
            s_drawn ? s_drawn->offset : 0, surface_offset);
    p_draws = s_draws; p_prog = s_draws_prog; p_tex = s_draws_tex;
    p_rtt = s_rtt_binds; p_skip = s_draws_skipped; p_clears = s_clears;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_vn = s_vb_native; p_ve = s_vb_expanded;
}

static void d3d11_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    HRESULT hr;

    if (!init())
        return;
    pump();
    s_drawn = present_target(surface_offset);
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
    flip_log(surface_offset);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (walker said 0x%08X),"
                " %llu clears, %llu draws, %llu skipped, %llu texture uploads,"
                " %llu rehashes, %llu combiner shaders, %llu zclears,"
                " %llu depth-tested draws, %llu render-target textures\n",
                (unsigned long long)s_presents,
                s_drawn ? s_drawn->offset : 0, surface_offset,
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_skipped,
                (unsigned long long)s_tex_uploads,
                (unsigned long long)s_tex_rehash,
                (unsigned long long)s_rc_compiles,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_d3d11_backend = {
    d3d11_on_clear,
    d3d11_on_zclear,
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
