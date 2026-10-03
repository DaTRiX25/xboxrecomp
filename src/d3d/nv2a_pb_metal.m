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
 *   on_draw   -> vertex-program batches through the program as MSL
 *                (d3d8_vsh_msl.c), pre-transformed (screen-space) ones through
 *                a pass-through vertex function; pixels through the register
 *                combiners as MSL (d3d8_combiners_msl.c) when they are
 *                programmed, else MODULATE. Four stages, textured from the
 *                shared decode (nv2a_backend_common.h).
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
 * Depth/stencil and render-to-texture as the D3D11 backend (M2); visibility
 * tests (GET_REPORT) from Metal visibility results through the shared
 * tracker (M3, "Visibility tests" below). Near clipping needs nothing extra:
 * the vertex programs hand Metal clip-space positions (w kept, as for D3D11)
 * and the default MTLDepthClipModeClip clips at the near and far planes, as
 * D3D11's DepthClipEnable does. Not yet: swizzled render targets. The mip
 * LOD bias is a sample() argument (MsPsConsts.tex_bias), not a sampler
 * field, so every macOS takes the same path.
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
#include <time.h>

#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "d3d8_msl.h"
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
/* A program's c[] (MSL constant float4 *c, buffer 1) is v->c as it is. */
_Static_assert(sizeof(((struct nv2a_pb_vsh *)0)->c) == NV2A_VS_MAX_CONSTANTS * 16,
               "vertex constants are not 192 float4");
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

/* The MSL PsConsts: the shared constants plus Metal's own trailing float4
 * tex_bias (per stage mip LOD bias, passed to sample() as bias()). Kept out
 * of nv2a_ps_consts, whose size the D3D11 backend pins to NV2APSConstants. */
typedef struct {
    struct nv2a_ps_consts b;
    float tex_bias[4];
} MsPsConsts;
_Static_assert(offsetof(MsPsConsts, tex_bias) == 416 && sizeof(MsPsConsts) == 432,
               "MsPsConsts.tex_bias moved");
_Static_assert(sizeof(float) == 4 && sizeof(uint32_t) == 4, "alpha_ref is read as uint bits");

/* One expanded vertex of a pass-through batch: attributes 0, 3, 4, 9..12 as
 * float4, in this order (the MSL reads vb[vid * VTX_F4 + k]). */
#define VTX_F4 7
static const int s_vtx_attr[VTX_F4] = {0, 3, 4, 9, 10, 11, 12};

static const char s_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VpConsts { float4 c[3]; uint4 u[2]; };\n"
    D3D8_MSL_PSCONSTS
    /* The emitters' VOut (d3d8_msl.h), so vs_pass links with a combiner
     * fs_main and a program's vs_main with fs_basic. */
    D3D8_MSL_VOUT
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
    "        c *= tex0.sample(samp0, uv * pc.tex_scale[0].xy, bias(pc.tex_bias.x));\n"
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
static MTLCompileOptions          *s_opts;     /* fast math off, kept for M2's shaders */
static id<MTLCommandBuffer>        s_cmd;      /* the frame's, begun lazily */
static id<MTLRenderCommandEncoder> s_enc;      /* open pass, or nil */
static int                         s_failed;
static int                         s_unified;  /* shared textures readable */

/* Vertex and index rings, reset after every wait (flip or overflow). */
static id<MTLBuffer> s_vb, s_ib;
static NSUInteger    s_vb_size, s_vb_pos, s_ib_size, s_ib_pos;

#define VIS_SLOTS 8192

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_cmd_errors;

/* ---- render targets: one per guest surface --------------------------------- */

/* Depth/stencil buffers, one per zeta surface and size, as the D3D11
 * backend's: Depth32Float_Stencil8, so the test compares the same normalised
 * float z (depth units / zmax) the CPU path keeps, and the 8 stencil bits
 * are Z24S8's. Never read back to guest memory. */
#define DT_MAX 4
typedef struct {
    uint32_t       zeta, w, h;
    id<MTLTexture> tex;
    uint64_t       last_use;
    int            fresh;      /* contents undefined: the next pass clears */
} DepthTarget;

#define RT_MAX 8
typedef struct {
    uint32_t       offset;     /* color_offset, as the title set it */
    uint32_t       addr;       /* its guest address (nv2a_pb_dma_resolve) */
    uint32_t       w, h;
    id<MTLTexture> tex;
    uint64_t       last_use;
    DepthTarget   *dt;         /* the depth buffer its passes attach, or NULL */
} RenderTarget;

static RenderTarget  s_rt[RT_MAX];
static RenderTarget *s_enc_rt;   /* the target s_enc draws into */
static DepthTarget  *s_enc_dt;   /* and its depth attachment, or NULL */
static DepthTarget   s_dt[DT_MAX];
static uint64_t      s_tick;
static uint64_t      s_zclears, s_draws_z, s_rtt_binds, s_self_copies;

/* Visibility tests (see "Visibility tests" below): the slot counting on the
 * open pass, opened by begin_pass and closed here. */
static void occ_pass_begin(void);
static void occ_pass_end(void);
static void occ_after_wait(void);
static id<MTLBuffer> s_vis;      /* visibility result slots, 8 bytes each */
static struct OccQuery *s_occ_q;     /* the query counting now, or NULL */
static int s_occ_slot = -1;          /* its slot on s_enc */

static void end_encoder(void)
{
    if (s_enc) {
        occ_pass_end();
        [s_enc endEncoding];
        [s_enc release];
        s_enc = nil;
    }
    s_enc_rt = NULL;
    s_enc_dt = NULL;
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
    if (wait)
        occ_after_wait();
}

static void ensure_cmd(void)
{
    if (!s_cmd)
        s_cmd = [[s_queue commandBuffer] retain];
}

/* Open a pass on rt: with `clear`, loadAction Clear to c; else Load. The
 * pass attaches rt->dt when it has one; zmask (CLEAR_SURFACE's bit 0 depth,
 * bit 1 stencil) clears it to z / st, and a fresh buffer clears to the far
 * plane and stencil 0, where the CPU path starts its own. */
static void begin_pass(RenderTarget *rt, int clear, const double c[4],
                       unsigned zmask, float z, uint32_t st)
{
    MTLRenderPassDescriptor *rp;
    DepthTarget *dt = rt->dt;
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
    if (dt) {
        int zc = (zmask & 1u) || dt->fresh, sc = (zmask & 2u) || dt->fresh;
        rp.depthAttachment.texture = dt->tex;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.loadAction = zc ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = (zmask & 1u) ? z : 1.0;
        rp.stencilAttachment.texture = dt->tex;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.loadAction = sc ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = (zmask & 2u) ? (st & 0xFFu) : 0u;
        dt->fresh = 0;
    }
    rp.visibilityResultBuffer = s_vis;
    s_enc = [[s_cmd renderCommandEncoderWithDescriptor:rp] retain];
    s_enc_rt = rt;
    s_enc_dt = dt;
    occ_pass_begin();
}

static void begin_encoder(RenderTarget *rt, int clear, const double c[4])
{
    begin_pass(rt, clear, c, 0, 1.0f, 0);
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
        s_opts = opts;
        lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:s_msl]
                                  options:opts error:&err];
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
        s_vis = [s_dev newBufferWithLength:VIS_SLOTS * 8u
                                   options:MTLResourceStorageModeShared];
        if (!s_vb || !s_ib || !s_vis) {
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

/* The depth buffer for the zeta surface now set, at w x h; made on first
 * use (fresh), the least recently used one giving way when all are taken. */
static DepthTarget *depth_target(uint32_t w, uint32_t h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    MTLTextureDescriptor *td;
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
        if (s_enc_dt == e)
            end_encoder();
        for (i = 0; i < RT_MAX; i++)
            if (s_rt[i].dt == e)
                s_rt[i].dt = NULL;
        [e->tex release];
        memset(e, 0, sizeof *e);
    }
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                            width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    e->tex = [s_dev newTextureWithDescriptor:td];
    if (!e->tex)
        return NULL;
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    e->fresh = 1;
    fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
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
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
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

/* CLEAR_SURFACE with the Z or stencil bit: the value scaled as the CPU path
 * scales it, by the zeta format's range; the stencil its low byte. A pass
 * that loads colour and clears the depth buffer. */
static void metal_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;

    if (!(param & 3u) || !init())
        return;
    @autoreleasepool {
        rt = surface();
        if (!rt)
            return;
        dt = depth_target(rt->w, rt->h);
        if (!dt)
            return;
        rt->dt = dt;
        begin_pass(rt, 0, NULL, param & 3u, nv2a_zclear_depth(g->format, v->zclear),
                   v->zclear & 0xFFu);
        if (param & 1u)
            s_zclears++;
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

/* A pipeline is its two functions plus the blend state. The functions are
 * cached for the life of the process (vs_cache, fs_cache), so their
 * pointers identify them. */
typedef struct {
    uint64_t vs, fs;
    uint32_t key[6];
} PsoKey;
#define PSO_MAX 512
static struct { PsoKey k; id<MTLRenderPipelineState> pso; } s_pso[PSO_MAX];
static int s_pso_n, s_pso_last;

static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vs, id<MTLFunction> fs)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    MTLRenderPipelineDescriptor *pd;
    MTLRenderPipelineColorAttachmentDescriptor *ca;
    id<MTLRenderPipelineState> pso;
    NSError *err = nil;
    PsoKey pk;
    uint32_t *key = pk.key;
    int i;

    memset(&pk, 0, sizeof pk);
    pk.vs = (uint64_t)(uintptr_t)vs;
    pk.fs = (uint64_t)(uintptr_t)fs;
    key[0] = g->blend_enable ? 1 : 0;
    key[1] = key[0] ? g->blend_sfactor : 0;
    key[2] = key[0] ? g->blend_dfactor : 0;
    key[3] = g->color_keep;             /* SET_COLOR_MASK, inverted */
    key[4] = key[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    key[5] = s_enc_dt ? 1 : 0;          /* the open pass has a depth buffer */
    if (s_pso_last < s_pso_n && !memcmp(&s_pso[s_pso_last].k, &pk, sizeof pk))
        return s_pso[s_pso_last].pso;
    for (i = 0; i < s_pso_n; i++)
        if (!memcmp(&s_pso[i].k, &pk, sizeof pk)) {
            s_pso_last = i;
            return s_pso[i].pso;
        }

    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
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
    if (key[5]) {
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
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
    s_pso[i].k = pk;
    s_pso[i].pso = pso;
    s_pso_last = i;
    return pso;
}

/* ---- textures ------------------------------------------------
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

/* SET_TEXTURE_FILTER [12:0]: the mip LOD bias, signed 5.8 fixed point. */
static float lod_bias(uint32_t f)
{
    int b = (int)(f & 0x1FFFu);
    if (b & 0x1000)
        b -= 0x2000;
    return (float)b / 256.0f;
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

/* ---- shaders (M2): vertex programs and register combiners ------------------
 *
 * As the D3D11 backend's vertex_shader and combiner_ps: the program is
 * parsed from the slots it starts at (d3d8_vsh_parse) and emitted as MSL
 * (d3d8_vsh_generate_msl, screen-space epilogue), keyed by
 * nv2a_vsh_program_hash; the combiner registers become NV2ACombinerState
 * (d3d8_combiners_from_regs) and MSL (d3d8_combiners_generate_msl), keyed by
 * the state's bytes. Each compiles once, with s_opts (fast math off). A
 * source that fails to compile is logged with its text, counted
 * (s_compile_errors, in the present log) and remembered as bad. */

static uint64_t s_compile_errors, s_vs_compiled, s_fs_compiled;

static id<MTLFunction> compile_fn(const char *src, NSString *name, const char *what)
{
    NSError *err = nil;
    id<MTLLibrary> lib;
    id<MTLFunction> fn;

    lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src]
                              options:s_opts error:&err];
    if (!lib) {
        s_compile_errors++;
        if (s_compile_errors <= 4)
            fprintf(stderr, LOGP "\x1b[1;31m%s: MSL compile failed: %s\x1b[0m\n%s\n",
                    what, err ? [[err localizedDescription] UTF8String] : "?", src);
        return nil;
    }
    fn = [lib newFunctionWithName:name];
    [lib release];
    if (!fn) {
        s_compile_errors++;
        fprintf(stderr, LOGP "%s: no function %s\n", what, [name UTF8String]);
    }
    return fn;
}

#define VS_MAX 512
typedef struct {
    uint32_t        hash;
    uint16_t        inputs;
    int             used, bad;
    id<MTLFunction> fn;
} VsEntry;
static VsEntry s_vs[VS_MAX];

static VsEntry *vs_cache(void)
{
    static NV2AVshProgram parsed;
    static char msl[65536];
    static struct { int valid; uint32_t dwords, start; VsEntry *e; } memo;
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash;
    VsEntry *e = NULL;
    char what[32];
    int i;

    /* Programs only change by upload (prog_dwords) or a new start slot. */
    if (memo.valid && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    hash = nv2a_vsh_program_hash(v, NULL);
    if (!hash)
        hash = 1;
    for (i = 0; i < VS_MAX; i++) {
        if (s_vs[i].used && s_vs[i].hash == hash) {
            e = &s_vs[i];
            break;
        }
    }
    if (!e) {
        for (i = 0; i < VS_MAX && s_vs[i].used; i++)
            ;
        if (i == VS_MAX)
            return NULL;   /* full: a title with this many programs wants LRU */
        e = &s_vs[i];
        e->used = 1;
        e->hash = hash;
        e->bad = 1;
        d3d8_vsh_parse(&v->prog[v->start][0], NV2A_VSH_SLOTS - (int)v->start, &parsed);
        snprintf(what, sizeof what, "program %08X", hash);
        if (d3d8_vsh_generate_msl(&parsed, D3D8_MSL_SCREEN_SPACE, msl, (int)sizeof msl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no MSL\n", what);
        } else if ((e->fn = compile_fn(msl, @"vs_main", what)) != nil) {
            e->inputs = parsed.inputs_read;
            e->bad = 0;
            s_vs_compiled++;
        }
    }
    memo.valid = 1;
    memo.dwords = v->prog_dwords;
    memo.start = v->start;
    memo.e = e->bad ? NULL : e;
    return memo.e;
}

#define FS_MAX 512
typedef struct {
    uint32_t          hash;
    int               bad;
    NV2ACombinerState st;
    id<MTLFunction>   fn;
} FsEntry;
static FsEntry *s_fs;   /* FS_MAX, allocated on first use */
static int      s_fs_n;

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *b++;
        h *= 0x01000193u;
    }
    return h;
}

/* The fragment function for the batch: its register combiners when they
 * are programmed (v->rc_set and a stage count), else fs_basic. As the D3D11
 * setup_pixel, only 2D textures are bound, so every sampling mode is passed
 * to the combiners as 2D. */
static id<MTLFunction> fs_select(const struct nv2a_pb_vsh *v)
{
    static char msl[65536];
    static struct { int valid; uint32_t r[36]; id<MTLFunction> fn; } memo;
    NV2ACombinerState st;
    uint32_t key[36], modes = 0, hash;
    FsEntry *e = NULL;
    char what[32];
    int n;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        uint32_t m = (uint32_t)s_stage[n].mode;
        if (m && m != 4) m = 1;
        modes |= m << (5 * n);
    }
    memcpy(key, v->rc_cicw, 32);
    memcpy(key + 8, v->rc_aicw, 32);
    memcpy(key + 16, v->rc_cocw, 32);
    memcpy(key + 24, v->rc_aocw, 32);
    key[32] = v->rc_ctl;
    key[33] = v->rc_fcw0;
    key[34] = v->rc_fcw1;
    key[35] = modes;
    if (memo.valid && !memcmp(memo.r, key, sizeof key))
        return memo.fn;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;   /* FOG.a is the interpolated vertex fog factor */
    hash = fnv(&st, sizeof st);
    if (!s_fs && !(s_fs = (FsEntry *)calloc(FS_MAX, sizeof *s_fs)))
        return s_fs_basic;
    for (n = 0; n < s_fs_n; n++)
        if (s_fs[n].hash == hash && !memcmp(&s_fs[n].st, &st, sizeof st)) {
            e = &s_fs[n];
            break;
        }
    if (!e) {
        if (s_fs_n == FS_MAX)
            return s_fs_basic;
        e = &s_fs[s_fs_n++];
        e->hash = hash;
        e->st = st;
        e->bad = 1;
        snprintf(what, sizeof what, "combiners %08X", hash);
        if (d3d8_combiners_generate_msl(&st, msl, (int)sizeof msl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no MSL\n", what);
        } else if ((e->fn = compile_fn(msl, @"fs_main", what)) != nil) {
            e->bad = 0;
            s_fs_compiled++;
        }
    }
    memcpy(memo.r, key, sizeof key);
    memo.valid = 1;
    memo.fn = e->bad ? s_fs_basic : e->fn;   /* as D3D11: the basic PS */
    return memo.fn;
}

/* ---- depth / stencil state ---------------------------------------------------
 *
 * As the D3D11 backend's depth_state: SET_DEPTH_FUNC and SET_STENCIL_FUNC
 * are GL compare codes in MTLCompareFunction's order (nv2a_cmp_from_gl), the
 * stencil ops in MTLStencilOperation's (nv2a_stencil_op_from_gl). Depth and
 * stencil each only when enabled; one state per distinct key. */
#define DSS_MAX 64
static struct { uint64_t key; id<MTLDepthStencilState> dss; } s_dss[DSS_MAX];
static int s_dss_n;

static id<MTLDepthStencilState> depth_state(int depth, int stencil)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t f = nv2a_cmp_from_gl(v->depth_func);
    uint64_t key = 0;
    MTLDepthStencilDescriptor *dd;
    id<MTLDepthStencilState> st;
    int i;

    if (depth)
        key |= 1u | f << 1 | (v->depth_mask ? 1u : 0u) << 4;
    if (stencil)
        key |= (uint64_t)(1u | (v->stencil_func & 7u) << 1
                          | nv2a_stencil_op_from_gl(v->stencil_op[0]) << 4
                          | nv2a_stencil_op_from_gl(v->stencil_op[1]) << 8
                          | nv2a_stencil_op_from_gl(v->stencil_op[2]) << 12
                          | (v->stencil_rmask & 0xFFu) << 16
                          | (v->stencil_wmask & 0xFFu) << 24) << 8;
    for (i = 0; i < s_dss_n; i++)
        if (s_dss[i].key == key)
            return s_dss[i].dss;
    dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = depth ? (MTLCompareFunction)f : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth && v->depth_mask ? YES : NO;
    if (stencil) {
        MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilCompareFunction = (MTLCompareFunction)(v->stencil_func & 7u);
        sd.stencilFailureOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[0]);
        sd.depthFailureOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[1]);
        sd.depthStencilPassOperation = (MTLStencilOperation)nv2a_stencil_op_from_gl(v->stencil_op[2]);
        sd.readMask = v->stencil_rmask & 0xFFu;
        sd.writeMask = v->stencil_wmask & 0xFFu;
        dd.frontFaceStencil = sd;               /* GL one-sided stencil */
        dd.backFaceStencil = sd;
        [sd release];
    }
    st = [s_dev newDepthStencilStateWithDescriptor:dd];
    [dd release];
    if (!st)
        return nil;
    if (s_dss_n >= DSS_MAX)
        return st;   /* uncached: a title with this many wants LRU (leaks) */
    s_dss[s_dss_n].key = key;
    s_dss[s_dss_n].dss = st;
    s_dss_n++;
    return st;
}

/* ---- render-to-texture ---------------------------------------------------------
 *
 * A stage whose texture is a surface this backend draws into samples the
 * render target itself, as the D3D11 backend's rt_texture: guest memory
 * under it only changes at a flip's write-back (and then only for the
 * present surface). 32-bit, non-compressed formats only; the low 27 bits of
 * the address compare, so either memory window matches. The target being
 * drawn into is sampled through a copy (s_self), made by a blit between
 * passes. RECOMP_METAL_NO_RTT=1 decodes guest memory instead (A/B). */
static RenderTarget s_self;   /* tex, w, h: the copy; offset: whose */

static RenderTarget *rt_texture(const Stage *t, RenderTarget *cur)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i;

    if (off < 0)
        off = getenv("RECOMP_METAL_NO_RTT") != NULL;
    if (off || !t->offset || d3d8_format_dxt_block_bytes(t->color)
            || nv2a_tex_texel_bytes(nv2a_tex_linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->tex || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (rt == cur) {
            self = rt;
            continue;
        }
        return rt;
    }
    if (!self)
        return NULL;
    if (!s_self.tex || s_self.w != self->w || s_self.h != self->h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:self->w height:self->h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        [s_self.tex release];
        s_self.tex = [s_dev newTextureWithDescriptor:td];
        if (!s_self.tex)
            return NULL;
        s_self.w = self->w;
        s_self.h = self->h;
    }
    {
        id<MTLBlitCommandEncoder> b;
        end_encoder();
        ensure_cmd();
        b = [s_cmd blitCommandEncoder];
        [b copyFromTexture:self->tex toTexture:s_self.tex];
        [b endEncoding];
    }
    s_self.offset = self->offset;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", self->offset);
    }
    return &s_self;
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
    id<MTLFunction> vfn, ffn;
    struct nv2a_vp_consts vc;
    MsPsConsts mpc;
    NSUInteger vb_off, ib_off, vbytes;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k, nf4 = 0;
    int slot[NV2A_VS_MAX_INPUTS];
    float *vf;
    void *ip;
    int cull = 0, cull_all = 0;
    /* Stencil only on a Z24S8 zeta surface, as on the CPU. */
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    id<MTLTexture> stex[4];

    if (!init())
        return;
    /* A batch that writes nothing is dropped, unless a visibility test is
     * counting it: a test's bounding draws typically mask every write. */
    if (g->color_keep == 0xFFFFFFFFu && !(v->depth_enable && v->depth_mask) && !use_s
            && !s_occ_q)
        return;
    if (!d->idx_count) {
        s_draws_skipped++;
        return;
    }
    @autoreleasepool {
        /* Vertex function and the float4 inputs it reads, in order. */
        if (d->program) {
            VsEntry *e = vs_cache();
            s_draws_prog++;
            if (!e) {
                s_draws_skipped++;
                return;
            }
            vfn = e->fn;
            for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
                if (e->inputs & (1u << i))
                    slot[nf4++] = (int)i;
        } else {
            vfn = s_vs_pass;
            for (k = 0; k < VTX_F4; k++)
                slot[nf4++] = s_vtx_attr[k];
        }
        rt = surface();
        if (!rt) {
            s_draws_skipped++;
            return;
        }
        build_stages();
        nv2a_ps_consts_fill(v, s_stage, &mpc.b);
        for (k = 0; k < 4; k++) {
            /* As the D3D11 MipLODBias: only a mipmapped stage has one. */
            const Stage *t = &s_stage[k];
            float b = t->levels > 1 && filter_mip(t->filter) ? lod_bias(t->filter) : 0.0f;
            mpc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
        }
        ffn = fs_select(v);
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
        vbytes = (NSUInteger)count * nf4 * 16u;
        if (vbytes < 16)
            vbytes = 16;
        vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos, vbytes, &vb_off);
        {
            uint64_t fl = s_flushes;
            ip = vf ? ring_alloc(s_ib, s_ib_size, &s_ib_pos, (NSUInteger)nidx * 4u, &ib_off)
                    : NULL;
            if (vf && ip && fl != s_flushes) {
                /* The index ring overflowed after the vertices were placed,
                 * and its flush reset the vertex ring: place them again. */
                vf = (float *)ring_alloc(s_vb, s_vb_size, &s_vb_pos, vbytes, &vb_off);
            }
        }
        if (!vf || !ip) {
            s_draws_skipped++;
            return;
        }
        /* Each input as float4, fetched as the CPU path does; a disabled
         * one (or one without data) is its SET_VERTEX_DATA value. */
        for (k = 0; k < nf4; k++) {
            const VertexAttr *a = &d->attr[slot[k]];
            int on = a->size && a->stride;
            for (i = 0; i < count; i++) {
                float *o = vf + ((size_t)i * nf4 + k) * 4;
                if (!on || !fetch(d, a, lo + i, o))
                    memcpy(o, v->inl[slot[k]], 16);
            }
        }
        memcpy(ip, idx, (size_t)nidx * 4u);

        nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, &vc);

        /* Depth: the zeta surface's buffer, attached to this target's
         * passes from now on (a new pass if the open one lacks it). */
        if (use_z) {
            DepthTarget *dt = depth_target(rt->w, rt->h);
            if (dt && rt->dt != dt) {
                rt->dt = dt;
                if (s_enc_rt == rt)
                    end_encoder();
            }
        }
        /* Textures before the pass: a self copy is a blit between passes. */
        {
            int textured = 0;
            for (k = 0; k < 4; k++) {
                const Stage *t = &s_stage[k];
                RenderTarget *src;
                stex[k] = s_black;
                if (!t->mode || t->mode == 4)
                    continue;
                textured = 1;
                if ((src = rt_texture(t, rt)) != NULL) {
                    stex[k] = src->tex;
                    /* Linear coordinates are texels of the surface as drawn. */
                    if (!nv2a_tex_size_from_format(t->color)) {
                        mpc.b.tex_scale[k][0] = 1.0f / (float)src->w;
                        mpc.b.tex_scale[k][1] = 1.0f / (float)src->h;
                    }
                    s_rtt_binds++;
                } else {
                    stex[k] = tex_bind(t);
                }
            }
            if (textured)
                s_draws_tex++;
        }

        if (!s_enc || s_enc_rt != rt)
            begin_encoder(rt, 0, NULL);
        pso = pipeline(vfn, ffn);
        if (!pso) {
            s_draws_skipped++;
            return;
        }
        [s_enc setRenderPipelineState:pso];
        if (s_enc_dt) {
            id<MTLDepthStencilState> dss = use_z ? depth_state(v->depth_enable, use_s)
                                                 : depth_state(0, 0);
            [s_enc setDepthStencilState:dss];
            [s_enc setStencilReferenceValue:use_s ? (v->stencil_ref & 0xFFu) : 0u];
            if (use_z)
                s_draws_z++;
        }
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
        if (d->program) {
            /* c[192] is 3 KB: under setVertexBytes' 4 KB. */
            [s_enc setVertexBytes:v->c length:sizeof v->c atIndex:1];
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:2];
        } else {
            [s_enc setVertexBytes:&vc length:sizeof vc atIndex:1];
        }
        [s_enc setFragmentBytes:&mpc length:sizeof mpc atIndex:0];
        for (k = 0; k < 4; k++) {
            [s_enc setFragmentTexture:stex[k] atIndex:k];
            [s_enc setFragmentSamplerState:sampler(&s_stage[k]) atIndex:k];
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

/* ---- Visibility tests: Metal visibility-result counting ---------------------
 *
 * The shared tracker (nv2a_occ, nv2a_backend_common.h) as the D3D11 backend
 * drives it: BeginVisibilityTest is CLEAR_REPORT_VALUE +
 * SET_ZPASS_PIXEL_COUNT_ENABLE(1), EndVisibilityTest is ENABLE(0) +
 * GET_REPORT; each stretch with counting on is one query, and a report sums
 * the stretches since the last clear, completing later (poll at every batch
 * and flip, with a flush on the ack thread's idle tick) or as visible after
 * 250 ms. The count is samples passing depth and stencil, alpha-test discard
 * applied.
 *
 * A query here is a list of slots in s_vis, one per render pass it spans:
 * Metal resets, at the start of a pass, every offset that pass names (even
 * in setVisibilityResultMode:Disabled), so a slot never outlives its pass
 * and a stretch over a pass change takes a new one. Slots accumulate within
 * the pass. A result is in once every slot's command buffer has completed;
 * a flush commits the open command buffer so that it can. A slot freed while
 * its command buffer is still in flight retires until the flip's wait.
 *
 * RECOMP_METAL_OCC=sync waits at GET_REPORT; =fixed leaves reports to the
 * walker (RECOMP_ZPASS_FIXED, every test visible). */
#define OCC_QSLOTS 16
#define OCC_POOL   (NV2A_OCC_PENDING * 2 + NV2A_OCC_SEG)
typedef struct OccQuery {
    int                   slot[OCC_QSLOTS];
    id<MTLCommandBuffer>  cmd[OCC_QSLOTS];   /* retained */
    int                   n, ovf;
    struct OccQuery      *next;              /* free list */
} OccQuery;
static OccQuery          s_qpool[OCC_POOL];
static OccQuery         *s_qfree;
static int               s_qmade;
static unsigned char     s_vis_used[VIS_SLOTS];
static int               s_vis_next;
static int               s_retire[VIS_SLOTS], s_nretire;
static struct nv2a_occ   s_occ;
static int               s_occ_mode = -1;  /* 0 defer, 1 sync, 2 fixed */
static uint64_t          s_occ_ovf;

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = getenv("RECOMP_METAL_OCC");
        s_occ_mode = !e ? 0 : !strcasecmp(e, "sync") ? 1 : !strcasecmp(e, "fixed") ? 2 : 0;
    }
    return s_occ_mode;
}

static int vis_alloc(void)
{
    int i, k;
    for (k = 0; k < VIS_SLOTS; k++) {
        i = (s_vis_next + k) % VIS_SLOTS;
        if (!s_vis_used[i]) {
            s_vis_used[i] = 1;
            s_vis_next = (i + 1) % VIS_SLOTS;
            ((uint64_t *)[s_vis contents])[i] = 0;
            return i;
        }
    }
    return -1;
}

static void occ_pass_begin(void)
{
    OccQuery *q = s_occ_q;
    int i;
    if (!q || !s_enc || s_occ_slot >= 0)
        return;
    if (q->n >= OCC_QSLOTS || (i = vis_alloc()) < 0) {
        q->ovf = 1;
        s_occ_ovf++;
        return;
    }
    q->slot[q->n] = i;
    q->cmd[q->n] = [s_cmd retain];
    q->n++;
    [s_enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)i * 8u];
    s_occ_slot = i;
}

static void occ_pass_end(void)
{
    if (s_occ_slot >= 0 && s_enc)
        [s_enc setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                offset:(NSUInteger)s_occ_slot * 8u];
    s_occ_slot = -1;
}

static uint64_t occ_now_us(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *occ_get(void *ctx)
{
    OccQuery *q;
    (void)ctx;
    if (s_qfree) {
        q = s_qfree;
        s_qfree = q->next;
    } else if (s_qmade < OCC_POOL) {
        q = &s_qpool[s_qmade++];
    } else {
        return NULL;
    }
    q->n = 0;
    q->ovf = 0;
    return q;
}

static int cmd_done(id<MTLCommandBuffer> c)
{
    MTLCommandBufferStatus st = [c status];
    return st == MTLCommandBufferStatusCompleted || st == MTLCommandBufferStatusError;
}

static void occ_put(void *ctx, void *qv)
{
    OccQuery *q = (OccQuery *)qv;
    int i;
    (void)ctx;
    if (!q)
        return;
    if (q == s_occ_q) {     /* not expected: the tracker ends a query first */
        occ_pass_end();
        s_occ_q = NULL;
    }
    for (i = 0; i < q->n; i++) {
        if (cmd_done(q->cmd[i]))
            s_vis_used[q->slot[i]] = 0;
        else
            s_retire[s_nretire++] = q->slot[i];
        [q->cmd[i] release];
    }
    q->n = 0;
    q->next = s_qfree;
    s_qfree = q;
}

static void occ_begin(void *ctx, void *qv)
{
    (void)ctx;
    occ_pass_end();
    s_occ_q = (OccQuery *)qv;
    occ_pass_begin();
}

static void occ_end(void *ctx, void *qv)
{
    (void)ctx;
    if (s_occ_q == (OccQuery *)qv) {
        occ_pass_end();
        s_occ_q = NULL;
    }
}

static int occ_result(void *ctx, void *qv, int flush_gpu, uint64_t *count)
{
    OccQuery *q = (OccQuery *)qv;
    const uint64_t *v = (const uint64_t *)[s_vis contents];
    uint64_t sum = 0;
    int i;
    (void)ctx;
    for (i = 0; i < q->n; i++) {
        if (q->cmd[i] == s_cmd) {       /* not committed yet */
            if (!flush_gpu)
                return 0;
            flush(0);
        }
        if (!cmd_done(q->cmd[i]))
            return 0;
    }
    for (i = 0; i < q->n; i++)
        sum += v[q->slot[i]];
    *count = q->ovf ? NV2A_OCC_VISIBLE : sum;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static int occ_ready_on(void)
{
    if (occ_mode() == 2 || !init())
        return 0;
    if (!s_occ.ops)
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
    return 1;
}

static void metal_on_zpass(int enable)
{
    if (occ_ready_on()) {
        @autoreleasepool {
            nv2a_occ_zpass(&s_occ, enable);
        }
    }
}

static void metal_on_zpass_clear(void)
{
    if (occ_ready_on()) {
        @autoreleasepool {
            nv2a_occ_zpass_clear(&s_occ);
        }
    }
}

static int metal_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    @autoreleasepool {
        nv2a_occ_report(&s_occ, va, occ_mode() == 1);
    }
    return 1;
}

static void metal_on_poll(void)
{
    if (s_occ.npend) {
        @autoreleasepool {
            nv2a_occ_poll(&s_occ, 1);
        }
    }
}

/* After the flip's wait every command buffer is done: retired slots free. */
static void occ_after_wait(void)
{
    while (s_nretire)
        s_vis_used[s_retire[--s_nretire]] = 0;
    if (s_occ.ops)
        nv2a_occ_poll(&s_occ, 0);
}

static void flip_log(uint32_t surface_offset, RenderTarget *rt)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_skip, p_cull, p_clears, p_tex, p_up,
                    p_chg, p_re, p_fl, p_occ, p_vis, p_late, p_super, p_sync, p_ovf;
    if (on < 0)
        on = getenv("RECOMP_FLIP_LOG") != NULL;
    if (!on)
        return;
    fprintf(stderr, "[METAL] flip %llu batches %llu prog %llu skipped %llu"
            " culled %llu clears %llu tex %llu uploads %llu changed %llu rehash %llu"
            " flushes %llu occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
            " sync %llu us, slot overflow %llu) present 0x%08X walker 0x%08X\n",
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
            (unsigned long long)(s_occ.reports - p_occ),
            (unsigned long long)(s_occ.vis - p_vis), s_occ.npend,
            (unsigned long long)(s_occ.late - p_late),
            (unsigned long long)(s_occ.super - p_super),
            (unsigned long long)(s_occ.sync_us - p_sync),
            (unsigned long long)(s_occ_ovf - p_ovf),
            rt ? rt->offset : 0, surface_offset);
    p_occ = s_occ.reports; p_vis = s_occ.vis; p_late = s_occ.late;
    p_super = s_occ.super; p_sync = s_occ.sync_us; p_ovf = s_occ_ovf;
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
                " %llu draws (%llu vertex-program), %llu skipped, %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs;"
                " shaders %llu vs + %llu fs, %llu compile errors;"
                " %llu z-clears, %llu depth draws, %llu rt binds (%llu self copies)\n",
                (unsigned long long)s_presents, surface_offset,
                rt ? "drawn here" : "guest bytes",
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_prog, (unsigned long long)s_draws_skipped,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads, (unsigned long long)s_tex_rehash,
                (unsigned long long)s_writebacks,
                (unsigned long long)s_vs_compiled, (unsigned long long)s_fs_compiled,
                (unsigned long long)s_compile_errors,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies);
    if (!pitch || !addr)
        return;
    xbox_FramebufferWindowSet(addr, pitch);
    xbox_FramebufferWindowPresent(addr, pitch);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_metal_backend = {
    metal_on_clear,
    metal_on_zclear,
    metal_on_draw,
    metal_on_flip,
    metal_on_zpass,
    metal_on_zpass_clear,
    metal_on_report,
    metal_on_poll,
    1,                  /* writes_back: on_flip reads the RT into guest memory */
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
