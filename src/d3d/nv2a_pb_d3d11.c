/**
 * A D3D11 backend for the pushbuffer executor (struct nv2a_pb_backend).
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's pushbuffer into
 * the state in nv2a_pb_state.h and calls the hooks of struct nv2a_pb_backend;
 * this file answers them with D3D11 instead of the CPU rasteriser:
 *
 *   on_clear  -> ClearRenderTargetView on the surface at color_offset
 *   on_draw   -> the batch's vertex program, translated to HLSL by d3d8_vsh.c
 *                and compiled with D3DCompile (Wine's d3dcompiler_47 under
 *                Proton), or a pass-through for pre-transformed batches
 *   on_flip   -> copy the surface last drawn into the swap chain, Present
 *
 * Opt-in: RECOMP_PB_BACKEND=d3d11 (see nv2a_pb_d3d11_register_from_env).
 *
 * Depth and stencil: a D32_FLOAT_S8X24_UINT buffer per zeta offset, cleared
 * by on_zclear.
 *
 * Not yet: the clear rectangle (whole surface), 16-bit surfaces, guest readback of
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
#include "nv2a_vsh_cpu.h"      /* the batch trace runs the CPU interpreter */
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
static ID3D11Buffer           *s_cb_vp;         /* b2: viewport, fog, specular */
static ID3D11Buffer           *s_cb_ps;         /* PS b0: PsConsts       */

/* Per-batch CPU memos (RECOMP_D3D11_MEMO=0 turns them off for A/B): a
 * constant buffer is only rewritten when its bytes change, and the shader
 * and texture lookups reuse the last batch's answer when their inputs are
 * the same. Each one returns what the full path would, so the output does
 * not change. */
static int s_memo = -1;
static int memo_on(void)
{
    if (s_memo < 0) {
        const char *e = getenv("RECOMP_D3D11_MEMO");
        s_memo = !(e && *e == '0');
    }
    return s_memo;
}

/* The pipeline state last bound, so a batch binding the same again skips
 * the call. Only state this file alone sets and nothing unbinds behind it;
 * SRVs (which the runtime nulls on a render-target hazard), the vertex and
 * index buffers (new offsets every batch) and the output merger's targets
 * and depth state are always set. Releasing a shader or input layout bumps
 * s_state_gen, since a new object can come back at a released one's
 * address. All 0xFF is "nothing known". */
static uint32_t s_state_gen;
static struct {
    uint32_t                  gen;
    ID3D11BlendState         *bs;
    float                     bf[4];    /* OMSetBlendState's blend factor */
    ID3D11RasterizerState    *rs;
    ID3D11InputLayout        *il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *vs_cb[3], *ps_cb;
    ID3D11SamplerState       *smp[4];
    D3D11_VIEWPORT            vp;
} s_sc;
static int s_sc_init;

static int sc_on(void)
{
    if (!memo_on())
        return 0;
    if (!s_sc_init || s_sc.gen != s_state_gen) {
        memset(&s_sc, 0xFF, sizeof s_sc);
        s_sc.gen = s_state_gen;
        s_sc_init = 1;
    }
    return 1;
}
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
static uint64_t s_draws_bconst;   /* drawn with a CONSTANT_* blend factor */
static uint64_t s_tex_uploads, s_tex_rehash, s_tex_changed;
static uint64_t s_tex_mip_uploads;   /* uploads with more than one level */
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

typedef struct Stage Stage;
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
    uint32_t          key[4];
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
    "    o.oD0 = i.v3; o.oD1 = i.v4; o.oFog = 1.0;\n"
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
    s_cb_vp     = make_buffer(80, D3D11_BIND_CONSTANT_BUFFER);
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

/* One depth-stencil buffer per zeta offset and surface size. The depth half
 * is float, so the test compares the same normalised z (depth units / zmax)
 * the CPU path stores in its float buffer, rather than D24S8's 24-bit fixed
 * point; the 8 stencil bits are the guest's Z24S8 stencil. */
#define DT_MAX 4
typedef struct {
    uint32_t                zeta;
    UINT                    w, h;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t                last_use;
} DepthTarget;

static DepthTarget s_dt[DT_MAX];
#define DSS_MAX 64
static struct { uint64_t key; ID3D11DepthStencilState *dss; } s_dss[DSS_MAX];
static int s_dss_count;
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
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
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
    /* The CPU path starts its buffer at the far plane, stencil 0. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, e->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* GL's stencil op codes as D3D11_STENCIL_OP; anything else keeps, as on the
 * CPU. */
static uint32_t stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* SET_DEPTH_FUNC takes GL's codes, 0x200 NEVER .. 0x207 ALWAYS, in the order
 * D3D11_COMPARISON_* numbers them from 1, and SET_STENCIL_FUNC the same (the
 * walker keeps its low nibble). Anything else tests as ALWAYS, as on the CPU.
 * Depth and stencil each only when enabled; one state per distinct key. */
static ID3D11DepthStencilState *depth_state(int depth, int stencil)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t f = (v->depth_func >= 0x200 && v->depth_func <= 0x207)
               ? v->depth_func - 0x200 : 7;
    uint64_t key = 0;
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    if (depth)
        key |= 1u | f << 1 | (v->depth_mask ? 1u : 0u) << 4;
    if (stencil)
        key |= (uint64_t)(1u | (v->stencil_func & 7u) << 1
                          | stencil_op(v->stencil_op[0]) << 4
                          | stencil_op(v->stencil_op[1]) << 8
                          | stencil_op(v->stencil_op[2]) << 12
                          | (v->stencil_rmask & 0xFFu) << 16
                          | (v->stencil_wmask & 0xFFu) << 24) << 8;
    for (i = 0; i < s_dss_count; i++)
        if (s_dss[i].key == key)
            return s_dss[i].dss;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = depth ? TRUE : FALSE;
    dd.DepthWriteMask = depth && v->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL
                                               : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = depth ? (D3D11_COMPARISON_FUNC)(f + 1) : D3D11_COMPARISON_ALWAYS;
    if (stencil) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = (UINT8)v->stencil_rmask;
        dd.StencilWriteMask = (UINT8)v->stencil_wmask;
        dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[0]);
        dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[1]);
        dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)stencil_op(v->stencil_op[2]);
        dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)((v->stencil_func & 7u) + 1);
        dd.BackFace = dd.FrontFace;             /* GL one-sided stencil */
    }
    if (s_dss_count >= DSS_MAX)
        return s_ds_off;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_dss[s_dss_count].dss)))
        return s_ds_off;
    s_dss[s_dss_count].key = key;
    return s_dss[s_dss_count++].dss;
}

/* CLEAR_SURFACE with the Z bit: the clear value scaled as the CPU path
 * scales it, by the zeta format's range. The stencil bit: its low byte. */
static void d3d11_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;
    float z;

    if (!(param & 3u) || !init())
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
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, dt->dsv,
        ((param & 1u) ? D3D11_CLEAR_DEPTH : 0u) | ((param & 2u) ? D3D11_CLEAR_STENCIL : 0u),
        z, (UINT8)(v->zclear & 0xFFu));
    if (param & 1u)
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

/* A batch's vertex streams, written through one Map of s_vb: vb_begin
 * reserves an upper bound for all of them, vb_alloc hands out 16-aligned
 * pieces of it, vb_end unmaps and gives back what was not used. Without a
 * reservation (memos off, or the bound too large) each piece is its own
 * ring_map, as before. One mapping also means a wrap's DISCARD cannot land
 * between two streams of the same batch. */
static uint8_t *s_vbm_base;
static UINT     s_vbm_at, s_vbm_used, s_vbm_cap;

static void vb_begin(UINT bound)
{
    s_vbm_base = NULL;
    if (!memo_on() || !bound || bound > s_vb_size / 4)
        return;
    s_vbm_base = (uint8_t *)ring_map(s_vb, s_vb_size, &s_vb_pos, bound, 16);
    if (s_vbm_base) {
        s_vbm_at = s_vb_pos - bound;
        s_vbm_used = 0;
        s_vbm_cap = bound;
    }
}

static void *vb_alloc(UINT bytes, UINT *off)
{
    void *p;
    if (s_vbm_base) {
        UINT o = (s_vbm_used + 15u) & ~15u;
        if (o + bytes > s_vbm_cap)
            return NULL;
        s_vbm_used = o + bytes;
        *off = s_vbm_at + o;
        return s_vbm_base + o;
    }
    p = ring_map(s_vb, s_vb_size, &s_vb_pos, bytes, 16);
    if (p)
        *off = s_vb_pos - bytes;
    return p;
}

/* After each piece is written: unmapped now unless it is part of a batch. */
static void vb_written(void)
{
    if (!s_vbm_base)
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
}

static void vb_end(void)
{
    if (s_vbm_base) {
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        s_vb_pos = s_vbm_at + s_vbm_used;
        s_vbm_base = NULL;
    }
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
    uint32_t hash = 0;
    VsEntry *e = NULL;
    ID3DBlob *code = NULL;
    const char *src;
    char name[32];
    int i;
    static struct { int valid, program; uint32_t dwords, start; VsEntry *e; } memo;

    if (memo_on() && memo.valid && memo.program == program
            && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    if (program)
        hash = program_hash(v);
    for (i = 0; i < VS_MAX; i++) {
        if (s_vs[i].used && s_vs[i].hash == hash) {
            memo.valid = 1;
            memo.program = program;
            memo.dwords = v->prog_dwords;
            memo.start = v->start;
            memo.e = s_vs[i].bad ? NULL : &s_vs[i];
            return memo.e;
        }
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
    if (e->lay[k].il) {
        ID3D11InputLayout_Release(e->lay[k].il);
        s_state_gen++;
    }
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
    uint32_t key[4];
    D3D11_BLEND_DESC bd;
    ID3D11BlendState *bs = NULL;
    int i;

    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    key[3] = g->color_keep;             /* SET_COLOR_MASK, inverted */
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
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(
        ((key[3] & 0x00FF0000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_RED)
      | ((key[3] & 0x0000FF00u) ? 0 : D3D11_COLOR_WRITE_ENABLE_GREEN)
      | ((key[3] & 0x000000FFu) ? 0 : D3D11_COLOR_WRITE_ENABLE_BLUE)
      | ((key[3] & 0xFF000000u) ? 0 : D3D11_COLOR_WRITE_ENABLE_ALPHA));
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

/* The factor D3D11_BLEND_BLEND_FACTOR reads: SET_BLEND_COLOR, unpacked
 * from ARGB to RGBA. D3D11 has no CONSTANT_ALPHA, so when only the
 * _ALPHA factors are in use the alpha is replicated into all four; a
 * draw mixing _COLOR and _ALPHA keeps the colour (not seen so far). With
 * no constant factor the default (1,1,1,1) is used, so a colour the title
 * sets without using it does not rebind the blend state. Returns whether a
 * constant factor is in use. */
static int blend_color(float bf[4])
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t s = g->blend_sfactor, d = g->blend_dfactor, c = g->blend_color;
    int col = s == 0x8001 || s == 0x8002 || d == 0x8001 || d == 0x8002;
    int alp = s == 0x8003 || s == 0x8004 || d == 0x8003 || d == 0x8004;

    if (!g->blend_enable || (!col && !alp)) {
        bf[0] = bf[1] = bf[2] = bf[3] = 1.0f;
        return 0;
    }
    bf[3] = (float)(c >> 24) / 255.0f;
    if (col) {
        bf[0] = (float)((c >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((c >>  8) & 0xFF) / 255.0f;
        bf[2] = (float)( c        & 0xFF) / 255.0f;
    } else {
        bf[0] = bf[1] = bf[2] = bf[3];
    }
    return 1;
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
 * Mipmaps: a swizzled or DXT texture brings the levels SET_TEXTURE_FORMAT
 * counts (clamped as xemu does: to CONTROL0's MAX_LOD_CLAMP + 1 and to the
 * level that reaches 1x1), packed one after another, each swizzled or
 * block-compressed at its own size (DXT levels round up to whole 4x4
 * blocks). The level count is part of the cache key and every level is in
 * the hash. Linear textures have one level. RECOMP_D3D11_NO_MIPS=1 uploads
 * level 0 only, for A/B.
 *
 * ponytail: DXT is decoded rather than uploaded as BC1-3; a texture
 * rewritten between two batches of one flip is not seen; cube maps and 3D
 * textures are face/slice 0 only. */

struct Stage {
    uint32_t offset, color, width, height, pitch, addr_u, addr_v;
    uint32_t palette, pal_len;  /* P8: resolved palette address, entries */
    uint32_t filter;            /* SET_TEXTURE_FILTER as written */
    uint32_t levels;            /* mip levels uploaded (1 for linear)    */
    uint32_t lod_min, lod_max;  /* CONTROL0 LOD clamps, within levels    */
    int      valid;
    int      on;        /* CONTROL0 enable as used (never-set stage 0 counts on) */
    int      mode;      /* 0 none, 1 2D, 4 pass-through, other: sampled as 2D */
};

static Stage s_stage[4];
/* What each stage's SRV came from, for the batch trace. */
static const char   *s_stage_src[4];
static RenderTarget *s_stage_rt[4];

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 512   /* a demo frame binds ~100; 64 thrashed */
typedef struct {
    uint32_t offset, color, width, height, pitch, palette, levels;
    uint32_t gen, used;
    uint64_t hash;
    int      live;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
} TexEntry;

static TexEntry  s_tc[TEX_CACHE];
static uint32_t  s_tc_clock;
/* Key hash -> entry + 1, checked before the scan. A live entry with a key
 * is the only one (one is made only when the scan finds none), so a hint
 * that matches is the entry the scan would find; a stale or colliding one
 * just falls through to it. */
#define TC_HINTS 2048
static uint16_t  s_tc_hint[TC_HINTS];

static uint32_t tc_slot(const Stage *t)
{
    uint32_t h = t->offset * 0x9E3779B1u;
    h ^= (t->color + (t->width << 8) + (t->height << 20)) * 0x85EBCA77u;
    h ^= (t->pitch ^ t->palette ^ (t->levels << 27)) * 0xC2B2AE3Du;
    return (h >> 16) & (TC_HINTS - 1);
}
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

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("RECOMP_D3D11_NO_MIPS") != NULL;
    return off;
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
        t->filter = r[0x14 / 4];
        /* P8: SET_TEXTURE_PALETTE (0x1B20): offset in bits 6-31, length
         * 256 >> bits 2-3, entries A8R8G8B8. */
        if (t->color == 0x0B) {
            uint32_t pal = r[0x20 / 4];
            t->palette = (pal & ~0x3Fu) ? nv2a_pb_dma_resolve(pal & ~0x3Fu) : 0;
            t->pal_len = 256u >> ((pal >> 2) & 3u);
        }
        t->levels = 1;
        if (tex_size_from_format(t->color) && !no_mips()) {
            /* As xemu's pgraph_get_texture_shape: the format's count, no
             * more than MAX_LOD_CLAMP + 1, none smaller than 1x1. The
             * clamps are whole levels (xemu reads the 12-bit fields as is). */
            uint32_t lu = (fmt >> 20) & 0xF, lv = (fmt >> 24) & 0xF;
            uint32_t lmax = (ctl0 >> 6) & 0xFFF, lmin = (ctl0 >> 18) & 0xFFF;
            uint32_t lv_n = (fmt >> 16) & 0xF;
            if (lv_n > lmax + 1u) lv_n = lmax + 1u;
            if (lv_n > (lu > lv ? lu : lv) + 1u) lv_n = (lu > lv ? lu : lv) + 1u;
            t->levels = lv_n ? lv_n : 1u;
            t->lod_min = lmin < t->levels - 1u ? lmin : t->levels - 1u;
            t->lod_max = lmax < t->levels - 1u ? lmax : t->levels - 1u;
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
        t->on = on;
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
    case 0x28: return 0x17;     /* G8B8 */
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
    case 0x20: case 0x37: case 0x35: case 0x3D: case 0x3E: case 0x17: return 2;
    case 0x13: case 0x1F: case 0x1B: case 0x0B: return 1;
    default: return 0;
    }
}

/* Bytes of guest memory the texture occupies; 0 if it cannot be decoded. */
static uint32_t tex_extent(const Stage *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color), tb, l, w, h, n = 0;
    if (!t->valid || (uint64_t)t->width * t->height > TEX_MAX_TEXELS)
        return 0;
    if (bb || d3d8_format_is_swizzled(t->color)) {
        tb = bb ? 0 : texel_bytes(linear_twin(t->color));
        if (!bb && !tb)
            return 0;
        for (l = 0, w = t->width, h = t->height; l < t->levels; l++) {
            n += bb ? ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb : w * h * tb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return n;
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
    case 0x17:                              /* G8B8: xemu RG8, swizzle (R,G,R,G) */
        t = ((const uint16_t *)p)[u];
        return ((t >> 8) << 24) | ((t & 0xFFu) << 16) | ((t >> 8) << 8) | (t & 0xFFu);
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

/* Level l's size: halved per level, never below 1. */
static uint32_t level_dim(uint32_t d, uint32_t l)
{
    d >>= l;
    return d ? d : 1u;
}

/* Texels in levels 0..levels-1, the size s_texels needs. */
static size_t tex_texels(const Stage *t)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < t->levels; l++)
        n += (size_t)level_dim(t->width, l) * level_dim(t->height, l);
    return n;
}

/* One swizzled or DXT level of w x h at mem into out (A8R8G8B8); returns
 * the guest bytes it took, which is where the next level starts. */
static uint32_t decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                             uint32_t h, uint32_t *out)
{
    uint32_t x, y, bb = d3d8_format_dxt_block_bytes(fmt);
    if (bb) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, w, &argb);
                out[(size_t)y * w + x] = argb;
            }
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb;
    } else {
        uint32_t mx, my, lin = linear_twin(fmt);
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < w; x++)
                out[(size_t)y * w + x] =
                    decode_texel(lin, mem, swizzle_deposit(x, mx) | oy);
        }
        return w * h * texel_bytes(lin);
    }
}

/* The whole texture into s_texels (A8R8G8B8): level 0 (width x height),
 * then each further level packed after it. */
static int decode_texture(const Stage *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    size_t n = tex_texels(t);
    uint32_t x, y, fmt = t->color;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n;
    }
    if (fmt == 0x0B) {
        s_pal = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset()
                                   + t->palette);
        s_pal_len = t->pal_len;
    }
    if (tex_size_from_format(fmt)) {
        uint32_t *out = s_texels, l;
        for (l = 0; l < t->levels; l++) {
            uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);
            mem += decode_level(mem, fmt, w, h, out);
            out += (size_t)w * h;
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
    const uint32_t *src;
    uint32_t l;
    if (!decode_texture(t))
        return 0;
    if (!e->tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = t->levels;
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
    for (l = 0, src = s_texels; l < t->levels; l++) {
        uint32_t w = level_dim(t->width, l), h = level_dim(t->height, l);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex,
                                              l, NULL, src, w * 4, 0);
        src += (size_t)w * h;
    }
    s_tex_uploads++;
    if (t->levels > 1)
        s_tex_mip_uploads++;
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
    uint32_t slot;

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
    slot = tc_slot(t);
    if (memo_on() && s_tc_hint[slot]) {
        TexEntry *c = &s_tc[s_tc_hint[slot] - 1];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette
                && c->levels == t->levels)
            e = c;
    }
    for (i = 0; !e && i < TEX_CACHE; i++) {
        TexEntry *c = &s_tc[i];
        if (c->live && c->offset == t->offset && c->color == t->color
                && c->width == t->width && c->height == t->height
                && c->pitch == t->pitch && c->palette == t->palette
                && c->levels == t->levels) {
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
        e->levels = t->levels;
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        if (!tex_upload(e, t)) {
            tex_free(e);
            return s_black_srv;
        }
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    s_tc_hint[slot] = (uint16_t)(e - s_tc + 1);
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

/* SET_TEXTURE_FILTER: LOD bias in bits 0-12 (signed, 8 fraction bits), MIN
 * in bits 16-21, MAG in bits 24-27. MIN: 1 nearest and 2 linear on level 0,
 * 3/4 nearest/linear within the nearest level, 5/6 nearest/linear blended
 * between two levels, 7 convolution on level 0 (taken as linear). MAG: 1
 * nearest, 2 linear, 4 convolution (linear). The LOD range is CONTROL0's
 * clamps, as xemu's sampler: 0..0 when MIN does not mipmap. RECOMP_D3D11_POINT=1
 * forces nearest texels (the CPU path's sampling) for every stage. */
static int filter_linear(uint32_t f, int mag)
{
    uint32_t m = mag ? (f >> 24) & 0xF : (f >> 16) & 0x3F;
    if (mag)
        return m == 2 || m == 4;
    return m == 2 || m == 4 || m == 6 || m == 7;
}

/* 0: level 0 only, 1: nearest level, 2: blend two levels. */
static int filter_mip(uint32_t f)
{
    uint32_t m = (f >> 16) & 0x3F;
    return m == 3 || m == 4 ? 1 : m == 5 || m == 6 ? 2 : 0;
}

static float lod_bias(uint32_t f)
{
    int b = (int)(f & 0x1FFFu);
    if (b & 0x1000)
        b -= 0x2000;
    return (float)b / 256.0f;
}

#define SMP_MAX 256
static struct { uint64_t key; ID3D11SamplerState *s; } s_smp[SMP_MAX];
static int s_smp_n;
static int s_smp_last[4];   /* per stage: the entry it got last */

static ID3D11SamplerState *sampler(const Stage *t)
{
    static int point = -1;
    static ID3D11SamplerState *fallback;
    D3D11_SAMPLER_DESC sd;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i;
    uint64_t key;
    ID3D11SamplerState *st = NULL;

    if (point < 0)
        point = getenv("RECOMP_D3D11_POINT") != NULL;
    mn = !point && filter_linear(t->filter, 0);
    mg = !point && filter_linear(t->filter, 1);
    mip = t->levels > 1 ? filter_mip(t->filter) : 0;
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9
        | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)(t->filter & 0x1FFFu) << 12
             | (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    if (s_smp_last[t - s_stage] < s_smp_n
            && s_smp[s_smp_last[t - s_stage]].key == key)
        return s_smp[s_smp_last[t - s_stage]].s;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key) {
            s_smp_last[t - s_stage] = i;
            return s_smp[i].s;
        }
    memset(&sd, 0, sizeof sd);
    sd.Filter = (D3D11_FILTER)((mn ? 0x10 : 0) | (mg ? 0x4 : 0)
                               | (mip == 2 ? 0x1 : 0));
    sd.AddressU = address_mode(u);
    sd.AddressV = address_mode(v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MipLODBias = mip ? lod_bias(t->filter) : 0.0f;
    if (sd.MipLODBias > 15.99f) sd.MipLODBias = 15.99f;
    if (sd.MipLODBias < -16.0f) sd.MipLODBias = -16.0f;
    sd.MinLOD = mip ? (float)t->lod_min : 0.0f;
    sd.MaxLOD = mip ? (float)t->lod_max : 0.0f;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &st);
    if (!st)
        return fallback;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        /* Full: replace one round robin. A released state's address can
         * come back, so the bound-state filter starts over. */
        static int next;
        i = next++ % SMP_MAX;
        if (s_smp[i].s == fallback)
            fallback = NULL;
        ID3D11SamplerState_Release(s_smp[i].s);
        s_state_gen++;
    }
    s_smp[i].key = key;
    s_smp[i].s = st;
    s_smp_last[t - s_stage] = i;
    if (!fallback)
        fallback = st;
    return st;
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
static RcEntry *s_rc_last;      /* the entry combiner_ps last returned from */

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
            s_rc_last = c;
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
        if (e->ps) {
            ID3D11PixelShader_Release(e->ps);
            s_state_gen++;
        }
    }
    memset(e, 0, sizeof *e);
    s_rc_last = e;
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
/* The surface being drawn into, sampled by the same batch (a full-screen
 * post pass reading the frame so far). D3D11 cannot bind one texture as both
 * target and resource, and the guest memory under it is empty -- the frame
 * lives on the GPU -- so the batch gets a copy taken just before it draws.
 * The CPU path reads the same thing: the surface memory as drawn so far. */
static RenderTarget     s_self;         /* tex/srv/w/h of the copy */
static uint64_t         s_self_copies;

static RenderTarget *rt_self_copy(RenderTarget *rt)
{
    if (s_self.tex && (s_self.w != rt->w || s_self.h != rt->h)) {
        if (s_self.srv) ID3D11ShaderResourceView_Release(s_self.srv);
        ID3D11Texture2D_Release(s_self.tex);
        s_self.tex = NULL;
        s_self.srv = NULL;
    }
    if (!s_self.tex) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(rt->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_self.tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)s_self.tex, NULL, &s_self.srv))) {
            ID3D11Texture2D_Release(s_self.tex);
            s_self.tex = NULL;
            return NULL;
        }
        s_self.w = rt->w;
        s_self.h = rt->h;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_self.tex,
                                     (ID3D11Resource *)rt->tex);
    s_self.offset = rt->offset;
    s_self.addr = rt->addr;
    s_self.last_draw = rt->last_draw;
    s_self.last_clear = rt->last_clear;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", rt->offset);
    }
    return &s_self;
}

static RenderTarget *rt_texture(const Stage *t)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i;

    if (off < 0)
        off = getenv("RECOMP_D3D11_NO_RTT") != NULL;   /* A/B: decode memory */
    if (off)
        return NULL;
    if (!t->offset || d3d8_format_dxt_block_bytes(t->color)
            || texel_bytes(linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        /* Low 27 bits: the same memory through either window. */
        if (!rt->tex || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (rt == s_cur) {
            self = rt;
            continue;
        }
        if (!rt->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv)))
            return NULL;
        return rt;
    }
    return self ? rt_self_copy(self) : NULL;
}

/* The combiner state of the batch being drawn (for the trace), and whether
 * it ran through combiners at all. */
static NV2ACombinerState s_last_rc;
static int               s_last_rc_on;

/* RECOMP_D3D11_DEBUG_PS=<what>: every batch draws one input of the pixel
 * stage instead of its real shader, to see which one goes wrong:
 *   d0 / d1     the interpolated diffuse / specular colour
 *   t0 .. t3    stage n's texture at its coordinates (alpha forced to 1)
 *   tc0 .. tc3  stage n's coordinates as colour (frac of s, t)
 * Unknown values draw white. */
/* RECOMP_D3D11_DEBUG_PS_SKIP_POST=1: batches that look like post passes --
 * depth test off, at most 8 vertices (a full-screen quad or two) -- keep
 * their real shader, so a glow or blur drawn over the scene does not
 * cover what the debug shader shows. Set per batch by on_draw. */
static int s_dbg_post;

static ID3D11PixelShader *debug_ps(void)
{
    static int tried;
    static ID3D11PixelShader *ps;
    const char *w;
    char src[4096], expr[256];
    ID3DBlob *code = NULL;

    if (tried)
        return ps;
    tried = 1;
    w = getenv("RECOMP_D3D11_DEBUG_PS");
    if (!w || !*w)
        return NULL;
    if (!strcmp(w, "d0"))
        snprintf(expr, sizeof expr, "float4(i.oD0.rgb, 1)");
    else if (!strcmp(w, "d1"))
        snprintf(expr, sizeof expr, "float4(i.oD1.rgb, 1)");
    else if (w[0] == 't' && w[1] >= '0' && w[1] <= '3' && !w[2])
        snprintf(expr, sizeof expr,
                 "float4(tex%c.Sample(samp%c, i.oT%c.xy * tex_scale[%c].xy).rgb, 1)",
                 w[1], w[1], w[1], w[1]);
    else if (w[0] == 't' && w[1] == 'c' && w[2] >= '0' && w[2] <= '3' && !w[3])
        snprintf(expr, sizeof expr, "float4(frac(i.oT%c.xy), 0, 1)", w[2]);
    else
        snprintf(expr, sizeof expr, "float4(1, 1, 1, 1)");
    snprintf(src, sizeof src, "%s%s"
             "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
             "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
             "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
             "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
             "float4 main(VS_OUT i) : SV_Target { return %s; }\n",
             VS_OUT_DECL, PS_CB_DECL, expr);
    if (compile("nv2a_ps_debug", src, "ps_5_0", &code)) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps);
        ID3D10Blob_Release(code);
    }
    fprintf(stderr, LOGP "RECOMP_D3D11_DEBUG_PS=%s: every batch draws %s%s\n",
            w, expr, ps ? "" : " (FAILED to build)");
    return ps;
}

/* Stages, textures, samplers and PS constants for the batch about to be
 * drawn. Returns the pixel shader to use. */
static PsConsts s_cb_ps_shadow;
static int      s_cb_ps_ok;

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
        s_stage_rt[n] = src;
        s_stage_src[n] = !t->mode ? "none" : t->mode == 4 ? "passthru" : "cache";
        if (src) {
            s_stage_src[n] = src == &s_self ? "render-target(self copy)"
                                            : "render-target";
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
            if (srv[n] == s_magenta_srv) s_stage_src[n] = "MAGENTA(undecoded)";
            else if (srv[n] == s_black_srv) s_stage_src[n] = "BLACK(too large/failed)";
            textured = 1;
        } else {
            srv[n] = s_black_srv;
        }
        smp[n] = sampler(t);
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
    if (memo_on() && s_cb_ps_ok && !memcmp(&s_cb_ps_shadow, &pc, sizeof pc)) {
        /* the buffer already holds these bytes */
    } else if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_ps, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_ps, 0);
        s_cb_ps_shadow = pc;
        s_cb_ps_ok = 1;
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    if (!sc_on() || memcmp(s_sc.smp, smp, sizeof smp)) {
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
        memcpy(s_sc.smp, smp, sizeof smp);
    }
    if (!sc_on() || s_sc.ps_cb != s_cb_ps) {
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_ps);
        s_sc.ps_cb = s_cb_ps;
    }
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
        /* The registers and modes that make the state: the same as the last
         * batch's give the same state and so the same shader. */
        static struct {
            uint32_t r[36];
            int      key_ok, ps_ok;   /* s_last_rc is r's state; ps is its shader */
            ID3D11PixelShader *ps;
            RcEntry *e;
            uint64_t compiles;
        } memo;
        uint32_t key[36];
        int hit;
        memcpy(key, v->rc_cicw, 32);
        memcpy(key + 8, v->rc_aicw, 32);
        memcpy(key + 16, v->rc_cocw, 32);
        memcpy(key + 24, v->rc_aocw, 32);
        key[32] = v->rc_ctl;
        key[33] = v->rc_fcw0;
        key[34] = v->rc_fcw1;
        key[35] = modes;
        hit = memo_on() && memo.key_ok && !memcmp(memo.r, key, sizeof key);
        if (!hit) {
            d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw,
                                     v->rc_aocw, v->rc_ctl, v->rc_fcw0,
                                     v->rc_fcw1, modes, &st);
            st.fog_input = 1;   /* FOG.a is the interpolated vertex fog factor */
            s_last_rc = st;
            memcpy(memo.r, key, sizeof key);
            memo.key_ok = 1;
            memo.ps_ok = 0;
        }
        s_last_rc_on = 1;
        if (!s_dbg_post && (ps = debug_ps()) != NULL)
            return ps;
        /* A compile since may have evicted the entry remembered. */
        if (hit && memo.ps_ok && memo.compiles == s_rc_compiles) {
            memo.e->last_use = s_tick;   /* as combiner_ps's hit does */
            ps = memo.ps;
        } else {
            ps = combiner_ps(&s_last_rc);
            memo.ps = ps;
            memo.e = s_rc_last;
            memo.compiles = s_rc_compiles;
            memo.ps_ok = 1;
        }
        if (ps)
            return ps;
    }
    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        s_last_rc_on = 0;
    {
        ID3D11PixelShader *ps = s_dbg_post ? NULL : debug_ps();
        if (ps)
            return ps;
    }
    return s_ps;
}


/* ---- batch trace (RECOMP_WHITE_TRACE) ---------------------------------------
 *
 * The D3D11 side of the CPU path's [WHITE] trace, selected the same way:
 * RECOMP_WHITE_TRACE=N traces the first N program batches of at least
 * RECOMP_WHITE_MIN vertices (default 300), or with RECOMP_WHITE_TEXOFF=<hex>
 * the first N with a texture at that address (low 27 bits). For each one:
 * the state, every stage and where its SRV came from, the input layout, the
 * vertex program's outputs for the first three vertices twice -- from the CPU
 * interpreter on the same inputs, and read back from the GPU through stream
 * output -- the generated HLSL (once per program/combiner), and the colour
 * of the first triangle's centroid before and after the draw. Lines start
 * "[D3D11-WHITE]". */

static int trace_pick(const struct nv2a_pb_draw *d)
{
    static int left = -1, min_n, by_tex;
    static uint32_t texoff;
    int n;

    if (left < 0) {
        const char *e = getenv("RECOMP_WHITE_TRACE"), *m = getenv("RECOMP_WHITE_MIN");
        const char *to = getenv("RECOMP_WHITE_TEXOFF");
        left = e ? atoi(e) : 0;
        min_n = m ? atoi(m) : 300;
        if (to) {
            by_tex = 1;
            texoff = (uint32_t)strtoul(to, NULL, 16);
        }
    }
    if (left <= 0 || !d->program)
        return 0;
    if (by_tex) {
        int hit = 0;
        for (n = 0; n < 4; n++)
            if (s_stage[n].offset && ((s_stage[n].offset ^ texoff) & 0x07FFFFFFu) == 0)
                hit = 1;
        if (!hit)
            return 0;
    } else if (d->idx_count < (uint32_t)min_n) {
        return 0;
    }
    left--;
    return 1;
}

static uint32_t read_pixel(RenderTarget *rt, int x, int y)
{
    static ID3D11Texture2D *st;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX b;
    uint32_t px = 0xDEADBEEFu;

    if (x < 0 || y < 0 || (UINT)x >= rt->w || (UINT)y >= rt->h)
        return px;
    if (!st) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
            return px;
    }
    b.left = (UINT)x; b.right = (UINT)x + 1;
    b.top = (UINT)y; b.bottom = (UINT)y + 1;
    b.front = 0; b.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                              (ID3D11Resource *)rt->tex, 0, &b);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        px = *(const uint32_t *)m.pData;   /* B,G,R,A bytes: 0xAARRGGBB */
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    return px;
}

/* The first three vertices of the bound batch through the VS, read back by
 * stream output: 7 float4 each (SV_POSITION, COLOR0/1, TEXCOORD0..3). */
static int trace_stream_out(VsEntry *vs, D3D11_PRIMITIVE_TOPOLOGY topo,
                            float out[3][7][4])
{
    static const D3D11_SO_DECLARATION_ENTRY so[7] = {
        {0, "SV_POSITION", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0},
        {0, "COLOR", 1, 0, 4, 0},       {0, "TEXCOORD", 0, 0, 4, 0},
        {0, "TEXCOORD", 1, 0, 4, 0},    {0, "TEXCOORD", 2, 0, 4, 0},
        {0, "TEXCOORD", 3, 0, 4, 0},
    };
    static ID3D11Buffer *buf, *stage;
    UINT stride = 7 * 16, zero = 0;
    ID3D11GeometryShader *gs = NULL;
    ID3D11Buffer *none = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    int ok = 0;

    if (!buf) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 3 * 7 * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &buf)))
            return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stage)))
            return 0;
    }
    hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(
        s_dev, ID3D10Blob_GetBufferPointer(vs->code), ID3D10Blob_GetBufferSize(vs->code),
        so, 7, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11-WHITE]  stream output: hr 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &buf, &zero);
    ID3D11DeviceContext_GSSetShader(s_ctx, gs, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_DrawIndexed(s_ctx, 3, 0, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &none, &zero);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
    ID3D11GeometryShader_Release(gs);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)buf);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, 3 * 7 * 16);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
        ok = 1;
    }
    return ok;
}

static void trace_print_hlsl_once(uint32_t key, const char *what, const char *src)
{
    static uint32_t seen[64];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (seen[i] == key)
            return;
    if (n < 64)
        seen[n++] = key;
    fprintf(stderr, "[D3D11-WHITE] ---- %s %08X ----\n%s\n[D3D11-WHITE] ---- end ----\n",
            what, key, src);
}

/* Before the draw: everything but the pixel. Returns the pixel to read back
 * after it in *px, *py (-1 if none). */
static void trace_batch(const struct nv2a_pb_draw *d, VsEntry *vs, RenderTarget *rt,
                        const uint8_t fmts[16], const UINT strides[16],
                        const UINT offs[16], D3D11_PRIMITIVE_TOPOLOGY topo,
                        int *px, int *py)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    float so[3][7][4];
    int k, i, have_so;

    *px = *py = -1;
    fprintf(stderr, "[D3D11-WHITE] batch prim %u n %u prog %08X start %u | rc %d ctl %08X"
            " shader %08X | depth %d func 0x%X mask %d | blend %d 0x%X 0x%X | atest %d"
            " 0x%X %u | surface 0x%08X %ux%u | fog en %d colour %08X\n",
            d->prim, d->idx_count, vs->hash, v->start, s_last_rc_on, v->rc_ctl,
            v->shader_prog, v->depth_enable, v->depth_func, v->depth_mask,
            g->blend_enable, g->blend_sfactor, g->blend_dfactor, v->atest_enable,
            v->atest_func, v->atest_ref, rt->offset, rt->w, rt->h, v->fog_enable,
            v->fog_color);
    for (k = 0; k < 4; k++) {
        const Stage *t = &s_stage[k];
        fprintf(stderr, "[D3D11-WHITE]  tex%d mode %d on %d fmt %02X %ux%u off %08X pitch %u"
                " addr %u/%u filter %08X pal %08X src %s", k, t->mode, t->on, t->color,
                t->width, t->height, t->offset, t->pitch, t->addr_u, t->addr_v,
                t->filter, t->palette,
                s_stage_src[k] ? s_stage_src[k] : "?");
        if (s_stage_rt[k])
            fprintf(stderr, " (surface 0x%08X %ux%u, drawn %llu, cleared %llu)",
                    s_stage_rt[k]->offset, s_stage_rt[k]->w, s_stage_rt[k]->h,
                    (unsigned long long)s_stage_rt[k]->last_draw,
                    (unsigned long long)s_stage_rt[k]->last_clear);
        fprintf(stderr, "\n");
    }
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        if (!(vs->inputs & (1u << i)) && !a->size)
            continue;
        fprintf(stderr, "[D3D11-WHITE]  attr%d %s type %u size %u stride %u offset 0x%X"
                " inline %d | IA dxgi %u stride %u at %u | inl %g %g %g %g\n", i,
                (vs->inputs & (1u << i)) ? "read" : "unread", a->type, a->size,
                a->stride, a->offset, d->inline_data, fmts[i], strides[i], offs[i],
                v->inl[i][0], v->inl[i][1], v->inl[i][2], v->inl[i][3]);
    }
    fprintf(stderr, "[D3D11-WHITE]  c58 %g %g %g %g c59 %g %g %g %g\n",
            v->c[58][0], v->c[58][1], v->c[58][2], v->c[58][3],
            v->c[59][0], v->c[59][1], v->c[59][2], v->c[59][3]);

    have_so = trace_stream_out(vs, topo, so);
    for (k = 0; k < 3 && (uint32_t)k < d->idx_count; k++) {
        float in[NV2A_VSH_INPUTS][4];
        Nv2aVshOut o;
        uint32_t index = d->idx[k];
        memset(in, 0, sizeof in);
        for (i = 0; i < NV2A_VSH_INPUTS && i < NV2A_VS_MAX_INPUTS; i++) {
            const VertexAttr *a = &d->attr[i];
            if (!(a->size && a->stride) || !fetch(d, a, index, in[i]))
                memcpy(in[i], v->inl[i], 16);
        }
        nv2a_vsh_run((const uint32_t (*)[4])v->prog, v->start,
                     (float (*)[4])v->c, 0, (const float (*)[4])in, &o);
        fprintf(stderr, "[D3D11-WHITE]  vert %u (index %u):", k, index);
        for (i = 0; i < 16; i++)
            if (d->attr[i].size)
                fprintf(stderr, " v%d(%g %g %g %g)", i, in[i][0], in[i][1], in[i][2], in[i][3]);
        fprintf(stderr, "\n[D3D11-WHITE]   cpu pos %g %g %g %g D0 %g %g %g %g D1 %g %g %g %g"
                " T0 %g %g %g %g T1 %g %g %g %g\n",
                o.o[0][0], o.o[0][1], o.o[0][2], o.o[0][3],
                o.o[3][0], o.o[3][1], o.o[3][2], o.o[3][3],
                o.o[4][0], o.o[4][1], o.o[4][2], o.o[4][3],
                o.o[9][0], o.o[9][1], o.o[9][2], o.o[9][3],
                o.o[10][0], o.o[10][1], o.o[10][2], o.o[10][3]);
        if (have_so)
            fprintf(stderr, "[D3D11-WHITE]   gpu clip %g %g %g %g D0 %g %g %g %g D1 %g %g %g %g"
                    " T0 %g %g %g %g T1 %g %g %g %g\n",
                    so[k][0][0], so[k][0][1], so[k][0][2], so[k][0][3],
                    so[k][1][0], so[k][1][1], so[k][1][2], so[k][1][3],
                    so[k][2][0], so[k][2][1], so[k][2][2], so[k][2][3],
                    so[k][3][0], so[k][3][1], so[k][3][2], so[k][3][3],
                    so[k][4][0], so[k][4][1], so[k][4][2], so[k][4][3]);
    }
    if (have_so && d->idx_count >= 3) {
        float sx = 0, sy = 0;
        int ok = 1;
        for (k = 0; k < 3; k++) {
            float w = so[k][0][3];
            if (w <= 0.0f) { ok = 0; break; }
            sx += (so[k][0][0] / w * 0.5f + 0.5f) * (float)rt->w;
            sy += (0.5f - so[k][0][1] / w * 0.5f) * (float)rt->h;
        }
        if (ok) {
            *px = (int)(sx / 3.0f);
            *py = (int)(sy / 3.0f);
        }
    }
    {
        static NV2AVshProgram parsed;
        static char hlsl[65536];
        d3d8_vsh_parse((const DWORD *)&v->prog[v->start][0],
                       NV2A_VSH_SLOTS - (int)v->start, &parsed);
        if (d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      hlsl, sizeof hlsl) > 0)
            trace_print_hlsl_once(vs->hash, "vertex program", hlsl);
        if (s_last_rc_on && d3d8_combiners_generate_hlsl(&s_last_rc, hlsl, (int)sizeof hlsl) >= 0) {
            uint32_t h = 2166136261u;
            const char *c;
            for (c = hlsl; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
            trace_print_hlsl_once(h, "combiner", hlsl);
        }
    }
    {
        for (k = 0; k < 8; k++)
            fprintf(stderr, "%s%08X/%08X%s", k ? " " : "[D3D11-WHITE]  factors c0/c1: ",
                    v->rc_f0[k], v->rc_f1[k], k == 7 ? "\n" : "");
        fprintf(stderr, "[D3D11-WHITE]  final cw %08X %08X, sf %08X %08X\n",
                v->rc_fcw0, v->rc_fcw1, v->rc_sf[0], v->rc_sf[1]);
    }
    if (*px >= 0)
        fprintf(stderr, "[D3D11-WHITE]  px (%d,%d) before %08X\n", *px, *py,
                read_pixel(rt, *px, *py));
}

/* ---- draw ------------------------------------------------------------------ */

/* ---- pixel probe ------------------------------------------------------------
 *
 * RECOMP_D3D11_PX=x,y[;x,y...] (up to 4) with RECOMP_D3D11_PX_FLIPS=a-b
 * (this backend's present count, the "[D3D11] flip N" number; default 0-0
 * means the first frame only): every batch drawn in that window that covers
 * one of the pixels is redrawn twice into eight float targets, with the same
 * vertex shader, inputs, textures and samplers, depth tested LESS_EQUAL
 * without writes against the depth the real draw left, and the pixel's
 * interpolated inputs read back:
 *   pass A  oD0, oD1, oT0..oT3 as interpolated, SV_Position, fog
 *   pass B  stage 0..3 texels as the combiner samples them (q divide for
 *           true 2D stages, tex_scale) and the coordinates it samples at
 * Lines start "[D3D11-PX]": the colour before and after the real draw, then
 * the inputs; then the batch's full [D3D11-WHITE] block (stages with mode,
 * enable, address and filter, attributes, vertex outputs, combiner factors;
 * the HLSL once per program/combiner).
 * RECOMP_D3D11_PX_MAX caps the batches printed (default 400). Alpha test and
 * stencil are not applied to the probe, so "covered" can include a fragment
 * the real draw discarded: compare before/after. */
static int s_px_n = -1, s_px_x[4], s_px_y[4], s_px_max;
static uint64_t s_px_f0, s_px_f1;

static int px_on(void)
{
    if (s_px_n < 0) {
        const char *e = getenv("RECOMP_D3D11_PX"), *f = getenv("RECOMP_D3D11_PX_FLIPS");
        const char *m = getenv("RECOMP_D3D11_PX_MAX");
        s_px_n = 0;
        while (e && *e && s_px_n < 4) {
            int x, y;
            if (sscanf(e, "%d,%d", &x, &y) != 2)
                break;
            s_px_x[s_px_n] = x;
            s_px_y[s_px_n] = y;
            s_px_n++;
            e = strchr(e, ';');
            if (e) e++;
        }
        if (f) {
            unsigned long long a = 0, b = 0;
            int k = sscanf(f, "%llu-%llu", &a, &b);
            s_px_f0 = a;
            s_px_f1 = k == 2 ? b : a;
        }
        s_px_max = m ? atoi(m) : 400;
        if (s_px_n)
            fprintf(stderr, LOGP "RECOMP_D3D11_PX: %d pixel(s), flips %llu-%llu\n",
                    s_px_n, (unsigned long long)s_px_f0,
                    (unsigned long long)s_px_f1);
    }
    return s_px_n > 0 && s_px_max > 0 && s_presents >= s_px_f0 && s_presents <= s_px_f1;
}

static ID3D11PixelShader *px_shader(int pass)
{
    static ID3D11PixelShader *ps[2];
    static int tried[2];
    char src[6144];
    ID3DBlob *code = NULL;

    if (tried[pass])
        return ps[pass];
    tried[pass] = 1;
    snprintf(src, sizeof src, "%s%s"
        "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
        "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
        "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
        "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
        "struct PO { float4 o0 : SV_Target0; float4 o1 : SV_Target1;\n"
        "    float4 o2 : SV_Target2; float4 o3 : SV_Target3; float4 o4 : SV_Target4;\n"
        "    float4 o5 : SV_Target5; float4 o6 : SV_Target6; float4 o7 : SV_Target7; };\n"
        "float2 uvof(float4 t, uint n) {\n"
        "    float2 uv = t.xy;\n"
        "    if (tex_mode[n] == 1u && t.w != 0.0 && t.w != 1.0) uv /= t.w;\n"
        "    return uv * tex_scale[n].xy;\n"
        "}\n"
        "PO main(VS_OUT i) {\n"
        "    PO o;\n"
        "%s"
        "    return o;\n"
        "}\n",
        VS_OUT_DECL, PS_CB_DECL,
        pass == 0
        ? "    o.o0 = i.oD0; o.o1 = i.oD1; o.o2 = i.oT0; o.o3 = i.oT1;\n"
          "    o.o4 = i.oT2; o.o5 = i.oT3; o.o6 = i.oPos;\n"
          "    o.o7 = float4(i.oFog, 1, 0, 0);\n"
        : "    float2 u0 = uvof(i.oT0, 0), u1 = uvof(i.oT1, 1);\n"
          "    float2 u2 = uvof(i.oT2, 2), u3 = uvof(i.oT3, 3);\n"
          "    o.o0 = tex0.Sample(samp0, u0); o.o1 = tex1.Sample(samp1, u1);\n"
          "    o.o2 = tex2.Sample(samp2, u2); o.o3 = tex3.Sample(samp3, u3);\n"
          "    o.o4 = float4(u0, 0, 0); o.o5 = float4(u1, 0, 0);\n"
          "    o.o6 = float4(u2, 0, 0); o.o7 = float4(u3, 0, 0);\n");
    if (compile("nv2a_ps_probe", src, "ps_5_0", &code)) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps[pass]);
        ID3D10Blob_Release(code);
    }
    if (!ps[pass])
        fprintf(stderr, LOGP "RECOMP_D3D11_PX: probe shader %d FAILED to build\n", pass);
    return ps[pass];
}

static void px_probe(const struct nv2a_pb_draw *d, VsEntry *vs, RenderTarget *rt,
                     const uint8_t fmts[16], const UINT strides[16], const UINT offs[16],
                     D3D11_PRIMITIVE_TOPOLOGY topo, UINT nidx,
                     ID3D11DepthStencilView *dsv, const uint32_t before[4])
{
    static ID3D11Texture2D *tex[8], *st;
    static ID3D11RenderTargetView *rtv[8];
    static UINT tw, th;
    static ID3D11DepthStencilState *dss_le;
    static const char *names[2][8] = {
        {"D0", "D1", "T0", "T1", "T2", "T3", "pos", "fog/cover"},
        {"tex0", "tex1", "tex2", "tex3", "uv0", "uv1", "uv2", "uv3"}};
    float val[2][4][8][4];
    const float zero[4] = {0, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    int pass, k, n, any = 0;

    if (!px_shader(0) || !px_shader(1))
        return;
    if (!tex[0] || tw != rt->w || th != rt->h) {
        D3D11_TEXTURE2D_DESC td;
        for (k = 0; k < 8; k++) {
            if (rtv[k]) ID3D11RenderTargetView_Release(rtv[k]);
            if (tex[k]) ID3D11Texture2D_Release(tex[k]);
            rtv[k] = NULL;
            tex[k] = NULL;
        }
        memset(&td, 0, sizeof td);
        td.Width = rt->w;
        td.Height = rt->h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (k = 0; k < 8; k++)
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex[k]))
                    || FAILED(ID3D11Device_CreateRenderTargetView(
                        s_dev, (ID3D11Resource *)tex[k], NULL, &rtv[k])))
                return;
        tw = rt->w;
        th = rt->h;
    }
    if (!st) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = 8;
        td.Height = 4;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
            return;
    }
    if (!dss_le) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &dss_le);
    }

    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < 8; k++)
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv[k], zero);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 8, rtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dsv ? dss_le : s_ds_off, 0);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_PSSetShader(s_ctx, px_shader(pass), NULL, 0);
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
        for (n = 0; n < s_px_n; n++) {
            D3D11_BOX b;
            if (s_px_x[n] < 0 || s_px_y[n] < 0
                    || (UINT)s_px_x[n] >= rt->w || (UINT)s_px_y[n] >= rt->h)
                continue;
            b.left = (UINT)s_px_x[n]; b.right = b.left + 1;
            b.top = (UINT)s_px_y[n]; b.bottom = b.top + 1;
            b.front = 0; b.back = 1;
            for (k = 0; k < 8; k++)
                ID3D11DeviceContext_CopySubresourceRegion(
                    s_ctx, (ID3D11Resource *)st, 0, (UINT)k, (UINT)n, 0,
                    (ID3D11Resource *)tex[k], 0, &b);
        }
        memset(val[pass], 0, sizeof val[pass]);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                              D3D11_MAP_READ, 0, &m))) {
            for (n = 0; n < s_px_n; n++)
                memcpy(val[pass][n], (const uint8_t *)m.pData + (size_t)n * m.RowPitch,
                       sizeof val[pass][n]);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
        }
    }
    /* back to the real draw's state; the state-call filter's view of it
     * stays true for everything restored here */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, dsv);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_sc.bs, s_sc.bf, 0xFFFFFFFF);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_sc.ps, NULL, 0);
    s_state_gen++;              /* and the next draw rebinds everything */

    for (n = 0; n < s_px_n; n++) {
        uint32_t after;
        if (val[0][n][7][1] != 1.0f)
            continue;           /* not covered */
        after = read_pixel(rt, s_px_x[n], s_px_y[n]);
        if (!any) {
            s_px_max--;
            fprintf(stderr, "[D3D11-PX] flip %llu draw %llu surface 0x%08X prog %08X"
                    " prim %u n %u\n", (unsigned long long)s_presents,
                    (unsigned long long)s_draws, rt->offset,
                    d->program ? vs->hash : 0u, d->prim, d->idx_count);
        }
        any = 1;
        fprintf(stderr, "[D3D11-PX]  px (%d,%d) before %08X after %08X\n",
                s_px_x[n], s_px_y[n], before[n], after);
        for (pass = 0; pass < 2; pass++)
            for (k = 0; k < 8; k++)
                fprintf(stderr, "[D3D11-PX]   %-9s %g %g %g %g\n", names[pass][k],
                        val[pass][n][k][0], val[pass][n][k][1],
                        val[pass][n][k][2], val[pass][n][k][3]);
    }
    if (any) {
        int px, py;
        trace_batch(d, vs, rt, fmts, strides, offs, topo, &px, &py);
    }
}

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
    {   /* the most the streams below can take, each 16-aligned */
        uint64_t bound = 0;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
            const VertexAttr *a = &d->attr[i];
            if (!(vs->inputs & (1u << i)))
                continue;
            bound += (a->size && a->stride)
                   ? (uint64_t)count * (a->stride > 16 ? a->stride : 16) + 48 : 32;
        }
        vb_begin(bound <= 0xFFFFFFFFu ? (UINT)bound : 0);
    }
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const VertexAttr *a = &d->attr[i];
        int on = a->size && a->stride;
        UINT bytes = on ? count * 16u : 16u, esz, at;
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
                uint8_t *dst = (uint8_t *)vb_alloc(bytes, &at);
                if (!dst) {
                    vb_end();
                    s_draws_skipped++;
                    return;
                }
                memcpy(dst, base + lo_off + (size_t)lo * a->stride, bytes);
                vb_written();
                for (j = i; j < NV2A_VS_MAX_INPUTS; j++) {
                    if (!(group & (1u << j)))
                        continue;
                    fmts[j] = (uint8_t)native_format(&d->attr[j], &esz);
                    offs[j] = at + (d->attr[j].offset - lo_off);
                    strides[j] = a->stride;
                    vbs[j] = s_vb;
                }
                s_vb_native += bytes;
                continue;
            }
            bytes = count * 16u;
        }
        fmts[i] = (uint8_t)DXGI_FORMAT_R32G32B32A32_FLOAT;
        f = (float *)vb_alloc(bytes, &at);
        if (!f) {
            vb_end();
            s_draws_skipped++;
            return;
        }
        offs[i] = at;
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
        vb_written();
        vbs[i] = s_vb;
    }
    vb_end();
    p = ring_map(s_ib, s_ib_size, &s_ib_pos, nidx * 4u, 4);
    if (!p) {
        s_draws_skipped++;
        return;
    }
    memcpy(p, idx, nidx * 4u);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);

    /* Constants: the program's c[192] at b1 and the pixels->NDC mapping. */
    {
        static uint8_t consts_shadow[sizeof v->c];
        static uint8_t vp_shadow[80];
        static int consts_ok, vp_ok;
        D3D11_MAPPED_SUBRESOURCE m;
        float zmax = ((g->format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
        float c[12];
        uint32_t u[8];
        uint8_t vp[80];
        c[0] = 2.0f / (float)rt->w;
        c[1] = -2.0f / (float)rt->h;
        c[2] = 1.0f / zmax;
        c[3] = 0.0f;
        c[4] = -1.0f;
        c[5] = 1.0f;
        c[6] = 0.0f;
        c[7] = 0.0f;
        /* Fog and specular, as the CPU path's vsh_fog_factor and
         * vsh_transform apply them (xemu vsh.c). */
        c[8] = v->fog_param[0];
        c[9] = v->fog_param[1];
        c[10] = v->fog_param[2];
        c[11] = 0.0f;
        memset(u, 0, sizeof u);
        switch (v->fog_mode) {
        case 0x2601: u[1] = 0; u[2] = 0; u[3] = 1; break;   /* LINEAR */
        case 0x0804: u[1] = 0; u[2] = 1; u[3] = 1; break;   /* LINEAR_ABS */
        case 0x0800: u[1] = 1; u[2] = 0; u[3] = 1; break;   /* EXP */
        case 0x0802: u[1] = 1; u[2] = 1; u[3] = 0; break;   /* EXP_ABS */
        case 0x0801: u[1] = 2; u[2] = 0; u[3] = 0; break;   /* EXP2 */
        case 0x0803: u[1] = 2; u[2] = 1; u[3] = 0; break;   /* EXP2_ABS */
        default: break;                                     /* not set: no fog */
        }
        u[0] = (v->fog_enable && (v->fog_mode == 0x2601
                || (v->fog_mode >= 0x800 && v->fog_mode <= 0x804))) ? 1u : 0u;
        u[4] = v->spec_enable ? 1u : 0u;
        u[5] = (v->light_ctl >> 17) & 1u;
        memcpy(vp, c, sizeof c);
        memcpy(vp + sizeof c, u, sizeof u);
        if (!d->program
                || (memo_on() && consts_ok && !memcmp(consts_shadow, v->c, sizeof v->c))) {
            /* the buffer already holds these bytes */
        } else if (SUCCEEDED(ID3D11DeviceContext_Map(
                s_ctx, (ID3D11Resource *)s_cb_consts, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, v->c, sizeof v->c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_consts, 0);
            memcpy(consts_shadow, v->c, sizeof v->c);
            consts_ok = 1;
        }
        if (memo_on() && vp_ok && !memcmp(vp_shadow, vp, sizeof vp)) {
            /* unchanged */
        } else if (SUCCEEDED(ID3D11DeviceContext_Map(
                s_ctx, (ID3D11Resource *)s_cb_vp, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, vp, sizeof vp);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_vp, 0);
            memcpy(vp_shadow, vp, sizeof vp);
            vp_ok = 1;
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
        /* Stencil only on a Z24S8 zeta surface, as on the CPU. */
        int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
        if (v->depth_enable || use_s) {
            DepthTarget *dt = depth_target(rt->w, rt->h);
            if (dt) {
                dsv = dt->dsv;
                dss = depth_state(v->depth_enable, use_s);
                s_draws_z++;
            }
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss,
                                                   use_s ? v->stencil_ref & 0xFFu : 0);
        {
            ID3D11BlendState *bs = blend_state();
            ID3D11InputLayout *il = input_layout(vs, fmts);
            int sc = sc_on();
            float bf[4];
            s_draws_bconst += (uint64_t)blend_color(bf);
            if (!sc || s_sc.bs != bs || memcmp(s_sc.bf, bf, sizeof bf)) {
                ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFF);
                s_sc.bs = bs;
                memcpy(s_sc.bf, bf, sizeof bf);
            }
            if (!sc || memcmp(&s_sc.vp, &vp, sizeof vp)) {
                ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
                s_sc.vp = vp;
            }
            if (!sc || s_sc.rs != s_rs) {
                ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
                s_sc.rs = s_rs;
            }
            if (!sc || s_sc.il != il) {
                ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
                s_sc.il = il;
            }
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, NV2A_VS_MAX_INPUTS,
                                                   vbs, strides, offs);
            ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT,
                                                 s_ib_pos - nidx * 4u);
            if (!sc || s_sc.topo != topo) {
                ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
                s_sc.topo = topo;
            }
            if (!sc || s_sc.vs != vs->vs) {
                ID3D11DeviceContext_VSSetShader(s_ctx, vs->vs, NULL, 0);
                s_sc.vs = vs->vs;
            }
            if (!sc || memcmp(s_sc.vs_cb, cbs, sizeof cbs)) {
                ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, cbs);
                memcpy(s_sc.vs_cb, cbs, sizeof cbs);
            }
        }
        s_cur = rt;
        {
            static int skip_post = -1;
            ID3D11PixelShader *ps;
            if (skip_post < 0)
                skip_post = getenv("RECOMP_D3D11_DEBUG_PS_SKIP_POST") != NULL;
            s_dbg_post = skip_post && !v->depth_enable && d->idx_count <= 8;
            ps = setup_pixel();
            /* setup_pixel may have compiled (and evicted) a shader */
            if (!sc_on() || s_sc.ps != ps) {
                ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
                s_sc.ps = ps;
            }
        }
        if (px_on()) {
            uint32_t before[4];
            int n;
            for (n = 0; n < s_px_n; n++)
                before[n] = read_pixel(rt, s_px_x[n], s_px_y[n]);
            ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
            px_probe(d, vs, rt, fmts, strides, offs, topo, nidx, dsv, before);
        } else if (trace_pick(d)) {
            int px, py;
            trace_batch(d, vs, rt, fmts, strides, offs, topo, &px, &py);
            ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
            if (px >= 0)
                fprintf(stderr, "[D3D11-WHITE]  px (%d,%d) after %08X\n", px, py,
                        read_pixel(rt, px, py));
        } else {
            ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
        }
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
 * frame on screen stays.
 *
 * Size first, though. "Named at a flip" is sticky, and a title that once
 * flipped with a downsample target bound (BLiNX 2: 0x00811000, 320x240)
 * left it a candidate for good; drawn after the back buffer, it was then
 * presented on 34 flips while the walker named 0x00104000/0x00230000. So
 * the candidates the size of the surface named at this flip go first (it
 * is the next back buffer, the same size as the one just finished) and
 * only those: if none of them was drawn or cleared, the frame on screen
 * stays. With no render target at the named surface yet (the first flips),
 * the largest candidates go first, then the rest. */
static uint64_t s_occ_reports, s_occ_late, s_occ_vis, s_occ_sync_us, s_occ_super;
static int      s_occ_npend;
static void     occ_poll(UINT flags);

static RenderTarget *present_target(uint32_t surface_offset)
{
    RenderTarget *best = NULL, *named = NULL;
    UINT max_area = 0, cand_area = 0;
    int i, pass, tier;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex)
            continue;
        if (surface_offset && rt->offset == surface_offset)
            rt->flipped = 1, named = rt;
        if (rt->w * rt->h > max_area)
            max_area = rt->w * rt->h;
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && (rt->flipped || rt->w * rt->h == max_area)
                && rt->w * rt->h > cand_area)
            cand_area = rt->w * rt->h;
    }
    for (tier = named ? 0 : 1; tier < (named ? 1 : 3) && !best; tier++)
    for (pass = 0; pass < 2 && !best; pass++) {
        uint64_t top = s_flip_seq;
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            uint64_t when;
            if (!rt->tex || !(rt->flipped || rt->w * rt->h == max_area))
                continue;
            if (tier == 0 && (rt->w != named->w || rt->h != named->h))
                continue;
            if (tier == 1 && rt->w * rt->h != cand_area)
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
                    p_re, p_vn, p_ve, p_skip, p_self, p_mip,
                    p_occ, p_occ_vis, p_occ_us, p_occ_late, p_occ_super;
    if (on < 0)
        on = getenv("RECOMP_FLIP_LOG") != NULL;
    if (!on)
        return;
    fprintf(stderr, "[D3D11] flip %llu batches %llu prog %llu tex %llu rtt %llu (self %llu)"
            " skipped %llu clears %llu uploads %llu (mip %llu) changed %llu rehash %llu"
            " vb %llu/%llu bytes occ %llu (visible %llu, pending %d, late %llu, superseded %llu, sync %llu us)"
            " present 0x%08X walker 0x%08X\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_rtt_binds - p_rtt),
            (unsigned long long)(s_self_copies - p_self),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_mip_uploads - p_mip),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_vb_native - p_vn),
            (unsigned long long)(s_vb_expanded - p_ve),
            (unsigned long long)(s_occ_reports - p_occ),
            (unsigned long long)(s_occ_vis - p_occ_vis), s_occ_npend,
            (unsigned long long)(s_occ_late - p_occ_late),
            (unsigned long long)(s_occ_super - p_occ_super),
            (unsigned long long)(s_occ_sync_us - p_occ_us),
            s_drawn ? s_drawn->offset : 0, surface_offset);
    p_draws = s_draws; p_prog = s_draws_prog; p_tex = s_draws_tex;
    p_rtt = s_rtt_binds; p_skip = s_draws_skipped; p_clears = s_clears;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_vn = s_vb_native; p_ve = s_vb_expanded; p_self = s_self_copies;
    p_mip = s_tex_mip_uploads;
    p_occ = s_occ_reports; p_occ_vis = s_occ_vis; p_occ_us = s_occ_sync_us;
    p_occ_late = s_occ_late; p_occ_super = s_occ_super;
}

static void d3d11_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    HRESULT hr;

    if (!init())
        return;
    pump();
    occ_poll(D3D11_ASYNC_GETDATA_DONOTFLUSH);
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
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "blend: %llu draws with a constant factor,"
                " %u SET_BLEND_COLOR writes (last 0x%08X)\n",
                (unsigned long long)s_draws_bconst,
                nv2a_pb_gpu_state()->blend_color_writes,
                nv2a_pb_gpu_state()->blend_color);
}

/* ---- registration ---------------------------------------------------------- */

/* ---- Visibility tests: D3D11 occlusion queries ----------------------
 *
 * BeginVisibilityTest is CLEAR_REPORT_VALUE + SET_ZPASS_PIXEL_COUNT_ENABLE(1),
 * EndVisibilityTest is ENABLE(0) + GET_REPORT. Each stretch with counting on
 * is one D3D11_QUERY_OCCLUSION (begun at enable, or at a clear while
 * enabled; ended at disable or the report), and a report sums the stretches
 * since the last clear. The count is samples passing depth and stencil with
 * the pixel shader's alpha-test discard applied, the GPU's version of the
 * CPU path's put_pixel count.
 *
 * Latency: a report is not completed at GET_REPORT. Its queries go on a
 * pending list, polled (GetData without a flush) at every batch and flip
 * and, with a flush, on the ack thread's idle tick (on_poll), so a title
 * spinning on the result still gets one. Until then the status keeps D3D's
 * 0xFFFFFFFF, which GetVisibilityTestResult reads as D3DERR_TESTINCOMPLETE.
 * When done, count then status 0 are written. RECOMP_D3D11_OCC=sync waits
 * for the result at GET_REPORT instead (the stall the flip log's "occ"
 * shows), =fixed leaves reports to the walker (RECOMP_ZPASS_FIXED, every
 * test visible). A report still pending after 250 ms, or one with too many
 * stretches, completes as visible (0x10000).
 *
 * Reused reports: the XDK recycles its report slots, so a GET_REPORT can name
 * a va that still has a report pending. The new one supersedes it: the old
 * queries go back to the pool unread and nothing is written for the old
 * test, so its late completion cannot land over the title's reset status or
 * the new count. */
#define OCC_SEG     8       /* stretches per report */
#define OCC_PENDING 128
#define OCC_POOL    (OCC_PENDING * 2 + OCC_SEG)
typedef struct {
    uint32_t     va;
    int          n, overflow;
    uint64_t     t0;
    ID3D11Query *q[OCC_SEG];
} OccReport;
static ID3D11Query *s_occ_pool[OCC_POOL];
static int          s_occ_pool_n, s_occ_made;
static ID3D11Query *s_occ_seg[OCC_SEG];      /* since the last clear */
static int          s_occ_nseg, s_occ_open, s_occ_en, s_occ_ovf;
static OccReport    s_occ_pend[OCC_PENDING];
static int          s_occ_mode = -1;         /* 0 defer, 1 sync, 2 fixed */

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = getenv("RECOMP_D3D11_OCC");
        s_occ_mode = !e ? 0 : !_stricmp(e, "sync") ? 1 : !_stricmp(e, "fixed") ? 2 : 0;
    }
    return s_occ_mode;
}

static uint64_t occ_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

static ID3D11Query *occ_get(void)
{
    ID3D11Query *q = NULL;
    D3D11_QUERY_DESC qd;
    if (s_occ_pool_n)
        return s_occ_pool[--s_occ_pool_n];
    if (s_occ_made >= OCC_POOL)
        return NULL;
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &q)))
        return NULL;
    s_occ_made++;
    return q;
}

static void occ_put(ID3D11Query *q)
{
    if (q && s_occ_pool_n < OCC_POOL)
        s_occ_pool[s_occ_pool_n++] = q;
    else if (q)
        ID3D11Query_Release(q);
}

static void occ_begin(void)
{
    ID3D11Query *q;
    if (s_occ_open)
        return;
    if (s_occ_nseg >= OCC_SEG || !(q = occ_get())) {
        s_occ_ovf = 1;
        return;
    }
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)q);
    s_occ_seg[s_occ_nseg++] = q;
    s_occ_open = 1;
}

static void occ_end(void)
{
    if (!s_occ_open)
        return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_occ_seg[s_occ_nseg - 1]);
    s_occ_open = 0;
}

static void occ_write(uint32_t va, uint32_t count)
{
    volatile uint32_t *r = (volatile uint32_t *)
        ((uint8_t *)xbox_GetMemoryOffset() + va);
    r[0] = 0;                              /* timestamp */
    r[1] = 0;
    r[2] = count;
    MemoryBarrier();                       /* the count before the status */
    r[3] = 0;                              /* status: done */
}

/* 1 when every query of the report has its result (summed into *count). */
static int occ_ready(OccReport *p, UINT flags, uint32_t *count)
{
    uint64_t sum = 0;
    int i;
    for (i = 0; i < p->n; i++) {
        UINT64 v = 0;
        if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)p->q[i],
                                        &v, sizeof v, flags) != S_OK)
            return 0;
        sum += v;
    }
    *count = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sum;
    return 1;
}

static void occ_finish(OccReport *p, uint32_t count)
{
    int i;
    occ_write(p->va, count);
    s_occ_vis += count != 0;
    for (i = 0; i < p->n; i++)
        occ_put(p->q[i]);
    p->n = 0;
}

static void occ_poll(UINT flags)
{
    uint64_t now;
    int i, j;
    if (!s_occ_npend)
        return;
    now = occ_now_us();
    for (i = j = 0; i < s_occ_npend; i++) {
        OccReport *p = &s_occ_pend[i];
        uint32_t count;
        if (p->overflow) {
            occ_finish(p, 0x10000u);
        } else if (occ_ready(p, flags, &count)) {
            occ_finish(p, count);
        } else if (now - p->t0 > 250000u) {
            s_occ_late++;
            occ_finish(p, 0x10000u);
        } else {
            s_occ_pend[j++] = *p;
        }
    }
    s_occ_npend = j;
}

static void d3d11_on_zpass(int enable)
{
    if (occ_mode() == 2 || !init())
        return;
    s_occ_en = enable;
    if (enable)
        occ_begin();
    else
        occ_end();
}

static void d3d11_on_zpass_clear(void)
{
    int i;
    if (occ_mode() == 2 || !init())
        return;
    occ_end();
    for (i = 0; i < s_occ_nseg; i++)
        occ_put(s_occ_seg[i]);   /* ended; Begin on reuse drops the result */
    s_occ_nseg = 0;
    s_occ_ovf = 0;
    if (s_occ_en)
        occ_begin();
}

static int d3d11_on_report(uint32_t va)
{
    OccReport *p;
    int i;
    if (occ_mode() == 2 || !init())
        return 0;
    occ_end();
    occ_poll(D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (s_occ_npend >= OCC_PENDING)
        occ_poll(0);
    /* Supersede a report still pending at this va (see above). */
    for (i = 0; i < s_occ_npend; i++)
        if (s_occ_pend[i].va == va) {
            int k;
            for (k = 0; k < s_occ_pend[i].n; k++)
                occ_put(s_occ_pend[i].q[k]);
            memmove(&s_occ_pend[i], &s_occ_pend[i + 1],
                    (size_t)(s_occ_npend - i - 1) * sizeof s_occ_pend[0]);
            s_occ_npend--;
            s_occ_super++;
            break;              /* at most one: each push supersedes */
        }
    if (s_occ_npend >= OCC_PENDING) {
        /* Still full: the oldest is answered as visible. */
        s_occ_late++;
        occ_finish(&s_occ_pend[0], 0x10000u);
        memmove(&s_occ_pend[0], &s_occ_pend[1],
                (size_t)(--s_occ_npend) * sizeof s_occ_pend[0]);
    }
    p = &s_occ_pend[s_occ_npend++];
    p->va = va;
    p->n = s_occ_nseg;
    p->overflow = s_occ_ovf;
    p->t0 = occ_now_us();
    for (i = 0; i < s_occ_nseg; i++)
        p->q[i] = s_occ_seg[i];
    /* The report owns them now; a report with no clear before the next one
     * counts from here (the XDK always clears first). */
    s_occ_nseg = 0;
    s_occ_ovf = 0;
    s_occ_reports++;
    if (occ_mode() == 1) {
        uint64_t t = occ_now_us();
        uint32_t count;
        while (!occ_ready(p, 0, &count) && occ_now_us() - t < 250000u)
            ;
        s_occ_sync_us += occ_now_us() - t;
        occ_poll(0);
    }
    if (s_occ_en)
        occ_begin();
    return 1;
}

static void d3d11_on_poll(void)
{
    if (s_occ_npend && s_dev)
        occ_poll(0);
}

static const struct nv2a_pb_backend s_d3d11_backend = {
    d3d11_on_clear,
    d3d11_on_zclear,
    d3d11_on_draw,
    d3d11_on_flip,
    d3d11_on_zpass,
    d3d11_on_zpass_clear,
    d3d11_on_report,
    d3d11_on_poll,
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
