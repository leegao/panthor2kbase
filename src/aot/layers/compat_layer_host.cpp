// ============================================================================
// src/aot/layers/compat_layer_host.cpp
// Host x86-64 Vulkan Layer (`VK_LAYER_COMPAT_DxvkMaliCompatLayer` /
// `libVkLayer_COMPAT_dxvk_mali_compat.so`)
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void* instance, const char* pName);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void* device, const char* pName);

static PFN_vkGetInstanceProcAddr g_compat_next_gipa = nullptr;
static PFN_vkGetDeviceProcAddr   g_compat_next_gdpa = nullptr;
static void* g_compat_instance = nullptr;
static uint64_t g_compat_queries = 0;

__attribute__((visibility("default")))
uint64_t compat_layer_get_query_count(void) {
    return g_compat_queries;
}

static void apply_dxvk_mali_features(uint32_t* feat_words) {
    if (!feat_words) return;
    for (int i = 0; i < 55; ++i) {
        if (i != 54)
            feat_words[i] = 1U;
    }
    ++g_compat_queries;
}

struct CompatVkExtProp { char extensionName[256]; uint32_t specVersion; };
static const CompatVkExtProp kCompatWsiInstanceExts[] = {
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

__attribute__((visibility("default")))
int32_t CompatLayer_EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, void* pProperties) {
    if (pLayerName && (std::strcmp(pLayerName, "VK_LAYER_COMPAT_DxvkMaliCompatLayer") == 0 ||
                       std::strcmp(pLayerName, "VK_LAYER_COMPAT_DxvkMaliCompatLayer1") == 0)) {
        if (pPropertyCount) *pPropertyCount = 0;
        return 0;
    }
    if (g_compat_next_gipa) {
        auto fn = reinterpret_cast<int32_t (*)(const char*, uint32_t*, void*)>(
            g_compat_next_gipa(g_compat_instance, "vkEnumerateInstanceExtensionProperties"));
        if (fn) return fn(pLayerName, pPropertyCount, pProperties);
    }
    if (!pLayerName && pPropertyCount) {
        const uint32_t kCount = static_cast<uint32_t>(sizeof(kCompatWsiInstanceExts) / sizeof(kCompatWsiInstanceExts[0]));
        if (!pProperties) { *pPropertyCount = kCount; return 0; }
        uint32_t to_copy = (*pPropertyCount < kCount) ? *pPropertyCount : kCount;
        std::memcpy(pProperties, kCompatWsiInstanceExts, to_copy * sizeof(CompatVkExtProp));
        *pPropertyCount = to_copy;
        return (to_copy < kCount) ? 5 : 0;
    }
    if (pPropertyCount) *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t CompatLayer_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1; return 0; }
    if (*pPropertyCount == 0) return 5;
    std::memset(pProperties, 0, 520);
    std::snprintf(static_cast<char*>(pProperties), 256, "VK_LAYER_COMPAT_DxvkMaliCompatLayer");
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperties) + 256) = (1U << 22) | (3U << 12);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperties) + 260) = 1U;
    std::snprintf(static_cast<char*>(pProperties) + 264, 256, "Mali DXVK 1.10.3 Feature & Limit Compatibility Layer");
    *pPropertyCount = 1;
    return 0;
}

__attribute__((visibility("default")))
int32_t CompatLayer_CreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    struct LayerLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; void* pfnNextGetPhysicalDeviceProcAddr; };
    struct LayerInfo { int32_t sType; const void* pNext; int32_t function; LayerLink* pLayerInfo; };
    const auto* base = static_cast<const LayerInfo*>(pCreateInfo);
    const LayerInfo* cur = base ? static_cast<const LayerInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 47 && cur->function == 0 && cur->pLayerInfo) {
            g_compat_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            const_cast<LayerInfo*>(cur)->pLayerInfo = static_cast<LayerLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerInfo*>(cur->pNext);
    }
    if (!g_compat_next_gipa) return -3;
    auto fpCreate = reinterpret_cast<int32_t (*)(const void*, const void*, void**)>(
        g_compat_next_gipa(nullptr, "vkCreateInstance"));
    if (!fpCreate) return -3;
    int32_t res = fpCreate(pCreateInfo, pAllocator, pInstance);
    if (res == 0 && pInstance) g_compat_instance = *pInstance;
    return res;
}

__attribute__((visibility("default")))
int32_t CompatLayer_CreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice) {
    struct LayerDevLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr; };
    struct LayerDevInfo { int32_t sType; const void* pNext; int32_t function; LayerDevLink* pLayerInfo; };
    const auto* base = static_cast<const LayerDevInfo*>(pCreateInfo);
    const LayerDevInfo* cur = base ? static_cast<const LayerDevInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 48 && cur->function == 0 && cur->pLayerInfo) {
            g_compat_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            g_compat_next_gdpa = cur->pLayerInfo->pfnNextGetDeviceProcAddr;
            const_cast<LayerDevInfo*>(cur)->pLayerInfo = static_cast<LayerDevLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerDevInfo*>(cur->pNext);
    }
    auto fpCreate = reinterpret_cast<int32_t (*)(void*, const void*, const void*, void**)>(
        g_compat_next_gipa ? g_compat_next_gipa(g_compat_instance, "vkCreateDevice") : nullptr);
    if (!fpCreate) return -3;
    return fpCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

__attribute__((visibility("default")))
void CompatLayer_GetPhysicalDeviceFeatures(void* physicalDevice, void* pFeatures) {
    if (g_compat_next_gipa) {
        auto fn = reinterpret_cast<void (*)(void*, void*)>(
            g_compat_next_gipa(g_compat_instance, "vkGetPhysicalDeviceFeatures"));
        if (fn) fn(physicalDevice, pFeatures);
    }
    apply_dxvk_mali_features(static_cast<uint32_t*>(pFeatures));
}

__attribute__((visibility("default")))
void CompatLayer_GetPhysicalDeviceFeatures2(void* physicalDevice, void* pFeatures) {
    if (g_compat_next_gipa) {
        auto fn = reinterpret_cast<void (*)(void*, void*)>(
            g_compat_next_gipa(g_compat_instance, "vkGetPhysicalDeviceFeatures2"));
        if (fn) fn(physicalDevice, pFeatures);
    }
    if (pFeatures) {
        apply_dxvk_mali_features(reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pFeatures) + 16));
    }
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(void* device, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    return g_compat_next_gdpa ? g_compat_next_gdpa(device, pName) : nullptr;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_EnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_EnumerateInstanceExtensionProperties);
    if (std::strcmp(pName, "vkCreateInstance") == 0)                   return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_CreateInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0)                     return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_CreateDevice);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_GetPhysicalDeviceFeatures);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&CompatLayer_GetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)                return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    return g_compat_next_gipa ? g_compat_next_gipa(instance, pName) : nullptr;
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

} // extern "C"
