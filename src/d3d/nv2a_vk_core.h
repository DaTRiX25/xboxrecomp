/**
 * The Vulkan device the pushbuffer executor's Vulkan backend (nv2a_pb_vulkan.c)
 * draws with and the SDL window's Vulkan presenter (src/video/
 * fb_present_vulkan.c) shows frames with: one instance, one physical device,
 * one logical device, one graphics queue.
 *
 * The window is made first, on the process main thread, before the guest
 * starts: its presenter calls nvk_core_init() with the instance extensions
 * SDL wants and a callback that makes the surface (SDL_Vulkan_CreateSurface),
 * so the device is picked for a queue that can present there. The backend
 * initialises lazily on the NV2A ack thread and adopts that device; headless,
 * it calls nvk_core_init() itself, without a surface.
 *
 * The queue is shared by two threads (the ack thread's submits, the main
 * thread's presents), so every vkQueueSubmit and vkQueuePresentKHR on it
 * holds nvk_queue_lock(). Submission order on the one queue is what orders
 * a backend blit into a window slot before the present that reads it.
 *
 * Requires Vulkan 1.3 (dynamic rendering, synchronization2, extended dynamic
 * state, timeline semaphores): RADV on the Steam Deck and MoltenVK 1.2.11+.
 */
#ifndef XBOXRECOMP_NV2A_VK_CORE_H
#define XBOXRECOMP_NV2A_VK_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvk_core {
    VkInstance                       inst;
    VkPhysicalDevice                 phys;
    VkDevice                         dev;
    VkQueue                          queue;
    uint32_t                         qfam;
    VkSurfaceKHR                     surface;   /* VK_NULL_HANDLE headless */
    VkPhysicalDeviceProperties       props;
    VkPhysicalDeviceMemoryProperties mem;
    VkFormat                         depth_format;   /* D32S8, else D24S8 */
    int                              portability;    /* MoltenVK */
    int                              validation;     /* layers on */
    int                              present_wait;   /* VK_KHR_present_id + _wait on */
    int                              demote;         /* shaderDemoteToHelperInvocation on: SPIR-V 1.6's discard */
    float                            max_aniso;      /* samplerAnisotropy on: the limit; else 0 */
    char                             name[256];
};

/* The device, made once. inst_exts/n: extensions the window system needs
 * (SDL_Vulkan_GetInstanceExtensions), NULL/0 headless. make_surface, when
 * given, is called with the new instance and must return the window's
 * surface; the device is then picked for a queue family that presents to it
 * and gets VK_KHR_swapchain. Returns the core, or NULL with a reason in err
 * (and on any later call, the same result: init runs once). Thread-safe. */
const struct nvk_core *nvk_core_init(const char *const *inst_exts, unsigned n,
                                     VkSurfaceKHR (*make_surface)(VkInstance, void *),
                                     void *ctx, char *err, size_t errn);
/* A window whose native surface was taken away and given back (an Android
 * app coming back from the background): the old VkSurfaceKHR is destroyed
 * and make_surface asked for a new one, which the core then holds. The
 * caller has destroyed the swapchain first. 0 when no surface could be made
 * (the core then has none; call again later). */
int nvk_core_replace_surface(VkSurfaceKHR (*make_surface)(VkInstance, void *), void *ctx);

/* The core if nvk_core_init has succeeded, else NULL. */
const struct nvk_core *nvk_core_get(void);

/* The device is lost (VK_ERROR_DEVICE_LOST from a submit or a present, or a
 * swapchain that cannot be made any more): said once in the log, and passed
 * to whoever shows the picture (nvk_set_lost_handler: the window, which tells
 * the player and ends the run the ordinary way). Without a handler, a run
 * that has no window: the process ends with status 70 rather than go on
 * drawing nothing. Any thread. */
void nvk_device_lost(const char *why);
void nvk_set_lost_handler(void (*fn)(const char *why));

void nvk_queue_lock(void);
void nvk_queue_unlock(void);

/* A memory type index for `bits` with all of `want`, preferring one that
 * also has `prefer`; -1 if none. */
int nvk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer);

/* A buffer with its own memory, mapped when host visible. */
struct nvk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    void          *map;
};
int  nvk_buffer_new(struct nvk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer);
void nvk_buffer_free(struct nvk_buffer *b);

/* A 2D image with its own device-local memory and a view of all of it. */
struct nvk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFormat       format;
    VkImageAspectFlags aspect;
    uint32_t       w, h, levels;
    VkImageLayout  layout;      /* as the recorded commands leave it */
};
int  nvk_image_new(struct nvk_image *im, VkFormat fmt, uint32_t w, uint32_t h,
                   uint32_t levels, VkImageUsageFlags usage, VkImageAspectFlags aspect);
void nvk_image_free(struct nvk_image *im);
/* A cube image (six layers) of side x side and its cube view. */
int  nvk_image_new_cube(struct nvk_image *im, VkFormat fmt, uint32_t side, uint32_t levels,
                        VkImageUsageFlags usage);

/* One image layout transition (synchronization2), with the stages and
 * accesses each layout is used with in this renderer. aspect 0: colour. */
void nvk_image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                       VkImageLayout from, VkImageLayout to);
/* nvk_image_barrier on im, from its tracked layout; updates it. A no-op when
 * it is already there, except for TRANSFER_DST and GENERAL, which take a
 * write-after-write barrier anyway. */
void nvk_image_to(VkCommandBuffer cb, struct nvk_image *im, VkImageLayout to);

const char *nvk_result_str(VkResult r);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_VK_CORE_H */
