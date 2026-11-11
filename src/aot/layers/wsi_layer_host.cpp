// ============================================================================
// src/aot/layers/wsi_layer_host.cpp
// Host x86-64 Vulkan Layer (`VK_LAYER_WSI_PanthorWsiLayer` / `libVkLayer_WSI_panthor_wsi.so`)
// Ensures VK_KHR_surface + X11/XCB/Wayland/Win32/Headless WSI instance and device
// extensions and surface/swapchain entrypoints are advertised across all ICDs and runners
// (`scripts/run_aot_lifter.sh`, `scripts/run_dxvk_aot.sh`, and host `libvulkan_panfrost.so`).
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>

extern "C" {

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void* instance, const char* pName);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void* device, const char* pName);

struct VkExtensionProperties {
    char     extensionName[256];
    uint32_t specVersion;
};

struct VkLayerProperties {
    char     layerName[256];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char     description[256];
};

static PFN_vkGetInstanceProcAddr g_wsi_next_gipa = nullptr;
static PFN_vkGetDeviceProcAddr   g_wsi_next_gdpa = nullptr;
static void* g_wsi_last_instance = nullptr;
static void* g_wsi_last_device   = nullptr;
static uint64_t g_wsi_queries    = 0;

static const VkExtensionProperties kWsiInstanceExts[] = {
    {"VK_KHR_surface", 25},
    {"VK_KHR_xcb_surface", 6},
    {"VK_KHR_xlib_surface", 6},
    {"VK_KHR_wayland_surface", 6},
    {"VK_KHR_win32_surface", 6},
    {"VK_EXT_headless_surface", 1},
    {"VK_KHR_display", 23},
    {"VK_KHR_get_physical_device_properties2", 2},
    {"VK_KHR_get_surface_capabilities2", 1},
    {"VK_EXT_surface_maintenance1", 1},
    {"VK_EXT_swapchain_colorspace", 4},
    {"VK_EXT_debug_utils", 2},
    {"VK_EXT_debug_report", 10},
    {"VK_KHR_external_memory_capabilities", 1},
    {"VK_KHR_external_semaphore_capabilities", 1},
    {"VK_KHR_external_fence_capabilities", 1},
    {"VK_KHR_device_group_creation", 1},
};

static const VkExtensionProperties kWsiDeviceExts[] = {
    {"VK_KHR_swapchain", 70},
    {"VK_KHR_swapchain_mutable_format", 1},
    {"VK_EXT_swapchain_maintenance1", 1},
    {"VK_KHR_incremental_present", 2},
};

static bool hasExt(const std::vector<VkExtensionProperties>& vec, const char* name) {
    for (const auto& e : vec) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

__attribute__((visibility("default")))
uint64_t wsi_layer_get_query_count(void) {
    return g_wsi_queries;
}

__attribute__((visibility("default")))
int32_t WsiLayer_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1; return 0; }
    if (*pPropertyCount == 0) return 5;
    std::memset(&pProperties[0], 0, sizeof(VkLayerProperties));
    std::snprintf(pProperties[0].layerName, 256, "VK_LAYER_WSI_PanthorWsiLayer");
    pProperties[0].specVersion = (1U << 22) | (3U << 12);
    pProperties[0].implementationVersion = 1U;
    std::snprintf(pProperties[0].description, 256,
                  "Panthor WSI Compatibility Layer (VK_KHR_surface + X11/XCB/Wayland/Win32/Headless)");
    *pPropertyCount = 1;
    return 0;
}

__attribute__((visibility("default")))
int32_t WsiLayer_EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    ++g_wsi_queries;
    const uint32_t kCount = static_cast<uint32_t>(sizeof(kWsiInstanceExts) / sizeof(kWsiInstanceExts[0]));
    if (pLayerName && (std::strcmp(pLayerName, "VK_LAYER_WSI_PanthorWsiLayer") == 0 ||
                       std::strcmp(pLayerName, "VK_LAYER_WSI_PanthorWsiLayer1") == 0)) {
        if (!pPropertyCount) return -3;
        if (!pProperties) { *pPropertyCount = kCount; return 0; }
        uint32_t to_copy = std::min(*pPropertyCount, kCount);
        std::memcpy(pProperties, kWsiInstanceExts, to_copy * sizeof(VkExtensionProperties));
        *pPropertyCount = to_copy;
        return (to_copy < kCount) ? 5 : 0;
    }
    std::vector<VkExtensionProperties> merged;
    if (g_wsi_next_gipa) {
        auto fn = reinterpret_cast<int32_t (*)(const char*, uint32_t*, VkExtensionProperties*)>(
            g_wsi_next_gipa(g_wsi_last_instance, "vkEnumerateInstanceExtensionProperties"));
        if (fn) {
            uint32_t cnt = 0;
            if (fn(pLayerName, &cnt, nullptr) == 0 && cnt > 0) {
                merged.resize(cnt);
                fn(pLayerName, &cnt, merged.data());
                merged.resize(cnt);
            }
        }
    }
    if (!pLayerName) {
        for (const auto& ext : kWsiInstanceExts) {
            if (!hasExt(merged, ext.extensionName)) merged.push_back(ext);
        }
    }
    if (!pPropertyCount) return -3;
    uint32_t total = static_cast<uint32_t>(merged.size());
    if (!pProperties) { *pPropertyCount = total; return 0; }
    uint32_t to_copy = std::min(*pPropertyCount, total);
    if (to_copy > 0) std::memcpy(pProperties, merged.data(), to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return (to_copy < total) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t WsiLayer_EnumerateDeviceExtensionProperties(void* physicalDevice, const char* pLayerName,
                                                    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    ++g_wsi_queries;
    const uint32_t kCount = static_cast<uint32_t>(sizeof(kWsiDeviceExts) / sizeof(kWsiDeviceExts[0]));
    if (pLayerName && (std::strcmp(pLayerName, "VK_LAYER_WSI_PanthorWsiLayer") == 0 ||
                       std::strcmp(pLayerName, "VK_LAYER_WSI_PanthorWsiLayer1") == 0)) {
        if (!pPropertyCount) return -3;
        if (!pProperties) { *pPropertyCount = kCount; return 0; }
        uint32_t to_copy = std::min(*pPropertyCount, kCount);
        std::memcpy(pProperties, kWsiDeviceExts, to_copy * sizeof(VkExtensionProperties));
        *pPropertyCount = to_copy;
        return (to_copy < kCount) ? 5 : 0;
    }
    std::vector<VkExtensionProperties> merged;
    if (g_wsi_next_gipa) {
        auto fn = reinterpret_cast<int32_t (*)(void*, const char*, uint32_t*, VkExtensionProperties*)>(
            g_wsi_next_gipa(g_wsi_last_instance, "vkEnumerateDeviceExtensionProperties"));
        if (fn) {
            uint32_t cnt = 0;
            if (fn(physicalDevice, pLayerName, &cnt, nullptr) == 0 && cnt > 0) {
                merged.resize(cnt);
                fn(physicalDevice, pLayerName, &cnt, merged.data());
                merged.resize(cnt);
            }
        }
    }
    if (!pLayerName) {
        for (const auto& ext : kWsiDeviceExts) {
            if (!hasExt(merged, ext.extensionName)) merged.push_back(ext);
        }
    }
    if (!pPropertyCount) return -3;
    uint32_t total = static_cast<uint32_t>(merged.size());
    if (!pProperties) { *pPropertyCount = total; return 0; }
    uint32_t to_copy = std::min(*pPropertyCount, total);
    if (to_copy > 0) std::memcpy(pProperties, merged.data(), to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return (to_copy < total) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t WsiLayer_CreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    struct LayerLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; void* pfnNextGetPhysicalDeviceProcAddr; };
    struct LayerInfo { int32_t sType; const void* pNext; int32_t function; LayerLink* pLayerInfo; };
    const auto* base = static_cast<const LayerInfo*>(pCreateInfo);
    const LayerInfo* cur = base ? static_cast<const LayerInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 47 && cur->function == 0 && cur->pLayerInfo) {
            g_wsi_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            const_cast<LayerInfo*>(cur)->pLayerInfo = static_cast<LayerLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerInfo*>(cur->pNext);
    }
    if (!g_wsi_next_gipa) return -3;
    auto fpCreate = reinterpret_cast<int32_t (*)(const void*, const void*, void**)>(
        g_wsi_next_gipa(nullptr, "vkCreateInstance"));
    if (!fpCreate) return -3;
    int32_t res = fpCreate(pCreateInfo, pAllocator, pInstance);
    if (res == 0 && pInstance) g_wsi_last_instance = *pInstance;
    return res;
}

__attribute__((visibility("default")))
int32_t WsiLayer_CreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice) {
    struct LayerDevLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr; };
    struct LayerDevInfo { int32_t sType; const void* pNext; int32_t function; LayerDevLink* pLayerInfo; };
    const auto* base = static_cast<const LayerDevInfo*>(pCreateInfo);
    const LayerDevInfo* cur = base ? static_cast<const LayerDevInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 48 && cur->function == 0 && cur->pLayerInfo) {
            g_wsi_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            g_wsi_next_gdpa = cur->pLayerInfo->pfnNextGetDeviceProcAddr;
            const_cast<LayerDevInfo*>(cur)->pLayerInfo = static_cast<LayerDevLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerDevInfo*>(cur->pNext);
    }
    auto fpCreate = reinterpret_cast<int32_t (*)(void*, const void*, const void*, void**)>(
        g_wsi_next_gipa ? g_wsi_next_gipa(g_wsi_last_instance, "vkCreateDevice") : nullptr);
    if (!fpCreate) return -3;
    int32_t res = fpCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == 0 && pDevice) g_wsi_last_device = *pDevice;
    return res;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(void* device, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (g_wsi_next_gdpa) {
        PFN_vkVoidFunction fn = g_wsi_next_gdpa(device, pName);
        if (fn) return fn;
    }
    return g_wsi_next_gipa ? g_wsi_next_gipa(g_wsi_last_instance, pName) : nullptr;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)                    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&WsiLayer_EnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&WsiLayer_EnumerateInstanceExtensionProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&WsiLayer_EnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkCreateInstance") == 0)                       return reinterpret_cast<PFN_vkVoidFunction>(&WsiLayer_CreateInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0)                         return reinterpret_cast<PFN_vkVoidFunction>(&WsiLayer_CreateDevice);
    return g_wsi_next_gipa ? g_wsi_next_gipa(instance, pName) : nullptr;
}

__attribute__((visibility("default")))
int32_t vkNegotiateLoaderLayerInterfaceVersion(void* pVersionStruct) {
    if (!pVersionStruct) return -3;
    auto* u32 = static_cast<uint32_t*>(pVersionStruct);
    if (u32[1] > 2U) u32[1] = 2U;
    auto* ptrs = reinterpret_cast<void**>(static_cast<uint8_t*>(pVersionStruct) + 16);
    ptrs[0] = reinterpret_cast<void*>(&vkGetInstanceProcAddr);
    ptrs[1] = reinterpret_cast<void*>(&vkGetDeviceProcAddr);
    ptrs[2] = nullptr;
    return 0;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction WsiLayer_GetInstanceProcAddr(void* instance, const char* pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction WsiLayer_GetDeviceProcAddr(void* device, const char* pName) {
    return vkGetDeviceProcAddr(device, pName);
}

__attribute__((visibility("default")))
int32_t WsiLayer_NegotiateLoaderLayerInterfaceVersion(void* pVersionStruct) {
    return vkNegotiateLoaderLayerInterfaceVersion(pVersionStruct);
}

} // extern "C"
