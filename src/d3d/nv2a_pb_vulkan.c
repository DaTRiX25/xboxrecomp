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
#include <unistd.h>

#include "nv2a_vk_core.h"
#include "nv2a_vk_glsl.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "d3d8_glsl.h"
#include "d3d8_swizzle.h"
#include "nv2a_texrep.h"
#include "nv2a_texscale.h"
#include "kernel_pause.h"
static void scale_prepare_loaded(void);
static int ui_scale_log(void);
#include "nv2a_pcache.h"
#include "recomp_env.h"
#include <pthread.h>

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
    float bump_mat[4][4];   /* per stage: SET_TEXTURE_SET_BUMP_ENV_MAT 00 01 10 11 */
    float bump_lum[4][4];   /* per stage: x bump-env luminance scale, y offset */
} VkPsConsts;
_Static_assert(offsetof(VkPsConsts, tex_bias) == 416 && offsetof(VkPsConsts, bump_mat) == 432
               && offsetof(VkPsConsts, bump_lum) == 496 && sizeof(VkPsConsts) == 560,
               "VkPsConsts tail moved (D3D8_GLSL_PSCONSTS)");

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
    "                       clamp(a_pos.z * vp.c[0].z + vp.c[0].w, 0.0, 1.0), 1.0);\n"
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
    VkFence           fence;           /* vk_poll=0: signalled by the submit fence_serial */
    uint64_t          fence_serial;    /* 0: not armed */
    VkDescriptorPool *dps;
    uint32_t          ndps, dp_cur;
} Frame;
static Frame    s_fr[FRAMES_MAX];
static int      s_fi;                  /* the frame being recorded */

/* What the main command buffer has bound and set (vk_on_draw); nothing is
 * known of a new one. */
typedef struct {
    VkPipeline    pso;
    VkBool32      ztest, zwrite, stest;
    VkCompareOp   zcmp, scmp;
    VkStencilOp   sop[3];
    uint32_t      srmask, swmask, sref, vw, vh;
    float         slope, bf[4];
    VkRect2D      sr;
    VkFrontFace   front;
    VkCullModeFlags cull;
} DynState;
static DynState s_dyn;
static int      s_dyn_valid;

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

/* ---- RECOMP_TRACE=vk_frame=<file> -------------------------------------------
 *
 * One CSV line per flip, to say what a frame cost the backend beyond its
 * draws: how many queue submits and why, how many render passes and how many
 * of them went back to a target the frame had already drawn (a tile-based
 * GPU stores and loads the whole target for each), and where the backend
 * called into the driver's waits. The frame's GPU time comes from a pair of
 * timestamp queries round each submit, read four flips later, so a line is
 * written that much after its frame. Off, each site is one load and a
 * branch (s_vf is NULL). */
enum { FL_FLIP, FL_READY, FL_REPORT, FL_POLL, FL_RING, FL_SAMPLER, FL_READBACK, FL_BATCH, FL_N };
#define VF_RING 8
#define VF_LAG  4
#define VF_TS   24                     /* submits a frame with a timestamp pair */
typedef struct {
    uint64_t present, t_ns;
    uint32_t flip, draws, passes, p_clear, p_load, p_resumed, p_empty, p_maxdraws,
             rt_switch, zload, zresumed, pass_draws;
    uint32_t sub[FL_N];
    uint32_t polls, waits, qres, nts, ready_done;
    uint64_t submit_ns, submit_max_ns, qlock_ns, poll_ns, poll_max_ns, wait_ns,
             qres_ns, endpass_ns, f_total, f_back, f_copy, f_submit, f_pub, f_adv;
} VkFrameRec;
static VkFrameRec   s_vfr[VF_RING];
static VkFrameRec  *s_vf;              /* the frame being recorded; NULL: off */
static FILE        *s_vf_out;
static VkQueryPool  s_vf_qp;
static int          s_vf_ts_open;      /* s_cmd has its start timestamp */
static void        *s_vf_last_rt;

/* ---- serials and garbage ---------------------------------------------------- */

/* How far the GPU is. With a timeline semaphore of the driver's own that is
 * one cheap question (vkGetSemaphoreCounterValue), asked wherever it helps.
 * Mesa's Turnip on Qualcomm's KGSL kernel interface (Android) has no such
 * semaphore: Mesa emulates it with a list of pending submits and answers the
 * question by "polling" each with IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID and
 * timeout 0 (vk_sync_timeline_gc_locked -> kgsl_syncobj_wait ->
 * wait_timestamp_safe, Mesa 26.2.4), and on the Retroid Pocket 5's 4.19
 * kernel that call does not return until the GPU has retired the submit:
 * measured 6 ms a call in a heavy frame at 2x, up to 11.9 ms (BO-85). Every
 * question was a wait for all the GPU work submitted so far, so the two
 * frames in flight never overlapped. A submit that signals such a semaphore
 * asks the same question of the oldest pending one
 * (vk_sync_timeline_alloc_point_locked), and waited as long. There
 * (vk_poll=0, the default on Android) the semaphore is not used and the
 * question is never asked: the last submit of a frame signals
 * the frame's fence, the fence is waited for when the frame's slot is taken
 * again (two flips on, long done), and what is known done is what such a
 * wait has shown. Garbage and occlusion slots are freed that much later;
 * occlusion counts are read from the query pool as soon as they are there. */
static int poll_ok(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_VK_POLL);
#if defined(__ANDROID__)
        on = e && *e == '1';
#else
        on = !(e && *e == '0');
#endif
    }
    return on;
}

static void serial_known(uint64_t v)
{
    if (v > s_done_serial)
        s_done_serial = v;
}

static uint64_t serial_done(void)
{
    uint64_t v = 0, t0;
    if (!poll_ok())
        return s_done_serial;
    t0 = s_vf ? now_ns() : 0;
    if (vkGetSemaphoreCounterValue(s_dev, s_timeline, &v) == VK_SUCCESS && v > s_done_serial)
        s_done_serial = v;
    if (s_vf) {
        uint64_t d = now_ns() - t0;
        s_vf->polls++;
        s_vf->poll_ns += d;
        if (d > s_vf->poll_max_ns)
            s_vf->poll_max_ns = d;
    }
    return s_done_serial;
}

static void serial_wait(uint64_t v)
{
    VkSemaphoreWaitInfo wi;
    uint64_t t0;
    int ok;
    if (!v || serial_done() >= v)
        return;
    t0 = s_vf ? now_ns() : 0;
    if (!poll_ok()) {
        /* No semaphore to wait on (see poll_ok): everything submitted. A
         * readback or a full ring, not a frame's normal path. */
        uint64_t last = s_next_serial - 1;
        nvk_queue_lock();
        ok = vkQueueWaitIdle(C->queue) == VK_SUCCESS;
        nvk_queue_unlock();
        if (ok)
            serial_known(last);
        if (s_vf) {
            s_vf->waits++;
            s_vf->wait_ns += now_ns() - t0;
        }
        return;
    }
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s_timeline;
    wi.pValues = &v;
    ok = vkWaitSemaphores(s_dev, &wi, 5000000000ull) == VK_SUCCESS;
    if (ok)
        serial_known(v);
    if (!ok) {
        static int said;
        if (said++ < 4)
            fprintf(stderr, LOGP "GPU wait for serial %llu timed out\n", (unsigned long long)v);
    }
    if (s_vf) {
        s_vf->waits++;
        s_vf->wait_ns += now_ns() - t0;
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
    if (!s_cmd) {
        s_cmd = cb_new();
        s_dyn_valid = 0;
        if (s_vf && s_vf_qp && s_cmd && s_vf->nts < VF_TS) {
            vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_vf_qp,
                                (uint32_t)((s_vf - s_vfr) * 2 * VF_TS + 2 * s_vf->nts));
            s_vf_ts_open = 1;
        }
    }
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
        if (s_vf) {
            uint64_t t0 = now_ns();
            vkCmdEndRendering(s_cmd);
            s_vf->endpass_ns += now_ns() - t0;
            if (!s_vf->pass_draws)
                s_vf->p_empty++;
            if (s_vf->pass_draws > s_vf->p_maxdraws)
                s_vf->p_maxdraws = s_vf->pass_draws;
        } else {
            vkCmdEndRendering(s_cmd);
        }
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
static int s_flush_fence;    /* the next submit signals the frame's fence */

/* vk_batch: a frame's commands go to the GPU in parts, at the boundaries
 * between render passes, so it draws the first part while the rest is being
 * recorded. With one submit a frame the GPU began only when the CPU had
 * finished, and a frame heavy on both (dense traffic) had the two times one
 * after the other. Never in the middle of a pass: a pass cut in two is
 * stored and loaded again by a tile-based GPU. */
static uint64_t s_batch_draw0, s_batch_t0;   /* s_draws and the time at the last submit */
static int      s_report_defer;              /* a report's counts wait for the pass to end */
static uint64_t s_report_defer_t;

static unsigned batch_draws(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = recomp_env(RENV_VK_BATCH);
#if defined(__ANDROID__)
        n = e && *e ? atoi(e) : 300;
#else
        n = e && *e ? atoi(e) : 0;
#endif
        if (n < 0)
            n = 0;
    }
    return (unsigned)n;
}

static void flush(int wait, int why)
{
    VkCommandBuffer cbs[2];
    VkSubmitInfo si;
    VkTimelineSemaphoreSubmitInfo ti;
    uint64_t serial;
    uint32_t n = 0;
    uint64_t t0 = 0, t1 = 0;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r;

    end_encoder();
    if (s_flush_fence && (s_cmd || s_up) && s_fr[s_fi].fence && !s_fr[s_fi].fence_serial)
        fence = s_fr[s_fi].fence;
    s_flush_fence = 0;
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
        if (s_vf_ts_open && s_vf && s_vf_qp) {
            vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, s_vf_qp,
                                (uint32_t)((s_vf - s_vfr) * 2 * VF_TS + 2 * s_vf->nts + 1));
            s_vf->nts++;
        }
        s_vf_ts_open = 0;
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
    if (poll_ok()) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_timeline;
    } else {
        si.pNext = NULL;
    }
    if (s_vf)
        t0 = now_ns();
    nvk_queue_lock();
    if (s_vf)
        t1 = now_ns();
    r = vkQueueSubmit(C->queue, 1, &si, fence);
    nvk_queue_unlock();
    {
        /* RECOMP_DEBUG=vk_device_lost=<flip>: this submit counts as lost. */
        static long at = -2;
        if (at == -2) {
            const char *e = recomp_env(RENV_VK_DEVICE_LOST);
            at = e && *e ? atol(e) : -1;
        }
        if (at >= 0 && (long)s_presents >= at && !s_failed) {
            fprintf(stderr, LOGP "vk_device_lost=%ld: this submit is treated as lost\n", at);
            r = VK_ERROR_DEVICE_LOST;
        }
    }
    if (fence && r == VK_SUCCESS)
        s_fr[s_fi].fence_serial = serial;
    if (s_vf) {
        uint64_t d = now_ns() - t1;
        s_vf->sub[why]++;
        s_vf->qlock_ns += t1 - t0;
        s_vf->submit_ns += d;
        if (d > s_vf->submit_max_ns)
            s_vf->submit_max_ns = d;
    }
    if (r != VK_SUCCESS) {
        s_submit_errors++;
        if (s_submit_errors <= 4)
            fprintf(stderr, LOGP "vkQueueSubmit failed: %s\n", nvk_result_str(r));
        if (r == VK_ERROR_DEVICE_LOST) {
            fprintf(stderr, LOGP "device lost: drawing stops\n");
            s_failed = 1;
            nvk_device_lost("vkQueueSubmit: VK_ERROR_DEVICE_LOST");
        }
    }
    s_fr[s_fi].serial = serial;
    s_cmd = VK_NULL_HANDLE;
    s_up = VK_NULL_HANDLE;
    s_flushes++;
    s_report_defer = 0;
    if (batch_draws()) {
        s_batch_draw0 = s_draws;
        s_batch_t0 = now_ns();
    }
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
        flush(1, FL_RING);   /* resets every ring */
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
    uint64_t         pass_flip;  /* vk_frame: s_presents + 1 at its last pass */
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
    uint64_t         gen;        /* s_rt_seq at its last clear or draw */
    int              presented;  /* ever picked by present_target */
    uint64_t         pass_flip;  /* vk_frame: s_presents + 1 at its last pass */
    struct nv2a_rt_own own;      /* is the guest memory under it still its own */
};

static RenderTarget  s_rt[RT_MAX];
static uint64_t      s_rt_seq;   /* bumped by every pass begun and draw recorded */
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
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &e->hw, &e->hh);
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
    if (s_cmd && batch_draws()
            && (s_report_defer || s_draws - s_batch_draw0 >= batch_draws()
                || (s_draws != s_batch_draw0 && now_ns() - s_batch_t0 > 2000000ull)))
        flush(0, s_report_defer ? FL_REPORT : FL_BATCH);
    ensure_cmd();
    rt->dirty = 1;
    rt->gen = ++s_rt_seq;
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
    if (s_vf) {
        s_vf->passes++;
        s_vf->pass_draws = 0;
        if (clear)
            s_vf->p_clear++;
        else
            s_vf->p_load++;
        if (!clear && rt->pass_flip == s_presents + 1)
            s_vf->p_resumed++;
        if (s_vf_last_rt != (void *)rt)
            s_vf->rt_switch++;
        s_vf_last_rt = rt;
        rt->pass_flip = s_presents + 1;
        if (dt) {
            if (da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                s_vf->zload++;
                if (dt->pass_flip == s_presents + 1)
                    s_vf->zresumed++;
            }
            dt->pass_flip = s_presents + 1;
        }
    }
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
static int upload_layer(struct nvk_image *im, uint32_t layer, uint32_t level, uint32_t w,
                        uint32_t h, const void *px, VkImageLayout final, int last)
{
    VkDeviceSize off;
    VkBufferImageCopy bc;
    void *dst = ring_alloc(RING_UP, (VkDeviceSize)w * h * 4u, 16, &off);

    if (!dst)
        return 0;
    memcpy(dst, px, (size_t)w * h * 4u);
    ensure_up();
    if (im->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        nvk_image_to(s_up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.mipLevel = level;
    bc.imageSubresource.baseArrayLayer = layer;
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

static int upload_level(struct nvk_image *im, uint32_t level, uint32_t w, uint32_t h,
                        const void *px, VkImageLayout final, int last)
{
    return upload_layer(im, 0, level, w, h, px, final, last);
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
/* Swizzled render targets (BO-24, as Metal): a surface of format type 2 lies
 * in guest memory in the NV2A's Z order (d3d8_swizzle.h); the seed reads it
 * so and a write-back stores it so, through a linear staging copy. Only a
 * power-of-two size is a swizzled layout. */
static int rt_swz_ok(int swz, uint32_t w, uint32_t h)
{
    return swz && w && h && !(w & (w - 1)) && !(h & (h - 1));
}

static uint8_t *s_swz_lin;
static size_t   s_swz_cap;

static uint8_t *swz_staging(size_t bytes)
{
    if (s_swz_cap < bytes) {
        uint8_t *n = (uint8_t *)realloc(s_swz_lin, bytes);
        if (!n)
            return NULL;
        s_swz_lin = n;
        s_swz_cap = bytes;
    }
    return s_swz_lin;
}

static void rt_seed(struct nvk_image *im, uint32_t addr, uint32_t pitch, uint32_t w,
                    uint32_t h, uint32_t hw, uint32_t hh, int swz)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + addr;
    VkDeviceSize off;
    uint8_t *dst;

    if (pitch >= w * 4u && rt_swz_ok(swz, w, h)) {
        uint8_t *lin = swz_staging((size_t)w * h * 4u);
        if (lin) {
            xbox_unswizzle_rect(lin, src, w, h, 4);
            src = lin;
            pitch = w * 4u;
        }
    }

    if (pitch >= w * 4u
            && (dst = (uint8_t *)ring_alloc(RING_UP, (VkDeviceSize)hw * hh * 4u, 16, &off))) {
        VkBufferImageCopy bc;
        if (hw == w) {
            uint32_t y;
            for (y = 0; y < h; y++)
                memcpy(dst + (size_t)y * w * 4u, src + (size_t)y * pitch, (size_t)w * 4u);
        } else if (hw % w == 0 && hh == h * (hw / w)) {
            nv2a_upscale_nearest32(src, pitch, w, h, hw / w, dst, hw * 4u);
        } else {            /* 1.5x */
            nv2a_resample_nearest32(src, pitch, w, h, dst, hw * 4u, hw, hh);
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
    int keep;

    /* The scale it has now against the scale it would get (in halves: 1.5x
     * is 3), by the sizes each gives its present guest size. */
    {
        uint32_t kw, kh;
        nv2a_host_size_x2(rt->w, rt->h, nv2a_host_render_scale_x2(), s_max_dim, &kw, &kh);
        keep = rt->hw == kw && rt->hh == kh;
    }
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &hw, &hh);
    if (!keep)
        rt_sync(rt, rt->pitch);
    end_encoder();
    if (!rt_image_new(&im, hw, hh))
        return 0;
    rt_seed(&im, rt->addr, rt->pitch, w, h, hw, hh, rt->swz);
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
    int i, swz = ((g->format >> 8) & 0xF) == 2;   /* the layout guest memory holds it in */

    if (!g->color_offset || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_tick++;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (rt->im.img && rt->offset == g->color_offset && rt->w == w && rt->h == h) {
            if (rt_drop_if_stale(rt, "reuse"))
                break;
            rt->last_use = s_tick;
            rt->swz = swz;   /* the guest layout for its next seed or write-back */
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
            rt->swz = swz;   /* the guest layout for its next seed or write-back */
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
    nv2a_host_size_x2(w, h, nv2a_host_render_scale_x2(), s_max_dim, &hw, &hh);
    if (!rt_image_new(&free_rt->im, hw, hh))
        return NULL;
    free_rt->offset = g->color_offset;
    free_rt->addr = nv2a_pb_dma_resolve(g->color_offset);
    free_rt->w = w;
    free_rt->h = h;
    free_rt->hw = hw;
    free_rt->hh = hh;
    free_rt->pitch = g->pitch;
    free_rt->swz = swz;
    free_rt->last_use = s_tick;
    rt_own_reset(free_rt);
    rt_seed(&free_rt->im, free_rt->addr, g->pitch, w, h, hw, hh, free_rt->swz);
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

/* A CLEAR_SURFACE of depth and colour reaches the backend as on_zclear and
 * then on_clear. Each used to open its own pass: one that cleared the depth
 * buffer and loaded the colour, ended at once without a draw, then one that
 * cleared the colour and loaded the depth; on a tile-based GPU the second
 * loads a whole buffer that was just cleared. on_zclear leaves its clear
 * here when on_clear is to follow, and on_clear opens one pass that clears
 * both (or, when its box is not the whole target, the depth-clearing pass
 * as before). */
static struct {
    RenderTarget *rt;
    unsigned      zmask;
    float         z;
    uint32_t      st;
} s_zpend;

static void zpend_flush(void)
{
    if (s_zpend.rt) {
        RenderTarget *rt = s_zpend.rt;
        s_zpend.rt = NULL;
        begin_pass(rt, 0, NULL, s_zpend.zmask, s_zpend.z, s_zpend.st);
    }
}

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
    if (!rt || !nv2a_clear_box(g, rt->w, rt->h, b)) {
        zpend_flush();
        return;
    }
    c[0] = (float)((g->clear_color >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((g->clear_color >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(g->clear_color & 0xFF) / 255.0f;
    c[3] = (float)(g->clear_color >> 24) / 255.0f;
    s_clears++;
    if (b[0] == 0 && b[1] == 0 && b[2] == rt->w && b[3] == rt->h) {
        if (s_zpend.rt == rt) {
            s_zpend.rt = NULL;
            begin_pass(rt, 1, c, s_zpend.zmask, s_zpend.z, s_zpend.st);
        } else {
            zpend_flush();
            begin_encoder(rt, 1, c);
        }
        return;
    }
    zpend_flush();
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
    if (!cr.rect.extent.width || !cr.rect.extent.height)
        return;
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
    if (param & 1u)
        s_zclears++;
    zpend_flush();
    if (param & 0xF0) {
        /* on_clear follows for the same CLEAR_SURFACE and opens the pass. */
        rt->dt = dt;
        s_zpend.rt = rt;
        s_zpend.zmask = param & 3u;
        s_zpend.z = nv2a_zclear_depth(g->format, v->zclear);
        s_zpend.st = v->zclear & 0xFFu;
        return;
    }
    if (s_pass && s_enc_rt == rt && s_enc_dt == dt && rt->dt == dt) {
        /* The pass on this target is open with this depth buffer: clear in
         * it. Ending it for a clearing pass would store and load the colour
         * for nothing. As a colour clear, not a zpass sample. */
        VkClearAttachment ca;
        VkClearRect cr;
        int counting = s_occ_slot >= 0;
        memset(&ca, 0, sizeof ca);
        ca.aspectMask = ((param & 1u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                      | ((param & 2u) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil.depth = nv2a_zclear_depth(g->format, v->zclear);
        ca.clearValue.depthStencil.stencil = v->zclear & 0xFFu;
        memset(&cr, 0, sizeof cr);
        cr.rect.extent.width = rt->hw < dt->hw ? rt->hw : dt->hw;
        cr.rect.extent.height = rt->hh < dt->hh ? rt->hh : dt->hh;
        cr.layerCount = 1;
        if (counting)
            occ_pass_end();
        vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
        if (counting)
            occ_query_begin();
        rt->gen = ++s_rt_seq;
        return;
    }
    rt->dt = dt;
    begin_pass(rt, 0, NULL, param & 3u, nv2a_zclear_depth(g->format, v->zclear),
               v->zclear & 0xFFu);
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
#define COUNT(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)

/* ---- the shader and pipeline tables, and who builds into them --------------------
 *
 * s_vs, s_fs and s_pso are filled by two threads: the ack thread, with what
 * a draw needs and the tables lack, and the pre-warm thread (pc_prewarm),
 * with the key list from disk, so that a start with a list builds nothing on
 * the ack thread (as the Metal backend). s_pc_mu guards the tables' slots; an
 * entry never moves or goes once it is in, so the ack thread keeps its memos
 * without the lock. Every build is counted by thread and kind
 * (RECOMP_TRACE=pcache logs each, =<file> adds one CSV line a flip). */
static pthread_mutex_t s_pc_mu = PTHREAD_MUTEX_INITIALIZER;
enum { PC_VS = 0, PC_FS = 1, PC_PSO = 2 };
static const char *const s_pc_kind[3] = { "vs", "fs", "pso" };
static struct {
    uint64_t n[2][3], ns[2][3];          /* [ack thread, pre-warm][kind] */
    uint64_t rt_flip_n, rt_flip_ns;      /* ack thread, since the last flip */
    uint64_t rt_10s;                     /* ack thread, first 10 s of the process */
    double   rt_max_ms;
} s_cs;
static int      s_pc_trace;              /* RECOMP_TRACE=pcache */
static FILE    *s_pc_csv;                /* RECOMP_TRACE=pcache=<file> */
static uint64_t s_t0_ns;                 /* process start, for the trace */

static uint64_t now_ns(void);
__attribute__((constructor)) static void pc_t0(void)
{
    s_t0_ns = now_ns();
}

/* One build done: counted, and logged under RECOMP_TRACE=pcache. */
static void pc_note(int prewarm, int kind, uint64_t t_start, const char *what)
{
    uint64_t t = now_ns(), ns = t - t_start;
    double ms = (double)ns / 1e6, at = (double)(t - s_t0_ns) / 1e9;
    pthread_mutex_lock(&s_pc_mu);
    s_cs.n[prewarm][kind]++;
    s_cs.ns[prewarm][kind] += ns;
    if (!prewarm) {
        s_cs.rt_flip_n++;
        s_cs.rt_flip_ns += ns;
        if (at < 10.0)
            s_cs.rt_10s++;
        if (ms > s_cs.rt_max_ms)
            s_cs.rt_max_ms = ms;
    }
    nv2a_pcache_stat_built(prewarm, kind, ns);
    pthread_mutex_unlock(&s_pc_mu);
    if (s_pc_trace)
        fprintf(stderr, LOGP "pcache: compiled %s %s on the %s thread: %.2f ms at %.3f s\n",
                s_pc_kind[kind], what, prewarm ? "pre-warm" : "ack", ms, at);
}

/* The ack thread missed while the start-up pre-warm is still on the
 * pipelines of the key list and their shaders (the first frames of a start,
 * which race it): what it needs is most likely on its way, so it waits for
 * that key instead of compiling the same thing beside it (pc_wait_more:
 * look again every half millisecond until it is there, the pipelines' part
 * is done, or the time is up).
 *
 * Bounded so that it cannot reach gameplay: only the pre-warm the backend
 * starts with has this part (a later one, after a pre-build has added to the
 * list, is never waited for), and all waits together end 3 s after it
 * started, which is 3 s after the title's first draw. Past that the ack
 * thread compiles what it needs itself, as without a list. Each wait is
 * logged. */
static int      s_pc_warming;           /* a pre-warm thread is running (atomic) */
static int      s_pc_warm_urgent;       /* the first one is on the pipelines' part (atomic) */
static uint64_t s_pc_wait_until;        /* the ack thread's waits end here */
static uint64_t s_pc_waits, s_pc_wait_ns;
#define PC_WARMING() __atomic_load_n(&s_pc_warm_urgent, __ATOMIC_ACQUIRE)
static int pc_wait_more(void)
{
    if (!PC_WARMING() || now_ns() >= __atomic_load_n(&s_pc_wait_until, __ATOMIC_ACQUIRE))
        return 0;
    nv2a_pcache_stat_waiting(1);        /* the window shows a line meanwhile */
    usleep(500);
    return 1;
}
static void pc_waited(uint64_t t0, const char *what)
{
    uint64_t t = now_ns();
    nv2a_pcache_stat_waiting(0);
    s_pc_waits++;
    s_pc_wait_ns += t - t0;
    fprintf(stderr, LOGP "pcache: the ack thread waited %.1f ms for the pre-warm's %s"
            " at %.3f s\n", (double)(t - t0) / 1e6, what, (double)(t - s_t0_ns) / 1e9);
}

/* Any thread. */
static VkShaderModule module_from_glsl(enum nvk_stage stage, const char *src, const char *what)
{
    char err[2048];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    if (!nvk_glsl_spirv(stage, src, &w, &n, &compiled, err, sizeof err)) {
        if (COUNT(s_compile_errors) <= 4)
            fprintf(stderr, LOGP "\x1b[1;31m%s: GLSL compile failed: %s\x1b[0m\n%s\n",
                    what, err, src);
        return VK_NULL_HANDLE;
    }
    if (compiled)
        COUNT(s_glsl_compiles);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(s_dev, &ci, NULL, &m) != VK_SUCCESS) {
        COUNT(s_compile_errors);
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
    uint64_t       kid;       /* nv2a_fnv64 of the slots: the disk key's id */
    VkShaderModule mod;
} VsEntry;
static VsEntry s_vs[VS_MAX];

/* The key list on disk (nv2a_pcache.h, the Metal pipeline cache's format):
 * what was built, as keys (types PC_VS, PC_FS, PC_PSO), so the next start
 * builds it before the title draws with it (pc_prewarm). */
#define PC_VS_PASS  1ull      /* ids of the two built-in shaders */
#define PC_FS_BASIC 2ull
static nv2a_pcache *s_pcf;
static void pc_put(uint32_t type, const void *data, uint32_t len);

/* The module for `n` slots of program (up to its FINAL), or VK_NULL_HANDLE;
 * its inputs in *inputs. Any thread: `parsed` and `glsl` (65536) are the
 * caller's. */
static VkShaderModule vs_build(const uint32_t *slots, int n, uint32_t hash, uint16_t *inputs,
                               NV2AVshProgram *parsed, char *glsl, int prewarm)
{
    VkShaderModule m = VK_NULL_HANDLE;
    uint64_t t = now_ns();
    char what[32];

    snprintf(what, sizeof what, "program %08X", hash);
    d3d8_vsh_parse(slots, n, parsed);
    if (d3d8_vsh_generate_glsl(parsed, D3D8_GLSL_SCREEN_SPACE, glsl, 65536) <= 0) {
        COUNT(s_compile_errors);
        fprintf(stderr, LOGP "%s: no GLSL\n", what);
    } else if ((m = module_from_glsl(NVK_STAGE_VERTEX, glsl, what)) != VK_NULL_HANDLE) {
        *inputs = parsed->inputs_read;
        COUNT(s_vs_compiled);
        pc_note(prewarm, PC_VS, t, what);
    }
    return m;
}

/* s_pc_mu held. */
static VsEntry *vs_find(uint32_t hash)
{
    int i;
    for (i = 0; i < VS_MAX; i++)
        if (s_vs[i].used && s_vs[i].hash == hash)
            return &s_vs[i];
    return NULL;
}

/* s_pc_mu held. A free entry taken for `hash`, or NULL when the table is full. */
static VsEntry *vs_take(uint32_t hash, uint64_t kid, VkShaderModule mod, uint16_t inputs)
{
    int i;
    for (i = 0; i < VS_MAX && s_vs[i].used; i++)
        ;
    if (i == VS_MAX)
        return NULL;
    s_vs[i].hash = hash;
    s_vs[i].kid = kid;
    s_vs[i].mod = mod;
    s_vs[i].inputs = inputs;
    s_vs[i].bad = mod ? 0 : 1;
    s_vs[i].used = 1;
    return &s_vs[i];
}

/* The id of a vertex program's key: nv2a_fnv64 of its slots. */
static uint64_t vs_kid(const void *slots, size_t bytes)
{
    uint64_t kid = nv2a_fnv64(slots, bytes, NV2A_FNV64_INIT);
    return kid <= PC_FS_BASIC ? kid + 3 : kid;
}

/* As Metal's vs_cache: the program from its start slot, parsed, emitted as
 * GLSL with the screen-space epilogue, keyed by nv2a_vsh_program_hash. */
static VsEntry *vs_cache(void)
{
    static NV2AVshProgram parsed;
    static char glsl[65536];
    static struct { int valid; uint32_t dwords, start; VsEntry *e; } memo;
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t hash, len = 0;
    VsEntry *e;

    if (memo.valid && memo.dwords == v->prog_dwords && memo.start == v->start)
        return memo.e;
    hash = nv2a_vsh_program_hash(v, &len);
    if (!hash)
        hash = 1;
    pthread_mutex_lock(&s_pc_mu);
    e = vs_find(hash);
    pthread_mutex_unlock(&s_pc_mu);
    if (!e && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            e = vs_find(hash);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!e && pc_wait_more());
        pc_waited(t0, "vertex shader");
    }
    if (!e) {
        const uint32_t *slots = &v->prog[v->start][0];
        uint16_t inputs = 0;
        VkShaderModule mod;
        if (!len || len > NV2A_VSH_SLOTS - v->start)
            len = NV2A_VSH_SLOTS - v->start;
        mod = vs_build(slots, (int)len, hash, &inputs, &parsed, glsl, 0);
        pthread_mutex_lock(&s_pc_mu);
        if ((e = vs_find(hash)) != NULL) {
            if (mod)                    /* the pre-warm thread was first */
                vkDestroyShaderModule(s_dev, mod, NULL);
            mod = VK_NULL_HANDLE;
        } else {
            e = vs_take(hash, vs_kid(slots, (size_t)len * 16u), mod, inputs);
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (!e)
            return NULL;                /* the table is full */
        if (mod)
            pc_put(PC_VS, slots, len * 16u);
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
    uint64_t          kid;
    NV2ACombinerState st;
    VkShaderModule    mod;
} FsEntry;
static FsEntry *s_fs;         /* FS_MAX, allocated in init */
static int      s_fs_n;

/* The module for combiner state `st`, or VK_NULL_HANDLE. Any thread: `glsl`
 * (65536) is the caller's. */
static VkShaderModule fs_build(const NV2ACombinerState *st, uint32_t hash, char *glsl,
                               int prewarm)
{
    VkShaderModule m = VK_NULL_HANDLE;
    uint64_t t = now_ns();
    char what[32];

    snprintf(what, sizeof what, "combiners %08X", hash);
    if (d3d8_combiners_generate_glsl(st, glsl, 65536) <= 0) {
        COUNT(s_compile_errors);
        fprintf(stderr, LOGP "%s: no GLSL\n", what);
    } else if ((m = module_from_glsl(NVK_STAGE_FRAGMENT, glsl, what)) != VK_NULL_HANDLE) {
        COUNT(s_fs_compiled);
        pc_note(prewarm, PC_FS, t, what);
    }
    return m;
}

/* s_pc_mu held. */
static FsEntry *fs_find(uint32_t hash, const NV2ACombinerState *st)
{
    int n;
    for (n = 0; n < s_fs_n; n++)
        if (s_fs[n].hash == hash && !memcmp(&s_fs[n].st, st, sizeof *st))
            return &s_fs[n];
    return NULL;
}

/* s_pc_mu held. The next entry taken for `st`, or NULL when the table is full. */
static FsEntry *fs_take(uint32_t hash, const NV2ACombinerState *st, VkShaderModule mod)
{
    FsEntry *e;
    if (s_fs_n == FS_MAX)
        return NULL;
    e = &s_fs[s_fs_n];
    e->hash = hash;
    e->st = *st;
    e->mod = mod;
    e->bad = mod ? 0 : 1;
    e->kid = nv2a_fnv64(st, sizeof *st, NV2A_FNV64_INIT);
    if (e->kid <= PC_FS_BASIC)
        e->kid += 3;
    s_fs_n++;               /* last: fs_find reads up to it */
    return e;
}

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
    static struct { int valid; uint32_t r[39]; VkShaderModule m; } memo;
    NV2ACombinerState st;
    uint32_t key[39], modes = 0, signs = 0, hash;
    FsEntry *e;
    int n;

    if (!(v->rc_set && (v->rc_ctl & 0xFF)))
        return s_fs_basic;
    for (n = 0; n < 4; n++) {
        uint32_t m = (uint32_t)s_stage[n].mode;
        if (s_stage[n].raw_mode == 5)
            m = 5;
        else if (m == 3 && s_stage[n].cube)
            m = 3;          /* CUBEMAP: a samplerCube (cube_bind) */
        else if ((m == 6 || m == 7) && n > 0)
            ;               /* BUMPENVMAP(_LUM): offset by the previous stage */
        else if (m && m != 4) m = 1;
        modes |= m << (5 * n);
        if (m == 1 || m == 6 || m == 7)
            signs |= (uint32_t)nv2a_tex_signed_mask(s_stage[n].filter) << (4 * n);
    }
    memcpy(key, v->rc_cicw, 32);
    memcpy(key + 8, v->rc_aicw, 32);
    memcpy(key + 16, v->rc_cocw, 32);
    memcpy(key + 24, v->rc_aocw, 32);
    key[32] = v->rc_ctl;
    key[33] = v->rc_fcw0;
    key[34] = v->rc_fcw1;
    key[35] = modes;
    key[36] = signs;
    key[37] = v->shader_ctl & 0x00FF0000u;  /* stage 2 and 3 input stages */
    key[38] = v->clip_plane_mode;
    if (memo.valid && !memcmp(memo.r, key, sizeof key))
        return memo.m;

    memset(&st, 0, sizeof st);
    d3d8_combiners_from_regs(v->rc_cicw, v->rc_aicw, v->rc_cocw, v->rc_aocw,
                             v->rc_ctl, v->rc_fcw0, v->rc_fcw1, modes, &st);
    st.fog_input = 1;
    for (n = 0; n < 4; n++) {
        st.tex_signed[n] = (uint8_t)((signs >> (4 * n)) & 0xFu);
        st.clip_mode[n] = (uint8_t)((v->clip_plane_mode >> (4 * n)) & 0xFu);
    }
    st.input_tex[2] = (uint8_t)((v->shader_ctl >> 16) & 0xFu);
    st.input_tex[3] = (uint8_t)((v->shader_ctl >> 20) & 0xFu);
    hash = fnv(&st, sizeof st);
    pthread_mutex_lock(&s_pc_mu);
    e = fs_find(hash, &st);
    pthread_mutex_unlock(&s_pc_mu);
    if (!e && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            e = fs_find(hash, &st);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!e && pc_wait_more());
        pc_waited(t0, "fragment shader");
    }
    if (!e) {
        VkShaderModule mod = fs_build(&st, hash, glsl, 0);
        pthread_mutex_lock(&s_pc_mu);
        if ((e = fs_find(hash, &st)) != NULL) {
            if (mod)                    /* the pre-warm thread was first */
                vkDestroyShaderModule(s_dev, mod, NULL);
            mod = VK_NULL_HANDLE;
        } else {
            e = fs_take(hash, &st, mod);
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (!e)
            return s_fs_basic;          /* the table is full */
        if (mod)
            pc_put(PC_FS, &st, sizeof st);
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

/* Any thread. */
static VkPipeline pipeline_new(const PsoKey *k, int prewarm)
{
    uint64_t t0 = now_ns();
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
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
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
    COUNT(s_pso_created);
    COUNT(s_pso_since_save);
    {
        char what[48];
        snprintf(what, sizeof what, "%016llX/%016llX", (unsigned long long)k->vs,
                 (unsigned long long)k->fs);
        pc_note(prewarm, PC_PSO, t0, what);
    }
    return p;
}

/* s_pc_mu held. The slot of `k`: its entry when *found, else the free slot
 * it would take. */
static uint32_t pso_slot(const PsoKey *k, int *found)
{
    uint32_t i = fnv(k, sizeof *k) & (PSO_CAP - 1);
    for (; s_pso[i].used; i = (i + 1) & (PSO_CAP - 1))
        if (!memcmp(&s_pso[i].k, k, sizeof *k)) {
            *found = 1;
            return i;
        }
    *found = 0;
    return i;
}

/* A pipeline key on disk: the shaders by id, not by module. */
typedef struct {
    uint64_t vs, fs;
    uint32_t nf4, topo, depth, wmask;
    uint32_t blend[4];
} PsoDisk;

static uint64_t module_kid(VkShaderModule m)
{
    int i;
    if (m == s_vs_pass)
        return PC_VS_PASS;
    if (m == s_fs_basic)
        return PC_FS_BASIC;
    for (i = 0; i < VS_MAX; i++)
        if (s_vs[i].used && s_vs[i].mod == m)
            return s_vs[i].kid;
    for (i = 0; i < s_fs_n; i++)
        if (s_fs[i].mod == m)
            return s_fs[i].kid;
    return 0;
}

static void pso_disk(const PsoKey *k, PsoDisk *d)
{
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&s_pc_mu);
    d->vs = module_kid((VkShaderModule)(uintptr_t)k->vs);
    d->fs = module_kid((VkShaderModule)(uintptr_t)k->fs);
    pthread_mutex_unlock(&s_pc_mu);
    d->nf4 = k->nf4;
    d->topo = k->topo;
    d->depth = k->depth;
    d->wmask = k->wmask;
    memcpy(d->blend, k->blend, sizeof d->blend);
}

static VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, uint32_t nf4,
                           enum nv2a_topology topo)
{
    static uint32_t last = 0xFFFFFFFFu;
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    PsoKey k;
    uint32_t i;
    int found, full;

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
    pthread_mutex_lock(&s_pc_mu);
    i = pso_slot(&k, &found);
    pthread_mutex_unlock(&s_pc_mu);
    if (!found && PC_WARMING()) {
        uint64_t t0 = now_ns();
        do {
            pthread_mutex_lock(&s_pc_mu);
            i = pso_slot(&k, &found);
            pthread_mutex_unlock(&s_pc_mu);
        } while (!found && pc_wait_more());
        pc_waited(t0, "pipeline");
    }
    pthread_mutex_lock(&s_pc_mu);
    full = !found && s_pso_n + 1 >= PSO_CAP * 3 / 4;
    pthread_mutex_unlock(&s_pc_mu);
    if (!found) {
        VkPipeline p;
        if (full)
            return VK_NULL_HANDLE;   /* a title with this many wants eviction */
        p = pipeline_new(&k, 0);
        pthread_mutex_lock(&s_pc_mu);
        i = pso_slot(&k, &found);
        if (!found) {
            /* A failed one is kept too, so it is not tried at every draw. */
            s_pso[i].k = k;
            s_pso[i].p = p;
            s_pso[i].used = 1;
            s_pso_n++;
        }
        pthread_mutex_unlock(&s_pc_mu);
        if (found) {                 /* the pre-warm thread was first */
            if (p)
                vkDestroyPipeline(s_dev, p, NULL);
        } else if (p) {
            PsoDisk dk;
            pso_disk(&k, &dk);
            pc_put(PC_PSO, &dk, sizeof dk);
            if (vk_trace())
                fprintf(stderr, LOGP "pipeline #%u: blend %u %04X/%04X op %u mask %08X"
                        " depth %u topo %u inputs %u\n", s_pso_n, k.blend[0], k.blend[1],
                        k.blend[2], k.blend[3], k.wmask, k.depth, k.topo, k.nf4);
        }
    }
    last = i;
    return s_pso[i].p;
}

/* ---- textures: Metal's cache, key and invalidation ------------------------- */

#define TEX_MAX_TEXELS (2048u * 2048u)
#define TEX_CACHE 256
typedef struct {
    uint32_t         offset, color, width, height, pitch, palette, levels;
    uint32_t         sign;          /* nv2a_tex_signed_mask the bytes carry */
    uint32_t         gen, used;
    uint64_t         hash;
    uint32_t         rep;           /* nv2a_texrep generation it was uploaded at; 0: not a replaced texture */
    uint8_t          ui;            /* a 2D draw has bound it (nv2a_texscale.h); stays */
    uint8_t          scale;         /* the factor wanted when it was uploaded: 0, 2, 4 */
    uint8_t          salgo;         /* and the scaler */
    uint8_t          up_scale;      /* the factor the image has (0 where the scaler refused) */
    uint8_t          use;           /* RECOMP_DEBUG=ui_scale_log: the kinds of draw seen */
    uint8_t          pend;          /* its scaled picture is being made (nv2a_texscale_get) */
    uint32_t         pal_len, pgen; /* P8 entries; the flip pend was last asked at */
    uint64_t         skey;          /* nv2a_texscale_key of the texels uploaded */
    int              live;
    struct nvk_image im;
} TexEntry;

static TexEntry         s_tc[TEX_CACHE];

/* BO-45, as Metal: the channels stage t's filter reads as signed, honoured
 * for textures decoded from guest memory when the pixel stage is a combiner
 * shader; uploaded offset by 128 (nv2a_tex_signed_xor) for the shader to
 * take back (d3d8_combiners_glsl.c). xemu #587. */
static int s_signed_on;
static uint8_t stage_signed(const struct nv2a_stage *t)
{
    return s_signed_on ? nv2a_tex_signed_mask(t->filter) : 0;
}
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

/* RECOMP_TRACE=pcache: this frame's new textures (tex_upload_log). */
static struct { uint32_t n; uint64_t texels, decode, image, copy; } s_fup;

/* Decoded on the CPU by the shared decode, uploaded as BGRA8 into a new
 * image: a draw already recorded this frame keeps the texels it was
 * recorded with, as Metal's new MTLTexture per change. */
static uint32_t s_tex_rep;    /* the last tex_upload's nv2a_texrep_apply */

/* graphics.ui_scale (nv2a_texscale.h), as Metal: the draw being recorded is
 * a 2D one, and the factor the texture tex_bind last returned has. */
static int       s_draw_2d;
static unsigned  s_bind_scale;
static uint32_t *s_scaled;
static size_t    s_scaled_cap;

static unsigned scale_levels(unsigned f)
{
    return f == 4 ? 3u : f == 2 ? 2u : 1u;
}

static uint32_t  s_swap_flip = 0xFFFFFFFFu;   /* the flip of the last swap */
static int       s_up_pend;    /* the last tex_upload queued the scaled picture */
static uint64_t  s_up_key;

/* The disk cache of scaled textures: beside the shader cache. Once. */
static void scale_store_init(void)
{
    static int done;
    char dir[900], sub[1000];
    if (done)
        return;
    done = 1;
    if (nvk_cache_dir(dir, sizeof dir)) {
        snprintf(sub, sizeof sub, "%s/ui_scale", dir);
        nv2a_texscale_cache_dir(sub);
    }
}

static int scaled_room(const Stage *t, unsigned f)
{
    size_t n = nv2a_tex_texels(t->width * f, t->height * f, scale_levels(f));
    if (n > s_scaled_cap) {
        uint32_t *p = (uint32_t *)realloc(s_scaled, n * 4);
        if (!p)
            return 0;
        s_scaled = p;
        s_scaled_cap = n;
    }
    return 1;
}

/* s_scaled's level 0 (the texture f times as large) as an image with the
 * levels down to the title's size (as Metal). 0: no image (out untouched). */
static int scaled_image(const Stage *t, unsigned f, struct nvk_image *out)
{
    uint32_t w = t->width * f, h = t->height * f, levels = scale_levels(f), l;
    const uint32_t *src;

    nv2a_texscale_mips(s_scaled, w, h, levels);
    if (!nvk_image_new(out, RT_FORMAT, w, h, levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    for (l = 0, src = s_scaled; l < levels; l++) {
        uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
        if (!upload_level(out, l, lw, lh, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          l + 1 == levels)) {
            garbage_push(out, VK_NULL_HANDLE, NULL);
            memset(out, 0, sizeof *out);
            return 0;
        }
        src += (size_t)lw * lh;
    }
    return 1;
}

/* The scaled picture of s_texels, if the store has it ready; else it is
 * queued (s_up_pend) and the title's own texture is uploaded meanwhile.
 * Nothing is scaled on this thread. */
static int tex_upload_scaled(const Stage *t, unsigned f, int algo, struct nvk_image *out)
{
    uint64_t t0 = ui_scale_log() ? now_ns() : 0;
    int r;

    s_up_pend = 0;
    /* Every texture a 2D draw binds, opaque pictures too (the World Tour's
     * photographs, the loading pictures): the owner wants the whole 2D front
     * end scaled. Transparency only steers the switch-on pass below. */
    if (!scaled_room(t, f))
        return 0;
    scale_store_init();
    s_up_key = nv2a_texscale_key(s_texels, t->width, t->height);
    r = nv2a_texscale_get(s_up_key, s_texels, t->width, t->height, f,
                          (enum nv2a_texscale_algo)algo, 0, s_scaled);
    if (ui_scale_log())
        fprintf(stderr, "[UISCALE] ask %016llx %ux%u x%u: %s, %.3f ms on the render thread,"
                " flip %llu\n", (unsigned long long)s_up_key, t->width, t->height, f,
                r == NV2A_TEXSCALE_READY ? "ready" : r == NV2A_TEXSCALE_PENDING ? "queued"
                                                                               : "not taken",
                (double)(now_ns() - t0) / 1e6, (unsigned long long)s_presents);
    if (r == NV2A_TEXSCALE_READY)
        return scaled_image(t, f, out);
    s_up_pend = r == NV2A_TEXSCALE_PENDING;
    return 0;
}

static int tex_upload(const Stage *t, unsigned scale, int algo, struct nvk_image *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t n = nv2a_tex_texels(t->width, t->height, t->levels);
    const uint32_t *pal = NULL, *src;
    uint64_t t0, t1, t2;
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
    t0 = s_pc_trace ? now_ns() : 0;
    nv2a_tex_decode(mem + t->offset, t->color, t->width, t->height, t->pitch,
                    t->levels, pal, pal ? t->pal_len : 0, s_texels);
    /* The title's button prompts as the pad in use prints them (input.glyphs,
     * nv2a_texrep.h): rectangles of a registered texture replaced here. */
    s_tex_rep = nv2a_texrep_apply(mem + t->offset, t->color, t->width, t->height,
                                  t->pitch, t->levels, s_texels);
    if (stage_signed(t)) {
        uint32_t x = nv2a_tex_signed_xor(stage_signed(t));
        size_t i;
        for (i = 0; i < n; i++)
            s_texels[i] ^= x;
    }
    s_bind_scale = 0;
    s_up_pend = 0;
    if (scale && tex_upload_scaled(t, scale, algo, out)) {
        s_bind_scale = scale;
        s_tex_uploads++;
        return 1;
    }
    t1 = s_pc_trace ? now_ns() : 0;
    if (!nvk_image_new(out, RT_FORMAT, t->width, t->height, t->levels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    t2 = s_pc_trace ? now_ns() : 0;
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
    if (s_pc_trace) {
        uint64_t t3 = now_ns();
        s_fup.n++;
        s_fup.texels += n;
        s_fup.decode += t1 - t0;
        s_fup.image += t2 - t1;
        s_fup.copy += t3 - t2;
    }
    return 1;
}

/* RECOMP_TRACE=pcache: a frame that spent 2 ms or more on new textures says
 * so at its flip. Pipelines are not the only first-use cost: a scene's first
 * frame decodes and uploads every texture it draws with (a race's first
 * frame: some 150, 70 ms on the RX 9060 XT). */
static void tex_upload_log(void)
{
    uint64_t ns = s_fup.decode + s_fup.image + s_fup.copy;
    if (s_fup.n && ns >= 2000000u)
        fprintf(stderr, LOGP "textures: the frame before present #%llu uploaded %u"
                " (%.1f M texels) in %.2f ms: decode %.2f, image %.2f, copy %.2f\n",
                (unsigned long long)s_presents + 1, s_fup.n, s_fup.texels / 1e6,
                ns / 1e6, s_fup.decode / 1e6, s_fup.image / 1e6, s_fup.copy / 1e6);
    memset(&s_fup, 0, sizeof s_fup);
}

static void rt_sync(RenderTarget *rt, uint32_t pitch);

/* graphics.ui_scale switched on (or changed) in the overlay: while the title
 * is held for the pause, every loaded texture the rule could scale is decoded
 * and queued, to be made and written to the disk cache in the background
 * (nothing is uploaded here; a texture's own bind takes its picture from the
 * cache later). "Could scale": one level, swizzled or DXT, some transparency;
 * whether a 2D draw uses it is not known for a texture bound before the
 * setting was on, so some that never will be are made too. The executor's
 * thread (xbox_HostPauseIdleFn), which is the thread of every other use of
 * the cache. */
static void scale_prepare_loaded(void)
{
    static uint32_t done_gen;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    unsigned f = nv2a_texscale_factor(), queued = 0;
    int algo = (int)nv2a_texscale_algo(), i;

    if (done_gen == nv2a_texscale_generation())
        return;
    done_gen = nv2a_texscale_generation();
    if (!f)
        return;
    scale_store_init();
    if (!nv2a_texscale_cache_path(NULL, 0))
        return;
    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry *e = &s_tc[i];
        Stage st;
        const uint32_t *pal = NULL;
        uint32_t bytes;
        size_t n;

        if (!e->live || e->sign || e->up_scale == f || e->pend
                || !nv2a_texscale_tex_ok(nv2a_tex_size_from_format(e->color), e->width,
                                         e->height, e->levels))
            continue;
        /* Nothing says here whether a 2D draw uses a texture, and the larger
         * one-level textures with alpha are mostly the world's (car
         * liveries, tree lines, sky strips, 8 MB each at 4x): those are left
         * to their own bind, which knows. Fonts and HUD art are within this. */
        if ((uint64_t)e->width * e->height > 256u * 256u)
            continue;
        memset(&st, 0, sizeof st);
        st.offset = e->offset;
        st.color = e->color;
        st.width = e->width;
        st.height = e->height;
        st.pitch = e->pitch;
        st.palette = e->palette;
        st.pal_len = e->pal_len;
        st.levels = e->levels;
        bytes = nv2a_tex_extent(st.color, st.width, st.height, st.pitch, st.levels);
        /* Only what is still the texture that was uploaded. */
        if (!bytes || tex_hash_pal(mem, &st, bytes) != e->hash)
            continue;
        n = nv2a_tex_texels(st.width, st.height, st.levels);
        if (n > s_texels_cap) {
            uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
            if (!p)
                return;
            s_texels = p;
            s_texels_cap = n;
        }
        if (st.color == NV2A_TEX_P8)
            pal = (const uint32_t *)(mem + st.palette);
        nv2a_tex_decode(mem + st.offset, st.color, st.width, st.height, st.pitch, st.levels,
                        pal, pal ? st.pal_len : 0, s_texels);
        (void)nv2a_texrep_apply(mem + st.offset, st.color, st.width, st.height, st.pitch,
                                st.levels, s_texels);
        /* Not known to be 2D here: art cut out with alpha is the likely kind;
         * an opaque picture waits for its own 2D bind. */
        if (!nv2a_texscale_has_alpha(s_texels, (size_t)st.width * st.height))
            continue;
        if (nv2a_texscale_get(nv2a_texscale_key(s_texels, st.width, st.height), s_texels,
                              st.width, st.height, f, (enum nv2a_texscale_algo)algo,
                              NV2A_TEXSCALE_DISK_ONLY, NULL) == NV2A_TEXSCALE_PENDING)
            queued++;
    }
    fprintf(stderr, "[UISCALE] setting changed to %ux: %u loaded textures queued for the"
            " cache\n", f, queued);
}

/* RECOMP_DEBUG=ui_scale_log, as Metal. */
static int ui_scale_log(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_UI_SCALE_LOG) != NULL;
    return on;
}

static void ui_scale_use(TexEntry *e, const Stage *t, const uint8_t *mem)
{
    uint8_t bit = s_draw_2d ? 1 : 2;
    if (e->use & bit)
        return;
    e->use |= bit;
    fprintf(stderr, "[UISCALE] tex %016llx fmt 0x%02x %ux%u levels %u use %u scale %u flip %llu\n",
            (unsigned long long)nv2a_texrep_hash(mem + t->offset, t->color, t->width,
                                                 t->height, t->pitch),
            t->color, t->width, t->height, t->levels, e->use, e->up_scale,
            (unsigned long long)s_presents);
}

static VkImageView tex_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = 0, i;
    TexEntry *e = NULL, *victim = &s_tc[0];
    unsigned want;
    int algo;

    s_bind_scale = 0;

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
                && c->levels == t->levels && c->sign == stage_signed(t)) {
            e = c;
            break;
        }
        if (!c->live || (victim->live && c->used < victim->used))
            victim = c;
    }
    /* graphics.ui_scale: a texture a 2D draw binds is scaled, from then on
     * and for every draw (off: want is 0 and nothing here differs). */
    want = nv2a_texscale_factor();
    algo = (int)nv2a_texscale_algo();
    if (want && !((s_draw_2d || (e && e->ui)) && !stage_signed(t)
                  && nv2a_texscale_tex_ok(nv2a_tex_size_from_format(t->color), t->width,
                                          t->height, t->levels)))
        want = 0;
    if (e) {
        /* A replaced texture whose variant changed is uploaded again; so is
         * one whose scale is not the one wanted now. */
        int stale = (e->rep && e->rep != nv2a_texrep_generation())
                 || e->scale != want || (want && e->salgo != algo);
        if (s_draw_2d)
            e->ui = 1;
        if (e->gen != g->flips || stale) {
            uint64_t h = tex_hash_pal(mem, t, bytes);
            s_tex_rehash++;
            e->gen = g->flips;
            if (h != e->hash || stale) {
                struct nvk_image ni;
                e->hash = h;
                s_tex_changed++;
                if (!tex_upload(t, want, algo, &ni))
                    return s_black.view;
                garbage_push(&e->im, VK_NULL_HANDLE, NULL);
                e->rep = s_tex_rep;
                e->im = ni;
                e->scale = (uint8_t)want;
                e->salgo = (uint8_t)algo;
                e->up_scale = (uint8_t)s_bind_scale;
                e->pend = (uint8_t)s_up_pend;
                e->skey = s_up_key;
                e->pgen = g->flips;
                e->pal_len = t->pal_len;
            }
        }
    } else {
        struct nvk_image ni;
        e = victim;
        if (e->live)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(&e->im, 0, sizeof e->im);
        e->live = 0;
        if (!tex_upload(t, want, algo, &ni))
            return s_black.view;
        e->ui = (uint8_t)(s_draw_2d != 0);
        e->scale = (uint8_t)want;
        e->salgo = (uint8_t)algo;
        e->up_scale = (uint8_t)s_bind_scale;
        e->pend = (uint8_t)s_up_pend;
        e->skey = s_up_key;
        e->pgen = g->flips;
        e->pal_len = t->pal_len;
        e->use = 0;
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->levels = t->levels;
        e->sign = stage_signed(t);
        e->gen = g->flips;
        e->hash = tex_hash_pal(mem, t, bytes);
        e->rep = s_tex_rep;
        e->im = ni;
        e->live = 1;
    }
    /* Its scaled picture was being made: asked for once a flip, swapped in
     * when it is there (as Metal). The old image goes when the frames that
     * may use it are done. */
    if (e->pend && e->pgen != g->flips && s_swap_flip != g->flips) {
        e->pgen = g->flips;
        if (!scaled_room(t, e->scale)) {
            e->pend = 0;
        } else {
            uint64_t t0 = ui_scale_log() ? now_ns() : 0;
            int r = nv2a_texscale_get(e->skey, NULL, t->width, t->height, e->scale,
                                      (enum nv2a_texscale_algo)e->salgo, 0, s_scaled);
            if (r == NV2A_TEXSCALE_READY) {
                struct nvk_image ni;
                s_swap_flip = g->flips;   /* one a flip: its upload is the cost */
                e->pend = 0;
                if (scaled_image(t, e->scale, &ni)) {
                    garbage_push(&e->im, VK_NULL_HANDLE, NULL);
                    e->im = ni;
                    e->up_scale = e->scale;
                    s_tex_uploads++;
                }
            } else if (r != NV2A_TEXSCALE_PENDING) {
                e->pend = 0;         /* refused or lost: stays as the title's */
            }
            if (ui_scale_log() && r != NV2A_TEXSCALE_PENDING)
                fprintf(stderr, "[UISCALE] swap %016llx %ux%u x%u: %s, %.3f ms on the render"
                        " thread, flip %llu\n", (unsigned long long)e->skey, t->width,
                        t->height, (unsigned)e->scale,
                        r == NV2A_TEXSCALE_READY ? "scaled texture in" : "no result",
                        (double)(now_ns() - t0) / 1e6, (unsigned long long)s_presents);
        }
    }
    e->used = ++s_tc_clock;
    s_bind_scale = e->up_scale;
    if (ui_scale_log())
        ui_scale_use(e, t, mem);
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

static VkSampler sampler(const Stage *t, unsigned scale)
{
    static int point = -1;
    VkSamplerCreateInfo sc;
    VkSampler st;
    uint32_t u = t->addr_u & 0xF, v = t->addr_v & 0xF;
    int mn, mg, mip, i, aniso;
    uint64_t key;

    if (point < 0)
        point = recomp_env(RENV_VK_POINT) != NULL;
    mn = !point && nv2a_tex_filter_linear(t->filter, 0);
    mg = !point && nv2a_tex_filter_linear(t->filter, 1);
    mip = t->levels > 1 ? nv2a_tex_filter_mip(t->filter) : 0;
    /* A scaled 2D texture has the levels down to the title's size, blended
     * between (as Metal). */
    if (scale)
        mip = 2;
    /* Anisotropy only on a mipmapped sampler with a linear minification
     * filter, as asked (RECOMP_GFX_ANISO), within the device's limit. */
    {
        const struct nvk_core *core = nvk_core_get();
        aniso = (mip && mn && !scale && core && core->max_aniso > 1.0f) ? nv2a_host_aniso() : 1;
        if (aniso > 1 && (float)aniso > core->max_aniso)
            aniso = (int)core->max_aniso;
    }
    key = u | v << 4 | (uint64_t)mn << 8 | (uint64_t)mg << 9 | (uint64_t)mip << 10;
    if (mip)
        key |= (uint64_t)t->lod_min << 25 | (uint64_t)t->lod_max << 29;
    key |= (uint64_t)aniso << 33;       /* the level is part of the sampler */
    key |= (uint64_t)scale << 40;
    for (i = 0; i < s_smp_n; i++)
        if (s_smp[i].key == key)
            return s_smp[i].s;
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE;
    sc.maxAnisotropy = aniso > 1 ? (float)aniso : 1.0f;
    sc.minFilter = mn ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.magFilter = mg ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = address_mode(u);
    sc.addressModeV = address_mode(v);
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.minLod = scale ? 0.0f : mip ? (float)t->lod_min : 0.0f;
    /* Not mipmapped: level 0, with the min/mag choice kept (the Vulkan
     * spec's maxLod 0.25 for Metal's MipFilterNotMipmapped). */
    sc.maxLod = scale ? (float)(scale_levels(scale) - 1u) : mip ? (float)t->lod_max : 0.25f;
    if (vkCreateSampler(s_dev, &sc, NULL, &st) != VK_SUCCESS)
        return s_smp_n ? s_smp[0].s : VK_NULL_HANDLE;
    if (s_smp_n < SMP_MAX) {
        i = s_smp_n++;
    } else {
        static unsigned next;
        i = (int)(next++ % SMP_MAX);
        /* A recorded draw may still use it. */
        flush(1, FL_SAMPLER);
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

/* ---- cube maps (BO-8): Metal's cube_bind ---------------------------------------
 *
 * A CUBEMAP stage samples a samplerCube (d3d8_combiners_glsl.c). The six
 * faces sit face_stride apart (nv2a_tex_face_stride, xemu texture.c); a face
 * this backend drew -- Burnout 3 renders its car reflections into six 128x128
 * targets every frame -- is copied from that target on the GPU whenever it
 * was drawn since the last copy (xemu renders a surface into the texture
 * that reads it, pgraph/gl/surface.c pgraph_gl_render_surface_to_texture);
 * the other faces decode from guest memory, re-checked once per flip; a
 * change makes a new cube, so a frame in flight keeps the one it used. */
#define CUBE_MAX 8
typedef struct {
    uint32_t         offset, color, width, levels, side;   /* side: host face size */
    uint32_t         gen, used;
    uint64_t         hash;
    uint64_t         face_gen[6];
    struct nvk_image im;
} CubeEntry;
static CubeEntry        s_cube[CUBE_MAX];
static uint32_t         s_cube_clock;
static struct nvk_image s_black_cube;
static uint64_t         s_cube_copies, s_cube_uploads;

static VkImageView black_cube(void)
{
    if (!s_black_cube.img) {
        static const uint32_t px = 0xFF000000u;
        int f;
        if (!nvk_image_new_cube(&s_black_cube, RT_FORMAT, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return s_black.view;   /* a 2D view: invalid there, but never NULL */
        for (f = 0; f < 6; f++)
            upload_layer(&s_black_cube, (uint32_t)f, 0, 1, 1, &px,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, f == 5);
    }
    return s_black_cube.view;
}

static RenderTarget *cube_face_rt(uint32_t addr, uint32_t w)
{
    int i;
    for (i = 0; i < RT_MAX; i++) {
        RenderTarget *rt = &s_rt[i];
        if (!rt->im.img || ((rt->addr ^ addr) & 0x07FFFFFFu) || rt->w < w || rt->h < w)
            continue;
        if (rt_drop_if_stale(rt, "bind"))
            continue;
        return rt;
    }
    return NULL;
}

static VkImageView cube_bind(const Stage *t)
{
    const struct nv2a_pb_gpu *g = nv2a_pb_gpu_state();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t *pal = t->color == NV2A_TEX_P8 ? (const uint32_t *)(mem + t->palette) : NULL;
    RenderTarget *frt[6];
    uint32_t w = t->width, side = t->width, levels, stride, bytes, f, l;
    CubeEntry *e = NULL, *victim = &s_cube[0];
    int any = 0, all = 1, fresh = 0, ncopy = 0, i;

    if (!t->cube || !w || t->width != t->height || (uint64_t)w * w * 6u > TEX_MAX_TEXELS)
        return black_cube();
    stride = nv2a_tex_face_stride(t->color, w, w, t->pitch, t->fmt_levels);
    bytes = nv2a_tex_extent(t->color, w, w, t->pitch, t->levels);
    if (!bytes)
        return black_cube();
    for (f = 0; f < 6; f++) {
        frt[f] = cube_face_rt(t->offset + f * stride, w);
        if (frt[f]) {
            any = 1;
            side = w * frt[f]->hw / frt[f]->w;
        } else {
            all = 0;
        }
    }
    levels = any ? 1u : t->levels;
    for (i = 0; i < CUBE_MAX; i++) {
        CubeEntry *c = &s_cube[i];
        if (c->im.img && c->offset == t->offset && c->color == t->color
                && c->width == w && c->levels == levels) {
            e = c;
            break;
        }
        if (!c->im.img || (victim->im.img && c->used < victim->used))
            victim = c;
    }
    if (!e) {
        e = victim;
        if (e->im.img)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(e, 0, sizeof *e);
        e->offset = t->offset;
        e->color = t->color;
        e->width = w;
        e->levels = levels;
        fresh = 1;
    }
    if (e->side != side)
        fresh = 1;
    if (!all && (fresh || e->gen != g->flips)) {
        uint64_t h = NV2A_TEX_HASH_SEED;
        for (f = 0; f < 6; f++)
            if (!frt[f])
                h = h * 31u + nv2a_tex_hash(mem + t->offset + f * stride, bytes,
                                            NV2A_TEX_HASH_SEED);
        if (pal)
            h ^= nv2a_tex_hash(mem + t->palette, t->pal_len * 4u, NV2A_TEX_HASH_SEED) * 31u;
        e->gen = g->flips;
        if (h != e->hash)
            fresh = 1;
        e->hash = h;
    }
    if (fresh) {
        size_t n = nv2a_tex_texels(w, w, levels);
        int uploaded = 0;
        if (e->im.img)
            garbage_push(&e->im, VK_NULL_HANDLE, NULL);
        memset(&e->im, 0, sizeof e->im);
        e->side = side;
        memset(e->face_gen, 0, sizeof e->face_gen);
        if (!nvk_image_new_cube(&e->im, RT_FORMAT, side, levels,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return black_cube();
        if (n > s_texels_cap) {
            uint32_t *p = (uint32_t *)realloc(s_texels, n * 4);
            if (!p)
                return black_cube();
            s_texels = p;
            s_texels_cap = n;
        }
        for (f = 0; f < 6; f++) {
            const uint32_t *src = s_texels;
            if (frt[f])
                continue;
            nv2a_tex_decode(mem + t->offset + f * stride, t->color, w, w, t->pitch,
                            levels, pal, pal ? t->pal_len : 0, s_texels);
            if (side != w) {    /* beside drawn faces at render.scale: upscaled */
                uint8_t *up = (uint8_t *)malloc((size_t)side * side * 4u);
                if (up) {
                    nv2a_upscale_nearest32((const uint8_t *)s_texels, w * 4u, w, w,
                                           side / w, up, side * 4u);
                    upload_layer(&e->im, f, 0, side, side, up,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);
                    free(up);
                    uploaded = 1;
                }
                continue;
            }
            for (l = 0; l < levels; l++) {
                uint32_t lw = nv2a_tex_level_dim(w, l);
                upload_layer(&e->im, f, l, lw, lw, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0);
                src += (size_t)lw * lw;
            }
            uploaded = 1;
        }
        if (uploaded && !any)
            nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else if (!uploaded) {
            /* All faces drawn: the copies below start from an undefined
             * image; a face never copied reads black (cleared). */
            VkClearColorValue z;
            VkImageSubresourceRange r;
            memset(&z, 0, sizeof z);
            z.float32[3] = 1.0f;
            memset(&r, 0, sizeof r);
            r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.levelCount = VK_REMAINING_MIP_LEVELS;
            r.layerCount = VK_REMAINING_ARRAY_LAYERS;
            ensure_up();
            nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_up, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
            if (!any)
                nvk_image_to(s_up, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        s_cube_uploads++;
    }
    for (f = 0; f < 6; f++)
        if (frt[f] && frt[f]->gen != e->face_gen[f])
            ncopy++;
    if (ncopy) {
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &e->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (f = 0; f < 6; f++) {
            VkImageCopy ic;
            if (!frt[f] || frt[f]->gen == e->face_gen[f])
                continue;
            nvk_image_to(s_cmd, &frt[f]->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            memset(&ic, 0, sizeof ic);
            ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ic.srcSubresource.layerCount = 1;
            ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ic.dstSubresource.baseArrayLayer = f;
            ic.dstSubresource.layerCount = 1;
            ic.extent.width = side;
            ic.extent.height = side;
            ic.extent.depth = 1;
            vkCmdCopyImage(s_cmd, frt[f]->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
            e->face_gen[f] = frt[f]->gen;
            s_cube_copies++;
        }
    }
    if (e->im.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        end_encoder();
        ensure_cmd();
        nvk_image_to(s_cmd, &e->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    e->used = ++s_cube_clock;
    return e->im.view;
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
    const float host_bias = nv2a_host_lod_bias();   /* RECOMP_GFX_LOD_BIAS; 0 by default */
    for (k = 0; k < 4; k++) {
        const Stage *t = &s_stage[k];
        float b = t->levels > 1 && nv2a_tex_filter_mip(t->filter)
                ? nv2a_tex_lod_bias(t->filter) + host_bias : 0.0f;
        pc.tex_bias[k] = b > 15.99f ? 15.99f : b < -16.0f ? -16.0f : b;
    }
    {   /* Bump-env registers: methods 0x1B28..0x1B3C of each stage block. */
        const uint32_t *tr = nv2a_pb_tex_regs();
        for (k = 0; k < 4; k++) {
            const uint32_t *r = &tr[(0x40u * k + 0x28u) / 4];
            memcpy(pc.bump_mat[k], r, 16);
            memcpy(&pc.bump_lum[k][0], &r[4], 4);
            memcpy(&pc.bump_lum[k][1], &r[5], 4);
            pc.bump_lum[k][2] = pc.bump_lum[k][3] = 0.0f;
        }
    }
    fmod = fs_select(v);
    s_signed_on = fmod != s_fs_basic;   /* fs_basic has no signed channels */
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
        s_draw_2d = 0;
        if (nv2a_texscale_factor() || ui_scale_log()) {
            const struct nv2a_pb_present *pr = nv2a_pb_present_state();
            struct nv2a_texscale_draw td2 = { d->program, v->depth_enable,
                                              v->depth_enable && v->depth_mask,
                                              rt->w, rt->h, pr->w, pr->h };
            s_draw_2d = nv2a_texscale_draw_is_2d(&td2);
        }
        for (k = 0; k < 4; k++) {
            const Stage *t = &s_stage[k];
            uint32_t sw, sh;
            sview[k] = s_black.view;
            ssmp[k] = sampler(t, 0);
            if (!t->mode || t->mode == 4)
                continue;
            textured = 1;
            if (t->mode == 3 && t->cube && fmod != s_fs_basic) {   /* samplerCube */
                sview[k] = cube_bind(t);
            } else if ((t->mode == 3 || !stage_signed(t))
                       && rt_texture(t, rt, &sview[k], &sw, &sh) != NULL) {
                if (!nv2a_tex_size_from_format(t->color)) {
                    pc.b.tex_scale[k][0] = 1.0f / (float)sw;
                    pc.b.tex_scale[k][1] = 1.0f / (float)sh;
                }
                s_rtt_binds++;
            } else {
                sview[k] = tex_bind(t);
                if (s_bind_scale)
                    ssmp[k] = sampler(t, s_bind_scale);
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
    {
        /* The draw's dynamic state, and what of it differs from what this
         * command buffer already has (every pipeline here leaves the same
         * states dynamic, so they outlive a bind and a pass): a title draws
         * hundreds of batches in a row with the same depth, stencil, cull
         * and viewport state, and each vkCmdSet* is a call into the driver. */
        DynState n;
        int z = s_enc_dt && use_z && v->depth_enable;
        int st = s_enc_dt && use_s;
        VkRect2D sr;
        if (!draw_scissor(g, rt, &sr)) {
            s_draws_skipped++;
            return;
        }
        memset(&n, 0, sizeof n);
        n.pso = pso;
        /* Depth and stencil only with an attachment and when enabled; the
         * compare and op codes are in VkCompareOp / VkStencilOp order. */
        n.ztest = z ? VK_TRUE : VK_FALSE;
        n.zwrite = z && v->depth_mask ? VK_TRUE : VK_FALSE;
        n.zcmp = z ? (VkCompareOp)nv2a_cmp_from_gl(v->depth_func) : VK_COMPARE_OP_ALWAYS;
        n.stest = st ? VK_TRUE : VK_FALSE;
        n.sop[0] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[0]);
        n.sop[1] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[2]);
        n.sop[2] = (VkStencilOp)nv2a_stencil_op_from_gl(v->stencil_op[1]);
        n.scmp = (VkCompareOp)(v->stencil_func & 7u);
        n.srmask = v->stencil_rmask & 0xFFu;
        n.swmask = v->stencil_wmask & 0xFFu;
        n.sref = st ? (v->stencil_ref & 0xFFu) : 0u;
        /* BO-10 polygon offset, as Metal: the constant part in the vertex
         * stage (vp_scale.w); the slope part per host pixel, so times
         * render.scale (xemu PR #2240). */
        n.slope = (s_enc_dt && use_z && v->poly_offset)
                ? v->poly_factor * (float)nv2a_host_render_scale_x2() * 0.5f : 0.0f;
        /* A negative height: D3D's y-up clip space onto rows from the top,
         * as Metal's viewport. */
        n.vw = rt->hw;
        n.vh = rt->hh;
        n.sr = sr;
        n.front = v->front_face != 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                         : VK_FRONT_FACE_CLOCKWISE;
        n.cull = cull == 1 ? VK_CULL_MODE_BACK_BIT
               : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        (void)nv2a_blend_constant(g->blend_enable, g->blend_sfactor,
                                  g->blend_dfactor, g->blend_color, n.bf, &(int){0});
        if (s_enc_dt && use_z)
            s_draws_z++;
#define DYN(cond) (!s_dyn_valid || (cond))
        if (DYN(n.pso != s_dyn.pso))
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        if (DYN(n.ztest != s_dyn.ztest))
            vkCmdSetDepthTestEnable(s_cmd, n.ztest);
        if (DYN(n.zwrite != s_dyn.zwrite))
            vkCmdSetDepthWriteEnable(s_cmd, n.zwrite);
        if (DYN(n.zcmp != s_dyn.zcmp))
            vkCmdSetDepthCompareOp(s_cmd, n.zcmp);
        if (DYN(n.stest != s_dyn.stest))
            vkCmdSetStencilTestEnable(s_cmd, n.stest);
        if (DYN(memcmp(n.sop, s_dyn.sop, sizeof n.sop) || n.scmp != s_dyn.scmp))
            vkCmdSetStencilOp(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.sop[0], n.sop[1],
                              n.sop[2], n.scmp);
        if (DYN(n.srmask != s_dyn.srmask))
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.srmask);
        if (DYN(n.swmask != s_dyn.swmask))
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.swmask);
        if (DYN(n.sref != s_dyn.sref))
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_FRONT_AND_BACK, n.sref);
        if (DYN(memcmp(&n.slope, &s_dyn.slope, sizeof n.slope))) {
            vkCmdSetDepthBiasEnable(s_cmd, n.slope != 0.0f ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthBias(s_cmd, 0.0f, 0.0f, n.slope);
        }
        if (DYN(n.vw != s_dyn.vw || n.vh != s_dyn.vh)) {
            VkViewport vp;
            vp.x = 0.0f;
            vp.y = (float)rt->hh;
            vp.width = (float)rt->hw;
            vp.height = -(float)rt->hh;
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(s_cmd, 0, 1, &vp);
        }
        if (DYN(memcmp(&n.sr, &s_dyn.sr, sizeof n.sr)))
            vkCmdSetScissor(s_cmd, 0, 1, &n.sr);
        if (DYN(n.front != s_dyn.front))
            vkCmdSetFrontFace(s_cmd, n.front);
        if (DYN(n.cull != s_dyn.cull))
            vkCmdSetCullMode(s_cmd, n.cull);
        if (DYN(memcmp(n.bf, s_dyn.bf, sizeof n.bf)))
            vkCmdSetBlendConstants(s_cmd, n.bf);
#undef DYN
        s_dyn = n;
        s_dyn_valid = 1;
    }
    dyn_off[0] = (uint32_t)uo[0];
    dyn_off[1] = (uint32_t)uo[1];
    dyn_off[2] = (uint32_t)uo[2];
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1,
                            &s_set0, 3, dyn_off);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1,
                            &tset, 0, NULL);
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring[RING_VB].buf, &vb_off);
    vkCmdBindIndexBuffer(s_cmd, s_ring[RING_IB].buf, ib_off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(s_cmd, nidx, 1, 0, 0, 0);
    s_draws++;
    if (s_vf) {
        s_vf->draws++;
        s_vf->pass_draws++;
    }
    rt->gen = ++s_rt_seq;
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
        flush(1, FL_READBACK);
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
    flush(1, FL_READBACK);
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
    static uint8_t *frac;
    static size_t frac_cap;

    /* A fractional scale (1.5x): to guest size first, nearest, then as an
     * unscaled target. */
    if (rt->hw % rt->w != 0 || rt->hh != rt->h * n) {
        size_t need = (size_t)w * rt->h * 4u;
        if (frac_cap < need) {
            uint8_t *nf = (uint8_t *)realloc(frac, need);
            if (!nf)
                return;
            frac = nf;
            frac_cap = need;
        }
        nv2a_resample_nearest32(px, rt->hw * 4u, rt->hw, rt->hh, frac, w * 4u, w, rt->h);
        px = frac;
        n = 1;
    }

    if (rt_swz_ok(rt->swz, rt->w, rt->h) && (bpp == 4 || bpp == 2)) {
        /* Swizzled: linear first (box-filtered when scaled), then Z order. */
        uint32_t *lin = (uint32_t *)swz_staging((size_t)w * rt->h * 4u);
        uint32_t i;
        if (!lin)
            return;
        if (n == 1)
            memcpy(lin, px, (size_t)w * rt->h * 4u);
        else
            nv2a_downscale_box32(px, rt->hw * 4u, w, rt->h, n, (uint8_t *)lin, w * 4u);
        if (bpp == 2) {
            uint16_t *o = (uint16_t *)lin;   /* in place: each texel shrinks */
            for (i = 0; i < w * rt->h; i++) {
                uint32_t c = lin[i];
                o[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
        xbox_swizzle_rect(dst, lin, w, rt->h, bpp);
        s_writebacks++;
        return;
    }
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
            flush(0, FL_POLL);
        }
    }
    /* Submitted. With a cheap poll the counts are read once their submits
     * are done; without one the query pool itself says which are there. */
    if (poll_ok()) {
        done = serial_done();
        for (i = 0; i < q->n; i++)
            if (q->serial[i] > done)
                return 0;
    }
    for (i = 0; i < q->n; i++) {
        uint64_t v = 0;
        uint64_t t0 = s_vf ? now_ns() : 0;
        VkResult qr = vkGetQueryPoolResults(s_dev, s_vkq, (uint32_t)q->slot[i], 1, sizeof v,
                                            &v, sizeof v, VK_QUERY_RESULT_64_BIT);
        if (s_vf) {
            s_vf->qres++;
            s_vf->qres_ns += now_ns() - t0;
        }
        if (qr == VK_SUCCESS)
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
    if (!s_occ.ops) {
        nv2a_occ_init(&s_occ, &s_occ_ops, (uint8_t *)xbox_GetMemoryOffset());
        nv2a_occ_set_scale_x2(&s_occ, nv2a_host_render_scale_x2());
    }
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
        if (s_pass && batch_draws()) {
            /* vk_batch: the open pass is not ended for it; the counts go to
             * the GPU with the next submit, at the latest when the frame is
             * complete (or from on_poll, if the title seems to wait). */
            if (!s_report_defer) {
                s_report_defer = 1;
                s_report_defer_t = now_ns();
            }
        } else {
            flush(0, FL_REPORT);
        }
    }
    return 1;
}

static void vk_on_poll(void)
{
    if (!s_occ.npend)
        return;
    /* A deferred report's counts are not asked for by ending the pass, unless
     * half a frame has gone and the title may be waiting for them. */
    if (s_report_defer && now_ns() - s_report_defer_t > 8000000ull)
        flush(0, FL_REPORT);
    nv2a_occ_poll(&s_occ, !s_report_defer);
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
    if (f->fence_serial) {
        /* vk_poll=0: the frame's fence, signalled long ago as a rule; the
         * wait is the question and, when the GPU is behind, the answer. */
        t0 = now_ns();
        if (vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, 5000000000ull) == VK_SUCCESS)
            serial_known(f->fence_serial);
        vkResetFences(s_dev, 1, &f->fence);
        f->fence_serial = 0;
        t0 = now_ns() - t0;
        if (t0 > 500000ull) {
            s_frame_waits++;
            s_frame_wait_ns += t0;
        }
        if (s_vf) {
            s_vf->waits++;
            s_vf->wait_ns += t0;
        }
    }
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

static int s_pc_save;                    /* the pre-warm thread asks for a save (atomic) */

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
    {
        /* What the driver kept of the file (it drops one of another driver). */
        size_t kept = 0;
        vkGetPipelineCacheData(s_dev, pc, &kept, NULL);
        fprintf(stderr, LOGP "pipeline cache: %s (%zu bytes, %zu kept by the driver)\n",
                n ? path : "empty", n, kept);
    }
    return pc;
}

/* ---- the key list and the pre-warm thread ---------------------------------------- */

static pthread_mutex_t s_pcf_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_pcf itself */
static int      s_pc_rescan;            /* asked for while one ran */
static uint64_t s_pc_warm[3], s_pc_warm_ns;
static char     s_pc_path[1100], s_pc_ver[256];

/* Appends a key the ack thread just built. */
static void pc_put(uint32_t type, const void *data, uint32_t len)
{
    pthread_mutex_lock(&s_pcf_mu);
    if (s_pcf && nv2a_pcache_put(s_pcf, type, data, len) < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, LOGP "pcache: cannot append; the key list is not written\n");
    }
    pthread_mutex_unlock(&s_pcf_mu);
}

/* A module of the tables by its key id, or VK_NULL_HANDLE. s_pc_mu held. */
static VkShaderModule module_by_kid(uint64_t kid, int fragment)
{
    int i;
    if (!fragment) {
        if (kid == PC_VS_PASS)
            return s_vs_pass;
        for (i = 0; i < VS_MAX; i++)
            if (s_vs[i].used && !s_vs[i].bad && s_vs[i].kid == kid)
                return s_vs[i].mod;
    } else {
        if (kid == PC_FS_BASIC)
            return s_fs_basic;
        for (i = 0; i < s_fs_n; i++)
            if (!s_fs[i].bad && s_fs[i].kid == kid)
                return s_fs[i].mod;
    }
    return VK_NULL_HANDLE;
}

/* Builds every key of the list into the ack thread's own tables: first the
 * shaders the list's pipelines use, then the pipelines, then the other
 * shaders (keys seeded from the title's executable that nothing drew yet:
 * nobody waits for those), so the ack thread finds them built: with the SPIR-V cache and the driver's pipeline cache warm this is
 * a few lookups, and after a driver update or a changed shader generator it
 * is the compiling the ack thread would otherwise do mid-game. One thread,
 * below the title's: it shares the machine with a title that is starting. */
static void *pc_prewarm(void *arg)
{
    static char glsl[65536];
    NV2AVshProgram *parsed = (NV2AVshProgram *)malloc(sizeof *parsed);
    (void)arg;
    do {
        nv2a_pcache *pc;
        size_t n, i, nneed;
        uint64_t t0 = now_ns(), nb[3] = {0, 0, 0}, *need;
        uint32_t listed[3] = {0, 0, 0};
        int pass;

        __atomic_store_n(&s_pc_rescan, 0, __ATOMIC_RELEASE);
        /* The list as it is on disk now (a pre-build may have appended to it
         * since the last pass). The ack thread's puts wait for the reopen. */
        pthread_mutex_lock(&s_pcf_mu);
        if (s_pcf) {
            char msg[256];
            nv2a_pcache_close(s_pcf);
            s_pcf = nv2a_pcache_open(s_pc_path, s_pc_ver, msg, sizeof msg);
        }
        pc = s_pcf;
        pthread_mutex_unlock(&s_pcf_mu);
        nvk_glsl_reload();
        n = nv2a_pcache_count(pc);
        for (i = 0; i < n; i++) {
            const void *d;
            uint32_t type, len;
            if (nv2a_pcache_get(pc, i, &type, &d, &len) && type <= PC_PSO)
                listed[type]++;
        }
        nv2a_pcache_stat_open(s_pc_path, listed);
        /* The shader ids the list's pipelines name. */
        need = (uint64_t *)malloc((listed[PC_PSO] ? listed[PC_PSO] : 1) * 2 * sizeof *need);
        nneed = 0;
        for (i = 0; need && i < n; i++) {
            const void *d;
            uint32_t type, len;
            if (nv2a_pcache_get(pc, i, &type, &d, &len) && type == PC_PSO
                    && len == sizeof(PsoDisk) && nneed + 2 <= (size_t)listed[PC_PSO] * 2) {
                memcpy(&need[nneed], d, 16);            /* PsoDisk's vs, fs */
                nneed += 2;
            }
        }
        for (pass = 0; parsed && pass < 3; pass++) {
            if (pass == 2)              /* the pipelines are in: nobody waits now */
                __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
            for (i = 0; i < n; i++) {
                const void *d;
                uint32_t type, len;
                size_t q;
                if (!nv2a_pcache_get(pc, i, &type, &d, &len))
                    continue;
                if (pass == 0 && (type == PC_VS || type == PC_FS)) {
                    uint64_t kid = vs_kid(d, len);      /* the id rule is one for both */
                    for (q = 0; q < nneed && need[q] != kid; q++)
                        ;
                    if (q == nneed)
                        continue;       /* pass 2 */
                }
                if (pass != 1 && type == PC_VS && len >= 16 && len % 16 == 0
                        && len <= NV2A_VSH_SLOTS * 16u) {
                    uint32_t hash = 2166136261u, k;
                    uint16_t inputs = 0;
                    VkShaderModule mod;
                    VsEntry *e;
                    for (k = 0; k < len / 4u; k++)      /* nv2a_vsh_program_hash */
                        hash = (hash ^ ((const uint32_t *)d)[k]) * 16777619u;
                    if (!hash)
                        hash = 1;
                    pthread_mutex_lock(&s_pc_mu);
                    e = vs_find(hash);
                    pthread_mutex_unlock(&s_pc_mu);
                    if (e || !(mod = vs_build((const uint32_t *)d, (int)(len / 16u), hash,
                                              &inputs, parsed, glsl, 1)))
                        continue;
                    pthread_mutex_lock(&s_pc_mu);
                    if (vs_find(hash) || !vs_take(hash, vs_kid(d, len), mod, inputs))
                        e = NULL;       /* the ack thread was first, or the table is full */
                    else
                        e = s_vs, nb[PC_VS]++;
                    pthread_mutex_unlock(&s_pc_mu);
                    if (!e)
                        vkDestroyShaderModule(s_dev, mod, NULL);
                } else if (pass != 1 && type == PC_FS && len == sizeof(NV2ACombinerState)) {
                    NV2ACombinerState st;
                    VkShaderModule mod;
                    uint32_t hash;
                    FsEntry *e;
                    memcpy(&st, d, sizeof st);
                    hash = fnv(&st, sizeof st);
                    pthread_mutex_lock(&s_pc_mu);
                    e = fs_find(hash, &st);
                    pthread_mutex_unlock(&s_pc_mu);
                    if (e || !(mod = fs_build(&st, hash, glsl, 1)))
                        continue;
                    pthread_mutex_lock(&s_pc_mu);
                    if (fs_find(hash, &st) || !fs_take(hash, &st, mod))
                        e = NULL;
                    else
                        e = s_fs, nb[PC_FS]++;
                    pthread_mutex_unlock(&s_pc_mu);
                    if (!e)
                        vkDestroyShaderModule(s_dev, mod, NULL);
                } else if (pass == 1 && type == PC_PSO && len == sizeof(PsoDisk)) {
                    PsoDisk dk;
                    PsoKey k;
                    VkPipeline p;
                    uint32_t at;
                    int found, full;
                    memcpy(&dk, d, sizeof dk);
                    memset(&k, 0, sizeof k);
                    pthread_mutex_lock(&s_pc_mu);
                    k.vs = (uint64_t)(uintptr_t)module_by_kid(dk.vs, 0);
                    k.fs = (uint64_t)(uintptr_t)module_by_kid(dk.fs, 1);
                    k.nf4 = dk.nf4;
                    k.topo = dk.topo;
                    k.depth = dk.depth;
                    k.wmask = dk.wmask;
                    memcpy(k.blend, dk.blend, sizeof k.blend);
                    found = 1;
                    full = 0;
                    if (k.vs && k.fs && dk.nf4 <= NV2A_VS_MAX_INPUTS) {
                        (void)pso_slot(&k, &found);
                        full = s_pso_n + 1 >= PSO_CAP * 3 / 4;
                    }
                    pthread_mutex_unlock(&s_pc_mu);
                    if (found || full || !(p = pipeline_new(&k, 1)))
                        continue;
                    pthread_mutex_lock(&s_pc_mu);
                    at = pso_slot(&k, &found);
                    if (!found) {
                        s_pso[at].k = k;
                        s_pso[at].p = p;
                        s_pso[at].used = 1;
                        s_pso_n++;
                        nb[PC_PSO]++;
                    }
                    pthread_mutex_unlock(&s_pc_mu);
                    if (found)          /* the ack thread was first */
                        vkDestroyPipeline(s_dev, p, NULL);
                }
            }
        }
        free(need);
        for (i = 0; i < 3; i++)
            s_pc_warm[i] += nb[i];
        s_pc_warm_ns += now_ns() - t0;
        fprintf(stderr, LOGP "pcache: pre-warmed %llu vs, %llu fs, %llu pipelines of %u/%u/%u"
                " on disk in %.0f ms (done at %.2f s)\n",
                (unsigned long long)nb[PC_VS], (unsigned long long)nb[PC_FS],
                (unsigned long long)nb[PC_PSO], listed[0], listed[1], listed[2],
                (double)(now_ns() - t0) / 1e6, (double)(now_ns() - s_t0_ns) / 1e9);
    } while (__atomic_load_n(&s_pc_rescan, __ATOMIC_ACQUIRE));
    free(parsed);
    __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
    nv2a_pcache_stat_warming(0);
    __atomic_store_n(&s_pc_warming, 0, __ATOMIC_RELEASE);
    /* What a pre-build made is on disk for the next start. */
    __atomic_store_n(&s_pc_save, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Starts the pre-warm thread, or has the one running go round again.
 * `first`: the one the backend starts with, whose pipelines the ack thread
 * may wait for (pc_wait_more). */
static void pc_warm_begin(int first)
{
    pthread_t th;
    __atomic_store_n(&s_pc_rescan, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&s_pc_warming, 1, __ATOMIC_ACQ_REL))
        return;
    nv2a_pcache_stat_warming(1);
    if (first) {
        __atomic_store_n(&s_pc_wait_until, now_ns() + 3000000000ull, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pc_warm_urgent, 1, __ATOMIC_RELEASE);
    }
    if (pthread_create(&th, NULL, pc_prewarm, NULL) == 0) {
        pthread_detach(th);
    } else {
        __atomic_store_n(&s_pc_warm_urgent, 0, __ATOMIC_RELEASE);
        nv2a_pcache_stat_warming(0);
        __atomic_store_n(&s_pc_warming, 0, __ATOMIC_RELEASE);
    }
}

/* nv2a_pcache_rescan: the list gained keys (a pre-build in another process). */
static void pc_warm_start(void)
{
    pc_warm_begin(0);
}

static void pc_start(void)
{
    const char *tr = recomp_env(RENV_PCACHE_TRACE);
    char dir[1024], msg[256];

    if (tr) {
        s_pc_trace = 1;
        if (strcmp(tr, "1") != 0) {
            if ((s_pc_csv = fopen(tr, "w")) != NULL)
                fprintf(s_pc_csv, "flip,t_ms,interval_ms,ack_compiles,ack_compile_ms\n");
            else
                fprintf(stderr, LOGP "pcache: cannot write %s\n", tr);
        }
    }
    if (!nvk_cache_dir(dir, sizeof dir)) {
        fprintf(stderr, LOGP "pcache: no disk cache (render.shader_cache)\n");
        return;
    }
    snprintf(s_pc_path, sizeof s_pc_path, "%s/vk_pipelines.bin", dir);
    /* What the keys mean, not the build (nv2a_pcache.h): a rebuild keeps
     * the list. Raise the number when a key changes meaning at its size. */
    snprintf(s_pc_ver, sizeof s_pc_ver, "vk pipelines 1; PsoDisk %zu; NV2ACombinerState %zu;"
             " vsh slots %d", sizeof(PsoDisk), sizeof(NV2ACombinerState),
             (int)NV2A_VSH_SLOTS);
    s_pcf = nv2a_pcache_open(s_pc_path, s_pc_ver, msg, sizeof msg);
    fprintf(stderr, LOGP "pcache: %s: %s\n", s_pc_path, msg);
    if (!s_pcf)
        return;
    {
        uint32_t none[3] = {0, 0, 0};
        nv2a_pcache_stat_open(s_pc_path, none);
    }
    nv2a_pcache_set_rescan(pc_warm_start);
    nv2a_pcache_set_seed(pc_put);
    if (nv2a_pcache_count(s_pcf))
        pc_warm_begin(1);
}

/* At every flip: the CSV line. */
static void pc_flip(uint64_t flip)
{
    static uint64_t last;
    uint64_t t, n, ns;
    if (!s_pc_csv)
        return;
    t = now_ns();
    pthread_mutex_lock(&s_pc_mu);
    n = s_cs.rt_flip_n;
    ns = s_cs.rt_flip_ns;
    s_cs.rt_flip_n = s_cs.rt_flip_ns = 0;
    pthread_mutex_unlock(&s_pc_mu);
    fprintf(s_pc_csv, "%llu,%.3f,%.3f,%llu,%.3f\n", (unsigned long long)flip,
            (double)(t - s_t0_ns) / 1e6, last ? (double)(t - last) / 1e6 : 0.0,
            (unsigned long long)n, (double)ns / 1e6);
    if ((flip % 60) == 0)
        fflush(s_pc_csv);
    last = t;
}

/* The present log's pipeline-cache line (the Metal backend's, field for field). */
static void pc_log(uint64_t presents)
{
    int i, nv = 0;
    pthread_mutex_lock(&s_pc_mu);
    for (i = 0; i < VS_MAX; i++)
        nv += s_vs[i].used;
    fprintf(stderr, LOGP "pcache at present #%llu: cached %d vs, %d fs, %u pipelines;"
            " ack-thread compiles %llu vs + %llu fs + %llu pipelines in %.1f ms (max %.1f,"
            " %llu in the first 10 s); pre-warm %llu + %llu + %llu in %.1f ms (%s);"
            " start-up waits %llu, %.1f ms\n",
            (unsigned long long)presents, nv, s_fs_n, s_pso_n,
            (unsigned long long)s_cs.n[0][PC_VS], (unsigned long long)s_cs.n[0][PC_FS],
            (unsigned long long)s_cs.n[0][PC_PSO],
            (double)(s_cs.ns[0][0] + s_cs.ns[0][1] + s_cs.ns[0][2]) / 1e6, s_cs.rt_max_ms,
            (unsigned long long)s_cs.rt_10s,
            (unsigned long long)s_cs.n[1][PC_VS], (unsigned long long)s_cs.n[1][PC_FS],
            (unsigned long long)s_cs.n[1][PC_PSO],
            (double)(s_cs.ns[1][0] + s_cs.ns[1][1] + s_cs.ns[1][2]) / 1e6,
            __atomic_load_n(&s_pc_warming, __ATOMIC_ACQUIRE) ? "running" : "done",
            (unsigned long long)s_pc_waits, (double)s_pc_wait_ns / 1e6);
    pthread_mutex_unlock(&s_pc_mu);
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
        if (!poll_ok()) {
            VkFenceCreateInfo fci;
            memset(&fci, 0, sizeof fci);
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(s_dev, &fci, NULL, &s_fr[i].fence) != VK_SUCCESS)
                s_fr[i].fence = VK_NULL_HANDLE;
        }
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
    s_fs = (FsEntry *)calloc(FS_MAX, sizeof *s_fs);
    if (!s_pso || !s_fs)
        return 0;
    s_pcache = pcache_load();
    s_vs_pass = module_from_glsl(NVK_STAGE_VERTEX, s_vs_pass_glsl, "vs_pass");
    s_fs_basic = module_from_glsl(NVK_STAGE_FRAGMENT, s_fs_basic_glsl, "fs_basic");
    if (!s_vs_pass || !s_fs_basic) {
        fprintf(stderr, LOGP "base shaders failed\n");
        return 0;
    }
    pc_start();
    if (!solid_texture(&s_black, 0xFF000000u) || !solid_texture(&s_magenta, NV2A_TEX_MAGENTA)) {
        fprintf(stderr, LOGP "base textures failed\n");
        return 0;
    }
    atexit(at_exit_save);
    fprintf(stderr, LOGP "backend up on %s: %d frames in flight, present %s, write-back %s,"
            " occlusion %s%s, GPU progress %s\n", C->name, s_frames,
            s_win ? "window slots" : C->surface ? "texture readback" : "headless",
            wb_always() ? "every flip" : "on demand",
            s_vkq ? "queries" : "fixed", s_qprecise ? " (precise)" : "",
            poll_ok() ? "polled (timeline semaphore)" : "from a fence per frame (vk_poll=0)");
    s_failed = 0;
    /* graphics.ui_scale switched on in the overlay: the loaded 2D textures
     * are prepared while the title is held (scale_prepare_loaded). */
    xbox_HostPauseIdleFn(scale_prepare_loaded);
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

/* vk_frame: the file and the timestamp pool, at the first flip. */
static void vf_open(void)
{
    static int tried;
    const char *path;
    if (tried)
        return;
    tried = 1;
    path = recomp_env(RENV_VK_FRAME);
    if (!path || !*path || !strcmp(path, "0") || !strcmp(path, "1"))
        return;
    s_vf_out = fopen(path, "w");
    if (!s_vf_out) {
        fprintf(stderr, LOGP "vk_frame: cannot write %s\n", path);
        return;
    }
    if (C->props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = VF_RING * 2 * VF_TS;
        if (vkCreateQueryPool(s_dev, &qi, NULL, &s_vf_qp) != VK_SUCCESS)
            s_vf_qp = VK_NULL_HANDLE;
        else
            vkResetQueryPool(s_dev, s_vf_qp, 0, VF_RING * 2 * VF_TS);
    }
    fprintf(s_vf_out, "present,flip,t_ms,draws,passes,p_clear,p_load,p_resumed,p_empty,"
            "p_maxdraws,rt_switch,zload,zresumed,s_flip,s_ready,s_report,s_poll,s_ring,"
            "s_sampler,s_readback,s_batch,submit_ms,submit_max_ms,qlock_ms,polls,poll_ms,"
            "poll_max_ms,waits,wait_ms,qres,qres_ms,endpass_ms,f_total,f_back,f_copy,"
            "f_submit,f_pub,f_adv,ready_done,g_n,g_first_ms,g_last_ms,g_active_ms\n");
    memset(s_vfr, 0, sizeof s_vfr);
    s_vf = &s_vfr[0];
    fprintf(stderr, LOGP "vk_frame: one line per flip into %s (GPU timestamps %s)\n", path,
            s_vf_qp ? "on" : "not available");
}

/* The frame just presented is closed; the one VF_LAG flips back is written
 * with its GPU times, and the next slot is made ready. */
static void vf_flip(void)
{
    static uint64_t g0;
    VkFrameRec *done = s_vf, *w;
    int idx = (int)(s_vf - s_vfr), wi = (idx + VF_RING - VF_LAG) % VF_RING, k;
    int ni = (idx + 1) % VF_RING;

    done->present = s_presents;
    done->flip = nv2a_pb_gpu_state()->flips;
    done->t_ns = now_ns();
    w = &s_vfr[wi];
    if (w->t_ns) {
        uint64_t ts[2 * VF_TS][2];
        double per = (double)C->props.limits.timestampPeriod * 1e-6, first = 0, last = 0,
               act = 0;
        uint32_t n = 0;
        memset(ts, 0, sizeof ts);
        if (s_vf_qp && w->nts
                && vkGetQueryPoolResults(s_dev, s_vf_qp, (uint32_t)(wi * 2 * VF_TS), 2 * w->nts,
                                         sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT
                                         | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) >= 0) {
            for (k = 0; k < (int)w->nts; k++) {
                if (!ts[2 * k][1] || !ts[2 * k + 1][1])
                    continue;
                if (!g0)
                    g0 = ts[2 * k][0];
                if (!n)
                    first = (double)(int64_t)(ts[2 * k][0] - g0) * per;
                last = (double)(int64_t)(ts[2 * k + 1][0] - g0) * per;
                act += (double)(int64_t)(ts[2 * k + 1][0] - ts[2 * k][0]) * per;
                n++;
            }
        }
        fprintf(s_vf_out, "%llu,%u,%.3f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                "%.3f,%.3f,%.3f,%u,%.3f,%.3f,%u,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                "%.3f,%u,%u,%.3f,%.3f,%.3f\n",
                (unsigned long long)w->present, w->flip, w->t_ns / 1e6, w->draws, w->passes,
                w->p_clear, w->p_load, w->p_resumed, w->p_empty, w->p_maxdraws, w->rt_switch,
                w->zload, w->zresumed, w->sub[FL_FLIP], w->sub[FL_READY], w->sub[FL_REPORT],
                w->sub[FL_POLL], w->sub[FL_RING], w->sub[FL_SAMPLER], w->sub[FL_READBACK],
                w->sub[FL_BATCH],
                w->submit_ns / 1e6, w->submit_max_ns / 1e6, w->qlock_ns / 1e6, w->polls,
                w->poll_ns / 1e6, w->poll_max_ns / 1e6, w->waits, w->wait_ns / 1e6, w->qres,
                w->qres_ns / 1e6, w->endpass_ns / 1e6, w->f_total / 1e6, w->f_back / 1e6,
                w->f_copy / 1e6, w->f_submit / 1e6, w->f_pub / 1e6, w->f_adv / 1e6,
                w->ready_done, n, first, last, act);
        if ((w->present & 63) == 0)
            fflush(s_vf_out);
    }
    s_vf = &s_vfr[ni];
    memset(s_vf, 0, sizeof *s_vf);
    if (s_vf_qp)
        vkResetQueryPool(s_dev, s_vf_qp, (uint32_t)(ni * 2 * VF_TS), 2 * VF_TS);
    s_vf_last_rt = NULL;
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
        uint64_t vt0 = 0, vt1 = 0, vt2 = 0, vt3 = 0, vt4 = 0;
        vf_open();
        if (s_vf) {
            /* Is the GPU done with what the frame has submitted so far? */
            vt0 = now_ns();
            s_vf->ready_done = s_done_serial >= s_next_serial - 1;
        }
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
            if (s_vf)
                vt1 = now_ns();
            slot = (struct nvk_image *)xbox_FramebufferWindowBackTexture(hcw, hch);
            if (s_vf)
                vt2 = now_ns();
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
                if (s_vf)
                    vt3 = now_ns();
                /* The frame's last submit, unless a readback follows. */
                s_flush_fence = !(rt && wb);
                flush(0, FL_FLIP);
                if (s_vf)
                    vt4 = now_ns();
                xbox_FramebufferWindowPublishTexture();
                if (s_vf) {
                    s_vf->f_back = vt2 - vt1;
                    s_vf->f_copy = vt3 - vt2;
                    s_vf->f_submit = vt4 - vt3;
                    s_vf->f_pub = now_ns() - vt4;
                }
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
        s_flush_fence = 1;
        flush(0, FL_FLIP);
        t = prof ? now_ns() : 0;
        {
            uint64_t w0 = s_frame_waits;
            uint64_t va = s_vf ? now_ns() : 0;
            frame_advance();
            if (s_vf) {
                uint64_t vn = now_ns();
                s_vf->f_adv = vn - va;
                s_vf->f_total = vn - vt0;
                vf_flip();
            }
            if (prof) {
                s_pa.wait += now_ns() - t;
                s_pa.waits += s_frame_waits - w0;
            }
        }
        if ((s_pso_since_save && (s_presents % 600) == 599)
                || __atomic_exchange_n(&s_pc_save, 0, __ATOMIC_ACQ_REL))
            pcache_save();
    }
    if (s_pc_trace)
        tex_upload_log();
    s_presents++;
#if defined(__ANDROID__)
    if (s_presents == 120 && !s_failed) {
        /* A driver on trial (the app's GpuDriver) has drawn two seconds of
         * frames: it stays. */
        const char *trial = getenv("RECOMP_ANDROID_DRIVER_TRIAL");
        if (trial && *trial && remove(trial) == 0)
            fprintf(stderr, LOGP "the chosen driver has shown its first frames: kept\n");
    }
#endif
    pc_flip(s_presents);
    flip_log(surface_offset, rt);
    if (s_presents == 1 || (s_presents % 300) == 0)
        pc_log(s_presents);
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

/* FLIP_STALL reached: submit what the frame has recorded so the GPU draws it
 * while the executor holds for the vblank (BO-74, as Metal's
 * metal_on_flip_ready); on_flip then adds only the slot copy. */
static void vk_on_flip_ready(void)
{
    if (!s_failed && (s_cmd || s_up))
        flush(0, FL_READY);
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
    vk_on_flip_ready,
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
