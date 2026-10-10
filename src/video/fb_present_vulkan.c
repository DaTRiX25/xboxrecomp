/**
 * The SDL window's Vulkan present: the zero-copy half of fb_present_sdl.c
 * when the Vulkan backend draws (RECOMP_PB_BACKEND=vulkan, not
 * RECOMP_DEBUG=vk_present=readback). The Vulkan sibling of
 * fb_present_metal.m, with the same split:
 *
 *   device         made here at window creation, before the guest starts
 *                  (nvk_core_init with the window's surface, made by
 *                  SDL_Vulkan_CreateSurface); the backend adopts it at its
 *                  lazy init, so a slot image takes the backend's copy and a
 *                  present on the same queue runs after the copy it shows
 *                  (submission order; the queue lock orders the threads).
 *   slots          three BGRA8 images, frame-sized (render.scale's host
 *                  size), kept in VK_IMAGE_LAYOUT_GENERAL. The backend copies
 *                  its present target into the back slot on the GPU; a
 *                  guest-bytes frame is uploaded into it.
 *   present        on the main thread: the swapchain's next image, cleared to
 *                  black, the slot blitted into nv2a_present_rect's rectangle,
 *                  nearest or linear as the SDL path filters. FIFO when vsync
 *                  is on (the default), so the display paces the main thread
 *                  and never the walker, as Metal's displaySyncEnabled.
 *   shot           RECOMP_WINDOW_SHOT: the swapchain image read back after
 *                  the blit.
 *   latency        the swapchain has the fewest images the driver allows
 *                  (minImageCount, at least 2): with FIFO, one image on
 *                  screen and at most one queued, so presenting adds at most
 *                  one frame (BO-74). The slot hand-off drops a stale frame
 *                  rather than queueing it, as on Metal.
 *   timing         every present's host time, and with VK_KHR_present_id +
 *                  VK_KHR_present_wait (RADV, MoltenVK) the time each frame
 *                  reached the display, from a waiter thread: present-to-
 *                  present intervals (p50/p95/p99, frames past 1.5 refresh
 *                  periods), every 5 s under RECOMP_TRACE=present_stats and
 *                  at exit; RECOMP_TRACE=vk_present=<file> writes one CSV line
 *                  per frame. The last displayed vblank is
 *                  fbv_display_vblank().
 *
 * Thread use: fbv_create/fbv_destroy/fbv_present on the main thread,
 * fbv_slot/fbv_upload on the NV2A ack thread (each with its own command
 * pool). Every submit and present holds nvk_queue_lock.
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <SDL_vulkan.h>
#include <dlfcn.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d/nv2a_vk_core.h"
#include "recomp_env.h"
#include "latency_trace.h"

uint64_t xbox_HostNowNs(void);
void xbox_PacingPresent(uint64_t when_ns);
void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns);
#include "kernel/nv2a_backend_common.h"
#include "fb_present_vulkan.h"

#define LOGP "[PRESENT] "
#define SC_MAX 8

static const struct nvk_core *C;
static VkSwapchainKHR   s_sc;
static VkImage          s_sc_img[SC_MAX];
static uint32_t         s_sc_n;
static VkFormat         s_sc_fmt;
static VkExtent2D       s_sc_ext;
static VkPresentModeKHR s_sc_mode;
static int              s_sc_shots;      /* TRANSFER_SRC usage: shots possible */
static int              s_recreate;
static int              s_vsync, s_shots;

static VkCommandPool    s_pool;          /* main thread */
static VkCommandBuffer  s_cb[2];
static VkFence          s_fence[2];
static VkSemaphore      s_acq[2];
static VkSemaphore      s_done[SC_MAX];
static int              s_fi;

static struct nvk_image s_slot[3];       /* GENERAL once written */

static VkCommandPool     s_up_pool;      /* ack thread */
static VkCommandBuffer   s_up_cb;
static VkFence           s_up_fence;
static int               s_up_pending;
static struct nvk_buffer s_up_buf;
static struct nvk_buffer s_shot_buf;

/* ---- present timing ----------------------------------------------------- */

#define IV_N 8192
typedef struct {
    float    iv[IV_N];      /* ms between consecutive frames */
    uint32_t n;             /* filled */
    uint32_t pos;
    uint64_t last_ns, count;
} Intervals;
static Intervals       s_host_iv, s_disp_iv;
static pthread_mutex_t s_tm_mu = PTHREAD_MUTEX_INITIALIZER;   /* the stats */
static pthread_mutex_t s_sc_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_sc vs the waiter */
static pthread_mutex_t s_id_mu = PTHREAD_MUTEX_INITIALIZER;   /* s_present_id */
static pthread_cond_t  s_sc_cv = PTHREAD_COND_INITIALIZER;
static uint64_t        s_present_id;        /* last id presented */
static uint64_t        s_sc_first_id;       /* the first id of this swapchain */
static int             s_waiter_run;
static pthread_t       s_waiter;
static FILE           *s_csv;
#define ID_RING 64
static uint32_t        s_id_flip[ID_RING];   /* present id -> the flip it showed */
static uint64_t        s_vbl_ns, s_vbl_period_ns;
static PFN_vkWaitForPresentKHR s_wait_present;   /* a device function */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void iv_add(Intervals *v, uint64_t t)
{
    if (v->last_ns && t > v->last_ns) {
        v->iv[v->pos] = (float)((double)(t - v->last_ns) / 1e6);
        v->pos = (v->pos + 1) % IV_N;
        if (v->n < IV_N)
            v->n++;
    }
    v->last_ns = t;
    v->count++;
}

static int fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* p50/p95/p99/max and the share past 1.5 periods, of the last IV_N. */
static void iv_line(const char *what, Intervals *v, char *out, size_t n)
{
    static float tmp[IV_N];
    uint32_t i, miss = 0;
    double period = 1000.0 / 60.0;
    if (!v->n) {
        snprintf(out, n, "%s: none", what);
        return;
    }
    memcpy(tmp, v->iv, v->n * sizeof *tmp);
    qsort(tmp, v->n, sizeof *tmp, fcmp);
    for (i = 0; i < v->n; i++)
        miss += tmp[i] > 1.5 * period;
    snprintf(out, n, "%s: %u intervals p50 %.2f p95 %.2f p99 %.2f max %.2f ms, %.2f%% past"
             " 25 ms", what, v->n, tmp[v->n / 2], tmp[(uint32_t)(v->n * 0.95)],
             tmp[(uint32_t)(v->n * 0.99)], tmp[v->n - 1], 100.0 * miss / v->n);
}

void fbv_timing_report(const char *when)
{
    char a[256], b[256];
    pthread_mutex_lock(&s_tm_mu);
    iv_line("host presents", &s_host_iv, a, sizeof a);
    iv_line("displayed", &s_disp_iv, b, sizeof b);
    pthread_mutex_unlock(&s_tm_mu);
    fprintf(stderr, LOGP "Vulkan %s: %s; %s%s\n", when, a, b,
            C && C->present_wait ? "" : " (no VK_KHR_present_wait)");
}

/* The last vblank a frame was shown at (CLOCK_MONOTONIC ns) and the
 * measured refresh period; 0 when not known (no present_wait). */
int fbv_display_vblank(uint64_t *t_ns, uint64_t *period_ns)
{
    uint64_t t = __atomic_load_n(&s_vbl_ns, __ATOMIC_ACQUIRE);
    *t_ns = t;
    *period_ns = __atomic_load_n(&s_vbl_period_ns, __ATOMIC_RELAXED);
    return t != 0;
}

/* Waits for each present id in turn to reach the display. s_id_mu guards
 * the id counter (held for instants); s_sc_mu is held across one wait (20 ms
 * at most), so the swapchain is not destroyed under it. */
static void *waiter_main(void *arg)
{
    uint64_t next = 1, prev_t = 0;
    (void)arg;
    for (;;) {
        VkResult r;
        pthread_mutex_lock(&s_id_mu);
        while (s_waiter_run && next > s_present_id)
            pthread_cond_wait(&s_sc_cv, &s_id_mu);
        pthread_mutex_unlock(&s_id_mu);
        if (!s_waiter_run)
            break;
        pthread_mutex_lock(&s_sc_mu);
        if (next < s_sc_first_id)
            next = s_sc_first_id;
        r = s_sc && next <= s_present_id
            ? s_wait_present(C->dev, s_sc, next, 20000000ull) : VK_NOT_READY;
        pthread_mutex_unlock(&s_sc_mu);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            uint64_t t = mono_ns();
            pthread_mutex_lock(&s_tm_mu);
            iv_add(&s_disp_iv, t);
            pthread_mutex_unlock(&s_tm_mu);
            if (prev_t && t - prev_t < 60000000ull) {
                /* A smoothed refresh period from single-period steps. */
                uint64_t d = t - prev_t, p = s_vbl_period_ns;
                if (!p)
                    p = d;
                else if (d < p * 3 / 2)
                    p = (p * 15 + d) / 16;
                __atomic_store_n(&s_vbl_period_ns, p, __ATOMIC_RELAXED);
            }
            __atomic_store_n(&s_vbl_ns, t, __ATOMIC_RELEASE);
            /* The real display refresh, for RECOMP_VBLANK_SYNC=display (the
             * emulated vblank's edges snap to it) and the pacing trace. A
             * frame shown is a refresh; FIFO shows every frame. */
            xbox_VblankDisplayRefresh(t, __atomic_load_n(&s_vbl_period_ns, __ATOMIC_RELAXED));
            xbox_PacingPresent(t);
            if (xbox_LatOn()) {
                char b[32];
                snprintf(b, sizeof b, "%llu", (unsigned long long)t);
                xbox_LatRow('D', s_id_flip[next % ID_RING], t, b);
            }
            if (s_csv)
                fprintf(s_csv, "%llu,display,%llu,\n", (unsigned long long)next,
                        (unsigned long long)t);
            prev_t = t;
            next++;
        } else if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            next = s_present_id + 1;    /* out of date: the next swapchain */
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int fbv_load_library(char *err, size_t errn)
{
    Dl_info di;
    const char *path = NULL;

    /* The loader linked into this binary: SDL_Vulkan_CreateSurface must use
     * the same instance functions as the instance made here. */
    if (dladdr((void *)&vkGetInstanceProcAddr, &di) && di.dli_fname)
        path = di.dli_fname;
    if (SDL_Vulkan_LoadLibrary(path) != 0 && (!path || SDL_Vulkan_LoadLibrary(NULL) != 0)) {
        snprintf(err, errn, "SDL_Vulkan_LoadLibrary(%s): %s", path ? path : "default",
                 SDL_GetError());
        return 0;
    }
    return 1;
}

static VkSurfaceKHR make_surface(VkInstance inst, void *ctx)
{
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)ctx, inst, &s)) {
        fprintf(stderr, LOGP "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return s;
}

static void destroy_swapchain(void)
{
    uint32_t i;
    for (i = 0; i < SC_MAX; i++) {
        if (s_done[i])
            vkDestroySemaphore(C->dev, s_done[i], NULL);
        s_done[i] = VK_NULL_HANDLE;
    }
    if (s_sc)
        vkDestroySwapchainKHR(C->dev, s_sc, NULL);
    s_sc = VK_NULL_HANDLE;
    s_sc_n = 0;
}

static void queue_idle(void)
{
    nvk_queue_lock();
    vkQueueWaitIdle(C->queue);
    nvk_queue_unlock();
}

static int make_swapchain(SDL_Window *win)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16];
    uint32_t nf = 64, nm = 16, i, n;
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = s_sc;
    VkSemaphoreCreateInfo si;
    int dw = 0, dh = 0;
    VkResult r;

    queue_idle();
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(C->phys, C->surface, &caps) != VK_SUCCESS)
        return 0;
    pthread_mutex_lock(&s_sc_mu);
    vkGetPhysicalDeviceSurfaceFormatsKHR(C->phys, C->surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(C->phys, C->surface, &nm, modes);
    /* UNORM: the frame's bytes go to the display as they are, as the SDL
     * renderer and the CAMetalLayer present show them. */
    s_sc_fmt = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM
                || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_sc_fmt = fmts[i].format;
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
                break;
        }
    s_sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!s_vsync)
        for (i = 0; i < nm; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                s_sc_mode = modes[i];
                break;
            }
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                s_sc_mode = modes[i];
        }
    SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        s_sc_ext = caps.currentExtent;
    } else {
        s_sc_ext.width = (uint32_t)(dw > 0 ? dw : 640);
        s_sc_ext.height = (uint32_t)(dh > 0 ? dh : 480);
    }
    if (s_sc_ext.width < caps.minImageExtent.width) s_sc_ext.width = caps.minImageExtent.width;
    if (s_sc_ext.height < caps.minImageExtent.height) s_sc_ext.height = caps.minImageExtent.height;
    if (s_sc_ext.width > caps.maxImageExtent.width) s_sc_ext.width = caps.maxImageExtent.width;
    if (s_sc_ext.height > caps.maxImageExtent.height) s_sc_ext.height = caps.maxImageExtent.height;
    if (!s_sc_ext.width || !s_sc_ext.height) {
        pthread_mutex_unlock(&s_sc_mu);
        return 0;     /* minimised */
    }
    /* The fewest images: one shown, at most one queued (BO-74 latency). */
    n = caps.minImageCount;
    if (n < 2) n = 2;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    if (n > SC_MAX) n = SC_MAX;
    s_sc_shots = s_shots && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = C->surface;
    ci.minImageCount = n;
    ci.imageFormat = s_sc_fmt;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = s_sc_ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  | (s_sc_shots ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha
                                                     & -caps.supportedCompositeAlpha);
    ci.presentMode = s_sc_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(C->dev, &ci, NULL, &s_sc);
    if (old) {
        VkSwapchainKHR keep = s_sc;
        s_sc = old;
        destroy_swapchain();
        s_sc = r == VK_SUCCESS ? keep : VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        s_sc = VK_NULL_HANDLE;
        pthread_mutex_unlock(&s_sc_mu);
        fprintf(stderr, LOGP "vkCreateSwapchainKHR: %s\n", nvk_result_str(r));
        return 0;
    }
    s_sc_first_id = s_present_id + 1;
    pthread_mutex_unlock(&s_sc_mu);
    s_sc_n = SC_MAX;
    vkGetSwapchainImagesKHR(C->dev, s_sc, &s_sc_n, s_sc_img);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < s_sc_n; i++)
        vkCreateSemaphore(C->dev, &si, NULL, &s_done[i]);
    s_recreate = 0;
    return 1;
}

int fbv_create(SDL_Window *win, int vsync, int shots, char *err, size_t errn)
{
    unsigned n = 0, i;
    const char **names = NULL;
    VkCommandPoolCreateInfo cp;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;

    s_vsync = vsync;
    s_shots = shots;
    if (!SDL_Vulkan_GetInstanceExtensions(win, &n, NULL)
            || !(names = (const char **)calloc(n ? n : 1, sizeof *names))
            || !SDL_Vulkan_GetInstanceExtensions(win, &n, names)) {
        snprintf(err, errn, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        free(names);
        return 0;
    }
    C = nvk_core_init(names, n, make_surface, win, err, errn);
    free(names);
    if (!C)
        return 0;
    if (!C->surface) {
        snprintf(err, errn, "the Vulkan device was made without the window");
        C = NULL;
        return 0;
    }
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = C->qfam;
    if (vkCreateCommandPool(C->dev, &cp, NULL, &s_pool) != VK_SUCCESS
            || vkCreateCommandPool(C->dev, &cp, NULL, &s_up_pool) != VK_SUCCESS) {
        snprintf(err, errn, "command pool failed");
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 2;
    vkAllocateCommandBuffers(C->dev, &ai, s_cb);
    ai.commandPool = s_up_pool;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(C->dev, &ai, &s_up_cb);
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < 2; i++) {
        vkCreateFence(C->dev, &fi, NULL, &s_fence[i]);
        vkCreateSemaphore(C->dev, &si, NULL, &s_acq[i]);
    }
    vkCreateFence(C->dev, &fi, NULL, &s_up_fence);
    if (!make_swapchain(win)) {
        snprintf(err, errn, "no swapchain for the window");
        return 0;
    }
    {
        const char *csv = recomp_env(RENV_VK_PRESENT_LOG);
        if (csv && strcmp(csv, "1") && (s_csv = fopen(csv, "w")) != NULL)
            fprintf(s_csv, "id,event,t_ns,flip\n");   /* t_ns: CLOCK_MONOTONIC */
    }
    if (C->present_wait)
        s_wait_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(C->dev,
                                                                      "vkWaitForPresentKHR");
    if (C->present_wait && s_wait_present) {
        s_waiter_run = 1;
        if (pthread_create(&s_waiter, NULL, waiter_main, NULL) != 0)
            s_waiter_run = 0;
    }
    fprintf(stderr, LOGP "Vulkan swapchain: %ux%u, %u images, %s, %s%s\n", s_sc_ext.width,
            s_sc_ext.height, s_sc_n, s_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8",
            s_sc_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)"
            : s_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "IMMEDIATE",
            s_waiter_run ? ", present_wait timing" : "");
    return 1;
}

void fbv_destroy(void)
{
    int k;
    if (!C)
        return;
    if (s_waiter_run) {
        pthread_mutex_lock(&s_id_mu);
        s_waiter_run = 0;
        pthread_cond_broadcast(&s_sc_cv);
        pthread_mutex_unlock(&s_id_mu);
        pthread_join(s_waiter, NULL);
    }
    fbv_timing_report("presents at exit");
    if (s_csv)
        fclose(s_csv);
    s_csv = NULL;
    queue_idle();
    for (k = 0; k < 3; k++)
        nvk_image_free(&s_slot[k]);
    nvk_buffer_free(&s_up_buf);
    nvk_buffer_free(&s_shot_buf);
    destroy_swapchain();
    /* The device, surface and pools stay: the backend holds the device and
     * the process is about to exit. */
}

void *fbv_slot(int k, uint32_t w, uint32_t h)
{
    struct nvk_image *s = &s_slot[k];
    if (!C)
        return NULL;
    if (!s->img || s->w != w || s->h != h) {
        if (s->img) {
            /* A present may still read it: rare (the frame size changed). */
            queue_idle();
            nvk_image_free(s);
        }
        if (!nvk_image_new(s, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT))
            return NULL;
    }
    return s;
}

int fbv_upload(int k, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
               uint32_t bpp)
{
    struct nvk_image *s;
    VkCommandBufferBeginInfo bi;
    VkBufferImageCopy bc;
    VkSubmitInfo si;
    VkDeviceSize need = (VkDeviceSize)w * h * 4u;
    uint32_t x, y;
    VkResult r;

    if (!C)
        return 0;
    if (s_up_pending) {
        vkWaitForFences(C->dev, 1, &s_up_fence, VK_TRUE, UINT64_MAX);
        s_up_pending = 0;
    }
    if (s_up_buf.size < need) {
        nvk_buffer_free(&s_up_buf);
        if (!nvk_buffer_new(&s_up_buf, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0))
            return 0;
    }
    s = (struct nvk_image *)fbv_slot(k, w, h);
    if (!s)
        return 0;
    for (y = 0; y < h; y++) {
        uint32_t *o = (uint32_t *)s_up_buf.map + (size_t)y * w;
        if (bpp == 4) {
            memcpy(o, src + (size_t)y * pitch, (size_t)w * 4u);
            continue;
        }
        for (x = 0; x < w; x++) {
            uint32_t v = ((const uint16_t *)(src + (size_t)y * pitch))[x];
            uint32_t b = (v & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2, rr = ((v >> 11) & 0x1F) << 3;
            o[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
        }
    }
    vkResetCommandBuffer(s_up_cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_up_cb, &bi);
    nvk_image_barrier(s_up_cb, s->img, VK_IMAGE_ASPECT_COLOR_BIT,
                      s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED
                                                             : VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    s->layout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_up_cb, s_up_buf.buf, s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vkEndCommandBuffer(s_up_cb);
    vkResetFences(C->dev, 1, &s_up_fence);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_up_cb;
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, s_up_fence);
    nvk_queue_unlock();
    s_up_pending = r == VK_SUCCESS;
    return r == VK_SUCCESS;
}

static void save_shot(const uint8_t *px, uint32_t w, uint32_t h, const char *path,
                      uint32_t flip)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)w, (int)h, 24,
                                                       SDL_PIXELFORMAT_BGR24);
    int rgba = s_sc_fmt == VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t x, y;

    if (!surf)
        return;
    for (y = 0; y < h; y++) {
        uint8_t *o = (uint8_t *)surf->pixels + (size_t)y * surf->pitch;
        const uint8_t *in = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            o[x * 3 + 0] = in[x * 4 + (rgba ? 2 : 0)];
            o[x * 3 + 1] = in[x * 4 + 1];
            o[x * 3 + 2] = in[x * 4 + (rgba ? 0 : 2)];
        }
    }
    if (SDL_SaveBMP(surf, path) == 0)
        fprintf(stderr, LOGP "window shot: %s (%ux%u, flip %u)\n", path, w, h, flip);
    else
        fprintf(stderr, LOGP "window shot at flip %u failed: %s\n", flip, SDL_GetError());
    SDL_FreeSurface(surf);
}

int fbv_present(SDL_Window *win, int k, uint32_t fw, uint32_t fh, int filter,
                const char *shot_path, uint32_t flip)
{
    VkCommandBuffer cb;
    VkCommandBufferBeginInfo bi;
    VkClearColorValue black;
    VkImageSubresourceRange rr;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint32_t idx = 0;
    int dw = 0, dh = 0, shot;
    VkResult r, pr;

    if (!C)
        return 0;
    SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;
    if (!s_sc || s_recreate || (uint32_t)dw != s_sc_ext.width || (uint32_t)dh != s_sc_ext.height) {
        if (!make_swapchain(win))
            return 0;
    }
    vkWaitForFences(C->dev, 1, &s_fence[s_fi], VK_TRUE, UINT64_MAX);
    {
        uint64_t t_acq0 = xbox_LatOn() ? xbox_HostNowNs() : 0;
        r = vkAcquireNextImageKHR(C->dev, s_sc, 1000000000ull, s_acq[s_fi], VK_NULL_HANDLE, &idx);
        if (t_acq0 && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
            char b[32];
            snprintf(b, sizeof b, "%llu", (unsigned long long)xbox_HostNowNs());
            xbox_LatRow('W', flip, t_acq0, b);
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        s_recreate = 1;
        return 0;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;     /* timeout: try again at the next wake */
    if (r == VK_SUBOPTIMAL_KHR)
        s_recreate = 1;
    vkResetFences(C->dev, 1, &s_fence[s_fi]);
    shot = shot_path && s_sc_shots;
    if (shot && s_shot_buf.size < (VkDeviceSize)s_sc_ext.width * s_sc_ext.height * 4u) {
        nvk_buffer_free(&s_shot_buf);
        if (!nvk_buffer_new(&s_shot_buf, (VkDeviceSize)s_sc_ext.width * s_sc_ext.height * 4u,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            shot = 0;
    }

    cb = s_cb[s_fi];
    vkResetCommandBuffer(cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    {
        /* From the acquire: chained to the semaphore wait (transfer stage),
         * the old contents discarded. */
        VkImageMemoryBarrier2 b;
        VkDependencyInfo di;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s_sc_img[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
    }
    memset(&black, 0, sizeof black);
    black.float32[3] = 1.0f;
    memset(&rr, 0, sizeof rr);
    rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rr.levelCount = 1;
    rr.layerCount = 1;
    vkCmdClearColorImage(cb, s_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    if (s_slot[k].img && s_slot[k].layout == VK_IMAGE_LAYOUT_GENERAL && fw && fh) {
        struct nv2a_rect r2;
        int whole = nv2a_present_rect(fw, fh, (uint32_t)s_sc_ext.width,
                                      (uint32_t)s_sc_ext.height, filter, &r2);
        if (r2.w > 0 && r2.h > 0) {
            int lin = filter == NV2A_PRESENT_LINEAR
                      || (filter == NV2A_PRESENT_INTEGER && !whole);
            VkImageBlit b;
            /* After the backend's copy into the slot (earlier on the queue),
             * and the clear above. */
            nvk_image_barrier(cb, s_slot[k].img, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.layerCount = 1;
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1].x = (int32_t)(fw < s_slot[k].w ? fw : s_slot[k].w);
            b.srcOffsets[1].y = (int32_t)(fh < s_slot[k].h ? fh : s_slot[k].h);
            b.srcOffsets[1].z = 1;
            b.dstOffsets[0].x = r2.x;
            b.dstOffsets[0].y = r2.y;
            b.dstOffsets[1].x = r2.x + r2.w;
            b.dstOffsets[1].y = r2.y + r2.h;
            b.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, s_slot[k].img, VK_IMAGE_LAYOUT_GENERAL, s_sc_img[idx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
    if (shot) {
        VkBufferImageCopy bc;
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = s_sc_ext.width;
        bc.imageExtent.height = s_sc_ext.height;
        bc.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, s_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_shot_buf.buf, 1, &bc);
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cb);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &s_acq[s_fi];
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_done[idx];
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_sc;
    pi.pImageIndices = &idx;
    {
        static VkPresentIdKHR pid;
        static uint64_t idv;
        if (C->present_wait) {
            idv = s_present_id + 1;
            memset(&pid, 0, sizeof pid);
            pid.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
            pid.swapchainCount = 1;
            pid.pPresentIds = &idv;
            pi.pNext = &pid;
        }
    }
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, s_fence[s_fi]);
    pr = r == VK_SUCCESS ? vkQueuePresentKHR(C->queue, &pi) : r;
    nvk_queue_unlock();
    if (r == VK_SUCCESS) {
        uint64_t t = mono_ns();
        pthread_mutex_lock(&s_tm_mu);
        iv_add(&s_host_iv, t);
        pthread_mutex_unlock(&s_tm_mu);
        if (xbox_LatOn())
            xbox_LatRow('P', flip, 0, NULL);
        if (!C->present_wait)
            xbox_PacingPresent(t);
        pthread_mutex_lock(&s_id_mu);
        s_present_id++;
        s_id_flip[s_present_id % ID_RING] = flip;
        if (s_csv)
            fprintf(s_csv, "%llu,present,%llu,%u\n", (unsigned long long)s_present_id,
                    (unsigned long long)t, flip);
        pthread_cond_broadcast(&s_sc_cv);
        pthread_mutex_unlock(&s_id_mu);
    }
    if (r != VK_SUCCESS) {
        static int said;
        if (said++ < 4)
            fprintf(stderr, LOGP "Vulkan present submit: %s\n", nvk_result_str(r));
        /* The fence will not signal: make it signalled again. */
        {
            VkFenceCreateInfo fi;
            vkDestroyFence(C->dev, s_fence[s_fi], NULL);
            memset(&fi, 0, sizeof fi);
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(C->dev, &fi, NULL, &s_fence[s_fi]);
        }
        return 0;
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
        s_recreate = 1;
    if (shot) {
        vkWaitForFences(C->dev, 1, &s_fence[s_fi], VK_TRUE, UINT64_MAX);
        save_shot((const uint8_t *)s_shot_buf.map, s_sc_ext.width, s_sc_ext.height,
                  shot_path, flip);
    } else if (shot_path) {
        fprintf(stderr, LOGP "window shot at flip %u: the swapchain cannot be read back\n",
                flip);
    }
    s_fi ^= 1;
    return 1;
}

#endif /* !_WIN32 */
