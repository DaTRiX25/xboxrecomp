/**
 * A Metal backend for the pushbuffer executor (struct nv2a_pb_backend), the
 * macOS sibling of nv2a_pb_d3d11.c. render-gpu-backend tasks §4, milestone M1.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's pushbuffer into
 * the state in nv2a_pb_state.h and calls the hooks of struct nv2a_pb_backend;
 * this file answers them with Metal instead of the CPU rasteriser:
 *
 *   on_clear  -> a render pass with loadAction Clear on the surface at
 *                color_offset (one MTLTexture per guest surface)
 *   on_draw   -> pre-transformed (screen-space) batches through a pass-through
 *                vertex function, with stage 0 textured from the shared decode
 *                (nv2a_backend_common.h). Vertex-program batches are M2 and are
 *                skipped (counted).
 *   on_flip   -> commit, wait, read the surface the walker picked for this flip
 *                (nv2a_pb_present_state) back into guest memory, then hand that
 *                surface to xbox_FramebufferWindowPresent as the CPU path does.
 *                So the SDL2 window, RECOMP_FB_DUMP and RECOMP_WINDOW_SHOT work
 *                unchanged: they read the same guest bytes.
 *
 * Opt-in: RECOMP_PB_BACKEND=metal (nv2a_pb_metal_register_from_env). The CPU
 * rasteriser stays the default and the oracle.
 *
 * Rules this file keeps (from the M1 plan):
 *  - Shaders are compiled at runtime (newLibraryWithSource) with fast math off,
 *    so float results match the CPU path's IEEE arithmetic.
 *  - Constant structs are float4/uint4 only on the MSL side; the C structs they
 *    mirror are checked with _Static_assert, as the HLSL ABI is in the D3D11
 *    backend.
 *  - Encoders begin lazily (first clear or draw on a surface) and end
 *    explicitly on a render-target change, before a readback and at the flip;
 *    every pass loads (loadAction Load) unless it starts with a clear.
 *  - Only the present surface is written back to guest memory, at the flip.
 *  - Guest memory is touched on the executor's thread only: no completion
 *    handlers; the flip waits for the command buffer and then copies.
 *
 * Not yet (M2+): vertex programs (MSL emitter), register combiners, stages
 * 1-3, depth/stencil, render-to-texture, occlusion queries (the walker reports
 * RECOMP_ZPASS_FIXED for a backend without on_report), LOD bias (Metal
 * samplers have none), swizzled render targets.
 *
 * Everything runs on the NV2A ack thread (or the smoke test's main thread).
 *
 * Off macOS this file compiles to nothing.
 */
#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
/* xbox_memory_layout.h pulls in the Win32 shim (BOOL, GetCurrentThread),
 * which collides with Objective-C and CoreServices: declare what is used. */
ptrdiff_t xbox_GetMemoryOffset(void);

#if __has_feature(objc_arc)
#error "nv2a_pb_metal.m is manual retain/release: build it without -fobjc-arc"
#endif

#define LOGP "[metal] "

extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);

/* ---- shader ABI ------------------------------------------------------------
 *
 * The MSL side declares only float4/uint4 members, so its layout is the C
 * struct's by construction: every member 16-aligned, no implicit padding.
 * The asserts pin the C structs (nv2a_backend_common.h) to that layout. */

/* VpConsts (MSL): float4 c[3]; uint4 u[2]; */
_Static_assert(sizeof(struct nv2a_vp_consts) == 80, "nv2a_vp_consts is not 5 x 16 bytes");
_Static_assert(offsetof(struct nv2a_vp_consts, u) == 48, "nv2a_vp_consts.u moved");

/* PsConsts (MSL): float4 c0[8], c1[8], fc0, fc1, fog_color; uint4 atest
 * (x alpha_ref as float bits, y alpha_func, z alpha_test_enable, w fog_enable);
 * uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode. */
#define PS_AT(m, off) \
    _Static_assert(offsetof(struct nv2a_ps_consts, m) == (off), "nv2a_ps_consts." #m " moved")
PS_AT(c0, 0);
PS_AT(c1, 128);
PS_AT(fc0, 256);
PS_AT(fc1, 272);
PS_AT(fog_color, 288);
PS_AT(alpha_ref, 304);
PS_AT(alpha_func, 308);
PS_AT(alpha_test_enable, 312);
PS_AT(fog_enable, 316);
PS_AT(alpha_only, 320);
PS_AT(tex_scale, 336);
PS_AT(tex_mode, 400);
#undef PS_AT
_Static_assert(sizeof(struct nv2a_ps_consts) == 416, "nv2a_ps_consts size changed");
_Static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4, "alpha_ref is read as uint bits");

/* One expanded vertex of a pass-through batch: attributes 0, 3, 4, 9..12 as
 * float4, in this order (the MSL reads vb[vid * VTX_F4 + k]). */
#define VTX_F4 7
static const int s_vtx_attr[VTX_F4] = {0, 3, 4, 9, 10, 11, 12};

static const char s_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VpConsts { float4 c[3]; uint4 u[2]; };\n"
    "struct PsConsts {\n"
    "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n"
    "    uint4 atest; uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n"
    "};\n"
    "struct VOut {\n"
    "    float4 pos [[position]];\n"
    "    float4 d0; float4 d1; float4 t0; float4 t1; float4 t2; float4 t3;\n"
    "    float  fog;\n"
    "    float  psize [[point_size]];\n"
    "};\n"
    /* Attribute 0 is already in surface pixels (x, y, z, rhw): c[0] is
     * (2/w, -2/h, 1/zmax, 0) and c[1] (-1, 1, 0, 0), as the D3D11
     * pass-through's vp_scale / vp_off. */
    "vertex VOut vs_pass(uint vid [[vertex_id]],\n"
    "                    const device float4 *vb [[buffer(0)]],\n"
    "                    constant VpConsts &vp [[buffer(1)]]) {\n"
    "    const device float4 *v = vb + vid * 7u;\n"
    "    VOut o;\n"
    "    o.pos = float4(v[0].xy * vp.c[0].xy + vp.c[1].xy,\n"
    "                   saturate(v[0].z * vp.c[0].z), 1.0);\n"
    "    o.d0 = v[1]; o.d1 = v[2];\n"
    "    o.t0 = v[3]; o.t1 = v[4]; o.t2 = v[5]; o.t3 = v[6];\n"
    "    o.fog = 1.0; o.psize = 1.0;\n"
    "    return o;\n"
    "}\n"
    /* Batches without register combiners (and, in M1, all of them): the CPU
     * path's MODULATE, oD0 * T0 when stage 0 is on, then the alpha test
     * (D3DCMP numbering, on the 8-bit values the way the CPU compares them).
     * Line for line the D3D11 backend's s_ps_src. */
    "fragment float4 fs_basic(VOut i [[stage_in]],\n"
    "                         constant PsConsts &pc [[buffer(0)]],\n"
    "                         texture2d<float> tex0 [[texture(0)]],\n"
    "                         sampler samp0 [[sampler(0)]]) {\n"
    "    float4 c = i.d0;\n"
    "    if (pc.tex_mode.x == 4u) {\n"
    "        c *= saturate(i.t0);\n"
    "    } else if (pc.tex_mode.x != 0u) {\n"
    "        float2 uv = i.t0.xy;\n"
    "        if (pc.tex_mode.x == 1u && i.t0.w != 0.0 && i.t0.w != 1.0) uv /= i.t0.w;\n"
    "        c *= tex0.sample(samp0, uv * pc.tex_scale[0].xy);\n"
    "    }\n"
    "    if (pc.atest.z != 0u) {\n"
    "        float ref = as_type<float>(pc.atest.x);\n"
    "        uint a = (uint)(saturate(c.a) * 255.0 + 0.5), r = (uint)(ref * 255.0 + 0.5);\n"
    "        bool ok = true;\n"
    "        uint f = pc.atest.y;\n"
    "        if      (f == 1u) ok = false;\n"
    "        else if (f == 2u) ok = a <  r;\n"
    "        else if (f == 3u) ok = a == r;\n"
    "        else if (f == 4u) ok = a <= r;\n"
    "        else if (f == 5u) ok = a >  r;\n"
    "        else if (f == 6u) ok = a != r;\n"
    "        else if (f == 7u) ok = a >= r;\n"
    "        if (!ok) discard_fragment();\n"
    "    }\n"
    "    return c;\n"
    "}\n";

/* ---- device ---------------------------------------------------------------- */

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
static id<MTLFunction>             s_vs_pass, s_fs_basic;
static id<MTLCommandBuffer>        s_cmd;      /* the frame's, begun lazily */
static id<MTLRenderCommandEncoder> s_enc;      /* open pass, or nil */
static int                         s_failed;
static int                         s_unified;  /* shared textures readable */

/* Vertex and index rings, reset after every wait (flip or overflow). */
static id<MTLBuffer> s_vb, s_ib;
static NSUInteger    s_vb_size, s_vb_pos, s_ib_size, s_ib_pos;

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_cmd_errors;

/* ---- render targets: one per guest surface --------------------------------- */

#define RT_MAX 8
typedef struct {
    uint32_t       offset;     /* color_offset, as the title set it */
    uint32_t       w, h;
    id<MTLTexture> tex;
    uint64_t       last_use;
} RenderTarget;

static RenderTarget  s_rt[RT_MAX];
static RenderTarget *s_enc_rt;   /* the target s_enc draws into */
static uint64_t      s_tick;

static void end_encoder(void)
{
    if (s_enc) {
        [s_enc endEncoding];
        [s_enc release];
        s_enc = nil;
    }
    s_enc_rt = NULL;
}

/* End the open pass, commit the frame's command buffer and (wait) block
 * until the GPU is done with it. After a wait the rings are free again. */
static void flush(int wait)
{
    end_encoder();
    if (!s_cmd) {
        if (wait) {            /* nothing in flight: the rings are free */
            s_vb_pos = 0;
            s_ib_pos = 0;
            s_flushes++;
        }
        return;
    }
    [s_cmd commit];
    if (wait) {
        [s_cmd waitUntilCompleted];
        if ([s_cmd status] == MTLCommandBufferStatusError) {
            s_cmd_errors++;
            if (s_cmd_errors <= 4)
                fprintf(stderr, LOGP "command buffer error: %s\n",
                        [[[s_cmd error] localizedDescription] UTF8String]);
        }
        s_vb_pos = 0;
        s_ib_pos = 0;
    }
    [s_cmd release];
    s_cmd = nil;
    s_flushes++;
}

static void ensure_cmd(void)
{
    if (!s_cmd)
        s_cmd = [[s_queue commandBuffer] retain];
}

/* Open a pass on rt: with `clear`, loadAction Clear to c; else Load. */
static void begin_encoder(RenderTarget *rt, int clear, const double c[4])
{
    MTLRenderPassDescriptor *rp;
    end_encoder();
    ensure_cmd();
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt->tex;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    s_enc = [[s_cmd renderCommandEncoderWithDescriptor:rp] retain];
    s_enc_rt = rt;
}

static int init(void)
{
    static int tried;
    NSError *err = nil;
    MTLCompileOptions *opts;
    id<MTLLibrary> lib;

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) {
            fprintf(stderr, LOGP "no Metal device\n");
            return 0;
        }
        s_unified = [s_dev hasUnifiedMemory] ? 1 : 0;
        s_queue = [s_dev newCommandQueue];

        opts = [[MTLCompileOptions alloc] init];
        /* Fast math off: the CPU path is IEEE; reassociation and flushed
         * denormals would move pixels across the golden tolerance. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        if (@available(macOS 15.0, *)) {
            opts.mathMode = MTLMathModeSafe;
            opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        }
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_msl]
                                  options:opts error:&err];
        [opts release];
        if (!lib) {
            fprintf(stderr, LOGP "MSL compile failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
            return 0;
        }
        s_vs_pass = [lib newFunctionWithName:@"vs_pass"];
        s_fs_basic = [lib newFunctionWithName:@"fs_basic"];
        [lib release];
        if (!s_vs_pass || !s_fs_basic) {
            fprintf(stderr, LOGP "MSL functions missing\n");
            return 0;
        }

        s_vb_size = 16u << 20;
        s_ib_size = 4u << 20;
        s_vb = [s_dev newBufferWithLength:s_vb_size options:MTLResourceStorageModeShared];
        s_ib = [s_dev newBufferWithLength:s_ib_size options:MTLResourceStorageModeShared];
        if (!s_vb || !s_ib) {
            fprintf(stderr, LOGP "buffer creation failed\n");
            return 0;
        }
        fprintf(stderr, LOGP "device up: %s (unified memory %d), fast math off\n",
                [[s_dev name] UTF8String], s_unified);
    }
    s_failed = 0;
    return 1;
}

/* ---- surfaces --------------------------------------------------------------- */

static void rt_free(RenderTarget *rt)
{
    if (s_enc_rt == rt)
        end_encoder();
    [rt->tex release];
    memset(rt, 0, sizeof *rt);
}

/* The render target for the surface at color_offset, the size of its clip
 * extent (the D3D11 backend's rule), made on first use. A new one starts from
 * the guest's bytes when the surface is 32-bit, as the CPU path draws over
 * whatever memory holds. */
static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    MTLTextureDescriptor *td;
    int i;

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096)
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
        rt_free(free_rt);
    }
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:w height:h
                                                        mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    /* Managed only for a discrete GPU (Intel Macs); deprecated on 27. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    td.storageMode = s_unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
    free_rt->tex = [s_dev newTextureWithDescriptor:td];
    if (!free_rt->tex)
        return NULL;
    free_rt->offset = g->color_offset;
    free_rt->w = w;
    free_rt->h = h;
    free_rt->last_use = s_tick;
    if (g->pitch >= w * 4u) {
        const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset()
                           + nv2a_pb_dma_resolve(g->color_offset);
        [free_rt->tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                          withBytes:src bytesPerRow:g->pitch];
    }
    fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
            g->color_offset, w, h);
    return free_rt;
}

static void metal_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    double c[4];

    if (!(param & 0xF0) || !init())
        return;
    @autoreleasepool {
        rt = surface();
        if (!rt)
            return;
        c[0] = (double)((g->clear_color >> 16) & 0xFF) / 255.0;
        c[1] = (double)((g->clear_color >> 8) & 0xFF) / 255.0;
        c[2] = (double)(g->clear_color & 0xFF) / 255.0;
        c[3] = (double)(g->clear_color >> 24) / 255.0;
        begin_encoder(rt, 1, c);
        s_clears++;
    }
}

/* ---- vertices ---------------------------------------------------------------- */

/* One attribute of one vertex as float4, decoded the way the CPU path's
 * fetch_attr does. 0 if the stream has no data for it. */
static int fetch(const struct nv2a_pb_draw *d, const VertexAttr *a,
                 uint32_t index, float out[4])
{
    const uint8_t *p;

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
    return nv2a_vtx_decode(a->type, a->size, p, out);
}

/* `bytes` from a ring, 16-aligned; on overflow the frame so far is flushed
 * (and waited for) and the ring starts over. NULL if it can never fit. */
static void *ring_alloc(id<MTLBuffer> b, NSUInteger size, NSUInteger *pos,
                        NSUInteger bytes, NSUInteger *off)
{
    NSUInteger at = (*pos + 15u) & ~(NSUInteger)15u;
    if (bytes > size)
        return NULL;
    if (at + bytes > size) {
        flush(1);   /* resets both rings */
        at = 0;
    }
    *pos = at + bytes;
    *off = at;
    return (uint8_t *)[b contents] + at;
}

/* ---- pipelines (blend state is part of them in Metal) ----------------------- */

static MTLBlendOperation blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return MTLBlendOperationSubtract;
    case 0x800B:
    case 0xF005: return MTLBlendOperationReverseSubtract;
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    default:     return MTLBlendOperationAdd;
    }
}

static MTLBlendFactor blend_factor(uint32_t gl, int alpha)
{
    static const MTLBlendFactor map[NV2A_BF_UNKNOWN + 1] = {
        MTLBlendFactorZero, MTLBlendFactorOne,
        MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
        MTLBlendFactorSourceAlphaSaturated,
        /* As the D3D11 backend: one RGBA constant, which
         * nv2a_blend_constant() fills for _COLOR or _ALPHA use. */
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
        MTLBlendFactorOne,                      /* unknown */
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

#define PSO_MAX 64
static struct { uint32_t key[5]; id<MTLRenderPipelineState> pso; } s_pso[PSO_MAX];
static int s_pso_n;

static id<MTLRenderPipelineState> pipeline(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    MTLRenderPipelineDescriptor *pd;
    MTLRenderPipelineColorAttachmentDescriptor *ca;
    id<MTLRenderPipelineState> pso;
    NSError *err = nil;
    uint32_t key[5];
    int i;

    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    key[3] = g->color_keep;             /* SET_COLOR_MASK, inverted */
    key[4] = key[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    for (i = 0; i < s_pso_n; i++)
        if (!memcmp(s_pso[i].key, key, sizeof key))
            return s_pso[i].pso;

    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = s_vs_pass;
    pd.fragmentFunction = s_fs_basic;
    ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.blendingEnabled = key[0] ? YES : NO;
    if (key[0]) {
        ca.sourceRGBBlendFactor = blend_factor(key[1], 0);
        ca.destinationRGBBlendFactor = blend_factor(key[2], 0);
        ca.sourceAlphaBlendFactor = blend_factor(key[1], 1);
        ca.destinationAlphaBlendFactor = blend_factor(key[2], 1);
        ca.rgbBlendOperation = (MTLBlendOperation)key[4];
        ca.alphaBlendOperation = (MTLBlendOperation)key[4];
    }
    ca.writeMask = ((key[3] & 0x00FF0000u) ? 0 : MTLColorWriteMaskRed)
                 | ((key[3] & 0x0000FF00u) ? 0 : MTLColorWriteMaskGreen)
                 | ((key[3] & 0x000000FFu) ? 0 : MTLColorWriteMaskBlue)
                 | ((key[3] & 0xFF000000u) ? 0 : MTLColorWriteMaskAlpha);
    pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    if (!pso) {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "pipeline failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
        return nil;
    }
    if (s_pso_n < PSO_MAX) {
        i = s_pso_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % PSO_MAX);
        [s_pso[i].pso release];   /* an encoded draw keeps its own reference */
    }
    memcpy(s_pso[i].key, key, sizeof key);
    s_pso[i].pso = pso;
    return pso;
}

/* ---- textures (stage 0 in M1) ------------------------------------------------
 *
 * Decoded to A8R8G8B8 on the CPU by the shared decode (nv2a_tex_decode), the
 * same texels the CPU sampler and the D3D11 backend see, and uploaded as
 * BGRA8Unorm (the same bytes). Cache key and invalidation as the D3D11
 * backend: offset + format + size + pitch + palette + levels; rehashed once
 * per flip and re-uploaded if it changed. A changed texture gets a new
 * MTLTexture rather than a replaceRegion, so a draw already encoded this frame
 * keeps the texels it was encoded with. */

typedef struct nv2a_stage Stage;
static Stage s_stage[4];

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 256
typedef struct {
    uint32_t       offset, color, width, height, pitch, palette, levels;
    uint32_t       gen, used;
    uint64_t       hash;
    int            live;
    id<MTLTexture> tex;
} TexEntry;

static TexEntry       s_tc[TEX_CACHE];
static uint32_t       s_tc_clock;
static uint32_t      *s_texels;
static size_t         s_texels_cap;
static id<MTLTexture> s_black, s_magenta;

static id<MTLTexture> solid_texture(uint32_t argb)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1
                                 mipmapped:NO];
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&argb
         bytesPerRow:4];
    return t;
}

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("RECOMP_METAL_NO_MIPS") != NULL;
    return off;
}

static void build_stages(void)
{
    const uint32_t *regs = nv2a_pb_tex_regs();
    const uint8_t  *sets = nv2a_pb_tex_regs_set();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    int n;

    for (n = 0; n < 4; n++) {
        Stage *t = &s_stage[n];
        nv2a_stage_decode(n, &regs[(0x40u * n) / 4], &sets[(0x40u * n) / 4],
                          v->shader_set, v->shader_prog, nv2a_pb_dma_resolve,
                          NV2A_STAGE_P8_PALETTE | NV2A_STAGE_PITCH_LINEAR
                          | (no_mips() ? 0u : NV2A_STAGE_MIPS), t);
        if (t->mode == 5)
            t->mode = 0;    /* CLIP_PLANE: no texture */
    }
}

static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = nv2a_tex_hash(mem + t->offset, bytes, NV2A_TEX_HASH_SEED);
    if (t->color == NV2A_TEX_P8)
        h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
    return h;
}

static id<MTLTexture> tex_upload(const Stage *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL, *src;
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    uint32_t l;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return nil;
        s_texels = p;
        s_texels_cap = n;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)(mem + t->palette);
    nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch,
                    t->levels, pal, pal ? t->pal_len : 0, s_texels);
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:t->width
                                                           height:t->height
                                                        mipmapped:t->levels > 1];
    if (t->levels > 1)
        td.mipmapLevelCount = t->levels;
    td.usage = MTLTextureUsageShaderRead;
    tex = [s_dev newTextureWithDescriptor:td];
    if (!tex)
        return nil;
    for (l = 0, src = s_texels; l < t->levels; l++) {
        uint32_t w = nv2a_tex_level_dim(t->width, l), h = nv2a_tex_level_dim(t->height, l);
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:src
               bytesPerRow:w * 4];
        src += (size_t)w * h;
    }
    s_tex_uploads++;
    return tex;
}

static id<MTLTexture> tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];

    if (t->valid && (uint64_t)t->width * t->height <= TEX_MAX_TEXELS)
        bytes = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    if (!bytes) {
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta;
    }
    for (i = 0; i < TEX_CACHE; i++) {
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
                id<MTLTexture> nt = tex_upload(t);
                e->hash = h;
                s_tex_changed++;
                if (!nt)
                    return s_black;
                [e->tex release];
                e->tex = nt;
            }
        }
    } else {
        id<MTLTexture> nt;
        e = victim;
        [e->tex release];
        e->tex = nil;
        e->live = 0;
        nt = tex_upload(t);
        if (!nt)
            return s_black;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        e->tex = nt;
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    return e->tex;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp. */
static MTLSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return MTLSamplerAddressModeRepeat;
    case 2:  return MTLSamplerAddressModeMirrorRepeat;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

/* SET_TEXTURE_FILTER as the D3D11 backend reads it (filter_linear,
 * filter_mip there). RECOMP_METAL_POINT=1 forces nearest texels. */
static int filter_linear(uint32_t f, int mag)
{
    uint32_t m = mag ? (f >> 24) & 0xF : (f >> 16) & 0x3F;
    if (mag)
        return m == 2 || m == 4;
    return m == 2 || m == 4 || m == 6 || m == 7;
}

static int filter_mip(uint32_t f)
{
    uint32_t m = (f >> 16) & 0x3F;
    return m == 3 || m == 4 ? 1 : m == 5 || m == 6 ? 2 : 0;
}

#define SMP_MAX 128
static struct { uint64_t key; id<MTLSamplerState> s; } s_smp[SMP_MAX];
static int s_smp_n;

static id<MTLSamplerState> sampler(const Stage *t)
{
    static int point = -1;
    MTLSamplerDescriptor *sd;
    id<MTLSamplerState> st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i;
    uint64_t key;

    if (point < 0)
        point = getenv("RECOMP_METAL_POINT") != NULL;
    mn = !point && filter_linear(t->filter, 0);
    mg = !point && filter_linear(t->filter, 1);
    mip = t->levels > 1 ? filter_mip(t->filter) : 0;
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key)
            return s_smp[i].s;
    sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = mn ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = mg ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mip == 2 ? MTLSamplerMipFilterLinear
                 : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = address_mode(u);
    sd.tAddressMode = address_mode(v);
    sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.lodMinClamp = mip ? (float)t->lod_min : 0.0f;
    sd.lodMaxClamp = mip ? (float)t->lod_max : 0.0f;
    st = [s_dev newSamplerStateWithDescriptor:sd];
    [sd release];
    if (!st)
        return s_smp_n ? s_smp[0].s : nil;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % SMP_MAX);
        [s_smp[i].s release];
    }
    s_smp[i].key = key;
    s_smp[i].s = st;
    return st;
}

/* ---- draws ------------------------------------------------------------------- */

static void metal_on_draw(const struct nv2a_pb_draw *d)
{
    static uint32_t idx[NV_MAX_INDICES * 3];
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    enum nv2a_topology topo;
    MTLPrimitiveType ptype;
    RenderTarget *rt;
    id<MTLRenderPipelineState> pso;
    struct nv2a_vp_consts vc;
    struct nv2a_ps_consts pc;
    NSUInteger vb_off, ib_off;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k;
    float *vf;
    void *ip;
    int cull = 0, cull_all = 0;

    if (!init())
        return;
    if (d->program) {          /* M2: vertex programs through an MSL emitter */
        s_draws_prog++;
        s_draws_skipped++;
        return;
    }
    if (g->color_keep == 0xFFFFFFFFu)   /* no colour writes, no depth yet */
        return;
    if (!d->idx_count) {
        s_draws_skipped++;
        return;
    }
    @autoreleasepool {
        rt = surface();
        pso = rt ? pipeline() : nil;
        if (!rt || !pso) {
            s_draws_skipped++;
            return;
        }
        for (i = 0; i < d->idx_count; i++) {
            if (d->idx[i] < lo) lo = d->idx[i];
            if (d->idx[i] > hi) hi = d->idx[i];
        }
        count = hi - lo + 1;
        nidx = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, lo, idx, &topo);
        if (!nidx) {
            s_draws_skipped++;
            return;
        }
        ptype = topo == NV2A_TOPO_POINTS ? MTLPrimitiveTypePoint
              : topo == NV2A_TOPO_LINES  ? MTLPrimitiveTypeLine
              :                            MTLPrimitiveTypeTriangle;
        if (v->cull_enable) {     /* as the D3D11 backend's rasterizer_state */
            if (v->cull_face == 0x405)      cull = 1;   /* BACK */
            else if (v->cull_face == 0x404) cull = 2;   /* FRONT */
            else if (v->cull_face == 0x408) cull_all = 1;
        }
        if (cull_all && ptype == MTLPrimitiveTypeTriangle) {
            s_draws_culled++;
            return;
        }

        /* Rings first: an overflow flushes, which ends the open pass. */
        vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos,
                                 (NSUInteger)count * VTX_F4 * 16u, &vb_off);
        {
            uint64_t fl = s_flushes;
            ip = vf ? ring_alloc(s_ib, s_ib_size, &s_ib_pos, (NSUInteger)nidx * 4u, &ib_off)
                    : NULL;
            if (vf && ip && fl != s_flushes) {
                /* The index ring overflowed after the vertices were placed,
                 * and its flush reset the vertex ring: place them again. */
                vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos,
                                         (NSUInteger)count * VTX_F4 * 16u, &vb_off);
            }
        }
        if (!vf || !ip) {
            s_draws_skipped++;
            return;
        }
        for (k = 0; k < VTX_F4; k++) {
            const VertexAttr *a = &d->attr[s_vtx_attr[k]];
            int on = a->size && a->stride;
            for (i = 0; i < count; i++) {
                float *o = vf + ((size_t)i * VTX_F4 + k) * 4;
                if (!on || !fetch(d, a, lo + i, o))
                    memcpy(o, v->inl[s_vtx_attr[k]], 16);
            }
        }
        memcpy(ip, idx, (size_t)nidx * 4u);

        nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, &vc);
        build_stages();
        nv2a_ps_consts_fill(v, s_stage, &pc);
        for (k = 1; k < 4; k++)   /* M1: stage 0 only */
            pc.tex_mode[k] = 0;

        if (!s_enc || s_enc_rt != rt)
            begin_encoder(rt, 0, NULL);
        [s_enc setRenderPipelineState:pso];
        {
            MTLViewport vp = {0.0, 0.0, (double)rt->w, (double)rt->h, 0.0, 1.0};
            [s_enc setViewport:vp];
        }
        [s_enc setFrontFacingWinding:v->front_face != 0x900
                                     ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [s_enc setCullMode:cull == 1 ? MTLCullModeBack
                         : cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        {
            float bf[4];
            (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                      g->blend_dfactor, g->blend_color, bf, &(int){0});
            [s_enc setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
        }
        [s_enc setVertexBuffer:s_vb offset:vb_off atIndex:0];
        [s_enc setVertexBytes:&vc length:sizeof vc atIndex:1];
        [s_enc setFragmentBytes:&pc length:sizeof pc atIndex:0];
        {
            const Stage *t = &s_stage[0];
            id<MTLTexture> tex = s_black;
            if (t->mode && t->mode != 4) {
                tex = tex_bind(t);
                s_draws_tex++;
            }
            [s_enc setFragmentTexture:tex atIndex:0];
            [s_enc setFragmentSamplerState:sampler(t) atIndex:0];
        }
        [s_enc drawIndexedPrimitives:ptype indexCount:nidx indexType:MTLIndexTypeUInt32
                         indexBuffer:s_ib indexBufferOffset:ib_off];
        s_draws++;
    }
}

/* ---- flip ---------------------------------------------------------------------- */

/* The render target for the surface the walker picked: its offset, and its
 * size when the walker knows it. NULL when this backend never drew there;
 * the guest's own bytes are presented then. */
static RenderTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->tex && surface_offset && rt->offset == surface_offset
                && (!p->offset || (rt->w == p->w && rt->h == p->h)))
            return rt;
    }
    return NULL;
}

/* rt's pixels into the guest surface at `addr`, `pitch` bytes a row: 32-bit
 * as they are (BGRA8 is X8R8G8B8's byte order), 16-bit as R5G6B5. Runs after
 * the wait, on the executor's thread. */
static void write_back(RenderTarget *rt, uint32_t addr, uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, y, x;

    if (bpp >= 4) {
        [rt->tex getBytes:dst bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, w, rt->h)
              mipmapLevel:0];
    } else if (bpp == 2) {
        static uint32_t *row;
        static uint32_t row_cap;
        if (row_cap < w) {
            uint32_t *n = (uint32_t *)realloc(row, (size_t)w * 4);
            if (!n)
                return;
            row = n;
            row_cap = w;
        }
        for (y = 0; y < rt->h; y++) {
            uint16_t *o = (uint16_t *)(dst + (size_t)y * pitch);
            [rt->tex getBytes:row bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, y, w, 1)
                  mipmapLevel:0];
            for (x = 0; x < w; x++) {
                uint32_t c = row[x];
                o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    } else {
        return;
    }
    s_writebacks++;
}

static void flip_log(uint32_t surface_offset, RenderTarget *rt)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_skip, p_cull, p_clears, p_tex, p_up,
                    p_chg, p_re, p_fl;
    if (on < 0)
        on = getenv("RECOMP_FLIP_LOG") != NULL;
    if (!on)
        return;
    fprintf(stderr, "[METAL] flip %llu batches %llu prog-skipped %llu skipped %llu"
            " culled %llu clears %llu tex %llu uploads %llu changed %llu rehash %llu"
            " flushes %llu present 0x%08X walker 0x%08X\n",
            (unsigned long long)s_presents,
            (unsigned long long)(s_draws - p_draws),
            (unsigned long long)(s_draws_prog - p_prog),
            (unsigned long long)(s_draws_skipped - p_skip),
            (unsigned long long)(s_draws_culled - p_cull),
            (unsigned long long)(s_clears - p_clears),
            (unsigned long long)(s_draws_tex - p_tex),
            (unsigned long long)(s_tex_uploads - p_up),
            (unsigned long long)(s_tex_changed - p_chg),
            (unsigned long long)(s_tex_rehash - p_re),
            (unsigned long long)(s_flushes - p_fl),
            rt ? rt->offset : 0, surface_offset);
    p_draws = s_draws; p_prog = s_draws_prog; p_skip = s_draws_skipped;
    p_cull = s_draws_culled; p_clears = s_clears; p_tex = s_draws_tex;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_fl = s_flushes;
}

static void metal_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    RenderTarget *rt = NULL;
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;

    if (init()) {
        @autoreleasepool {
            rt = present_target(surface_offset);
            if (rt && !s_unified) {
                /* Managed storage: bring the GPU's copy back first. */
                id<MTLBlitCommandEncoder> b;
                end_encoder();
                ensure_cmd();
                b = [s_cmd blitCommandEncoder];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [b synchronizeResource:rt->tex];
#pragma clang diagnostic pop
                [b endEncoding];
            }
            flush(1);
            if (rt && addr && pitch && pitch >= rt->w * 2u)
                write_back(rt, addr, pitch);
        }
    }
    s_presents++;
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0)
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (%s), %llu clears,"
                " %llu draws, %llu skipped (%llu vertex-program), %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs\n",
                (unsigned long long)s_presents, surface_offset,
                rt ? "drawn here" : "guest bytes",
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_skipped, (unsigned long long)s_draws_prog,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads, (unsigned long long)s_tex_rehash,
                (unsigned long long)s_writebacks);
    if (!pitch || !addr)
        return;
    xbox_FramebufferWindowSet(addr, pitch);
    xbox_FramebufferWindowPresent(addr, pitch);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_metal_backend = {
    metal_on_clear,
    NULL,               /* on_zclear: depth is M3 */
    metal_on_draw,
    metal_on_flip,
    NULL, NULL, NULL, NULL,   /* visibility tests: the walker's fixed answer */
};

const struct nv2a_pb_backend *nv2a_pb_metal_backend(void)
{
    if (!s_black && init()) {
        @autoreleasepool {
            s_black = solid_texture(0xFF000000u);
            s_magenta = solid_texture(NV2A_TEX_MAGENTA);
        }
    }
    return &s_metal_backend;
}

/* RECOMP_PB_BACKEND=metal routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_metal_register_from_env(void)
{
    const char *b = getenv("RECOMP_PB_BACKEND");
    if (b && !strcasecmp(b, "metal")) {
        const struct nv2a_pb_backend *mb = nv2a_pb_metal_backend();
        if (s_failed) {
            fprintf(stderr, LOGP "RECOMP_PB_BACKEND=metal: no device, staying on"
                    " the CPU rasteriser\n");
            return;
        }
        nv2a_pb_set_backend(mb);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=metal: executor draws through Metal\n");
    }
}

#endif /* __APPLE__ */
