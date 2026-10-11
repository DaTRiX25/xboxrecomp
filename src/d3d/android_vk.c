/*
 * android_vk.c - the Vulkan loader the runtime calls on Android: the
 * system's, or a replacement driver opened through libadrenotools. See
 * android_vk.h.
 *
 * The replacement (android_host.h has the variables; the activity sets them):
 *   RECOMP_ANDROID_DRIVER_DIR   a directory in the app's own internal storage
 *                               or its native library directory (the dynamic
 *                               linker refuses anything on shared storage)
 *   RECOMP_ANDROID_DRIVER_NAME  the driver's file name there, e.g.
 *                               libvulkan_freedreno.so (Mesa Turnip)
 *   RECOMP_ANDROID_LIB_DIR      the app's native library directory, where
 *                               libadrenotools' hook libraries are
 * libadrenotools (github.com/bylaws/libadrenotools, BSD-2-Clause) opens a
 * private copy of the system's libvulkan.so whose driver lookup ends at that
 * file; no root, nothing outside the app. Without the variables, or built
 * without libadrenotools, the loader is the system's.
 */
#if defined(__ANDROID__)

#include "android_vk.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XBOXRECOMP_HAVE_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr s_gipa;
static char s_driver[256] = "system";
static int s_failed;

static void open_loader(void)
{
    void *h = NULL;
    const char *dir = getenv("RECOMP_ANDROID_DRIVER_DIR");
    const char *name = getenv("RECOMP_ANDROID_DRIVER_NAME");
    const char *lib = getenv("RECOMP_ANDROID_LIB_DIR");

    if (dir && *dir && name && *name && strcmp(name, "system")) {
#if defined(XBOXRECOMP_HAVE_ADRENOTOOLS)
        char d[1024];
        size_t n = strlen(dir);
        /* libadrenotools joins directory and name as they are. */
        snprintf(d, sizeof d, "%s%s", dir, n && dir[n - 1] == '/' ? "" : "/");
        if (lib && *lib)
            h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, lib,
                                           d, name, NULL, NULL);
        if (h) {
            snprintf(s_driver, sizeof s_driver, "%s", name);
            fprintf(stderr, "[vulkan] replacement driver %s%s opened through libadrenotools\n",
                    d, name);
        } else {
            s_failed = 1;
            fprintf(stderr, "[vulkan] replacement driver %s%s could not be opened through"
                    " libadrenotools (hooks in %s); the system driver instead\n",
                    d, name, lib && *lib ? lib : "(RECOMP_ANDROID_LIB_DIR not set)");
        }
#else
        s_failed = 1;
        fprintf(stderr, "[vulkan] RECOMP_ANDROID_DRIVER_NAME=%s: this build has no"
                " libadrenotools; the system driver instead\n", name);
#endif
    }
    if (!h)
        h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (h)
        s_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!s_gipa)
        fprintf(stderr, "[vulkan] no Vulkan loader: %s\n", dlerror());
}

const char *xr_android_vk_driver(void)
{
    pthread_once(&s_once, open_loader);
    return s_driver;
}

int xr_android_vk_driver_failed(void)
{
    pthread_once(&s_once, open_loader);
    return s_failed;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name)
{
    pthread_once(&s_once, open_loader);
    return s_gipa ? s_gipa(instance, name) : NULL;
}

/* The four commands that exist before an instance does start as these: they
 * open the loader and go on to its own. vkCreateInstance then fills in every
 * other pointer from the new instance (one instance and one device in this
 * runtime, so the instance-level entry points serve the device too). */
static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceVersion(uint32_t *version)
{
    PFN_vkEnumerateInstanceVersion f = (PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!f) {               /* a Vulkan 1.0 loader has none */
        *version = VK_API_VERSION_1_0;
        return s_gipa ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(version);
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    PFN_vkEnumerateInstanceExtensionProperties f = (PFN_vkEnumerateInstanceExtensionProperties)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!f) {
        *count = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(layer, count, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceLayerProperties(
    uint32_t *count, VkLayerProperties *props)
{
    PFN_vkEnumerateInstanceLayerProperties f = (PFN_vkEnumerateInstanceLayerProperties)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    if (!f) {
        *count = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(count, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkCreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    PFN_vkCreateInstance f = (PFN_vkCreateInstance)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r;

    if (!f)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = f(ci, alloc, instance);
    if (r != VK_SUCCESS)
        return r;
#define XR_ANDROID_VK_LOAD(name) \
    if (strcmp(#name, "vkCreateInstance") && strcmp(#name, "vkEnumerateInstanceVersion") \
            && strcmp(#name, "vkEnumerateInstanceExtensionProperties") \
            && strcmp(#name, "vkEnumerateInstanceLayerProperties")) \
        xr_##name = (PFN_##name)s_gipa(*instance, #name);
    XR_ANDROID_VK_FUNCS(XR_ANDROID_VK_LOAD)
#undef XR_ANDROID_VK_LOAD
    /* The KHR names of what Vulkan 1.3 took in: the same commands. */
    if (!xr_vkCmdBeginRenderingKHR) xr_vkCmdBeginRenderingKHR = xr_vkCmdBeginRendering;
    if (!xr_vkCmdEndRenderingKHR)   xr_vkCmdEndRenderingKHR = xr_vkCmdEndRendering;
    if (!xr_vkCmdBeginRendering)    xr_vkCmdBeginRendering = xr_vkCmdBeginRenderingKHR;
    if (!xr_vkCmdEndRendering)      xr_vkCmdEndRendering = xr_vkCmdEndRenderingKHR;
    return r;
}

/* The pointers. Null until an instance exists, but for the four above. */
#define XR_ANDROID_VK_DEFINE(name) PFN_##name xr_##name;
XR_ANDROID_VK_FUNCS(XR_ANDROID_VK_DEFINE)
#undef XR_ANDROID_VK_DEFINE

__attribute__((constructor)) static void boot_pointers(void)
{
    xr_vkEnumerateInstanceVersion = boot_vkEnumerateInstanceVersion;
    xr_vkEnumerateInstanceExtensionProperties = boot_vkEnumerateInstanceExtensionProperties;
    xr_vkEnumerateInstanceLayerProperties = boot_vkEnumerateInstanceLayerProperties;
    xr_vkCreateInstance = boot_vkCreateInstance;
}

#else
typedef int xr_android_vk_unused;
#endif
