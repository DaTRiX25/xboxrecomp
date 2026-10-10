/**
 * A Vulkan backend for the pushbuffer executor (struct nv2a_pb_backend): the
 * native-Linux sibling of nv2a_pb_metal.m, and a port of it. Where this file
 * does what the Metal backend does, it does it the same way and in the same
 * order, and the comments there apply; what differs is Vulkan's:
 *
 *   on_clear  -> a dynamic-rendering pass with loadOp CLEAR on the surface's
 *                render target (one per guest surface, grown to its largest
 *                clip); a clear of part of it (clip, clear rect) is
 *                vkCmdClearAttachments on the open pass
 *   on_draw   -> vertex-program batches through the program as GLSL
 *                (d3d8_vsh_glsl.c), pre-transformed ones through vs_pass;
 *                pixels through the register combiners as GLSL
 *                (d3d8_combiners_glsl.c) when programmed, else fs_basic.
 *                GLSL becomes SPIR-V with glslang (nv2a_vk_glsl.c, cached on
 *                disk); pipelines go through a VkPipelineCache saved to disk.
 *   on_flip   -> submit without waiting: two frames in flight, each with its
 *                own command buffers, rings and descriptor pools, reused once
 *                the timeline semaphore says the frame two flips back is
 *                done. With the SDL window's Vulkan presenter up
 *                (src/video/fb_present_vulkan.c, whose device this backend
 *                adopts), the surface the walker picked is copied into the
 *                window's back slot on the GPU and published: no CPU copy.
 *                xemu's Vulkan renderer waits for the frame at every flip
 *                (hw/xbox/nv2a/pgraph/vk/draw.c, pgraph_vk_finish); this does
 *                not.
 *   sync_guest -> write a surface back to guest memory when the executor
 *                dumps it, on demand (vk_writeback=lazy, the default) or at
 *                every flip (vk_writeback=always, vk_no_rtt), as Metal.
 *
 * Surfaces as the Metal backend: a render target per guest surface (RT_MAX
 * 32, toolkit 73ed271), every draw scissored to the surface clip (eedf538),
 * a dirty target written back before it is evicted (6407087), 16-bit
 * (R5G6B5) targets sampled directly through a view whose alpha reads 1, the
 * render-target ownership check (nv2a_rt_own) and render.scale. Visibility
 * tests through the shared tracker (nv2a_occ) with occlusion queries, one
 * per render pass a stretch spans, reset from the host.
 *
 * Coordinates: the emitters produce D3D-style clip space (y up, z 0..1), as
 * for Metal; the viewport has a negative height (Vulkan 1.1), so framebuffer
 * rows, scissors and winding are the Metal backend's as they are.
 *
 * Command buffers: the frame's main one, and an upload one submitted ahead
 * of it, which takes every copy into an image made for it (texture uploads,
 * seeds of new render targets): those can be recorded while a pass is open
 * on the main one, as Metal's replaceRegion can. Everything that touches an
 * image the main command buffer already uses goes there, outside a pass.
 *
 * Everything runs on the NV2A ack thread (or the smoke test's main thread),
 * except the window present, which only shares the queue (nvk_queue_lock).
 *
 * Opt-in: RECOMP_PB_BACKEND=vulkan (nv2a_pb_vulkan_register_from_env).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "nv2a_vk_core.h"
#include "nv2a_vk_glsl.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "d3d8_glsl.h"
#include "recomp_env.h"

ptrdiff_t xbox_GetMemoryOffset(void);

#define LOGP "[vulkan] "

extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);
extern int  xbox_FramebufferWindowRunning(void);
extern void xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                                uint32_t pitch, uint32_t bpp);
/* The Vulkan window (fb_present_vulkan.c through fb_present_sdl.c): 1 when
 * it presents slot images on the core's device; the back slot (a struct
 * nvk_image *) a frame is copied into, and its publish. */
extern int   xbox_FramebufferWindowVulkan(void);
extern void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h);
extern void  xbox_FramebufferWindowPublishTexture(void);

/* ---- shader ABI ------------------------------------------------------------ */

_Static_assert(sizeof(struct nv2a_vp_consts) == 80, "nv2a_vp_consts is not 5 x 16 bytes");
_Static_assert(sizeof(((struct nv2a_pb_vsh *)0)->c) == NV2A_VS_MAX_CONSTANTS * 16,
               "vertex constants are not 192 vec4");
_Static_assert(sizeof(struct nv2a_ps_consts) == 416, "nv2a_ps_consts size changed");

/* D3D8_GLSL_PSCONSTS: the shared constants, then tex_bias, std140. */
typedef struct {
    struct nv2a_ps_consts b;
    float tex_bias[4];
} VkPsConsts;
_Static_assert(offsetof(VkPsConsts, tex_bias) == 416 && sizeof(VkPsConsts) == 432,
               "VkPsConsts.tex_bias moved");

#define VS_C_BYTES  (NV2A_VS_MAX_CONSTANTS * 16)
#define VP_C_BYTES  ((uint32_t)sizeof(struct nv2a_vp_consts))
#define PS_C_BYTES  ((uint32_t)sizeof(VkPsConsts))

/* One expanded vertex of a pass-through batch: attributes 0, 3, 4, 9..12,
 * as Metal's. */
#define VTX_F4 7
static const int s_vtx_attr[VTX_F4] = {0, 3, 4, 9, 10, 11, 12};

static const char s_vs_pass_glsl[] =
    "#version 450\n"
    "layout(set = 0, binding = 1, std140) uniform VpC { vec4 c[3]; uvec4 u[2]; } vp;\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_d0;\n"
    "layout(location = 2) in vec4 a_d1;\n"
    "layout(location = 3) in vec4 a_t0;\n"
    "layout(location = 4) in vec4 a_t1;\n"
    "layout(location = 5) in vec4 a_t2;\n"
    "layout(location = 6) in vec4 a_t3;\n"
    D3D8_GLSL_VS_OUT
    /* Attribute 0 is already in surface pixels (x, y, z, rhw): c[0] is
     * (2/w, -2/h, 1/zmax, 0) and c[1] (-1, 1, 0, 0), as Metal's vs_pass. */
    "void main() {\n"
    "    gl_Position = vec4(a_pos.xy * vp.c[0].xy + vp.c[1].xy,\n"
    "                       clamp(a_pos.z * vp.c[0].z, 0.0, 1.0), 1.0);\n"
    "    o_d0 = a_d0; o_d1 = a_d1;\n"
    "    o_t0 = a_t0; o_t1 = a_t1; o_t2 = a_t2; o_t3 = a_t3;\n"
    "    o_fog = 1.0;\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";

/* Metal's fs_basic: MODULATE, then the alpha test. */
static const char s_fs_basic_glsl[] =
    "#version 450\n"
    D3D8_GLSL_PSCONSTS
    D3D8_GLSL_FS_IN
    "layout(set = 1, binding = 0) uniform sampler2D tex0;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 c = i_d0;\n"
    "    if (pc.tex_mode.x == 4u) {\n"
    "        c *= clamp(i_t0, 0.0, 1.0);\n"
    "    } else if (pc.tex_mode.x != 0u) {\n"
    "        vec2 uv = i_t0.xy;\n"
    "        if (pc.tex_mode.x == 1u && i_t0.w != 0.0 && i_t0.w != 1.0) uv /= i_t0.w;\n"
    "        c *= texture(tex0, uv * pc.tex_scale[0].xy, pc.tex_bias.x);\n"
    "    }\n"
    "    if (pc.atest.z != 0u) {\n"
    "        float ref = uintBitsToFloat(pc.atest.x);\n"
    "        uint a = uint(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5), r = uint(ref * 255.0 + 0.5);\n"
    "        bool ok = true;\n"
    "        uint f = pc.atest.y;\n"
    "        if      (f == 1u) ok = false;\n"
    "        else if (f == 2u) ok = a <  r;\n"
    "        else if (f == 3u) ok = a == r;\n"
    "        else if (f == 4u) ok = a <= r;\n"
    "        else if (f == 5u) ok = a >  r;\n"
    "        else if (f == 6u) ok = a != r;\n"
    "        else if (f == 7u) ok = a >= r;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    o_color = c;\n"
    "}\n";

/* ---- device ---------------------------------------------------------------- */

#define FRAMES_MAX 2
#define RT_FORMAT  VK_FORMAT_B8G8R8A8_UNORM   /* X8R8G8B8's bytes */

static const struct nvk_core *C;
static VkDevice               s_dev;
static int                    s_failed;
static int                    s_win;           /* the window takes slot images */
static uint32_t               s_max_dim;
static int                    s_frames = FRAMES_MAX;
static VkDeviceSize           s_ubo_align;

static VkSemaphore            s_timeline;
static uint64_t               s_next_serial = 1;   /* the next submit signals it */
static uint64_t               s_done_serial;       /* known complete */

static VkDescriptorSetLayout  s_dsl[2];
static VkPipelineLayout       s_playout;
static VkDescriptorPool       s_set0_pool;
static VkDescriptorSet        s_set0;
static VkPipelineCache        s_pcache;
static VkShaderModule         s_vs_pass, s_fs_basic;
static VkQueryPool            s_vkq;
static int                    s_qprecise;

/* Rings: one buffer each, FRAMES_MAX regions; a frame's region is reused
 * when its serial is done. Vertex and index as Metal's; uniform for the
 * three constant blocks of each draw; staging for uploads. */
enum { RING_VB, RING_IB, RING_UB, RING_UP, RING_N };
static struct nvk_buffer s_ring[RING_N];
static VkDeviceSize      s_ring_size[RING_N];      /* per frame */
static VkDeviceSize      s_ring_pos[RING_N];       /* within the frame's region */

typedef struct {
    VkCommandPool     pool;
    VkCommandBuffer  *cbs;
    uint32_t          ncbs, used;
    uint64_t          serial;          /* last submit while this frame was current */
    VkDescriptorPool *dps;
    uint32_t          ndps, dp_cur;
} Frame;
static Frame    s_fr[FRAMES_MAX];
static int      s_fi;                  /* the frame being recorded */

static VkCommandBuffer s_cmd;          /* main, begun lazily */
static VkCommandBuffer s_up;           /* uploads, submitted ahead of s_cmd */
static int             s_pass;         /* a render pass is open on s_cmd */

/* Resources an in-flight command buffer may still use, freed once the
 * serial they were last recorded under is done. */
typedef struct {
    struct nvk_image  im;
    VkImageView       view2;
    struct nvk_buffer buf;
    uint64_t          serial;
} Garbage;
static Garbage *s_garbage;
static size_t   s_ngarbage, s_garbage_cap;

static uint64_t s_presents, s_clears, s_draws, s_draws_skipped, s_draws_prog,
                s_draws_culled, s_draws_tex, s_tex_uploads, s_tex_rehash,
                s_tex_changed, s_flushes, s_writebacks, s_submit_errors,
                s_pso_created, s_pso_since_save, s_frame_waits, s_frame_wait_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int vk_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_VK_TRACE) != NULL;
    return on;
}

/* ---- serials and garbage ---------------------------------------------------- */

static uint64_t serial_done(void)
{
    uint64_t v = 0;
    if (vkGetSemaphoreCounterValue(s_dev, s_timeline, &v) == VK_SUCCESS && v > s_done_serial)
        s_done_serial = v;
    return s_done_serial;
}

static void serial_wait(uint64_t v)
{
    VkSemaphoreWaitInfo wi;
    if (!v || serial_done() >= v)
        return;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s_timeline;
    wi.pValues = &v;
    if (vkWaitSemaphores(s_dev, &wi, 5000000000ull) != VK_SUCCESS) {
        static int said;
        if (said++ < 4)
            fprintf(stderr, LOGP "GPU wait for serial %llu timed out\n", (unsigned long long)v);
    }
    serial_done();
}

static void garbage_push(const struct nvk_image *im, VkImageView view2,
                         const struct nvk_buffer *buf)
{
    Garbage *g;
    if (s_ngarbage == s_garbage_cap) {
        size_t nc = s_garbage_cap ? s_garbage_cap * 2 : 64;
        Garbage *n = (Garbage *)realloc(s_garbage, nc * sizeof *n);
        if (!n) {
            /* Out of memory: wait for the GPU and free now. */
            serial_wait(s_next_serial - 1);
            if (im) { struct nvk_image t = *im; nvk_image_free(&t); }
            if (view2) vkDestroyImageView(s_dev, view2, NULL);
            if (buf) { struct nvk_buffer t = *buf; nvk_buffer_free(&t); }
            return;
        }
        s_garbage = n;
        s_garbage_cap = nc;
    }
    g = &s_garbage[s_ngarbage++];
    memset(g, 0, sizeof *g);
    if (im)
        g->im = *im;
    g->view2 = view2;
    if (buf)
        g->buf = *buf;
    /* Whatever was recorded so far is in this or an earlier serial. */
    g->serial = s_next_serial;
}

static void garbage_collect(void)
{
    uint64_t done = serial_done();
    size_t i = 0;
    while (i < s_ngarbage) {
        Garbage *g = &s_garbage[i];
        if (g->serial > done) {
            i++;
            continue;
        }
        if (g->view2)
            vkDestroyImageView(s_dev, g->view2, NULL);
        if (g->im.img || g->im.view)
            nvk_image_free(&g->im);
        if (g->buf.buf)
            nvk_buffer_free(&g->buf);
        s_garbage[i] = s_garbage[--s_ngarbage];
    }
}

/* ---- command buffers ---------------------------------------------------------- */

static VkCommandBuffer cb_new(void)
{
    Frame *f = &s_fr[s_fi];
    VkCommandBufferBeginInfo bi;
    VkCommandBuffer cb;

    if (f->used == f->ncbs) {
        VkCommandBufferAllocateInfo ai;
        VkCommandBuffer *n = (VkCommandBuffer *)realloc(f->cbs, (f->ncbs + 8) * sizeof *n);
        if (!n)
            return VK_NULL_HANDLE;
        f->cbs = n;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 8;
        if (vkAllocateCommandBuffers(s_dev, &ai, f->cbs + f->ncbs) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        f->ncbs += 8;
    }
    cb = f->cbs[f->used++];
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void ensure_cmd(void)
{
    if (!s_cmd)
        s_cmd = cb_new();
}

static void ensure_up(void)
{
    if (!s_up)
        s_up = cb_new();
}

/* Visibility tests: the slot counting on the open pass. */
static void occ_pass_begin(void);
static void occ_pass_end(void);
static void occ_after_wait(void);

typedef struct DepthTarget DepthTarget;
typedef struct RenderTarget RenderTarget;
static RenderTarget *s_enc_rt;   /* the target the open pass draws into */
static DepthTarget  *s_enc_dt;   /* and its depth attachment, or NULL */

static void end_encoder(void)
{
    if (s_pass) {
        occ_pass_end();
        vkCmdEndRendering(s_cmd);
        s_pass = 0;
    }
    s_enc_rt = NULL;
    s_enc_dt = NULL;
}

/* Last uniform data placed, per block, so an unchanged block is not placed
 * again; reset with the ring. */
static struct {
    int          valid[3];
    VkDeviceSize off[3];
    uint8_t      vs[VS_C_BYTES];
    uint8_t      vp[sizeof(struct nv2a_vp_consts)];
    uint8_t      ps[sizeof(VkPsConsts)];
} s_umemo;
static VkDescriptorSet s_tset_last;      /* the last texture set, this frame */
static VkImageView     s_tset_views[4];
static VkSampler       s_tset_smp[4];

static void rings_reset(void)
{
    int k;
    for (k = 0; k < RING_N; k++)
        s_ring_pos[k] = 0;
    memset(s_umemo.valid, 0, sizeof s_umemo.valid);
}

/* End the open pass, submit what is recorded (uploads first) and, with
 * `wait`, block until the GPU is done with it. After a wait every ring is
 * free again. */
static void flush(int wait)
{
    VkCommandBuffer cbs[2];
    VkSubmitInfo si;
    VkTimelineSemaphoreSubmitInfo ti;
    uint64_t serial;
    uint32_t n = 0;
    VkResult r;

    end_encoder();
    if (!s_cmd && !s_up) {
        if (wait) {
            serial_wait(s_next_serial - 1);
            rings_reset();
            s_flushes++;
            occ_after_wait();
        }
        return;
    }
    if (s_up) {
        vkEndCommandBuffer(s_up);
        cbs[n++] = s_up;
    }
    if (s_cmd) {
        vkEndCommandBuffer(s_cmd);
        cbs[n++] = s_cmd;
    }
    serial = s_next_serial++;
    memset(&ti, 0, sizeof ti);
    ti.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &serial;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &ti;
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_timeline;
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, VK_NULL_HANDLE);
    nvk_queue_unlock();
    if (r != VK_SUCCESS) {
        s_submit_errors++;
        if (s_submit_errors <= 4)
            fprintf(stderr, LOGP "vkQueueSubmit failed: %s\n", nvk_result_str(r));
        if (r == VK_ERROR_DEVICE_LOST) {
            fprintf(stderr, LOGP "device lost: drawing stops\n");
            s_failed = 1;
        }
    }
    s_fr[s_fi].serial = serial;
    s_cmd = VK_NULL_HANDLE;
    s_up = VK_NULL_HANDLE;
    s_flushes++;
    if (wait) {
        serial_wait(serial);
        rings_reset();
        occ_after_wait();
    }
}

/* `bytes` from ring k, aligned; on overflow the frame so far is flushed
 * (and waited for) and the ring starts over. Returns the mapped pointer
 * and *off, the offset in the ring's buffer; NULL if it can never fit. */
static void *ring_alloc(int k, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize *off)
{
    VkDeviceSize at = (s_ring_pos[k] + align - 1) & ~(align - 1);
    if (bytes > s_ring_size[k])
        return NULL;
    if (at + bytes > s_ring_size[k]) {
        flush(1);   /* resets every ring */
        at = 0;
    }
    s_ring_pos[k] = at + bytes;
    *off = (VkDeviceSize)s_fi * s_ring_size[k] + at;
    return (uint8_t *)s_ring[k].map + *off;
}

/* ---- render targets: one per guest surface --------------------------------- */

#define DT_MAX 4
struct DepthTarget {
    uint32_t         zeta, w, h;
    uint32_t         hw, hh;     /* host size: w x h at render.scale */
    struct nvk_image im;
    uint64_t         last_use;
    int              fresh;      /* contents undefined: the next pass clears */
};

/* As Metal: 32 slots hold any frame Burnout 3 draws (14 surfaces in Crash
 * mode); xemu keeps an unbounded surface list. */
#define RT_MAX 32
struct RenderTarget {
    uint32_t         offset;     /* color_offset, as the title set it */
    uint32_t         addr;       /* its guest address (nv2a_pb_dma_resolve) */
    uint32_t         w, h;       /* guest size: the cache key, the mapping */
    uint32_t         hw, hh;     /* host size: the image (render.scale) */
    struct nvk_image im;
    VkImageView      opaque;     /* a view with alpha read as 1 (16-bit) */
    uint64_t         last_use;
    DepthTarget     *dt;         /* the depth buffer its passes attach, or NULL */
    uint32_t         pitch;      /* the guest pitch it was made with */
    int              swz;        /* made as a swizzled surface (format type 2) */
    int              dirty;      /* drawn since its last write-back */
    int              presented;  /* ever picked by present_target */
    struct nv2a_rt_own own;      /* is the guest memory under it still its own */
};

static RenderTarget  s_rt[RT_MAX];
static DepthTarget   s_dt[DT_MAX];
static uint64_t      s_tick;
static uint64_t      s_zclears, s_draws_z, s_rtt_binds, s_self_copies;
static uint64_t      s_stale_drops, s_retired, s_decode_syncs;
static uint64_t      s_grows, s_partial_clears;

static int wb_always(void)
{
    const char *e = recomp_env(RENV_VK_WRITEBACK);
    return (e && !strcasecmp(e, "always")) || recomp_env(RENV_VK_NO_RTT) != NULL;
}

/* ---- render-target ownership: as Metal (nv2a_backend_common.h) ------------- */

static int alias_check_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_RT_ALIAS_CHECK);
        on = !(e && e[0] == '0');
    }
    return on;
}

static void rt_sync(RenderTarget *rt, uint32_t pitch);
static void rt_free(RenderTarget *rt);

static void rt_own_reset(RenderTarget *rt)
{
    if (!alias_check_on() || !rt->pitch)
        return;
    nv2a_rt_own_reset(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                      rt->pitch, rt->h, (uint32_t)s_presents);
}

static int rt_drop_if_stale(RenderTarget *rt, const char *why)
{
    static int said;
    if (!alias_check_on() || !rt->im.img || !rt->pitch)
        return 0;
    if (!nv2a_rt_own_stale(&rt->own, (const uint8_t *)xbox_GetMemoryOffset(), rt->addr,
                           rt->pitch, rt->h, (uint32_t)s_presents))
        return 0;
    s_stale_drops++;
    if (said < 20 || vk_trace()) {
        said++;
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target dropped (%s): the title"
                " rewrote its %u bytes; last drawn at flip %u, now %llu%s\n",
                rt->offset, rt->w, rt->h, why, rt->pitch * rt->h, rt->own.draw_flip,
                (unsigned long long)s_presents, said == 20 ? " (further drops: the"
                " present summary)" : "");
    }
    rt->dirty = 0;   /* the title's bytes win: no write-back */
    rt_free(rt);
    return 1;
}

static void rt_free(RenderTarget *rt)
{
    if (rt->dirty && rt->presented && rt_drop_if_stale(rt, "evict"))
        return;
    if (rt->dirty && rt->presented)
        rt_sync(rt, rt->pitch);
    if (s_enc_rt == rt)
        end_encoder();
    garbage_push(&rt->im, rt->opaque, NULL);
    memset(rt, 0, sizeof *rt);
}

static DepthTarget *depth_target(uint32_t w, uint32_t h)
{
    uint32_t zeta = nv2a_pb_vsh_state()->zeta_offset;
    DepthTarget *e = NULL, *lru = &s_dt[0];
    int i;

    for (i = 0; i < DT_MAX; i++) {
        DepthTarget *d = &s_dt[i];
        if (d->im.img && d->zeta == zeta && d->w == w && d->h == h) {
            d->last_use = s_tick;
            return d;
        }
        if (!d->im.img && !e)
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
        garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(e, 0, sizeof *e);
    }
    nv2a_host_size(w, h, nv2a_host_render_scale(), s_max_dim, &e->hw, &e->hh);
    if (!nvk_image_new(&e->im, C->depth_format, e->hw, e->hh, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        return NULL;
    e->zeta = zeta;
    e->w = w;
    e->h = h;
    e->last_use = s_tick;
    e->fresh = 1;
    if (e->hw != w)
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer (%ux%u host)\n",
                zeta, w, h, e->hw, e->hh);
    else
        fprintf(stderr, LOGP "zeta 0x%08X: %ux%u depth buffer\n", zeta, w, h);
    return e;
}

/* Open a pass on rt: with `clear`, loadOp CLEAR to c; else LOAD. As Metal's
 * begin_pass: the depth buffer rt->dt is attached when it has one, zmask
 * (bit 0 depth, bit 1 stencil) clears it to z / st, and a fresh one clears
 * to the far plane and stencil 0. */
static void begin_pass(RenderTarget *rt, int clear, const float c[4],
                       unsigned zmask, float z, uint32_t st)
{
    DepthTarget *dt = rt->dt;
    VkRenderingAttachmentInfo ca, da, sa;
    VkRenderingInfo ri;

    end_encoder();
    ensure_cmd();
    rt->dirty = 1;
    nv2a_rt_own_drawn(&rt->own, (uint32_t)s_presents);
    nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = rt->im.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear)
        memcpy(ca.clearValue.color.float32, c, 16);
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = rt->hw;
    ri.renderArea.extent.height = rt->hh;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (dt) {
        int zc = (zmask & 1u) || dt->fresh, sc = (zmask & 2u) || dt->fresh;
        nvk_image_to(s_cmd, &dt->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        memset(&da, 0, sizeof da);
        da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        da.imageView = dt->im.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sa = da;
        da.loadOp = zc ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.clearValue.depthStencil.depth = (zmask & 1u) ? z : 1.0f;
        sa.loadOp = sc ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.clearValue.depthStencil.stencil = (zmask & 2u) ? (st & 0xFFu) : 0u;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &sa;
        dt->fresh = 0;
    }
    vkCmdBeginRendering(s_cmd, &ri);
    s_pass = 1;
    s_enc_rt = rt;
    s_enc_dt = dt;
    occ_pass_begin();
}

static void begin_encoder(RenderTarget *rt, int clear, const float c[4])
{
    begin_pass(rt, clear, c, 0, 1.0f, 0);
}

/* Bytes into image `im` (level `level`, w x h, tightly packed BGRA8) through
 * the staging ring, on the upload command buffer: im must be new this
 * batch (nothing in s_cmd uses it yet). Leaves it in `final`. */
static int upload_level(struct nvk_image *im, uint32_t level, uint32_t w, uint32_t h,
                        const void *px, VkImageLayout final, int last)
{
    VkDeviceSize off;
    VkBufferImageCopy bc;
    void *dst = ring_alloc(RING_UP, (VkDeviceSize)w * h * 4u, 16, &off);

    if (!dst)
        return 0;
    memcpy(dst, px, (size_t)w * h * 4u);
    ensure_up();
    if (level == 0)
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.mipLevel = level;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_up, s_ring[RING_UP].buf, im->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    if (last)
        nvk_image_to(s_up, im, final);
    return 1;
}

static int rt_image_new(struct nvk_image *im, uint32_t hw, uint32_t hh)
{
    return nvk_image_new(im, RT_FORMAT, hw, hh, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
}

/* im (w x h guest, hw x hh host, new) from the guest's bytes at addr when
 * the surface is 32-bit, as Metal's rt_seed; else cleared to transparent
 * black, as a new Metal target's margin is. On the upload command buffer;
 * im ends as a colour attachment. */
static void rt_seed(struct nvk_image *im, uint32_t addr, uint32_t pitch, uint32_t w,
                    uint32_t h, uint32_t hw, uint32_t hh)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + addr;
    VkDeviceSize off;
    uint8_t *dst;

    if (pitch >= w * 4u
            && (dst = (uint8_t *)ring_alloc(RING_UP, (VkDeviceSize)hw * hh * 4u, 16, &off))) {
        VkBufferImageCopy bc;
        if (hw == w) {
            uint32_t y;
            for (y = 0; y < h; y++)
                memcpy(dst + (size_t)y * w * 4u, src + (size_t)y * pitch, (size_t)w * 4u);
        } else {
            nv2a_upscale_nearest32(src, pitch, w, h, hw / w, dst, hw * 4u);
        }
        ensure_up();
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.bufferOffset = off;
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = hw;
        bc.imageExtent.height = hh;
        bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_up, s_ring[RING_UP].buf, im->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    } else {
        VkClearColorValue z;
        VkImageSubresourceRange r;
        memset(&z, 0, sizeof z);
        memset(&r, 0, sizeof r);
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.levelCount = 1;
        r.layerCount = 1;
        ensure_up();
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(s_up, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
    }
    nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

/* rt made w x h (guest pixels), what it holds kept at its top left: Metal's
 * rt_grow. The new image is seeded on the upload command buffer, which runs
 * first, and the old contents are copied over it on the main one. */
static int rt_grow(RenderTarget *rt, uint32_t w, uint32_t h)
{
    static int said;
    struct nvk_image im;
    uint32_t hw, hh;
    unsigned n;
    int keep;

    n = nv2a_host_size(w, h, nv2a_host_render_scale(), s_max_dim, &hw, &hh);
    keep = rt->hw == rt->w * n && rt->hh == rt->h * n;
    if (!keep)
        rt_sync(rt, rt->pitch);
    end_encoder();
    if (!rt_image_new(&im, hw, hh))
        return 0;
    rt_seed(&im, rt->addr, rt->pitch, w, h, hw, hh);
    if (keep) {
        VkImageCopy ic;
        ensure_cmd();
        nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        nvk_image_to(s_cmd, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = rt->hw;
        ic.extent.height = rt->hh;
        ic.extent.depth = 1;
        vkCmdCopyImage(s_cmd, rt->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    }
    if (said++ < 20)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target grown to %ux%u\n",
                rt->offset, rt->w, rt->h, w, h);
    garbage_push(&rt->im, rt->opaque, NULL);
    rt->opaque = VK_NULL_HANDLE;
    rt->im = im;
    rt->w = w;
    rt->h = h;
    rt->hw = hw;
    rt->hh = hh;
    rt->dt = NULL;
    rt_own_reset(rt);
    s_grows++;
    return 1;
}

/* The render target for the surface at color_offset: Metal's surface(),
 * rule for rule (see the comments there). */
static RenderTarget *surface(void)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    uint32_t w = g->clip_x + g->clip_w, h = g->clip_y + g->clip_h;
    RenderTarget *free_rt = NULL, *lru = &s_rt[0];
    uint32_t hw, hh;
    int i;

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            if (rt_drop_if_stale(rt, "reuse"))
                break;
            rt->last_use = s_tick;
            return rt;
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || rt->offset != g->color_offset || rt->pitch != g->pitch)
            continue;
        if (rt_drop_if_stale(rt, "reuse"))
            break;
        if ((rt->w >= w && rt->h >= h)
                || rt_grow(rt, rt->w > w ? rt->w : w, rt->h > h ? rt->h : h)) {
            rt->last_use = s_tick;
            return rt;
        }
        break;
    }
    if (alias_check_on()) {
        uint32_t addr = nv2a_pb_dma_resolve(g->color_offset);
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            if (!rt->im.img || !nv2a_rt_overlap(rt->addr, rt->pitch * rt->h, addr, g->pitch * h))
                continue;
            if (rt_drop_if_stale(rt, "overlap"))
                continue;
            rt_sync(rt, rt->pitch);
            if (s_retired++ < 20)
                fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target retired:"
                        " 0x%08X %ux%u overlaps it\n", rt->offset, rt->w, rt->h,
                        g->color_offset, w, h);
            rt_free(rt);
        }
    }
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img && !free_rt)
            free_rt = rt;
        if (rt->last_use < lru->last_use)
            lru = rt;
    }
    if (!free_rt) {
        /* Out of slots: the least recently used target goes, written back
         * first, presented or not (6407087, BO-47). */
        free_rt = lru;
        if (free_rt->dirty && !rt_drop_if_stale(free_rt, "evict"))
            rt_sync(free_rt, free_rt->pitch);
        if (free_rt->im.img)
            rt_free(free_rt);
    }
    nv2a_host_size(w, h, nv2a_host_render_scale(), s_max_dim, &hw, &hh);
    if (!rt_image_new(&free_rt->im, hw, hh))
        return NULL;
    free_rt->offset = g->color_offset;
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->hw = hw;
    free_rt->hh = hh;
    free_rt->pitch = g->pitch;
    free_rt->swz = ((g->format >> 8) & 0xF) == 2;
    free_rt->last_use = s_tick;
    rt_own_reset(free_rt);
    rt_seed(&free_rt->im, free_rt->addr, g->pitch, w, h, hw, hh);
    if (hw != w)
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target (%ux%u host)\n",
                g->color_offset, w, h, hw, hh);
    else
        fprintf(stderr, LOGP "surface 0x%08X: %ux%u render target\n",
                g->color_offset, w, h);
    return free_rt;
}

/* ---- visibility tests (declared early: clears pause the count) --------------- */

#define VIS_SLOTS  8192
#define OCC_QSLOTS 16
#define OCC_POOL   (NV2A_OCC_PENDING * 2 + NV2A_OCC_SEG)
typedef struct OccQuery {
    int              slot[OCC_QSLOTS];
    uint64_t         serial[OCC_QSLOTS];   /* the submit it is recorded in */
    int              n, ovf;
    struct OccQuery *next;                 /* free list */
} OccQuery;
static OccQuery         *s_occ_q;          /* the query counting now, or NULL */
static int               s_occ_slot = -1;  /* its slot on the open pass */

/* ---- clears -------------------------------------------------------------------- */

/* A colour clear covers its clip and clear rect only (nv2a_clear_box), as on
 * Metal: a box that is the whole target is a clearing pass, a smaller one
 * vkCmdClearAttachments on a loading pass. A clear is not a zpass sample:
 * the query counting on the pass is ended around it and the count goes on
 * in a new slot. */
static void vk_on_clear(uint32_t param);
static int init(void);
static void occ_query_begin(void);

static void vk_on_clear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    RenderTarget *rt;
    float c[4];
    uint32_t b[4];
    VkClearAttachment ca;
    VkClearRect cr;

    if (!(param & 0xF0) || !init())
        return;
    rt = surface();
    if (!rt || !nv2a_clear_box(g, rt->w, rt->h, b))
        return;
    c[0] = (float)((g->clear_color >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((g->clear_color >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(g->clear_color & 0xFF) / 255.0f;
    c[3] = (float)(g->clear_color >> 24) / 255.0f;
    s_clears++;
    if (b[0] == 0 && b[1] == 0 && b[2] == rt->w && b[3] == rt->h) {
        begin_encoder(rt, 1, c);
        return;
    }
    if (!s_pass || s_enc_rt != rt)
        begin_encoder(rt, 0, NULL);
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    memcpy(ca.clearValue.color.float32, c, 16);
    memset(&cr, 0, sizeof cr);
    cr.rect.offset.x = (int32_t)((uint64_t)b[0] * rt->hw / rt->w);
    cr.rect.offset.y = (int32_t)((uint64_t)b[1] * rt->hh / rt->h);
    cr.rect.extent.width = (uint32_t)((uint64_t)b[2] * rt->hw / rt->w) - (uint32_t)cr.rect.offset.x;
    cr.rect.extent.height = (uint32_t)((uint64_t)b[3] * rt->hh / rt->h) - (uint32_t)cr.rect.offset.y;
    cr.layerCount = 1;
    if (s_occ_slot >= 0) {
        occ_pass_end();
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
        occ_query_begin();
    } else {
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
    }
    s_partial_clears++;
}

static void vk_on_zclear(uint32_t param)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    RenderTarget *rt;
    DepthTarget *dt;

    if (!(param & 3u) || !init())
        return;
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

/* ---- vertices ---------------------------------------------------------------- */

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

/* ---- shaders -------------------------------------------------------------------- */

static uint64_t s_compile_errors, s_vs_compiled, s_fs_compiled, s_glsl_compiles;

static VkShaderModule module_from_glsl(enum nvk_stage stage, const char *src, const char *what)
{
    static char err[8192];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    if (!nvk_glsl_spirv(stage, src, &w, &n, &compiled, err, sizeof err)) {
        s_compile_errors++;
        if (s_compile_errors <= 4)
            fprintf(stderr, LOGP "\x1b[1;31m%s: GLSL compile failed: %s\x1b[0m\n%s\n",
                    what, err, src);
        return VK_NULL_HANDLE;
    }
    s_glsl_compiles += compiled ? 1 : 0;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(s_dev, &ci, NULL, &m) != VK_SUCCESS) {
        s_compile_errors++;
        fprintf(stderr, LOGP "%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    if (vk_trace())
        fprintf(stderr, LOGP "%s: %s (%zu words)\n", what,
                compiled ? "compiled" : "from the SPIR-V cache", n);
    return m;
}

#define VS_MAX 2048
typedef struct {
    uint32_t       hash;
    uint16_t       inputs;
    int            used, bad;
    VkShaderModule mod;
} VsEntry;
static VsEntry s_vs[VS_MAX];

/* As Metal's vs_cache: the program from its start slot, parsed, emitted as
 * GLSL with the screen-space epilogue, keyed by nv2a_vsh_program_hash. */
static VsEntry *vs_cache(void)
{
    static NV2AVshProgram parsed;
    static char glsl[65536];
    static struct { int valid; uint32_t dwords, start; VsEntry *e; } memo;
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash;
    VsEntry *e = NULL;
    char what[32];
    int i;

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
            return NULL;
        e = &s_vs[i];
        e->used = 1;
        e->hash = hash;
        e->bad = 1;
        d3d8_vsh_parse(&v->prog[v->start][0], NV2A_VSH_SLOTS - (int)v->start, &parsed);
        snprintf(what, sizeof what, "program %08X", hash);
        if (d3d8_vsh_generate_glsl(&parsed, D3D8_GLSL_SCREEN_SPACE, glsl, (int)sizeof glsl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no GLSL\n", what);
        } else if ((e->mod = module_from_glsl(NVK_STAGE_VERTEX, glsl, what)) != VK_NULL_HANDLE) {
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

#define FS_MAX 2048
typedef struct {
    uint32_t          hash;
    int               bad;
    NV2ACombinerState st;
    VkShaderModule    mod;
} FsEntry;
static FsEntry *s_fs;
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

typedef struct nv2a_stage Stage;
static Stage s_stage[4];

/* As Metal's fs_select: the combiners when programmed, else fs_basic; only
 * 2D textures are bound, so every sampling mode goes to the combiners as 2D. */
static VkShaderModule fs_select(const struct nv2a_pb_vsh *v)
{
    static char glsl[65536];
    static struct { int valid; uint32_t r[36]; VkShaderModule m; } memo;
    NV2ACombinerState st;
    uint32_t key[36], modes = 0, hash;
    FsEntry *e = NULL;
    char what[32];
    int n;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        uint32_t m = (uint32_t)s_stage[n].mode;
        if (s_stage[n].raw_mode == 5)
            m = 5;
        else if (m && m != 4) m = 1;
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
        return memo.m;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;
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
        if (d3d8_combiners_generate_glsl(&st, glsl, (int)sizeof glsl) <= 0) {
            s_compile_errors++;
            fprintf(stderr, LOGP "%s: no GLSL\n", what);
        } else if ((e->mod = module_from_glsl(NVK_STAGE_FRAGMENT, glsl, what)) != VK_NULL_HANDLE) {
            e->bad = 0;
            s_fs_compiled++;
        }
    }
    memcpy(memo.r, key, sizeof key);
    memo.valid = 1;
    memo.m = e->bad ? s_fs_basic : e->mod;
    return memo.m;
}

/* ---- pipelines -------------------------------------------------------------------- */

static VkBlendOp blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B:
    case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static VkBlendFactor blend_factor(uint32_t gl, int alpha)
{
    static const VkBlendFactor map[NV2A_BF_UNKNOWN + 1] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        /* One RGBA constant, which nv2a_blend_constant() fills for _COLOR
         * or _ALPHA use, as Metal and D3D11. */
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE,                      /* unknown */
    };
    uint32_t bf = nv2a_blend_from_gl(gl);
    return map[alpha ? nv2a_blend_for_alpha(bf) : bf];
}

/* A pipeline: its two modules, the vertex inputs, the blend state and
 * colour mask, the depth attachment and the topology. Everything else
 * (viewport, scissor, cull, winding, depth and stencil tests, blend
 * constant, stencil reference) is dynamic state, set per draw. */
typedef struct {
    uint64_t vs, fs;
    uint32_t nf4, topo, depth, wmask;
    uint32_t blend[4];       /* enable, src, dst, op */
} PsoKey;
#define PSO_CAP 8192         /* power of two */
static struct { PsoKey k; VkPipeline p; int used; } *s_pso;
static uint32_t s_pso_n;

static VkPipeline pipeline_new(const PsoKey *k)
{
    VkPipelineShaderStageCreateInfo ss[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[NV2A_VS_MAX_INPUTS];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo pr;
    VkGraphicsPipelineCreateInfo gp;
    VkFormat cf = RT_FORMAT;
    VkPipeline p = VK_NULL_HANDLE;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    uint32_t i;
    VkResult r;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = (VkShaderModule)(uintptr_t)k->vs;
    ss[0].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = (VkShaderModule)(uintptr_t)k->fs;
    ss[1].pName = "main";
    memset(&vb, 0, sizeof vb);
    vb.binding = 0;
    vb.stride = k->nf4 * 16u;
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    for (i = 0; i < k->nf4; i++) {
        va[i].location = i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = i * 16u;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = k->nf4 ? 1 : 0;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = k->nf4;
    vi.pVertexAttributeDescriptions = va;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = k->topo == NV2A_TOPO_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == NV2A_TOPO_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = k->blend[0] ? VK_TRUE : VK_FALSE;
    if (k->blend[0]) {
        cba.srcColorBlendFactor = blend_factor(k->blend[1], 0);
        cba.dstColorBlendFactor = blend_factor(k->blend[2], 0);
        cba.srcAlphaBlendFactor = blend_factor(k->blend[1], 1);
        cba.dstAlphaBlendFactor = blend_factor(k->blend[2], 1);
        cba.colorBlendOp = (VkBlendOp)k->blend[3];
        cba.alphaBlendOp = (VkBlendOp)k->blend[3];
    }
    cba.colorWriteMask = ((k->wmask & 0x00FF0000u) ? 0 : VK_COLOR_COMPONENT_R_BIT)
                       | ((k->wmask & 0x0000FF00u) ? 0 : VK_COLOR_COMPONENT_G_BIT)
                       | ((k->wmask & 0x000000FFu) ? 0 : VK_COLOR_COMPONENT_B_BIT)
                       | ((k->wmask & 0xFF000000u) ? 0 : VK_COLOR_COMPONENT_A_BIT);
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    memset(&pr, 0, sizeof pr);
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &cf;
    pr.depthAttachmentFormat = k->depth ? C->depth_format : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = k->depth ? C->depth_format : VK_FORMAT_UNDEFINED;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &pr;
    gp.stageCount = 2;
    gp.pStages = ss;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = s_playout;
    r = vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &gp, NULL, &p);
    if (r != VK_SUCCESS) {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "pipeline failed: %s\n", nvk_result_str(r));
        return VK_NULL_HANDLE;
    }
    s_pso_created++;
    s_pso_since_save++;
    return p;
}

static VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, uint32_t nf4,
                           enum nv2a_topology topo)
{
    static uint32_t last = 0xFFFFFFFFu;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    PsoKey k;
    uint32_t h, i;

    memset(&k, 0, sizeof k);
    k.vs = (uint64_t)(uintptr_t)vs;
    k.fs = (uint64_t)(uintptr_t)fs;
    k.nf4 = nf4;
    k.topo = (uint32_t)topo;
    k.depth = s_enc_dt ? 1 : 0;
    k.wmask = g->color_keep;
    k.blend[0] = g->blend_enable ? 1 : 0;
    k.blend[1] = k.blend[0] ? g->blend_sfactor : 0;
    k.blend[2] = k.blend[0] ? g->blend_dfactor : 0;
    k.blend[3] = k.blend[0] ? (uint32_t)blend_op(g->blend_equation) : 0;
    if (last < PSO_CAP && s_pso[last].used && !memcmp(&s_pso[last].k, &k, sizeof k))
        return s_pso[last].p;
    h = fnv(&k, sizeof k) & (PSO_CAP - 1);
    for (i = h; s_pso[i].used; i = (i + 1) & (PSO_CAP - 1))
        if (!memcmp(&s_pso[i].k, &k, sizeof k)) {
            last = i;
            return s_pso[i].p;
        }
    if (s_pso_n + 1 >= PSO_CAP * 3 / 4)
        return VK_NULL_HANDLE;   /* a title with this many wants eviction */
    s_pso[i].k = k;
    s_pso[i].p = pipeline_new(&k);
    s_pso[i].used = 1;
    s_pso_n++;
    last = i;
    if (vk_trace() && s_pso[i].p)
        fprintf(stderr, LOGP "pipeline #%u: blend %u %04X/%04X op %u mask %08X depth %u"
                " topo %u inputs %u\n", s_pso_n, k.blend[0], k.blend[1], k.blend[2],
                k.blend[3], k.wmask, k.depth, k.topo, k.nf4);
    return s_pso[i].p;
}

/* ---- textures: Metal's cache, key and invalidation ------------------------- */

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 256
typedef struct {
    uint32_t         offset, color, width, height, pitch, palette, levels;
    uint32_t         gen, used;
    uint64_t         hash;
    int              live;
    struct nvk_image im;
} TexEntry;

static TexEntry         s_tc[TEX_CACHE];
static uint32_t         s_tc_clock;
static uint32_t        *s_texels;
static size_t           s_texels_cap;
static struct nvk_image s_black, s_magenta;

static int solid_texture(struct nvk_image *im, uint32_t argb)
{
    if (!nvk_image_new(im, RT_FORMAT, 1, 1, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    return upload_level(im, 0, 1, 1, &argb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
}

static int no_mips(void)
{
    static int off = -1;
    if (off < 0)
        off = recomp_env(RENV_VK_NO_MIPS) != NULL;
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
            t->mode = 0;
    }
}

static uint64_t tex_hash_pal(const uint8_t *mem, const Stage *t, uint32_t bytes)
{
    uint64_t h = nv2a_tex_hash(mem + t->offset, bytes, NV2A_TEX_HASH_SEED);
    if (t->color == NV2A_TEX_P8)
        h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
    return h;
}

/* Decoded on the CPU by the shared decode, uploaded as BGRA8 into a new
 * image: a draw already recorded this frame keeps the texels it was
 * recorded with, as Metal's new MTLTexture per change. */
static int tex_upload(const Stage *t, struct nvk_image *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL, *src;
    uint32_t l;

    if (n > s_texels_cap) {
        uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
        if (!p)
            return 0;
        s_texels = p;
        s_texels_cap = n;
    }
    if (t->color == NV2A_TEX_P8)
        pal = (const uint32_t *)(mem + t->palette);
    nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch,
                    t->levels, pal, pal ? t->pal_len : 0, s_texels);
    if (!nvk_image_new(out, RT_FORMAT, t->width, t->height, t->levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    for (l = 0, src = s_texels; l < t->levels; l++) {
        uint32_t w = nv2a_tex_level_dim(t->width, l), h = nv2a_tex_level_dim(t->height, l);
        if (!upload_level(out, l, w, h, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          l + 1 == t->levels)) {
            garbage_push(out, VK_NULL_HANDLE, NULL);
            memset(out, 0, sizeof *out);
            return 0;
        }
        src += (size_t)w * h;
    }
    s_tex_uploads++;
    return 1;
}

static void rt_sync(RenderTarget *rt, uint32_t pitch);

static VkImageView tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];

    if (t->valid && (uint64_t)t->width * t->height <= TEX_MAX_TEXELS)
        bytes = nv2a_tex_extent(t->color, t->width, t->height, t->pitch, t->levels);
    /* A render target over the bytes is written back first (Metal's decode
     * sync). */
    if (bytes) {
        uint32_t lo = t->offset & 0x07FFFFFFu, hi = lo + bytes;
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *rt = &s_rt[i];
            uint32_t rlo = rt->addr & 0x07FFFFFFu;
            if (rt->im.img && rt->dirty && rt->pitch
                    && lo < rlo + rt->pitch * rt->h && rlo < hi) {
                if (rt_drop_if_stale(rt, "sync"))
                    continue;
                s_decode_syncs++;
                if (vk_trace())
                    fprintf(stderr, LOGP "decode sync: target 0x%08X %ux%u (%u bytes) for"
                            " texture 0x%08X fmt 0x%02X %ux%u (%u bytes), flip %llu\n",
                            rt->offset, rt->w, rt->h, rt->pitch * rt->h, t->offset,
                            t->color, t->width, t->height, bytes,
                            (unsigned long long)s_presents);
                rt_sync(rt, rt->pitch);
            }
        }
    }
    if (!bytes) {
        static uint8_t said[256];
        if ((uint64_t)t->width * t->height > TEX_MAX_TEXELS)
            return s_black.view;
        if (!said[t->color & 0xFF]) {
            said[t->color & 0xFF] = 1;
            fprintf(stderr, LOGP "\x1b[1;35mtexture format 0x%02X (%ux%u at"
                    " 0x%08X) not decoded: drawn magenta\x1b[0m\n",
                    t->color, t->width, t->height, t->offset);
        }
        return s_magenta.view;
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
                struct nvk_image ni;
                e->hash = h;
                s_tex_changed++;
                if (!tex_upload(t, &ni))
                    return s_black.view;
                garbage_push(&e->im, VK_NULL_HANDLE, NULL);
                e->im = ni;
            }
        }
    } else {
        struct nvk_image ni;
        e = victim;
        if (e->live)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(&e->im, 0, sizeof e->im);
        e->live = 0;
        if (!tex_upload(t, &ni))
            return s_black.view;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        e->im = ni;
        e->live = 1;
    }
    e->used = ++s_tc_clock;
    return e->im.view;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp. */
static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

#define SMP_MAX 128
static struct { uint64_t key; VkSampler s; } s_smp[SMP_MAX];
static int s_smp_n;

static VkSampler sampler(const Stage *t)
{
    static int point = -1;
    VkSamplerCreateInfo sc;
    VkSampler st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i;
    uint64_t key;

    if (point < 0)
        point = recomp_env(RENV_VK_POINT) != NULL;
    mn = !point && nv2a_tex_filter_linear(t->filter, 0);
    mg = !point && nv2a_tex_filter_linear(t->filter, 1);
    mip = t->levels > 1 ? nv2a_tex_filter_mip(t->filter) : 0;
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key)
            return s_smp[i].s;
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.minFilter = mn ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.magFilter = mg ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = address_mode(u);
    sc.addressModeV = address_mode(v);
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.minLod = mip ? (float)t->lod_min : 0.0f;
    /* Not mipmapped: level 0, with the min/mag choice kept (the Vulkan
     * spec's maxLod 0.25 for Metal's MipFilterNotMipmapped). */
    sc.maxLod = mip ? (float)t->lod_max : 0.25f;
    if (vkCreateSampler(s_dev, &sc, NULL, &st) != VK_SUCCESS)
        return s_smp_n ? s_smp[0].s : VK_NULL_HANDLE;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % SMP_MAX);
        /* A recorded draw may still use it. */
        flush(1);
        vkDestroySampler(s_dev, s_smp[i].s, NULL);
    }
    s_smp[i].key = key;
    s_smp[i].s = st;
    return st;
}

/* ---- render-to-texture: Metal's rt_texture ----------------------------------
 *
 * A stage whose texture is a surface drawn here samples the render target
 * itself; 16-bit (R5G6B5) ones through a view whose alpha reads 1 (xemu's
 * check_surface_to_texture_compatiblity, hw/xbox/nv2a/pgraph/vk/texture.c).
 * The target being drawn into is sampled through a copy (s_self). The image
 * goes to SHADER_READ_ONLY, which ends an open pass when it is not there. */
static struct nvk_image s_self;       /* the copy */
static uint32_t         s_self_w, s_self_h, s_self_offset;

static VkImageView rt_opaque(RenderTarget *rt)
{
    if (!rt->opaque) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = rt->im.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = RT_FORMAT;
        vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &rt->opaque) != VK_SUCCESS)
            rt->opaque = VK_NULL_HANDLE;
    }
    return rt->opaque;
}

/* rt's image readable by the fragment shader: outside any pass. */
static void rt_to_sampled(struct nvk_image *im)
{
    if (im->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    end_encoder();
    ensure_cmd();
    nvk_image_to(s_cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static RenderTarget *rt_texture(const Stage *t, RenderTarget *cur, VkImageView *view,
                                uint32_t *sw, uint32_t *sh)
{
    static int off = -1;
    RenderTarget *self = NULL;
    int i, r565;

    if (off < 0)
        off = recomp_env(RENV_VK_NO_RTT) != NULL;
    if (off || !t->offset || d3d8_format_dxt_block_bytes(t->color))
        return NULL;
    r565 = nv2a_tex_linear_twin(t->color) == 0x11;
    if (!r565 && nv2a_tex_texel_bytes(nv2a_tex_linear_twin(t->color)) != 4)
        return NULL;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || ((rt->addr ^ t->offset) & 0x07FFFFFFu))
            continue;
        if (r565 && (rt->pitch / rt->w != 2u || rt->w != t->width || rt->h != t->height
                     || (t->color == 0x11 ? rt->swz || t->pitch != rt->pitch : !rt->swz)))
            continue;
        if (rt_drop_if_stale(rt, "bind"))
            continue;
        if (rt == cur) {
            if (!r565)
                self = rt;
            continue;
        }
        if (r565) {
            VkImageView ov = rt_opaque(rt);
            if (!ov)
                return NULL;
            *view = ov;
        } else {
            *view = rt->im.view;
        }
        rt_to_sampled(&rt->im);
        *sw = rt->w;
        *sh = rt->h;
        return rt;
    }
    if (!self)
        return NULL;
    if (!s_self.img || s_self_w != self->w || s_self_h != self->h
            || s_self.w != self->hw || s_self.h != self->hh) {
        if (s_self.img)
            garbage_push(&s_self, VK_NULL_HANDLE, NULL);
        if (!nvk_image_new(&s_self, RT_FORMAT, self->hw, self->hh, 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
            memset(&s_self, 0, sizeof s_self);
            return NULL;
        }
        s_self_w = self->w;
        s_self_h = self->h;
    }
    {
        VkImageCopy ic;
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &self->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        nvk_image_to(s_cmd, &s_self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = self->hw;
        ic.extent.height = self->hh;
        ic.extent.depth = 1;
        vkCmdCopyImage(s_cmd, self->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_self.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        nvk_image_to(s_cmd, &s_self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    s_self_offset = self->offset;
    s_self_copies++;
    {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "surface 0x%08X sampled while drawn into:"
                    " bound as a copy\n", self->offset);
    }
    *view = s_self.view;
    *sw = self->w;
    *sh = self->h;
    return self;
}

/* ---- descriptor sets --------------------------------------------------------- */

static VkDescriptorSet texture_set(const VkImageView v[4], const VkSampler s[4])
{
    Frame *f = &s_fr[s_fi];
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int k;

    if (s_tset_last && !memcmp(v, s_tset_views, sizeof s_tset_views)
            && !memcmp(s, s_tset_smp, sizeof s_tset_smp))
        return s_tset_last;
    for (;;) {
        if (f->dp_cur < f->ndps) {
            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = f->dps[f->dp_cur];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &s_dsl[1];
            if (vkAllocateDescriptorSets(s_dev, &ai, &set) == VK_SUCCESS)
                break;
            f->dp_cur++;
            continue;
        }
        {
            VkDescriptorPoolSize ps;
            VkDescriptorPoolCreateInfo pi;
            VkDescriptorPool *n = (VkDescriptorPool *)realloc(f->dps, (f->ndps + 1) * sizeof *n);
            if (!n)
                return VK_NULL_HANDLE;
            f->dps = n;
            ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ps.descriptorCount = 4096 * 4;
            memset(&pi, 0, sizeof pi);
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = 4096;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(s_dev, &pi, NULL, &f->dps[f->ndps]) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            f->ndps++;
        }
    }
    for (k = 0; k < 4; k++) {
        ii[k].sampler = s[k];
        ii[k].imageView = v[k];
        ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    memcpy(s_tset_views, v, sizeof s_tset_views);
    memcpy(s_tset_smp, s, sizeof s_tset_smp);
    s_tset_last = set;
    return set;
}

/* One constant block into the uniform ring, unless it is the last placed. */
static int ubo_place(int which, const void *p, uint32_t n, VkDeviceSize *off)
{
    uint8_t *memo = which == 0 ? s_umemo.vs : which == 1 ? s_umemo.vp : s_umemo.ps;
    void *dst;

    if (s_umemo.valid[which] && !memcmp(memo, p, n)) {
        *off = s_umemo.off[which];
        return 1;
    }
    dst = ring_alloc(RING_UB, n, s_ubo_align, off);
    if (!dst)
        return 0;
    memcpy(dst, p, n);
    memcpy(memo, p, n);
    s_umemo.off[which] = *off;
    s_umemo.valid[which] = 1;
    return 1;
}

/* ---- draws ------------------------------------------------------------------- */

/* Metal's draw_scissor: the surface clip, clamped to rt, in host pixels
 * (xemu scissors every draw to it, pgraph/gl/draw.c). */
static int draw_scissor(const struct nv2a_pb_gpu *g, const RenderTarget *rt, VkRect2D *sr)
{
    uint32_t x0 = g->clip_x, y0 = g->clip_y;
    uint32_t x1 = g->clip_x + g->clip_w, y1 = g->clip_y + g->clip_h;

    if (x1 > rt->w) x1 = rt->w;
    if (y1 > rt->h) y1 = rt->h;
    if (x0 >= x1 || y0 >= y1)
        return 0;
    sr->offset.x = (int32_t)((uint64_t)x0 * rt->hw / rt->w);
    sr->offset.y = (int32_t)((uint64_t)y0 * rt->hh / rt->h);
    sr->extent.width = (uint32_t)((uint64_t)x1 * rt->hw / rt->w) - (uint32_t)sr->offset.x;
    sr->extent.height = (uint32_t)((uint64_t)y1 * rt->hh / rt->h) - (uint32_t)sr->offset.y;
    return 1;
}

static void vk_on_draw(const struct nv2a_pb_draw *d)
{
    static uint32_t *idx;
    static uint32_t idx_cap;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    enum nv2a_topology topo;
    RenderTarget *rt;
    VkPipeline pso;
    VkShaderModule vmod, fmod;
    struct nv2a_vp_consts vc;
    VkPsConsts pc;
    VkDeviceSize vb_off = 0, ib_off = 0, uo[3] = {0, 0, 0}, vbytes;
    uint32_t lo = 0xFFFF, hi = 0, count, nidx, i, k, nf4 = 0;
    uint32_t dyn_off[3];
    int slot[NV2A_VS_MAX_INPUTS];
    float *vf;
    void *ip;
    int cull = 0, cull_all = 0, tries;
    int use_s = v->stencil_enable && ((g->format >> 4) & 0xF) == 2;
    int use_z = v->depth_enable || use_s;
    VkImageView sview[4];
    VkSampler   ssmp[4];
    VkDescriptorSet tset;

    if (!init())
        return;
    if (g->color_keep == 0xFFFFFFFFu && !(v->depth_enable && v->depth_mask) && !use_s
            && !s_occ_q)
        return;
    if (!d->idx_count) {
        s_draws_skipped++;
        return;
    }
    if (d->program) {
        VsEntry *e = vs_cache();
        s_draws_prog++;
        if (!e) {
            s_draws_skipped++;
            return;
        }
        vmod = e->mod;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++)
            if (e->inputs & (1u << i))
                slot[nf4++] = (int)i;
    } else {
        vmod = s_vs_pass;
        for (k = 0; k < VTX_F4; k++)
            slot[nf4++] = s_vtx_attr[k];
    }
    rt = surface();
    if (!rt) {
        s_draws_skipped++;
        return;
    }
    build_stages();
    nv2a_ps_consts_fill(v, s_stage, &pc.b);
    for (k = 0; k < 4; k++) {
        const Stage *t = &s_stage[k];
        float b = t->levels > 1 && nv2a_tex_filter_mip(t->filter)
                ? nv2a_tex_lod_bias(t->filter) : 0.0f;
        pc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
    }
    fmod = fs_select(v);
    for (i = 0; i < d->idx_count; i++) {
        if (d->idx[i] < lo) lo = d->idx[i];
        if (d->idx[i] > hi) hi = d->idx[i];
    }
    count = hi - lo + 1;
    if (d->idx_count * 3u > idx_cap) {
        uint32_t *grown = (uint32_t *)realloc(idx, (size_t)d->idx_count * 12u);
        if (!grown) {
            s_draws_skipped++;
            return;
        }
        idx = grown;
        idx_cap = d->idx_count * 3u;
    }
    nidx = nv2a_prim_to_list(d->prim, d->idx, d->idx_count, lo, idx, &topo);
    if (!nidx) {
        s_draws_skipped++;
        return;
    }
    if (v->cull_enable) {
        if (v->cull_face == 0x405)      cull = 1;   /* BACK */
        else if (v->cull_face == 0x404) cull = 2;   /* FRONT */
        else if (v->cull_face == 0x408) cull_all = 1;
    }
    if (cull_all && topo == NV2A_TOPO_TRIANGLES) {
        s_draws_culled++;
        return;
    }
    nv2a_vp_consts_fill(v, g->format, rt->w, rt->h, &vc);

    /* Depth: the zeta surface's buffer, attached to this target's passes
     * from now on (a new pass if the open one lacks it). */
    if (use_z) {
        DepthTarget *dt = depth_target(rt->w, rt->h);
        if (dt && rt->dt != dt) {
            rt->dt = dt;
            if (s_enc_rt == rt)
                end_encoder();
        }
    }
    /* Textures before the pass: a self copy or a layout change is outside
     * one. */
    {
        int textured = 0;
        for (k = 0; k < 4; k++) {
            const Stage *t = &s_stage[k];
            uint32_t sw, sh;
            sview[k] = s_black.view;
            ssmp[k] = sampler(t);
            if (!t->mode || t->mode == 4)
                continue;
            textured = 1;
            if (rt_texture(t, rt, &sview[k], &sw, &sh) != NULL) {
                if (!nv2a_tex_size_from_format(t->color)) {
                    pc.b.tex_scale[k][0] = 1.0f / (float)sw;
                    pc.b.tex_scale[k][1] = 1.0f / (float)sh;
                }
                s_rtt_binds++;
            } else {
                sview[k] = tex_bind(t);
            }
        }
        if (textured)
            s_draws_tex++;
    }

    /* Rings: an overflow flushes (ending the pass and resetting every
     * ring), so on a flush the placement starts over. */
    vbytes = (VkDeviceSize)count * nf4 * 16u;
    if (vbytes < 16)
        vbytes = 16;
    for (tries = 0; tries < 2; tries++) {
        uint64_t fl = s_flushes;
        vf = (float *)ring_alloc(RING_VB, vbytes, 16, &vb_off);
        ip = vf ? ring_alloc(RING_IB, (VkDeviceSize)nidx * 4u, 16, &ib_off) : NULL;
        if (vf && ip && d->program && !ubo_place(0, v->c, VS_C_BYTES, &uo[0]))
            ip = NULL;
        if (vf && ip && !ubo_place(1, &vc, VP_C_BYTES, &uo[1]))
            ip = NULL;
        if (vf && ip && !ubo_place(2, &pc, PS_C_BYTES, &uo[2]))
            ip = NULL;
        if (fl == s_flushes)
            break;
    }
    if (!vf || !ip) {
        s_draws_skipped++;
        return;
    }
    if (!d->program)
        uo[0] = s_umemo.valid[0] ? s_umemo.off[0] : 0;
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
    tset = texture_set(sview, ssmp);
    if (!tset) {
        s_draws_skipped++;
        return;
    }

    if (!s_pass || s_enc_rt != rt)
        begin_encoder(rt, 0, NULL);
    pso = pipeline(vmod, fmod, nf4, topo);
    if (!pso) {
        s_draws_skipped++;
        return;
    }
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    dyn_off[0] = (uint32_t)uo[0];
    dyn_off[1] = (uint32_t)uo[1];
    dyn_off[2] = (uint32_t)uo[2];
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1,
                            &s_set0, 3, dyn_off);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1,
                            &tset, 0, NULL);
    {
        /* Depth and stencil only with an attachment and when enabled; the
         * compare and op codes are in VkCompareOp / VkStencilOp order. */
        int z = s_enc_dt && use_z && v->depth_enable;
        int s = s_enc_dt && use_s;
        vkCmdSetDepthTestEnable(s_cmd, z ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(s_cmd, z && v->depth_mask ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(s_cmd, z ? (VkCompareOp)nv2a_cmp_from_gl(v->depth_func)
                                        : VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilTestEnable(s_cmd, s ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                          (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[0]),
                          (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[2]),
                          (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[1]),
                          (VkCompareOp)(v->stencil_func & 7u));
        vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, v->stencil_rmask & 0xFFu);
        vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, v->stencil_wmask & 0xFFu);
        vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                 s ? (v->stencil_ref & 0xFFu) : 0u);
        if (s_enc_dt && use_z)
            s_draws_z++;
    }
    {
        /* A negative height: D3D's y-up clip space onto rows from the top,
         * as Metal's viewport. */
        VkViewport vp;
        VkRect2D sr;
        if (!draw_scissor(g, rt, &sr)) {
            s_draws_skipped++;
            return;
        }
        vp.x = 0.0f;
        vp.y = (float)rt->hh;
        vp.width = (float)rt->hw;
        vp.height = -(float)rt->hh;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(s_cmd, 0, 1, &vp);
        vkCmdSetScissor(s_cmd, 0, 1, &sr);
    }
    vkCmdSetFrontFace(s_cmd, v->front_face != 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                    : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetCullMode(s_cmd, cull == 1 ? VK_CULL_MODE_BACK_BIT
                          : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    {
        float bf[4];
        (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                  g->blend_dfactor, g->blend_color, bf, &(int){0});
        vkCmdSetBlendConstants(s_cmd, bf);
    }
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring[RING_VB].buf, &vb_off);
    vkCmdBindIndexBuffer(s_cmd, s_ring[RING_IB].buf, ib_off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(s_cmd, nidx, 1, 0, 0, 0);
    s_draws++;
}

/* ---- flip ---------------------------------------------------------------------- */

static RenderTarget *present_target(uint32_t surface_offset)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    int i;

    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && surface_offset && rt->offset == surface_offset
                && (!p->offset || (rt->w >= p->w && rt->h >= p->h))) {
            rt->presented = 1;
            return rt;
        }
    }
    return NULL;
}

static void present_extent(const RenderTarget *rt, uint32_t *w, uint32_t *h)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    *w = rt->w;
    *h = rt->h;
    if (p->offset && p->offset == rt->offset && p->w && p->h) {
        if (p->w < *w) *w = p->w;
        if (p->h < *h) *h = p->h;
    }
}

static uint32_t s_shown_w, s_shown_h;

/* rt's host pixels read back: a copy into the readback buffer, a submit and
 * a wait. hw x hh BGRA8, tightly packed; valid until the next readback. The
 * ring positions survive the wait, since a draw may have placed its
 * vertices already (tex_bind runs inside on_draw). */
static struct nvk_buffer s_rb;

static const uint8_t *host_readback(RenderTarget *rt)
{
    VkDeviceSize need = (VkDeviceSize)rt->hw * rt->hh * 4u;
    VkDeviceSize pos[RING_N];
    VkBufferImageCopy bc;
    int k;

    if (s_rb.size < need) {
        flush(1);
        nvk_buffer_free(&s_rb);
        if (!nvk_buffer_new(&s_rb, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            return NULL;
    }
    end_encoder();
    ensure_cmd();
    nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = rt->hw;
    bc.imageExtent.height = rt->hh;
    bc.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, rt->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_rb.buf, 1, &bc);
    {
        /* The copy's writes visible to the host. */
        VkMemoryBarrier2 mb;
        VkDependencyInfo di;
        memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(s_cmd, &di);
    }
    for (k = 0; k < RING_N; k++)
        pos[k] = s_ring_pos[k];
    flush(1);
    for (k = 0; k < RING_N; k++)
        s_ring_pos[k] = pos[k];
    return (const uint8_t *)s_rb.map;
}

/* The host pixels (hw x hh) into the guest surface at addr: 32-bit as they
 * are, 16-bit as R5G6B5; a scaled target box-filtered to guest size. */
static void write_back_px(RenderTarget *rt, const uint8_t *px, uint32_t addr, uint32_t pitch)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + addr;
    uint32_t bpp = pitch / rt->w, w = rt->w, n = rt->hw / rt->w, y, x;
    static uint32_t *row;
    static uint32_t row_cap;

    if (bpp >= 4) {
        if (n == 1)
            for (y = 0; y < rt->h; y++)
                memcpy(dst + (size_t)y * pitch, px + (size_t)y * w * 4u, (size_t)w * 4u);
        else
            nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, dst, pitch);
    } else if (bpp == 2) {
        if (row_cap < w) {
            uint32_t *nr = (uint32_t *)realloc(row, (size_t)w * 4);
            if (!nr)
                return;
            row = nr;
            row_cap = w;
        }
        for (y = 0; y < rt->h; y++) {
            uint16_t *o = (uint16_t *)(dst + (size_t)y * pitch);
            const uint32_t *in;
            if (n == 1) {
                in = (const uint32_t *)(px + (size_t)y * w * 4u);
            } else {
                nv2a_downscale_box32(px + (size_t)y * n * rt->hw * 4u, rt->hw * 4u, w, 1, n,
                                     (uint8_t *)row, w * 4u);
                in = row;
            }
            for (x = 0; x < w; x++) {
                uint32_t c = in[x];
                o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                  | ((c >> 3) & 0x001F));
            }
        }
    } else {
        return;
    }
    s_writebacks++;
}

/* rt into guest memory now, if drawn since its last write-back (Metal's
 * rt_sync): a GPU wait. */
static void rt_sync(RenderTarget *rt, uint32_t pitch)
{
    const uint8_t *px;

    if (!rt->im.img || !rt->dirty || !pitch || pitch < rt->w * 2u)
        return;
    if ((px = host_readback(rt)) != NULL)
        write_back_px(rt, px, rt->addr, pitch);
    rt->dirty = 0;
    rt_own_reset(rt);
}

static void vk_sync_guest(uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h)
{
    RenderTarget *rt = NULL;
    uint32_t addr = nv2a_pb_dma_resolve(offset) & 0x07FFFFFFu;
    int i;

    if (s_failed)
        return;
    for (i = 0; i < RT_MAX && !rt; i++)
        if (s_rt[i].im.img && s_rt[i].offset == offset && s_rt[i].w >= w && s_rt[i].h >= h)
            rt = &s_rt[i];
    if (!rt) {
        for (i = 0; i < RT_MAX; i++) {
            RenderTarget *c = &s_rt[i];
            if (c->im.img && (c->addr & 0x07FFFFFFu) == addr
                    && (!rt || c->last_use > rt->last_use))
                rt = c;
        }
    }
    if (rt && !rt_drop_if_stale(rt, "sync"))
        rt_sync(rt, pitch);
}

/* For tests/nv2a_backend_smoke, the shapes of the Metal ones. */
uint64_t nv2a_pb_vulkan_writebacks(void) { return s_writebacks; }

int nv2a_pb_vulkan_shown(uint32_t offset, uint32_t *tw, uint32_t *th,
                         uint32_t *sw, uint32_t *sh)
{
    int i;
    for (i = 0; i < RT_MAX; i++)
        if (s_rt[i].im.img && s_rt[i].offset == offset) {
            *tw = s_rt[i].w;
            *th = s_rt[i].h;
            *sw = s_shown_w;
            *sh = s_shown_h;
            return 1;
        }
    return 0;
}

const uint8_t *nv2a_pb_vulkan_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && rt->offset == offset) {
            *hw = rt->hw;
            *hh = rt->hh;
            return host_readback(rt);
        }
    }
    return NULL;
}

/* ---- visibility tests: occlusion queries -------------------------------------
 *
 * Metal's scheme (see "Visibility tests" in nv2a_pb_metal.m) with Vulkan
 * queries in place of visibility-result slots: a query is begun inside a
 * render pass and must end in it, so a stretch that spans passes takes a slot
 * per pass; slots are reset from the host when taken (hostQueryReset) and
 * read once the submit they were recorded in is done. GET_REPORT submits the
 * open command buffer when the report counts in it, so the count can land
 * within the frame. vk_occ=sync waits at GET_REPORT; =fixed leaves reports
 * to the walker (every test visible). */
static OccQuery          s_qpool[OCC_POOL];
static OccQuery         *s_qfree;
static int               s_qmade;
static unsigned char     s_vis_used[VIS_SLOTS];
static int               s_vis_next;
static int               s_retire[VIS_SLOTS];
static uint64_t          s_retire_serial[VIS_SLOTS];
static int               s_nretire;
static struct nv2a_occ   s_occ;
static int               s_occ_mode = -1;
static uint64_t          s_occ_ovf, s_report_commits;

static int occ_mode(void)
{
    if (s_occ_mode < 0) {
        const char *e = recomp_env(RENV_VK_OCC);
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
            vkResetQueryPool(s_dev, s_vkq, (uint32_t)i, 1);
            return i;
        }
    }
    return -1;
}

static void occ_query_begin(void)
{
    OccQuery *q = s_occ_q;
    int i;
    if (!q || !s_pass || s_occ_slot >= 0)
        return;
    if (q->n >= OCC_QSLOTS || (i = vis_alloc()) < 0) {
        q->ovf = 1;
        s_occ_ovf++;
        return;
    }
    q->slot[q->n] = i;
    q->serial[q->n] = s_next_serial;
    q->n++;
    vkCmdBeginQuery(s_cmd, s_vkq, (uint32_t)i, s_qprecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_occ_slot = i;
}

static void occ_pass_begin(void)
{
    occ_query_begin();
}

static void occ_pass_end(void)
{
    if (s_occ_slot >= 0 && s_pass)
        vkCmdEndQuery(s_cmd, s_vkq, (uint32_t)s_occ_slot);
    s_occ_slot = -1;
}

static uint64_t occ_now_us(void *ctx)
{
    (void)ctx;
    return now_ns() / 1000u;
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

static void occ_put(void *ctx, void *qv)
{
    OccQuery *q = (OccQuery *)qv;
    uint64_t done = serial_done();
    int i;
    (void)ctx;
    if (!q)
        return;
    if (q == s_occ_q) {
        occ_pass_end();
        s_occ_q = NULL;
    }
    for (i = 0; i < q->n; i++) {
        if (q->serial[i] <= done) {
            s_vis_used[q->slot[i]] = 0;
        } else {
            s_retire[s_nretire] = q->slot[i];
            s_retire_serial[s_nretire++] = q->serial[i];
        }
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
    occ_query_begin();
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
    uint64_t sum = 0, done;
    int i;
    (void)ctx;
    for (i = 0; i < q->n; i++) {
        if (q->serial[i] == s_next_serial) {     /* not submitted yet */
            if (!flush_gpu)
                return 0;
            flush(0);
        }
    }
    done = serial_done();
    for (i = 0; i < q->n; i++)
        if (q->serial[i] > done)
            return 0;
    for (i = 0; i < q->n; i++) {
        uint64_t v = 0;
        if (vkGetQueryPoolResults(s_dev, s_vkq, (uint32_t)q->slot[i], 1, sizeof v, &v,
                                  sizeof v, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            sum += v;
        else
            return 0;
    }
    *count = q->ovf ? NV2A_OCC_VISIBLE : sum;
    return 1;
}

static const struct nv2a_occ_ops s_occ_ops = {
    occ_get, occ_put, occ_begin, occ_end, occ_result, occ_now_us, NULL
};

static int occ_ready_on(void)
{
    if (occ_mode() == 2 || !init() || !s_vkq)
        return 0;
    if (!s_occ.ops)
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
    return 1;
}

static void vk_on_zpass(int enable)
{
    if (occ_ready_on())
        nv2a_occ_zpass(&s_occ, enable);
}

static void vk_on_zpass_clear(void)
{
    if (occ_ready_on())
        nv2a_occ_zpass_clear(&s_occ);
}

static int report_in_cmd(uint32_t va)
{
    const struct nv2a_occ_report *p;
    int k, i;
    if (!s_occ.npend || s_occ.pend[s_occ.npend - 1].va != va)
        return 0;
    p = &s_occ.pend[s_occ.npend - 1];
    for (k = 0; k < p->n; k++) {
        const OccQuery *q = (const OccQuery *)p->q[k];
        for (i = 0; i < q->n; i++)
            if (q->serial[i] == s_next_serial)
                return 1;
    }
    return 0;
}

static int vk_on_report(uint32_t va)
{
    if (!occ_ready_on())
        return 0;
    nv2a_occ_report(&s_occ, va, occ_mode() == 1);
    if (occ_mode() == 0 && (s_cmd || s_up) && report_in_cmd(va)) {
        s_report_commits++;
        flush(0);
    }
    return 1;
}

static void vk_on_poll(void)
{
    if (s_occ.npend)
        nv2a_occ_poll(&s_occ, 1);
}

/* Retired slots whose submit is done are free again. */
static void occ_after_wait(void)
{
    uint64_t done = serial_done();
    int i = 0;
    while (i < s_nretire) {
        if (s_retire_serial[i] <= done) {
            s_vis_used[s_retire[i]] = 0;
            s_retire[i] = s_retire[--s_nretire];
            s_retire_serial[i] = s_retire_serial[s_nretire];
        } else {
            i++;
        }
    }
    if (s_occ.ops)
        nv2a_occ_poll(&s_occ, 0);
}

/* ---- frames ------------------------------------------------------------------- */

/* The flip's frame advance: the frame just recorded is submitted, and the
 * next one is reused once its last submit (s_frames flips back) is done. */
static void frame_advance(void)
{
    Frame *f;
    uint32_t k;
    uint64_t t0;

    s_fi = (s_fi + 1) % s_frames;
    f = &s_fr[s_fi];
    if (f->serial && serial_done() < f->serial) {
        t0 = now_ns();
        serial_wait(f->serial);
        s_frame_waits++;
        s_frame_wait_ns += now_ns() - t0;
    }
    vkResetCommandPool(s_dev, f->pool, 0);
    f->used = 0;
    for (k = 0; k < f->ndps; k++)
        vkResetDescriptorPool(s_dev, f->dps[k], 0);
    f->dp_cur = 0;
    s_tset_last = VK_NULL_HANDLE;
    rings_reset();
    garbage_collect();
    occ_after_wait();
}

/* ---- caches on disk ------------------------------------------------------------- */

static void pcache_path(char *p, size_t n, int *ok)
{
    char dir[1024];
    *ok = nvk_cache_dir(dir, sizeof dir)
          && snprintf(p, n, "%s/vk_pipeline_%04X_%04X.bin", dir, C->props.vendorID,
                      C->props.deviceID) < (int)n;
}

static void pcache_save(void)
{
    char path[1100], tmp[1200];
    size_t n = 0;
    void *data;
    FILE *f;
    int ok;

    if (!s_pcache || !s_pso_since_save)
        return;
    pcache_path(path, sizeof path, &ok);
    if (!ok || vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    data = malloc(n);
    if (!data)
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, data) == VK_SUCCESS) {
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if ((f = fopen(tmp, "wb")) != NULL) {
            int good = fwrite(data, 1, n, f) == n;
            if (fclose(f) == 0 && good && rename(tmp, path) == 0) {
                s_pso_since_save = 0;
                if (vk_trace())
                    fprintf(stderr, LOGP "pipeline cache: %zu bytes to %s\n", n, path);
            } else {
                remove(tmp);
            }
        }
    }
    free(data);
}

static void at_exit_save(void)
{
    if (C && !s_failed)
        pcache_save();
}

static VkPipelineCache pcache_load(void)
{
    char path[1100];
    VkPipelineCacheCreateInfo ci;
    VkPipelineCache pc = VK_NULL_HANDLE;
    void *data = NULL;
    size_t n = 0;
    FILE *f;
    int ok;

    pcache_path(path, sizeof path, &ok);
    if (ok && (f = fopen(path, "rb")) != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)sz)) != NULL) {
                n = fread(data, 1, (size_t)sz, f);
                if (n != (size_t)sz)
                    n = 0;
            }
        }
        fclose(f);
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = n;
    ci.pInitialData = n ? data : NULL;
    /* The driver validates the header (vendor, device, cache UUID) and
     * starts empty when it does not match. */
    if (vkCreatePipelineCache(s_dev, &ci, NULL, &pc) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(s_dev, &ci, NULL, &pc);
        n = 0;
    }
    free(data);
    fprintf(stderr, LOGP "pipeline cache: %s (%zu bytes)\n", n ? path : "empty", n);
    return pc;
}

/* ---- init ---------------------------------------------------------------------- */

static int init(void)
{
    static int tried;
    char err[256] = "";
    VkSemaphoreTypeCreateInfo st;
    VkSemaphoreCreateInfo sci;
    VkDescriptorSetLayoutBinding b0[3], b1[4];
    VkDescriptorSetLayoutCreateInfo dl;
    VkPipelineLayoutCreateInfo pl;
    int i;

    if (tried)
        return !s_failed;
    tried = 1;
    s_failed = 1;

    s_win = xbox_FramebufferWindowVulkan();
    C = nvk_core_get();
    if (!C)
        C = nvk_core_init(NULL, 0, NULL, NULL, err, sizeof err);
    if (!C) {
        fprintf(stderr, LOGP "no Vulkan device: %s\n", err);
        return 0;
    }
    if (!s_win && C->surface) {
        /* The window made the device but takes no slots (readback). */
        s_win = 0;
    }
    s_dev = C->dev;
    s_max_dim = C->props.limits.maxImageDimension2D;
    if (s_max_dim > 16384u)
        s_max_dim = 16384u;
    s_ubo_align = C->props.limits.minUniformBufferOffsetAlignment;
    if (s_ubo_align < 16)
        s_ubo_align = 16;
    {
        const char *e = recomp_env(RENV_VK_FRAMES);
        s_frames = (e && atoi(e) == 1) ? 1 : FRAMES_MAX;
    }

    memset(&st, 0, sizeof st);
    st.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    st.initialValue = 0;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &st;
    if (vkCreateSemaphore(s_dev, &sci, NULL, &s_timeline) != VK_SUCCESS) {
        fprintf(stderr, LOGP "timeline semaphore failed\n");
        return 0;
    }
    for (i = 0; i < s_frames; i++) {
        VkCommandPoolCreateInfo cp;
        memset(&cp, 0, sizeof cp);
        cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cp.queueFamilyIndex = C->qfam;
        if (vkCreateCommandPool(s_dev, &cp, NULL, &s_fr[i].pool) != VK_SUCCESS) {
            fprintf(stderr, LOGP "command pool failed\n");
            return 0;
        }
    }

    /* Rings, per frame: vertices as Metal's (16 MB), indices for the
     * largest batch the walker accepts, uniforms for some thousands of
     * draws, staging for a frame's uploads. */
    s_ring_size[RING_VB] = 16u << 20;
    s_ring_size[RING_IB] = (VkDeviceSize)NV_MAX_INDICES * 3u * 4u;
    s_ring_size[RING_UB] = 16u << 20;
    s_ring_size[RING_UP] = 48u << 20;
    {
        static const VkBufferUsageFlags use[RING_N] = {
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        for (i = 0; i < RING_N; i++)
            if (!nvk_buffer_new(&s_ring[i], s_ring_size[i] * s_frames, use[i],
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                i == RING_UP ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                fprintf(stderr, LOGP "ring buffer %d failed\n", i);
                return 0;
            }
    }

    /* Set 0: the three constant blocks, dynamic offsets into the uniform
     * ring. Set 1: four combined image samplers. */
    memset(b0, 0, sizeof b0);
    for (i = 0; i < 3; i++) {
        b0[i].binding = (uint32_t)i;
        b0[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = i == 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    }
    memset(b1, 0, sizeof b1);
    for (i = 0; i < 4; i++) {
        b1[i].binding = (uint32_t)i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = 1;
        b1[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dl, 0, sizeof dl);
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 3;
    dl.pBindings = b0;
    if (vkCreateDescriptorSetLayout(s_dev, &dl, NULL, &s_dsl[0]) != VK_SUCCESS)
        return 0;
    dl.bindingCount = 4;
    dl.pBindings = b1;
    if (vkCreateDescriptorSetLayout(s_dev, &dl, NULL, &s_dsl[1]) != VK_SUCCESS)
        return 0;
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = s_dsl;
    if (vkCreatePipelineLayout(s_dev, &pl, NULL, &s_playout) != VK_SUCCESS)
        return 0;
    {
        VkDescriptorPoolSize ps;
        VkDescriptorPoolCreateInfo pi;
        VkDescriptorSetAllocateInfo ai;
        VkDescriptorBufferInfo bi[3];
        VkWriteDescriptorSet w[3];
        static const uint32_t range[3] = { VS_C_BYTES, sizeof(struct nv2a_vp_consts),
                                           sizeof(VkPsConsts) };
        ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ps.descriptorCount = 3;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s_dev, &pi, NULL, &s_set0_pool) != VK_SUCCESS)
            return 0;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = s_set0_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s_dsl[0];
        if (vkAllocateDescriptorSets(s_dev, &ai, &s_set0) != VK_SUCCESS)
            return 0;
        for (i = 0; i < 3; i++) {
            bi[i].buffer = s_ring[RING_UB].buf;
            bi[i].offset = 0;
            bi[i].range = range[i];
            memset(&w[i], 0, sizeof w[i]);
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_set0;
            w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(s_dev, 3, w, 0, NULL);
    }
    {
        /* Occlusion queries, reset from the host (hostQueryReset). */
        VkPhysicalDeviceFeatures f;
        VkPhysicalDeviceVulkan12Features f12;
        VkPhysicalDeviceFeatures2 f2;
        memset(&f12, 0, sizeof f12);
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        memset(&f2, 0, sizeof f2);
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(C->phys, &f2);
        f = f2.features;
        s_qprecise = f.occlusionQueryPrecise ? 1 : 0;
        if (f12.hostQueryReset) {
            VkQueryPoolCreateInfo qi;
            memset(&qi, 0, sizeof qi);
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_OCCLUSION;
            qi.queryCount = VIS_SLOTS;
            if (vkCreateQueryPool(s_dev, &qi, NULL, &s_vkq) != VK_SUCCESS)
                s_vkq = VK_NULL_HANDLE;
        }
    }
    s_pso = calloc(PSO_CAP, sizeof *s_pso);
    if (!s_pso)
        return 0;
    s_pcache = pcache_load();
    s_vs_pass = module_from_glsl(NVK_STAGE_VERTEX, s_vs_pass_glsl, "vs_pass");
    s_fs_basic = module_from_glsl(NVK_STAGE_FRAGMENT, s_fs_basic_glsl, "fs_basic");
    if (!s_vs_pass || !s_fs_basic) {
        fprintf(stderr, LOGP "base shaders failed\n");
        return 0;
    }
    if (!solid_texture(&s_black, 0xFF000000u) || !solid_texture(&s_magenta, NV2A_TEX_MAGENTA)) {
        fprintf(stderr, LOGP "base textures failed\n");
        return 0;
    }
    atexit(at_exit_save);
    fprintf(stderr, LOGP "backend up on %s: %d frames in flight, present %s, write-back %s,"
            " occlusion %s%s\n", C->name, s_frames,
            s_win ? "window slots" : C->surface ? "texture readback" : "headless",
            wb_always() ? "every flip" : "on demand",
            s_vkq ? "queries" : "fixed", s_qprecise ? " (precise)" : "");
    s_failed = 0;
    return 1;
}

/* ---- flip ----------------------------------------------------------------------- */

static void flip_log(uint32_t surface_offset, RenderTarget *rt)
{
    static int on = -1;
    static uint64_t p_draws, p_prog, p_skip, p_cull, p_clears, p_tex, p_up,
                    p_chg, p_re, p_fl, p_occ, p_vis, p_late, p_super, p_sync, p_ovf;
    if (on < 0)
        on = recomp_env(RENV_FLIP_LOG) != NULL;
    if (!on)
        return;
    fprintf(stderr, "[VULKAN] flip %llu batches %llu prog %llu skipped %llu"
            " culled %llu clears %llu tex %llu uploads %llu changed %llu rehash %llu"
            " flushes %llu occ %llu (visible %llu, pending %d, late %llu, superseded %llu,"
            " sync %llu us, slot overflow %llu) present 0x%08X walker 0x%08X shown %ux%u\n",
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
            rt ? rt->offset : 0, surface_offset, rt ? s_shown_w : 0, rt ? s_shown_h : 0);
    p_occ = s_occ.reports; p_vis = s_occ.vis; p_late = s_occ.late;
    p_super = s_occ.super; p_sync = s_occ.sync_us; p_ovf = s_occ_ovf;
    p_draws = s_draws; p_prog = s_draws_prog; p_skip = s_draws_skipped;
    p_cull = s_draws_culled; p_clears = s_clears; p_tex = s_draws_tex;
    p_up = s_tex_uploads; p_chg = s_tex_changed; p_re = s_tex_rehash;
    p_fl = s_flushes;
}

/* RECOMP_TRACE=vk_prof: on_flip's cost every 300 flips. */
static struct { uint64_t n, flip, wait, wb, blit, waits; } s_pa;

static void vk_on_flip(uint32_t surface_offset, uint32_t pitch)
{
    static int prof = -1;
    RenderTarget *rt = NULL;
    uint32_t addr = surface_offset ? nv2a_pb_dma_resolve(surface_offset) : 0;
    const uint8_t *hpx = NULL;
    uint64_t t_flip0 = 0, t;
    int always = wb_always(), win = 0, layer = 0;
    uint32_t cw = 0, ch = 0, hcw = 0, hch = 0;

    if (prof < 0)
        prof = recomp_env(RENV_VK_PROF) != NULL;
    if (prof)
        t_flip0 = now_ns();
    if (init()) {
        int wb;
        rt = present_target(surface_offset);
        if (rt) {
            present_extent(rt, &cw, &ch);
            hcw = (uint32_t)((uint64_t)cw * rt->hw / rt->w);
            hch = (uint32_t)((uint64_t)ch * rt->hh / rt->h);
            s_shown_w = cw;
            s_shown_h = ch;
        }
        win = rt && xbox_FramebufferWindowRunning();
        layer = win && s_win;
        wb = rt && addr && pitch && pitch >= rt->w * 2u && always;
        if (layer) {
            /* The frame into the window's back slot on the GPU, in this
             * frame's command buffer; published once submitted, so the
             * main thread's present that shows it comes after it on the
             * queue. */
            struct nvk_image *slot;
            t = prof ? now_ns() : 0;
            slot = (struct nvk_image *)xbox_FramebufferWindowBackTexture(hcw, hch);
            if (slot) {
                VkImageCopy ic;
                end_encoder();
                ensure_cmd();
                nvk_image_to(s_cmd, &rt->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                /* GENERAL to GENERAL: after the last present that read it. */
                nvk_image_barrier(s_cmd, slot->img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  slot->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                  ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_GENERAL);
                slot->layout = VK_IMAGE_LAYOUT_GENERAL;
                memset(&ic, 0, sizeof ic);
                ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.srcSubresource.layerCount = 1;
                ic.dstSubresource = ic.srcSubresource;
                ic.extent.width = hcw;
                ic.extent.height = hch;
                ic.extent.depth = 1;
                vkCmdCopyImage(s_cmd, rt->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                flush(0);
                xbox_FramebufferWindowPublishTexture();
            }
            if (prof)
                s_pa.blit += now_ns() - t;
        }
        if (rt && (wb || (win && !layer)))
            hpx = host_readback(rt);
        if (wb && hpx) {
            t = prof ? now_ns() : 0;
            write_back_px(rt, hpx, addr, pitch);
            rt->dirty = 0;
            if (addr == rt->addr && pitch == rt->pitch)
                rt_own_reset(rt);
            if (prof)
                s_pa.wb += now_ns() - t;
        }
        /* Submit the frame and move on: the GPU runs it while the next
         * one is recorded. */
        flush(0);
        t = prof ? now_ns() : 0;
        {
            uint64_t w0 = s_frame_waits;
            frame_advance();
            if (prof) {
                s_pa.wait += now_ns() - t;
                s_pa.waits += s_frame_waits - w0;
            }
        }
        if (s_pso_since_save && (s_presents % 600) == 599)
            pcache_save();
    }
    s_presents++;
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0) {
        uint64_t sh = 0, sc = 0;
        nvk_glsl_stats(&sh, &sc);
        fprintf(stderr, LOGP "present #%llu: surface 0x%08X (%s), %llu clears,"
                " %llu draws (%llu vertex-program), %llu skipped, %llu culled,"
                " %llu texture uploads, %llu rehashes, %llu write-backs (%s),"
                " %llu decode syncs, %llu stale drops, %llu grows, %llu partial clears;"
                " shaders %llu vs + %llu fs (SPIR-V: %llu cached, %llu compiled),"
                " %llu pipelines, %llu compile errors;"
                " %llu z-clears, %llu depth draws, %llu rt binds (%llu self copies);"
                " %llu submits, %llu at GET_REPORT, %llu frame waits (%.2f ms avg)\n",
                (unsigned long long)s_presents, surface_offset,
                rt ? "drawn here" : "guest bytes",
                (unsigned long long)s_clears, (unsigned long long)s_draws,
                (unsigned long long)s_draws_prog, (unsigned long long)s_draws_skipped,
                (unsigned long long)s_draws_culled,
                (unsigned long long)s_tex_uploads, (unsigned long long)s_tex_rehash,
                (unsigned long long)s_writebacks, always ? "always" : "lazy",
                (unsigned long long)s_decode_syncs, (unsigned long long)s_stale_drops,
                (unsigned long long)s_grows, (unsigned long long)s_partial_clears,
                (unsigned long long)s_vs_compiled, (unsigned long long)s_fs_compiled,
                (unsigned long long)sh, (unsigned long long)sc,
                (unsigned long long)s_pso_created, (unsigned long long)s_compile_errors,
                (unsigned long long)s_zclears, (unsigned long long)s_draws_z,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies,
                (unsigned long long)s_flushes, (unsigned long long)s_report_commits,
                (unsigned long long)s_frame_waits,
                s_frame_waits ? (double)s_frame_wait_ns / (double)s_frame_waits / 1e6 : 0.0);
    }
    if (prof) {
        s_pa.flip += now_ns() - t_flip0;
        if (++s_pa.n == 300) {
            double n = (double)s_pa.n;
            fprintf(stderr, LOGP "prof %llu flips: on_flip %.3f ms = frame wait %.3f"
                    " (%llu waits) + write-back %.3f + slot copy %.3f (+rest);"
                    " %llu write-backs, %llu decode syncs total\n",
                    (unsigned long long)s_presents, s_pa.flip / n / 1e6, s_pa.wait / n / 1e6,
                    (unsigned long long)s_pa.waits, s_pa.wb / n / 1e6, s_pa.blit / n / 1e6,
                    (unsigned long long)s_writebacks, (unsigned long long)s_decode_syncs);
            memset(&s_pa, 0, sizeof s_pa);
        }
    }
    if (!pitch || !addr || layer)
        return;
    xbox_FramebufferWindowSet(addr, pitch);
    /* Readback mode (no Vulkan window): the frame's part of the host
     * pixels, read back above. A surface never drawn here goes as the CPU
     * path presents it. */
    if (rt && hpx && win) {
        xbox_FramebufferWindowPresentPixels(hpx, hcw, hch, rt->hw * 4u, 4u);
        return;
    }
    xbox_FramebufferWindowPresent(addr, pitch);
}

/* ---- registration ---------------------------------------------------------- */

static const struct nv2a_pb_backend s_vk_backend = {
    vk_on_clear,
    vk_on_zclear,
    vk_on_draw,
    vk_on_flip,
    vk_on_zpass,
    vk_on_zpass_clear,
    vk_on_report,
    vk_on_poll,
    1,                  /* writes_back: dumps read guest memory, after sync_guest */
    vk_sync_guest,
};

const struct nv2a_pb_backend *nv2a_pb_vulkan_backend(void)
{
    (void)init();
    return &s_vk_backend;
}

/* RECOMP_PB_BACKEND=vulkan routes the executor here. Called once from the
 * executor's first method, on the ack thread. */
void nv2a_pb_vulkan_register_from_env(void)
{
    const char *b = recomp_env(RENV_PB_BACKEND);
    if (b && (!strcasecmp(b, "vulkan") || !strcasecmp(b, "vk"))) {
        const struct nv2a_pb_backend *vb = nv2a_pb_vulkan_backend();
        if (s_failed) {
            fprintf(stderr, LOGP "RECOMP_PB_BACKEND=vulkan: no device, staying on"
                    " the CPU rasteriser\n");
            return;
        }
        nv2a_pb_set_backend(vb);
        fprintf(stderr, LOGP "RECOMP_PB_BACKEND=vulkan: executor draws through Vulkan\n");
    }
}
