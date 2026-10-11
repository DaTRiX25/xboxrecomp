/*
 * android_vk.h - Vulkan through function pointers, on Android only.
 *
 * Everywhere else the runtime calls the Vulkan loader it links (libvulkan,
 * vulkan-1.dll, MoltenVK). On Android the driver a title wants is often not
 * the system's: a phone's stock Adreno driver stops at Vulkan 1.1, and a
 * replacement (Mesa Turnip) is loaded into the app through libadrenotools,
 * which hands back its own copy of the system loader. Calls into the
 * libvulkan.so this library links would still reach the stock driver, so on
 * Android every vk* name the runtime uses is a pointer filled from the loader
 * android_vk.c opened, the way volk does it.
 *
 * The build force-includes this header into the runtime's Vulkan sources
 * (-include, src/d3d/CMakeLists.txt): those sources are not edited, and a
 * call to a function missing from the list below does not compile (there
 * are no prototypes), so the list cannot fall behind unnoticed.
 *
 * vkGetInstanceProcAddr is the exception: a real, exported function of the
 * game's library (android_vk.c). The presenter asks SDL to use "the loader
 * linked into this binary" (dladdr on it, fb_present_vulkan.c), SDL finds
 * this one, and the surface it makes belongs to the same loader as the
 * instance.
 *
 * Which driver: android_vk.c reads RECOMP_ANDROID_DRIVER_DIR and
 * RECOMP_ANDROID_DRIVER_NAME (android_host.h).
 */
#ifndef RECOMP_ANDROID_VK_H
#define RECOMP_ANDROID_VK_H

#if defined(__ANDROID__)

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include <vulkan/vulkan.h>

/* Dear ImGui's Vulkan backend reads VK_NO_PROTOTYPES as "you load the
 * functions for me" unless a loader that defines the vk* names is there,
 * which it tells by volk's include guard. This header is such a loader. */
#ifndef VOLK_H_
#define VOLK_H_
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Every Vulkan command the runtime and Dear ImGui's backend call. */
#define XR_ANDROID_VK_FUNCS(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginQuery) \
    X(vkCmdBeginRendering) \
    X(vkCmdBeginRenderingKHR) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBlitImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdEndQuery) \
    X(vkCmdEndRendering) \
    X(vkCmdEndRenderingKHR) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdPushConstants) \
    X(vkCmdResetQueryPool) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetCullMode) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetFrontFace) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewport) \
    X(vkCmdWriteTimestamp) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) \
    X(vkCreateFence) \
    X(vkCreateFramebuffer) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreateInstance) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateQueryPool) \
    X(vkCreateRenderPass) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) \
    X(vkDestroyFence) \
    X(vkDestroyFramebuffer) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyInstance) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineCache) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyQueryPool) \
    X(vkDestroyRenderPass) \
    X(vkDestroySampler) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySurfaceKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFlushMappedMemoryRanges) \
    X(vkFreeCommandBuffers) \
    X(vkFreeDescriptorSets) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPipelineCacheData) \
    X(vkGetQueryPoolResults) \
    X(vkGetSemaphoreCounterValue) \
    X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandBuffer) \
    X(vkResetCommandPool) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkResetQueryPool) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences) \
    X(vkWaitForPresentKHR) \
    X(vkWaitSemaphores)

#define XR_ANDROID_VK_DECLARE(name) extern PFN_##name xr_##name;
XR_ANDROID_VK_FUNCS(XR_ANDROID_VK_DECLARE)
#undef XR_ANDROID_VK_DECLARE

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name);

/* What the loader is: "system" or the replacement driver's file name; and
 * whether a replacement was asked for and could not be opened. */
const char *xr_android_vk_driver(void);
int xr_android_vk_driver_failed(void);

#ifdef __cplusplus
}
#endif

#define vkAcquireNextImageKHR xr_vkAcquireNextImageKHR
#define vkAllocateCommandBuffers xr_vkAllocateCommandBuffers
#define vkAllocateDescriptorSets xr_vkAllocateDescriptorSets
#define vkAllocateMemory xr_vkAllocateMemory
#define vkBeginCommandBuffer xr_vkBeginCommandBuffer
#define vkBindBufferMemory xr_vkBindBufferMemory
#define vkBindImageMemory xr_vkBindImageMemory
#define vkCmdBeginQuery xr_vkCmdBeginQuery
#define vkCmdBeginRendering xr_vkCmdBeginRendering
#define vkCmdBeginRenderingKHR xr_vkCmdBeginRenderingKHR
#define vkCmdBindDescriptorSets xr_vkCmdBindDescriptorSets
#define vkCmdBindIndexBuffer xr_vkCmdBindIndexBuffer
#define vkCmdBindPipeline xr_vkCmdBindPipeline
#define vkCmdBindVertexBuffers xr_vkCmdBindVertexBuffers
#define vkCmdBlitImage xr_vkCmdBlitImage
#define vkCmdClearAttachments xr_vkCmdClearAttachments
#define vkCmdClearColorImage xr_vkCmdClearColorImage
#define vkCmdCopyBufferToImage xr_vkCmdCopyBufferToImage
#define vkCmdCopyImage xr_vkCmdCopyImage
#define vkCmdCopyImageToBuffer xr_vkCmdCopyImageToBuffer
#define vkCmdDraw xr_vkCmdDraw
#define vkCmdDrawIndexed xr_vkCmdDrawIndexed
#define vkCmdEndQuery xr_vkCmdEndQuery
#define vkCmdEndRendering xr_vkCmdEndRendering
#define vkCmdEndRenderingKHR xr_vkCmdEndRenderingKHR
#define vkCmdPipelineBarrier xr_vkCmdPipelineBarrier
#define vkCmdPipelineBarrier2 xr_vkCmdPipelineBarrier2
#define vkCmdPushConstants xr_vkCmdPushConstants
#define vkCmdResetQueryPool xr_vkCmdResetQueryPool
#define vkCmdSetBlendConstants xr_vkCmdSetBlendConstants
#define vkCmdSetCullMode xr_vkCmdSetCullMode
#define vkCmdSetDepthBias xr_vkCmdSetDepthBias
#define vkCmdSetDepthBiasEnable xr_vkCmdSetDepthBiasEnable
#define vkCmdSetDepthCompareOp xr_vkCmdSetDepthCompareOp
#define vkCmdSetDepthTestEnable xr_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable xr_vkCmdSetDepthWriteEnable
#define vkCmdSetFrontFace xr_vkCmdSetFrontFace
#define vkCmdSetScissor xr_vkCmdSetScissor
#define vkCmdSetStencilCompareMask xr_vkCmdSetStencilCompareMask
#define vkCmdSetStencilOp xr_vkCmdSetStencilOp
#define vkCmdSetStencilReference xr_vkCmdSetStencilReference
#define vkCmdSetStencilTestEnable xr_vkCmdSetStencilTestEnable
#define vkCmdSetStencilWriteMask xr_vkCmdSetStencilWriteMask
#define vkCmdSetViewport xr_vkCmdSetViewport
#define vkCmdWriteTimestamp xr_vkCmdWriteTimestamp
#define vkCreateBuffer xr_vkCreateBuffer
#define vkCreateCommandPool xr_vkCreateCommandPool
#define vkCreateDescriptorPool xr_vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout xr_vkCreateDescriptorSetLayout
#define vkCreateDevice xr_vkCreateDevice
#define vkCreateFence xr_vkCreateFence
#define vkCreateFramebuffer xr_vkCreateFramebuffer
#define vkCreateGraphicsPipelines xr_vkCreateGraphicsPipelines
#define vkCreateImage xr_vkCreateImage
#define vkCreateImageView xr_vkCreateImageView
#define vkCreateInstance xr_vkCreateInstance
#define vkCreatePipelineCache xr_vkCreatePipelineCache
#define vkCreatePipelineLayout xr_vkCreatePipelineLayout
#define vkCreateQueryPool xr_vkCreateQueryPool
#define vkCreateRenderPass xr_vkCreateRenderPass
#define vkCreateSampler xr_vkCreateSampler
#define vkCreateSemaphore xr_vkCreateSemaphore
#define vkCreateShaderModule xr_vkCreateShaderModule
#define vkCreateSwapchainKHR xr_vkCreateSwapchainKHR
#define vkDestroyBuffer xr_vkDestroyBuffer
#define vkDestroyCommandPool xr_vkDestroyCommandPool
#define vkDestroyDescriptorPool xr_vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout xr_vkDestroyDescriptorSetLayout
#define vkDestroyDevice xr_vkDestroyDevice
#define vkDestroyFence xr_vkDestroyFence
#define vkDestroyFramebuffer xr_vkDestroyFramebuffer
#define vkDestroyImage xr_vkDestroyImage
#define vkDestroyImageView xr_vkDestroyImageView
#define vkDestroyInstance xr_vkDestroyInstance
#define vkDestroyPipeline xr_vkDestroyPipeline
#define vkDestroyPipelineCache xr_vkDestroyPipelineCache
#define vkDestroyPipelineLayout xr_vkDestroyPipelineLayout
#define vkDestroyQueryPool xr_vkDestroyQueryPool
#define vkDestroyRenderPass xr_vkDestroyRenderPass
#define vkDestroySampler xr_vkDestroySampler
#define vkDestroySemaphore xr_vkDestroySemaphore
#define vkDestroyShaderModule xr_vkDestroyShaderModule
#define vkDestroySurfaceKHR xr_vkDestroySurfaceKHR
#define vkDestroySwapchainKHR xr_vkDestroySwapchainKHR
#define vkDeviceWaitIdle xr_vkDeviceWaitIdle
#define vkEndCommandBuffer xr_vkEndCommandBuffer
#define vkEnumerateDeviceExtensionProperties xr_vkEnumerateDeviceExtensionProperties
#define vkEnumerateInstanceExtensionProperties xr_vkEnumerateInstanceExtensionProperties
#define vkEnumerateInstanceLayerProperties xr_vkEnumerateInstanceLayerProperties
#define vkEnumerateInstanceVersion xr_vkEnumerateInstanceVersion
#define vkEnumeratePhysicalDevices xr_vkEnumeratePhysicalDevices
#define vkFlushMappedMemoryRanges xr_vkFlushMappedMemoryRanges
#define vkFreeCommandBuffers xr_vkFreeCommandBuffers
#define vkFreeDescriptorSets xr_vkFreeDescriptorSets
#define vkFreeMemory xr_vkFreeMemory
#define vkGetBufferMemoryRequirements xr_vkGetBufferMemoryRequirements
#define vkGetDeviceProcAddr xr_vkGetDeviceProcAddr
#define vkGetDeviceQueue xr_vkGetDeviceQueue
#define vkGetImageMemoryRequirements xr_vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceFeatures xr_vkGetPhysicalDeviceFeatures
#define vkGetPhysicalDeviceFeatures2 xr_vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceFormatProperties xr_vkGetPhysicalDeviceFormatProperties
#define vkGetPhysicalDeviceMemoryProperties xr_vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceProperties xr_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties xr_vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR xr_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR xr_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR xr_vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR xr_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkGetPipelineCacheData xr_vkGetPipelineCacheData
#define vkGetQueryPoolResults xr_vkGetQueryPoolResults
#define vkGetSemaphoreCounterValue xr_vkGetSemaphoreCounterValue
#define vkGetSwapchainImagesKHR xr_vkGetSwapchainImagesKHR
#define vkMapMemory xr_vkMapMemory
#define vkQueuePresentKHR xr_vkQueuePresentKHR
#define vkQueueSubmit xr_vkQueueSubmit
#define vkQueueWaitIdle xr_vkQueueWaitIdle
#define vkResetCommandBuffer xr_vkResetCommandBuffer
#define vkResetCommandPool xr_vkResetCommandPool
#define vkResetDescriptorPool xr_vkResetDescriptorPool
#define vkResetFences xr_vkResetFences
#define vkResetQueryPool xr_vkResetQueryPool
#define vkUnmapMemory xr_vkUnmapMemory
#define vkUpdateDescriptorSets xr_vkUpdateDescriptorSets
#define vkWaitForFences xr_vkWaitForFences
#define vkWaitForPresentKHR xr_vkWaitForPresentKHR
#define vkWaitSemaphores xr_vkWaitSemaphores

#endif /* __ANDROID__ */
#endif /* RECOMP_ANDROID_VK_H */
