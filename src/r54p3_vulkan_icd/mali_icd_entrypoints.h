#pragma once
// Forward declarations for Vulkan ICD entrypoints implemented in
// mali_icd_physdev.cpp and mali_icd_wsi.cpp.
#include <cstdint>
#include <cstddef>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

extern "C" {
// From mali_icd_physdev.cpp
int32_t vkEnumerateInstanceVersion(uint32_t* pApiVersion);
int32_t vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* pProperties);
int32_t vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, void* pProperties);
int32_t vkCreateInstance(const void* pCreateInfo, const void* /*pAllocator*/, void** pInstance);
void vkDestroyInstance(void* instance, const void* /*pAllocator*/);
int32_t vkEnumeratePhysicalDevices(void* instance, uint32_t* pPhysicalDeviceCount, void** pPhysicalDevices);
void vkGetPhysicalDeviceProperties(void* physicalDevice, void* pProperties);
void vkGetPhysicalDeviceProperties2(void* physicalDevice, void* pProperties2);
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties);
// From mali_physdev_wrappers_autogen.inc
void vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures);
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures);
void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties);
void vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties);
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties);
void vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties);
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties);
void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties);
VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties);
VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties);
VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties);
void vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t* pPropertyCount, VkSparseImageFormatProperties* pProperties);
void vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo, uint32_t* pPropertyCount, VkSparseImageFormatProperties2* pProperties);
void vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo, VkExternalBufferProperties* pExternalBufferProperties);
void vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo, VkExternalFenceProperties* pExternalFenceProperties);
void vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo, VkExternalSemaphoreProperties* pExternalSemaphoreProperties);
VkResult vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);
// From mali_icd_wsi.cpp
int32_t vkGetPhysicalDeviceDisplayPropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties);
int32_t vkGetPhysicalDeviceDisplayPlanePropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties);
int32_t vkGetDisplayPlaneSupportedDisplaysKHR(void* /*physicalDevice*/, uint32_t /*planeIndex*/, uint32_t* pDisplayCount, uint64_t* pDisplays);
int32_t vkGetDisplayModePropertiesKHR(void* /*physicalDevice*/, uint64_t /*display*/, uint32_t* pPropertyCount, void* pProperties);
int32_t vkCreateDisplayModeKHR(void* /*physicalDevice*/, uint64_t /*display*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pMode);
int32_t vkGetDisplayPlaneCapabilitiesKHR(void* /*physicalDevice*/, uint64_t /*mode*/, uint32_t /*planeIndex*/, void* pCapabilities);
int32_t vkCreateDebugUtilsMessengerEXT(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pMessenger);
void vkDestroyDebugUtilsMessengerEXT(void* instance, uint64_t messenger, const void* pAllocator);
int32_t vkSetDebugUtilsObjectNameEXT(void* device, const void* pNameInfo);
void vkCmdBeginDebugUtilsLabelEXT(void* commandBuffer, const void* pLabelInfo);
void vkCmdEndDebugUtilsLabelEXT(void* commandBuffer);
void vkCmdInsertDebugUtilsLabelEXT(void* commandBuffer, const void* pLabelInfo);
int32_t vkCreatePipelineCache(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pPipelineCache);
void vkDestroyPipelineCache(void* device, uint64_t pipelineCache, const void* /*pAllocator*/);
int32_t vkGetPipelineCacheData(void* device, uint64_t pipelineCache, size_t* pDataSize, void* pData);
int32_t vkMergePipelineCaches(void* device, uint64_t dstCache, uint32_t srcCacheCount, const uint64_t* pSrcCaches);
void vkGetImageSubresourceLayout(void* device, uint64_t image, const void* pSubresource, void* pLayout);
int32_t vkFlushMappedMemoryRanges(void* device, uint32_t memoryRangeCount, const void* pMemoryRanges);
int32_t vkInvalidateMappedMemoryRanges(void* device, uint32_t memoryRangeCount, const void* pMemoryRanges);
int32_t vkGetFenceStatus(void* device, uint64_t fence);
int32_t vkCreateHeadlessSurfaceEXT(void* /*instance*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSurface);
int32_t vkCreateDisplayPlaneSurfaceKHR(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pSurface);
int32_t vkCreateXcbSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface);
int32_t vkCreateXlibSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface);
int32_t vkCreateWaylandSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface);
uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*connection*/, uint32_t /*visual_id*/);
uint32_t vkGetPhysicalDeviceXlibPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*dpy*/, uint64_t /*visualID*/);
uint32_t vkGetPhysicalDeviceWaylandPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*display*/);
void vkDestroySurfaceKHR(void* /*instance*/, uint64_t surface, const void* /*pAllocator*/);
int32_t vkGetPhysicalDeviceSurfaceSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, uint64_t /*surface*/, uint32_t* pSupported);
int32_t vkGetPhysicalDeviceSurfaceCapabilitiesKHR(void* /*physicalDevice*/, uint64_t surface, void* pSurfaceCapabilities);
int32_t vkGetPhysicalDeviceSurfaceFormatsKHR(void* /*physicalDevice*/, uint64_t /*surface*/, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats);
int32_t vkGetPhysicalDeviceSurfacePresentModesKHR(void* /*physicalDevice*/, uint64_t /*surface*/, uint32_t* pPresentModeCount, uint32_t* pPresentModes);
int32_t vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSwapchain);
void vkDestroySwapchainKHR(void* /*device*/, uint64_t swapchain, const void* /*pAllocator*/);
int32_t vkGetSwapchainImagesKHR(void* /*device*/, uint64_t swapchain, uint32_t* pSwapchainImageCount, uint64_t* pSwapchainImages);
int32_t vkAcquireNextImageKHR(void* /*device*/, uint64_t swapchain, uint64_t /*timeout*/, uint64_t /*semaphore*/, uint64_t /*fence*/, uint32_t* pImageIndex);
int32_t vkAcquireNextImage2KHR(void* device, const void* pAcquireInfo, uint32_t* pImageIndex);
int32_t vkQueuePresentKHR(void* /*queue*/, const void* pPresentInfo);
int32_t vkQueueWaitIdle(void* /*queue*/);
int32_t vkDeviceWaitIdle(void* device);
void vkCmdClearAttachments(void* commandBuffer, uint32_t attachmentCount, const void* pAttachments, uint32_t /*rectCount*/, const void* /*pRects*/);
void vkCmdClearColorImage(void* commandBuffer, uint64_t image, uint32_t /*imageLayout*/, const void* pColor, uint32_t /*rangeCount*/, const void* /*pRanges*/);
void vkCmdClearDepthStencilImage(void* /*commandBuffer*/, uint64_t /*image*/, uint32_t /*imageLayout*/, const void* /*pDepthStencil*/, uint32_t /*rangeCount*/, const void* /*pRanges*/);
void vkCmdExecuteCommands(void* commandBuffer, uint32_t commandBufferCount, void* const* pCommandBuffers);
void vkCmdNextSubpass(void* /*commandBuffer*/, uint32_t /*contents*/);
int32_t vkCreateEvent(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pEvent);
void vkDestroyEvent(void* device, uint64_t event, const void* pAllocator);
int32_t vkGetEventStatus(void* device, uint64_t event);
int32_t vkSetEvent(void* device, uint64_t event);
int32_t vkResetEvent(void* device, uint64_t event);
void vkCmdSetEvent(void* /*commandBuffer*/, uint64_t /*event*/, uint32_t /*stageMask*/);
void vkCmdResetEvent(void* /*commandBuffer*/, uint64_t /*event*/, uint32_t /*stageMask*/);
void vkCmdWaitEvents(void* /*commandBuffer*/, uint32_t /*eventCount*/, const uint64_t* /*pEvents*/, uint32_t /*srcStageMask*/, uint32_t /*dstStageMask*/, uint32_t /*memoryBarrierCount*/, const void* /*pMemoryBarriers*/, uint32_t /*bufferMemoryBarrierCount*/, const void* /*pBufferMemoryBarriers*/, uint32_t /*imageMemoryBarrierCount*/, const void* /*pImageMemoryBarriers*/);
int32_t vkGetPhysicalDeviceSurfaceCapabilities2KHR(void* physicalDevice, const void* pSurfaceInfo, void* pSurfaceCapabilities);
int32_t vkGetPhysicalDeviceSurfaceFormats2KHR(void* physicalDevice, const void* pSurfaceInfo, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats);
int32_t vkGetImageDrmFormatModifierPropertiesEXT(void* /*device*/, uint64_t /*image*/, void* pProperties);
int32_t vkGetMemoryFdKHR(void* /*device*/, const void* pGetFdInfo, int* pFd);
} // extern "C"
