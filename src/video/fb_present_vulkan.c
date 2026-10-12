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
#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)

#include <SDL.h>
#include <SDL_vulkan.h>
#if defined(_WIN32)
#include <windows.h>
#include <SDL_syswm.h>
#define COBJMACROS
#include <dxgi.h>
#else
#include <dlfcn.h>
#endif
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
#include "d3d/nv2a_vk_glsl.h"
#include "post_shaders.h"
#include "smaa.h"
#if defined(XBOXRECOMP_HAVE_OVERLAY)
#include "overlay.h"
#include "overlay_vulkan.h"
#endif

uint64_t xbox_HostNowNs(void);
void xbox_PacingPresent(uint64_t when_ns);
void xbox_VblankDisplayRefresh(uint64_t when_ns, uint64_t period_ns);
int xbox_VblankDisplayLockOn(void);
#include "kernel/nv2a_backend_common.h"
#include "fb_present_vulkan.h"

#define LOGP "[PRESENT] "
#define SC_MAX 8

static const struct nvk_core *C;
static VkSwapchainKHR   s_sc;
static VkImage          s_sc_img[SC_MAX];
static VkImageView      s_sc_view[SC_MAX];   /* for the overlay pass */
static uint32_t         s_sc_n;
static VkFormat         s_sc_fmt;
static VkExtent2D       s_sc_ext;
static VkPresentModeKHR s_sc_mode;
static int              s_sc_shots;      /* TRANSFER_SRC usage: shots possible */
static int              s_sc_color;      /* COLOR_ATTACHMENT usage: the overlay can draw */
static int              s_recreate;
/* VK_SUBOPTIMAL_KHR asks for a new swapchain, except on Android: there it is
 * returned for as long as the swapchain's transform is not the display's,
 * which with the identity transform on a sideways-mounted panel (the Retroid
 * Pocket 5's is 1080x1920) is every frame, and rebuilding it every frame is
 * what that cost. A size change is caught by the extent check. */
#if defined(__ANDROID__)
#define FBV_SUBOPTIMAL_RECREATES 0
#else
#define FBV_SUBOPTIMAL_RECREATES 1
#endif
/* Android: the swapchain may be smaller than the window, and the system's
 * compositor scales it to the panel (RECOMP_ANDROID_SWAPCHAIN_HEIGHT=<rows>,
 * the width follows the window's shape): this presenter's own scaling pass
 * then writes that many pixels, not the panel's. The idea is Xerenge's
 * Android build's. The window size the swapchain was made for is kept apart,
 * for the "has the window changed" test. */
#if defined(__ANDROID__)
static uint32_t s_and_w, s_and_h;
#define FBV_SIZE_CHANGED(dw, dh) ((uint32_t)(dw) != s_and_w || (uint32_t)(dh) != s_and_h)
#else
#define FBV_SIZE_CHANGED(dw, dh) \
    ((uint32_t)(dw) != s_sc_ext.width || (uint32_t)(dh) != s_sc_ext.height)
#endif
static int              s_vsync, s_shots;

static VkCommandPool    s_pool;          /* main thread */
static VkCommandBuffer  s_cb[2];
static VkFence          s_fence[2];
static VkSemaphore      s_acq[2];
static VkSemaphore      s_done[SC_MAX];
static int              s_fi;

static struct nvk_image s_slot[3];       /* GENERAL once written */
static void post_destroy(void);          /* the post passes, below */

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
#if defined(_WIN32)
    return xbox_HostNowNs();     /* the clock the pacing code compares against */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
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
             * frame shown is a refresh; FIFO shows every frame. No period:
             * the step to the last one is a whole number of refreshes,
             * which the kernel resolves (s_vbl_period_ns is the frame
             * rate, not the display's, above 60 Hz). */
            xbox_VblankDisplayRefresh(t, 0);
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

#if defined(_WIN32)
/* Windows: the drivers do not all offer VK_KHR_present_wait (AMD's did not on
 * the RX 9060 XT, driver 25.x), the Linux source of the display's refresh
 * times for RECOMP_VBLANK_SYNC=display. The D3D11 path's source is used
 * instead: a thread that waits for each vertical blank of the output the
 * window is on (IDXGIOutput::WaitForVBlank, as the D3D11 presenter and
 * RetroArch/DuckStation-style "sync to host refresh" do) and reports it to
 * the vblank timer (kernel_pacing.c). DXGI is loaded at run time, so the
 * exe has no link dependency on it. */
static volatile int s_dxgi_run;
static HANDLE       s_dxgi_th;

static DWORD WINAPI dxgi_vblank_main(LPVOID param)
{
    IDXGIOutput *out = (IDXGIOutput *)param;
    uint64_t last = 0;
    unsigned fails = 0, fast = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (s_dxgi_run) {
        uint64_t t;
        if (FAILED(IDXGIOutput_WaitForVBlank(out))) {
            /* A lost output (mode change, display off): back off, never spin. */
            Sleep(fails < 8 ? 16 : 250);
            fails++;
            continue;
        }
        fails = 0;
        t = mono_ns();
        /* An output that is asleep answers at once, with success: this
         * thread then spun at time-critical priority, a core gone and, on a
         * loaded PC, the title's core with it (BO-75: the monitor had gone
         * to sleep before a run; under sixteen CPU-bound jobs the title
         * stood for up to 0.7 s with this thread on its core). No refresh
         * comes within half a millisecond of the last: three in a row is
         * not a display, so nothing is reported and the thread sleeps as
         * for a lost output, until a wait takes its time again. */
        if (last && t - last < 500000ull) {
            if (++fast >= 3) {
                Sleep(fast < 64 ? 16 : 250);
                last = mono_ns();
                continue;
            }
        } else {
            fast = 0;
        }
        if (last && t - last < 60000000ull) {
            pthread_mutex_lock(&s_tm_mu);
            iv_add(&s_disp_iv, t);
            pthread_mutex_unlock(&s_tm_mu);
        }
        last = t;
        xbox_VblankDisplayRefresh(t, 0);
        if (s_csv)
            fprintf(s_csv, "0,display,%llu,\n", (unsigned long long)t);
    }
    IDXGIOutput_Release(out);
    return 0;
}

static void dxgi_vblank_start(SDL_Window *win)
{
    typedef HRESULT (WINAPI *create_fn)(REFIID, void **);
    static const GUID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba,
        { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    SDL_SysWMinfo wm;
    HMODULE dll;
    create_fn create;
    IDXGIFactory1 *fac = NULL;
    IDXGIOutput *found = NULL;
    HMONITOR mon;
    UINT a, o;

    if (!xbox_VblankDisplayLockOn())
        return;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS)
        return;
    dll = LoadLibraryA("dxgi.dll");
    create = dll ? (create_fn)(void (*)(void))GetProcAddress(dll, "CreateDXGIFactory1") : NULL;
    if (!create || FAILED(create(&iid_factory1, (void **)&fac)) || !fac) {
        fprintf(stderr, LOGP "vblank sync: no DXGI; the vblank keeps its own 60 Hz timer\n");
        return;
    }
    mon = MonitorFromWindow(wm.info.win.window, MONITOR_DEFAULTTONEAREST);
    for (a = 0; !found; a++) {
        IDXGIAdapter1 *ad = NULL;
        if (FAILED(IDXGIFactory1_EnumAdapters1(fac, a, &ad)) || !ad)
            break;
        for (o = 0; !found; o++) {
            IDXGIOutput *out = NULL;
            DXGI_OUTPUT_DESC od;
            if (FAILED(IDXGIAdapter1_EnumOutputs(ad, o, &out)) || !out)
                break;
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == mon)
                found = out;
            else
                IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);
    if (!found) {
        fprintf(stderr, LOGP "vblank sync: the window's monitor has no DXGI output; the"
                " vblank keeps its own 60 Hz timer\n");
        return;
    }
    s_dxgi_run = 1;
    s_dxgi_th = CreateThread(NULL, 0, dxgi_vblank_main, found, 0, NULL);
    if (!s_dxgi_th) {
        s_dxgi_run = 0;
        IDXGIOutput_Release(found);
        return;
    }
    fprintf(stderr, LOGP "vblank sync: the emulated vblank follows the output's vertical"
            " blank (DXGI WaitForVBlank, RECOMP_VBLANK_SYNC=display)\n");
}

static void dxgi_vblank_stop(void)
{
    if (!s_dxgi_th)
        return;
    s_dxgi_run = 0;
    /* The thread may be inside WaitForVBlank (one refresh at most). */
    WaitForSingleObject(s_dxgi_th, 500);
    CloseHandle(s_dxgi_th);
    s_dxgi_th = NULL;
}
#endif

int fbv_load_library(char *err, size_t errn)
{
    const char *path = NULL;

    /* The loader linked into this binary: SDL_Vulkan_CreateSurface must use
     * the same instance functions as the instance made here. On Windows that
     * is vulkan-1.dll, which SDL's default load finds already mapped. */
#if !defined(_WIN32)
    Dl_info di;
    if (dladdr((void *)&vkGetInstanceProcAddr, &di) && di.dli_fname)
        path = di.dli_fname;
#endif
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
        if (s_sc_view[i])
            vkDestroyImageView(C->dev, s_sc_view[i], NULL);
        s_sc_view[i] = VK_NULL_HANDLE;
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

#if defined(__ANDROID__)
/* An Android app that leaves the screen (Home, recents, screen off) has its
 * window's surface destroyed, and gets another when it comes back. The
 * VkSurfaceKHR and the swapchain made on the old one are dead then: both are
 * dropped and made again on the window's new surface. Marked from the app's
 * life-cycle events (fbv_android_surface_gone, fb_present_sdl.c) and from
 * any VK_ERROR_SURFACE_LOST_KHR. */
static volatile int s_surface_lost;
static VkSurfaceKHR make_surface(VkInstance inst, void *ctx);
static void destroy_swapchain(void);

void fbv_android_surface_gone(void)
{
    s_surface_lost = 1;
}

static int android_resurface(SDL_Window *win)
{
    static unsigned said;
    if (!C)
        return 0;
    queue_idle();
    pthread_mutex_lock(&s_sc_mu);
    destroy_swapchain();
    pthread_mutex_unlock(&s_sc_mu);
    if (!nvk_core_replace_surface(make_surface, win)) {
        if (said++ < 3)
            fprintf(stderr, LOGP "no surface for the window yet; trying again\n");
        return 0;
    }
    s_surface_lost = 0;
    s_recreate = 1;
    fprintf(stderr, LOGP "the window's surface was replaced: a new VkSurfaceKHR and swapchain\n");
    return 1;
}
#endif

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
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(C->phys, C->surface, &caps);
#if defined(__ANDROID__)
    if (r == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
#endif
    if (r != VK_SUCCESS)
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
#if defined(__ANDROID__)
    s_and_w = (uint32_t)dw;
    s_and_h = (uint32_t)dh;
    {
        const char *e = getenv("RECOMP_ANDROID_SWAPCHAIN_HEIGHT");
        uint32_t rows = e ? (uint32_t)strtoul(e, NULL, 10) : 0;
        if (rows >= 240 && rows < s_sc_ext.height) {
            s_sc_ext.width = ((s_sc_ext.width * rows / s_sc_ext.height) + 1u) & ~1u;
            s_sc_ext.height = rows;
            if (s_sc_ext.width < caps.minImageExtent.width) s_sc_ext.width = caps.minImageExtent.width;
        }
    }
#endif
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
    s_sc_color = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  | (s_sc_shots ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)
                  | (s_sc_color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
#if defined(__ANDROID__)
    /* A panel mounted sideways reports a rotation here, and a swapchain made
     * with it must be drawn rotated. This presenter does not: identity, and
     * the compositor turns the picture. */
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
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
    for (i = 0; i < s_sc_n; i++) {
        vkCreateSemaphore(C->dev, &si, NULL, &s_done[i]);
        if (s_sc_color) {
            VkImageViewCreateInfo vi;
            memset(&vi, 0, sizeof vi);
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = s_sc_img[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = s_sc_fmt;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(C->dev, &vi, NULL, &s_sc_view[i]) != VK_SUCCESS)
                s_sc_view[i] = VK_NULL_HANDLE;
        }
    }
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
#if defined(_WIN32)
    if (!s_waiter_run)
        dxgi_vblank_start(win);
#endif
#if defined(__ANDROID__)
    /* No present_wait through Android's loader: the Choreographer's refresh
     * times instead (android_vsync.c). RECOMP_ANDROID_VSYNC=0: the old
     * free-running timer, for A/B. */
    if (!s_waiter_run && !(getenv("RECOMP_ANDROID_VSYNC") && getenv("RECOMP_ANDROID_VSYNC")[0] == '0')) {
        extern void xbox_AndroidVsyncStart(void);
        xbox_AndroidVsyncStart();
    }
#endif
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
#if defined(_WIN32)
    dxgi_vblank_stop();
#endif
#if defined(__ANDROID__)
    {
        extern void xbox_AndroidVsyncStop(void);
        xbox_AndroidVsyncStop();
    }
#endif
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
    post_destroy();
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
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           /* The post passes (AA, sharpen) sample the slot: without
                            * SAMPLED the read is undefined (black on AMD's Windows
                            * driver; RADV happened to tolerate it). */
                           | VK_IMAGE_USAGE_SAMPLED_BIT,
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

/* ---- the post passes ------------------------------------------------------
 *
 * FXAA or SMAA's three passes, then CAS (post_shaders.h, smaa.h), as
 * fullscreen-triangle passes with dynamic rendering at the frame's own size:
 * the slot (or the pass before) is the input, an image of the slot's size the
 * target, and the scale to the window then blits the last target instead of
 * the slot. GLSL compiled at the first use (the SPIR-V cache serves it
 * after), a pipeline each. A failure leaves that pass off, logged once; the
 * present is unaffected. A target exists only while its setting is on: it is
 * made at the first frame that needs it and freed (after the queue is idle)
 * when no setting does. One descriptor set per pass and frame in flight,
 * rewritten each frame: the fence wait at the top of fbv_present has retired
 * its last use.
 *
 * RECOMP_TRACE=post_gpu: each pass's GPU time, from timestamp queries around
 * it, read a frame later and averaged every 300 presents. */

static const char s_post_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 vUv;\n"
    "void main() {\n"
    "    vUv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define POST_GLSL_HEAD \
    "#version 450\n" \
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n" \
    "#define saturate(x) clamp((x), 0.0, 1.0)\n" \
    "layout(set = 0, binding = 0) uniform sampler2D tex0;\n" \
    "layout(push_constant) uniform PC { vec2 rcpv; float sharpv; } pc;\n" \
    "layout(location = 0) in vec2 vUv;\n" \
    "layout(location = 0) out vec4 outColor;\n" \
    "#define FETCH(p) texture(tex0, (p)).rgb\n" \
    "#define RETURN(c) { outColor = vec4((c), 1.0); return; }\n" \
    "void main() {\n" \
    "    vec2 uv = vUv; vec2 rcp = pc.rcpv; float sharp = pc.sharpv;\n"

static const char s_post_fxaa[] = POST_GLSL_HEAD POST_FXAA_BODY "}\n";
static const char s_post_cas[] = POST_GLSL_HEAD POST_CAS_BODY "}\n";

/* SMAA's three: the reference library after a header with the push constant
 * (1/w 1/h w h) and the passes' own bindings. */
static const char s_smaa_head[] =
    "#version 450\n"
    "#define SMAA_GLSL_4\n"
    "#define SMAA_PRESET_HIGH\n"
    "layout(push_constant) uniform PC { vec4 rtm; } pc;\n"
    "#define SMAA_RT_METRICS pc.rtm\n"
    "layout(location = 0) in vec2 vUv;\n"
    "layout(location = 0) out vec4 outColor;\n";
static const char *const s_smaa_bind[3] = {
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n",
    "layout(set = 0, binding = 0) uniform sampler2D edgesTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D areaTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D searchTex;\n",
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n",
};
static const char *const s_smaa_main[3] = {
    "void main() { vec4 offset[3]; SMAAEdgeDetectionVS(vUv, offset);\n"
    "    outColor = vec4(SMAALumaEdgeDetectionPS(vUv, offset, colorTex), 0.0, 0.0); }\n",
    "void main() { vec4 offset[3]; vec2 pixcoord; SMAABlendingWeightCalculationVS(vUv, pixcoord, offset);\n"
    "    outColor = SMAABlendingWeightCalculationPS(vUv, pixcoord, offset, edgesTex, areaTex,\n"
    "                                               searchTex, vec4(0.0)); }\n",
    "void main() { vec4 offset; SMAANeighborhoodBlendingVS(vUv, offset);\n"
    "    outColor = vec4(SMAANeighborhoodBlendingPS(vUv, offset, colorTex, blendTex).rgb, 1.0); }\n",
};

enum { PP_FXAA, PP_CAS, PP_SM_EDGES, PP_SM_WEIGHTS, PP_SM_BLEND, PP_COUNT };
/* Targets: 0 FXAA's and SMAA's result, 1 CAS's, 2 edges, 3 weights. */
enum { TG_AA, TG_CAS, TG_EDGES, TG_WEIGHTS, TG_COUNT };
static const VkFormat s_tg_fmt[TG_COUNT] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                             VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
static const char *const s_pp_name[PP_COUNT] = { "fxaa", "cas", "smaa edges", "smaa weights",
                                                 "smaa blend" };
#define PP_TS 16                           /* timestamp slots a frame */

static struct {
    int tried, ok, smaa_tried, smaa_ok;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkSampler smp;
    VkDescriptorPool pool;
    VkDescriptorSet ds[2][PP_COUNT];       /* [frame in flight][pass] */
    VkPipeline pso[PP_COUNT];
    struct nvk_image img[TG_COUNT];
    struct nvk_image lut[2];               /* SMAA's area and search tables */
    struct nvk_buffer stage[2];            /* their upload, until freed with them */
    int lut_done;
    /* post_gpu */
    VkQueryPool qp;
    int ts_n[2], ts_pass[2][PP_COUNT];     /* stamps written / which passes, per frame */
    double ts_sum[PP_COUNT];
    unsigned ts_cnt[PP_COUNT], ts_presents;
} P;

static VkShaderModule post_module(enum nvk_stage st, const char *src, const char *what)
{
    char err[2048];
    const uint32_t *w;
    size_t n;
    int compiled;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    if (!nvk_glsl_spirv(st, src, &w, &n, &compiled, err, sizeof err)) {
        fprintf(stderr, LOGP "post pass %s: GLSL compile failed: %s\n", what, err);
        return VK_NULL_HANDLE;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = n * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(C->dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static VkPipeline post_pipeline(VkShaderModule vs, VkShaderModule fs, VkFormat cf)
{
    VkPipelineShaderStageCreateInfo ss[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo pr;
    VkGraphicsPipelineCreateInfo gp;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipeline p = VK_NULL_HANDLE;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = vs;
    ss[0].pName = ss[1].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = fs;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    memset(&pr, 0, sizeof pr);
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &cf;
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
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = P.pl;
    if (vkCreateGraphicsPipelines(C->dev, VK_NULL_HANDLE, 1, &gp, NULL, &p) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

/* The three SMAA fragment sources, made when it is first asked for. */
static void smaa_pipelines(VkShaderModule vs)
{
    static const VkFormat fmt[3] = { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_FORMAT_B8G8R8A8_UNORM };
    int k;

    P.smaa_ok = 1;
    for (k = 0; k < 3; k++) {
        const char *lib = smaa_glsl_library();
        size_t n = strlen(s_smaa_head) + strlen(s_smaa_bind[k]) + strlen(lib)
                 + strlen(s_smaa_main[k]) + 1;
        char *src = (char *)malloc(n);
        VkShaderModule fs;
        if (!src) {
            P.smaa_ok = 0;
            return;
        }
        snprintf(src, n, "%s%s%s%s", s_smaa_head, s_smaa_bind[k], lib, s_smaa_main[k]);
        fs = post_module(NVK_STAGE_FRAGMENT, src, s_pp_name[PP_SM_EDGES + k]);
        free(src);
        if (fs) {
            P.pso[PP_SM_EDGES + k] = post_pipeline(vs, fs, fmt[k]);
            vkDestroyShaderModule(C->dev, fs, NULL);
        }
        if (!P.pso[PP_SM_EDGES + k])
            P.smaa_ok = 0;
    }
    fprintf(stderr, LOGP "SMAA %s (preset high)\n", P.smaa_ok ? "ready" : "unavailable");
}

/* SMAA's pipelines, when it is first asked for (the larger compile, once;
 * the SPIR-V cache serves it after). */
static int smaa_prepare(void)
{
    VkShaderModule vs;

    if (P.smaa_tried)
        return P.smaa_ok;
    P.smaa_tried = 1;
    vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
    if (vs) {
        smaa_pipelines(vs);
        vkDestroyShaderModule(C->dev, vs, NULL);
    }
    return P.smaa_ok;
}

/* Made at the first use; 1 when the passes can run. */
static int post_init(void)
{
    VkDescriptorSetLayoutBinding lb[3];
    VkDescriptorSetLayoutCreateInfo li;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo pli;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetAllocateInfo dai;
    VkDescriptorSetLayout lay[2 * PP_COUNT];
    VkShaderModule vs, fs[2];
    int i;

    if (P.tried)
        return P.ok;
    P.tried = 1;
    memset(lb, 0, sizeof lb);
    for (i = 0; i < 3; i++) {
        lb[i].binding = (uint32_t)i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = lb;
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = 16;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    memset(&psz, 0, sizeof psz);
    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = 3 * 2 * PP_COUNT;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 2 * PP_COUNT;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &psz;
    if (vkCreateDescriptorSetLayout(C->dev, &li, NULL, &P.dsl) != VK_SUCCESS)
        goto fail;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(C->dev, &pli, NULL, &P.pl) != VK_SUCCESS
            || vkCreateSampler(C->dev, &sci, NULL, &P.smp) != VK_SUCCESS
            || vkCreateDescriptorPool(C->dev, &pci, NULL, &P.pool) != VK_SUCCESS)
        goto fail;
    for (i = 0; i < 2 * PP_COUNT; i++)
        lay[i] = P.dsl;
    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = P.pool;
    dai.descriptorSetCount = 2 * PP_COUNT;
    dai.pSetLayouts = lay;
    {
        VkDescriptorSet sets[2 * PP_COUNT];
        if (vkAllocateDescriptorSets(C->dev, &dai, sets) != VK_SUCCESS)
            goto fail;
        for (i = 0; i < 2 * PP_COUNT; i++)
            P.ds[i / PP_COUNT][i % PP_COUNT] = sets[i];
    }
    vs = post_module(NVK_STAGE_VERTEX, s_post_vs, "vertex");
    fs[PP_FXAA] = post_module(NVK_STAGE_FRAGMENT, s_post_fxaa, "FXAA");
    fs[PP_CAS] = post_module(NVK_STAGE_FRAGMENT, s_post_cas, "CAS");
    if (vs) {
        for (i = 0; i < 2; i++)
            if (fs[i])
                P.pso[i] = post_pipeline(vs, fs[i], VK_FORMAT_B8G8R8A8_UNORM);
    }
    /* The modules are no longer needed once the pipelines exist. */
    for (i = 0; i < 2; i++)
        if (fs[i])
            vkDestroyShaderModule(C->dev, fs[i], NULL);
    if (vs)
        vkDestroyShaderModule(C->dev, vs, NULL);
    P.ok = P.pso[PP_FXAA] || P.pso[PP_CAS];
    fprintf(stderr, LOGP "post passes: FXAA %s, CAS %s\n", P.pso[PP_FXAA] ? "ready" : "unavailable",
            P.pso[PP_CAS] ? "ready" : "unavailable");
    if (recomp_env(RENV_POST_GPU) && C->props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * PP_TS;
        if (vkCreateQueryPool(C->dev, &qi, NULL, &P.qp) != VK_SUCCESS)
            P.qp = VK_NULL_HANDLE;
    }
    return P.ok;
fail:
    fprintf(stderr, LOGP "post passes unavailable (Vulkan objects failed)\n");
    return 0;
}

static void post_free_targets(int luts_too)
{
    int i;
    queue_idle();
    for (i = 0; i < TG_COUNT; i++)
        nvk_image_free(&P.img[i]);
    if (luts_too) {
        for (i = 0; i < 2; i++) {
            nvk_image_free(&P.lut[i]);
            nvk_buffer_free(&P.stage[i]);
        }
        P.lut_done = 0;
    }
}

static void post_destroy(void)
{
    int i;
    if (!P.tried)
        return;
    post_free_targets(1);
    for (i = 0; i < PP_COUNT; i++)
        if (P.pso[i])
            vkDestroyPipeline(C->dev, P.pso[i], NULL);
    if (P.qp)
        vkDestroyQueryPool(C->dev, P.qp, NULL);
    if (P.pool)
        vkDestroyDescriptorPool(C->dev, P.pool, NULL);
    if (P.smp)
        vkDestroySampler(C->dev, P.smp, NULL);
    if (P.pl)
        vkDestroyPipelineLayout(C->dev, P.pl, NULL);
    if (P.dsl)
        vkDestroyDescriptorSetLayout(C->dev, P.dsl, NULL);
    memset(&P, 0, sizeof P);
}

/* The target t at w x h, made if it is not (0 when it cannot be). */
static struct nvk_image *post_target(int t, uint32_t w, uint32_t h)
{
    struct nvk_image *im = &P.img[t];
    if (im->img && (im->w != w || im->h != h)) {
        queue_idle();                   /* a present may still read it: rare */
        nvk_image_free(im);
    }
    if (!im->img && !nvk_image_new(im, s_tg_fmt[t], w, h, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    return im;
}

/* SMAA's lookup tables, uploaded in cb the first time. */
static int post_luts(VkCommandBuffer cb)
{
    static const struct { VkFormat f; uint32_t w, h, bpp; } d[2] = {
        { VK_FORMAT_R8G8_UNORM, SMAA_AREA_W, SMAA_AREA_H, 2 },
        { VK_FORMAT_R8_UNORM, SMAA_SEARCH_W, SMAA_SEARCH_H, 1 },
    };
    const unsigned char *src[2];
    int i;

    if (P.lut_done)
        return 1;
    src[0] = smaa_area_bytes;
    src[1] = smaa_search_bytes;
    for (i = 0; i < 2; i++) {
        VkBufferImageCopy bc;
        VkDeviceSize n = (VkDeviceSize)d[i].w * d[i].h * d[i].bpp;
        if (!nvk_image_new(&P.lut[i], d[i].f, d[i].w, d[i].h, 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)
                || !nvk_buffer_new(&P.stage[i], n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)) {
            post_free_targets(1);
            return 0;
        }
        memcpy(P.stage[i].map, src[i], (size_t)n);
        nvk_image_to(cb, &P.lut[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bc, 0, sizeof bc);
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = d[i].w;
        bc.imageExtent.height = d[i].h;
        bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, P.stage[i].buf, P.lut[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &bc);
        nvk_image_to(cb, &P.lut[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    P.lut_done = 1;
    return 1;
}

/* One pass in cb: `in` (up to three images, as laid out now) through pass p
 * into target t (cleared first when `clear`). pc is the 16 bytes of push
 * constants. */
static void post_pass(VkCommandBuffer cb, int p, struct nvk_image *dst, int clear,
                      struct nvk_image *const *in, const VkImageLayout *in_layout, int nin,
                      const float *pc)
{
    VkDescriptorImageInfo di[3];
    VkWriteDescriptorSet wr[3];
    VkRenderingAttachmentInfo att;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    int i, slot = -1;

    for (i = 0; i < nin; i++) {
        memset(&di[i], 0, sizeof di[i]);
        di[i].sampler = P.smp;
        di[i].imageView = in[i]->view;
        di[i].imageLayout = in_layout[i];
        memset(&wr[i], 0, sizeof wr[i]);
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = P.ds[s_fi][p];
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &di[i];
    }
    vkUpdateDescriptorSets(C->dev, (uint32_t)nin, wr, 0, NULL);

    nvk_image_to(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (P.qp && P.ts_n[s_fi] + 2 <= PP_TS) {
        slot = P.ts_n[s_fi];
        P.ts_pass[s_fi][slot / 2] = p;
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, P.qp, (uint32_t)(s_fi * PP_TS + slot));
    }
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = dst->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = dst->w;
    ri.renderArea.extent.height = dst->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)dst->w;
    vp.height = (float)dst->h;
    vp.maxDepth = 1.0f;
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = dst->w;
    sc.extent.height = dst->h;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pso[p]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &P.ds[s_fi][p], 0, NULL);
    vkCmdPushConstants(cb, P.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    /* Readable by the next pass or the blit. */
    nvk_image_to(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (slot >= 0) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, P.qp,
                            (uint32_t)(s_fi * PP_TS + slot + 1));
        P.ts_n[s_fi] += 2;
    }
}

/* The last frame in this slot's passes' timestamps, read now that its fence
 * has signalled; a line every 300 presents. */
static void post_gpu_read(void)
{
    uint64_t t[PP_TS];
    int n = P.ts_n[s_fi], i;
    double period = (double)C->props.limits.timestampPeriod * 1e-6;     /* ms a tick */

    if (!P.qp || !n)
        return;
    P.ts_n[s_fi] = 0;
    if (vkGetQueryPoolResults(C->dev, P.qp, (uint32_t)(s_fi * PP_TS), (uint32_t)n,
                              sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (i = 0; i + 1 < n; i += 2) {
        int p = P.ts_pass[s_fi][i / 2];
        P.ts_sum[p] += (double)(t[i + 1] - t[i]) * period;
        P.ts_cnt[p]++;
    }
    if (++P.ts_presents % 300 == 0) {
        fprintf(stderr, LOGP "post GPU ms (avg of the last 300):");
        for (i = 0; i < PP_COUNT; i++) {
            if (P.ts_cnt[i])
                fprintf(stderr, " %s %.3f", s_pp_name[i], P.ts_sum[i] / (double)P.ts_cnt[i]);
            P.ts_sum[i] = 0;
            P.ts_cnt[i] = 0;
        }
        fprintf(stderr, "\n");
    }
}

/* The passes asked for (RECOMP_GFX_AA, RECOMP_GFX_SHARPEN), recorded into cb
 * over slot k. Returns the image and layout the scale to the window reads:
 * the last target, or the slot itself when no pass ran. */
static void post_run(VkCommandBuffer cb, int k, VkImage *src, VkImageLayout *layout)
{
    int aa = nv2a_host_post_aa(), cas = nv2a_host_sharpen() > 0.001f;
    float sharp = nv2a_host_sharpen();
    struct nvk_image *slot = &s_slot[k], *cur = slot, *in[3], *t;
    VkImageLayout cur_layout = VK_IMAGE_LAYOUT_GENERAL, inl[3];
    float pc[4];
    int fx = aa == NV2A_AA_FXAA, sm = aa == NV2A_AA_SMAA, ran = 0;

    *src = slot->img;
    *layout = VK_IMAGE_LAYOUT_GENERAL;
    if (!(fx || sm || cas) && !P.tried)
        return;
    if (!post_init())
        return;
    fx = fx && P.pso[PP_FXAA];
    sm = sm && smaa_prepare();
    cas = cas && P.pso[PP_CAS];
    {
        static int last_aa = -1;
        static float last_sharp = -1.0f;
        if (aa != last_aa || sharp != last_sharp) {
            last_aa = aa;
            last_sharp = sharp;
            fprintf(stderr, LOGP "post passes: AA %s, sharpen %.2f (%ux%u)\n",
                    aa == NV2A_AA_FXAA ? "FXAA" : aa == NV2A_AA_SMAA ? (sm ? "SMAA" : "SMAA (unavailable)")
                    : "off", sharp, slot->w, slot->h);
        }
    }
    post_gpu_read();
    /* Targets only while a setting needs them. */
    if (!fx && !sm && !cas && !P.img[TG_AA].img && !P.img[TG_CAS].img && !P.img[TG_EDGES].img
            && !P.lut[0].img)
        return;
    if (!sm && (P.img[TG_EDGES].img || P.img[TG_WEIGHTS].img || P.lut[0].img)) {
        /* SMAA went off: its targets and tables go (the queue is idle first). */
        queue_idle();
        nvk_image_free(&P.img[TG_EDGES]);
        nvk_image_free(&P.img[TG_WEIGHTS]);
        for (int i = 0; i < 2; i++) {
            nvk_image_free(&P.lut[i]);
            nvk_buffer_free(&P.stage[i]);
        }
        P.lut_done = 0;
    }
    if (!fx && !sm && P.img[TG_AA].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_AA]);
    }
    if (!cas && P.img[TG_CAS].img) {
        queue_idle();
        nvk_image_free(&P.img[TG_CAS]);
    }
    if (!fx && !sm && !cas)
        return;
    /* After the backend's copy into the slot (earlier on the queue). */
    nvk_image_barrier(cb, slot->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL);
    if (P.qp) {
        vkCmdResetQueryPool(cb, P.qp, (uint32_t)(s_fi * PP_TS), PP_TS);
        P.ts_n[s_fi] = 0;
    }
    memset(pc, 0, sizeof pc);
    pc[0] = 1.0f / (float)slot->w;
    pc[1] = 1.0f / (float)slot->h;
    pc[2] = sharp;
    if (fx && (t = post_target(TG_AA, slot->w, slot->h)) != NULL) {
        in[0] = cur;
        inl[0] = cur_layout;
        post_pass(cb, PP_FXAA, t, 0, in, inl, 1, pc);
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    } else if (sm && post_luts(cb)) {
        struct nvk_image *ed = post_target(TG_EDGES, slot->w, slot->h),
                         *wt = post_target(TG_WEIGHTS, slot->w, slot->h),
                         *out = post_target(TG_AA, slot->w, slot->h);
        if (ed && wt && out) {
            float m[4] = { pc[0], pc[1], (float)slot->w, (float)slot->h };
            in[0] = cur;
            inl[0] = cur_layout;
            post_pass(cb, PP_SM_EDGES, ed, 1, in, inl, 1, m);
            in[0] = ed;
            inl[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            in[1] = &P.lut[0];
            inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            in[2] = &P.lut[1];
            inl[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            post_pass(cb, PP_SM_WEIGHTS, wt, 1, in, inl, 3, m);
            in[0] = cur;
            inl[0] = cur_layout;
            in[1] = wt;
            inl[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            post_pass(cb, PP_SM_BLEND, out, 0, in, inl, 2, m);
            cur = out;
            cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ran = 1;
        }
    }
    if (cas && (t = post_target(TG_CAS, slot->w, slot->h)) != NULL) {
        in[0] = cur;
        inl[0] = cur_layout;
        post_pass(cb, PP_CAS, t, 0, in, inl, 1, pc);
        cur = t;
        cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ran = 1;
    }
    if (ran) {
        nvk_image_to(cb, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        *src = cur->img;
        *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
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
#if defined(__ANDROID__)
    if (s_surface_lost && !android_resurface(win))
        return 0;
#endif
    if (!s_sc || s_recreate || FBV_SIZE_CHANGED(dw, dh)) {
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
#if defined(__ANDROID__)
    if (r == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
#endif
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return 0;     /* timeout: try again at the next wake */
    if (r == VK_SUBOPTIMAL_KHR && FBV_SUBOPTIMAL_RECREATES)
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
        struct nv2a_rect r2, sr;
        uint32_t sw = fw < s_slot[k].w ? fw : s_slot[k].w, sh = fh < s_slot[k].h ? fh : s_slot[k].h;
        int whole;
        nv2a_present_frame(flip);
        whole = nv2a_present_layout(sw, sh, (uint32_t)s_sc_ext.width,
                                    (uint32_t)s_sc_ext.height, filter, &sr, &r2);
        if (r2.w > 0 && r2.h > 0) {
            int lin = filter == NV2A_PRESENT_LINEAR
                      || (filter == NV2A_PRESENT_INTEGER && !whole);
            VkImageBlit b;
            VkImage bsrc;
            VkImageLayout bsrc_layout;
            /* The post passes, if asked for, then the blit reads their last
             * target; else after the backend's copy into the slot (earlier on
             * the queue), and the clear above. */
            post_run(cb, k, &bsrc, &bsrc_layout);
            if (bsrc == s_slot[k].img)
                nvk_image_barrier(cb, s_slot[k].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.layerCount = 1;
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = sr.x;
            b.srcOffsets[0].y = sr.y;
            b.srcOffsets[1].x = sr.x + sr.w;
            b.srcOffsets[1].y = sr.y + sr.h;
            b.srcOffsets[1].z = 1;
            b.dstOffsets[0].x = r2.x;
            b.dstOffsets[0].y = r2.y;
            b.dstOffsets[1].x = r2.x + r2.w;
            b.dstOffsets[1].y = r2.y + r2.h;
            b.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, bsrc, bsrc_layout, s_sc_img[idx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
#if defined(XBOXRECOMP_HAVE_OVERLAY)
    /* The settings overlay, over the game's image and under the present
     * (overlay.h): a dynamic-rendering pass on the swapchain image. */
    if (s_sc_view[idx] && xbox_OverlayWants()) {
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        xbox_OverlayVulkanDraw(C, cb, s_sc_view[idx], s_sc_ext.width, s_sc_ext.height,
                               s_sc_fmt, s_sc_n);
        nvk_image_barrier(cb, s_sc_img[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
#endif
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
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || (pr == VK_SUBOPTIMAL_KHR && FBV_SUBOPTIMAL_RECREATES))
        s_recreate = 1;
#if defined(__ANDROID__)
    if (pr == VK_ERROR_SURFACE_LOST_KHR)
        s_surface_lost = 1;
#endif
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
