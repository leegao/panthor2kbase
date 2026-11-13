// ============================================================================
// src/aot/src/mali_icd_physdev.cpp
// Instance & PhysicalDevice enumeration, limits, features, and extension
// queries (including VP_DXVK_d3d11_level_11_1_baseline & vkgears sample counts).
// ============================================================================
#include "mali_icd_internal.h"

// ============================================================================
// Section 5: Exported Vulkan ICD Functions
// ============================================================================
extern "C" {

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    if (!pApiVersion) return -3;
    g_aot_rt.ensureInitialized();
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkEnumerateInstanceVersion) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumerateInstanceVersion, pApiVersion);
        g_aot_rt.lifted_vk_calls_executed++;
        if (r == 0 && *pApiVersion != 0) return 0;
    }
    *pApiVersion = g_aot_rt.aot_queried_api_version;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    g_aot_rt.ensureInitialized();
    if (aot_vkEnumerateInstanceLayerProperties) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pPropertyCount);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pProperties);
        aot_vkEnumerateInstanceLayerProperties(&g_aot_rt.cpu);
        return static_cast<int32_t>(g_aot_rt.cpu.x[0]);
    }
    *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    g_aot_rt.ensureInitialized();
    if (pLayerName && pLayerName[0] != '\0') {
        *pPropertyCount = 0;
        return 0;
    }
    std::vector<VkExtensionPropertiesIcd> exts;
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkEnumerateInstanceExtensionProperties) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        uint32_t aot_count = 0;
        g_aot_rt.cpu.x[0] = 0;
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(&aot_count);
        g_aot_rt.cpu.x[2] = 0;
        aot_vkEnumerateInstanceExtensionProperties(&g_aot_rt.cpu);
        if (aot_count > 0 && aot_count <= 128) {
            exts.resize(aot_count);
            g_aot_rt.cpu.x[0] = 0;
            g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(&aot_count);
            g_aot_rt.cpu.x[2] = reinterpret_cast<uint64_t>(exts.data());
            aot_vkEnumerateInstanceExtensionProperties(&g_aot_rt.cpu);
        }
    }
    static const struct { const char* name; uint32_t ver; } kStdInstExts[] = {
        {"VK_KHR_surface", 25U},
        {"VK_EXT_headless_surface", 1U},
        {"VK_KHR_display", 23U},
        {"VK_KHR_xcb_surface", 6U},
        {"VK_KHR_xlib_surface", 6U},
        {"VK_KHR_wayland_surface", 6U},
        {"VK_KHR_get_physical_device_properties2", 2U},
        {"VK_KHR_get_surface_capabilities2", 1U},
        {"VK_KHR_external_memory_capabilities", 1U},
        {"VK_KHR_external_semaphore_capabilities", 1U},
        {"VK_KHR_external_fence_capabilities", 1U},
        {"VK_KHR_device_group_creation", 1U},
        {"VK_EXT_debug_utils", 2U},
        {"VK_EXT_debug_report", 10U},
        {"VK_EXT_swapchain_colorspace", 4U},
    };
    const char* dis_wl = getenv("MALI_AOT_DISABLE_WAYLAND_EXT");
    bool disable_wayland = false;
    if (dis_wl && dis_wl[0]) {
        disable_wayland = (std::atoi(dis_wl) != 0);
    } else {
        const char* layers = getenv("VK_INSTANCE_LAYERS");
        const char* disp = getenv("DISPLAY");
        const char* preload = getenv("LD_PRELOAD");
        if (layers && std::strstr(layers, "VK_LAYER_window_system_integration") != nullptr &&
            disp && disp[0] != '\0' &&
            preload && std::strstr(preload, "libmali_kbase_emu") != nullptr) {
            disable_wayland = true;
        }
    }
    if (disable_wayland) {
        exts.erase(std::remove_if(exts.begin(), exts.end(), [](const VkExtensionPropertiesIcd& e) {
            return std::strcmp(e.extensionName, "VK_KHR_wayland_surface") == 0;
        }), exts.end());
    }
    for (const auto& wext : kStdInstExts) {
        if (disable_wayland && std::strcmp(wext.name, "VK_KHR_wayland_surface") == 0) continue;
        bool found = false;
        for (auto& e : exts) {
            if (std::strcmp(e.extensionName, wext.name) == 0) {
                if (e.specVersion < wext.ver) e.specVersion = wext.ver;
                found = true;
                break;
            }
        }
        if (!found) {
            VkExtensionPropertiesIcd ep{};
            std::snprintf(ep.extensionName, sizeof(ep.extensionName), "%s", wext.name);
            ep.specVersion = wext.ver;
            exts.push_back(ep);
        }
    }
    uint32_t total = static_cast<uint32_t>(exts.size());
    if (!pProperties) {
        *pPropertyCount = total;
        return 0;
    }
    uint32_t n = std::min(*pPropertyCount, total);
    std::memcpy(pProperties, exts.data(), n * sizeof(VkExtensionPropertiesIcd));
    *pPropertyCount = n;
    return (n < total) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateInstance(const void* pCreateInfo, const void* /*pAllocator*/, void** pInstance) {
    if (!pInstance) return -3;
    if (!g_aot_rt.ensureInitialized()) return -3;
    auto* inst = new MaliIcdInstance();
    inst->loader_magic = ICD_LOADER_MAGIC;
    inst->phys_dev.loader_magic = ICD_LOADER_MAGIC;
    inst->phys_dev.parent_instance = inst;
    inst->phys_dev.initAotInternalLayout();
    *pInstance = inst;
    if (mali_icd_log_enabled() && pCreateInfo) {
        struct InstCI {
            uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
            uint32_t layerCount; const char* const* ppLayers;
            uint32_t extCount; const char* const* ppExts;
        };
        const auto* ci = static_cast<const InstCI*>(pCreateInfo);
        std::string exts_str;
        if (ci->extCount <= 64 && ci->ppExts) {
            for (uint32_t i = 0; i < ci->extCount; ++i) {
                if (ci->ppExts[i]) {
                    if (!exts_str.empty()) exts_str += ", ";
                    exts_str += ci->ppExts[i];
                }
            }
        }
        mali_icd_log("vkCreateInstance -> instance=%p driver_so='%s' lifted_vk_entrypoints=%lu enabled_exts(%u)=[%s]",
                     static_cast<void*>(inst),
                     g_aot_rt.mapper.so_path_loaded.empty() ? "standalone-builtin" : g_aot_rt.mapper.so_path_loaded.c_str(),
                     static_cast<unsigned long>(aot_get_lifted_vk_entrypoint_count()),
                     ci->extCount, exts_str.c_str());
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyInstance(void* instance, const void* /*pAllocator*/) {
    delete static_cast<MaliIcdInstance*>(instance);
}

__attribute__((visibility("default")))
int32_t vkEnumeratePhysicalDevices(void* instance, uint32_t* pPhysicalDeviceCount, void** pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return -3;
    auto* inst = static_cast<MaliIcdInstance*>(instance);
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return 0;
    }
    if (*pPhysicalDeviceCount == 0) return 5; // VK_INCOMPLETE
    pPhysicalDevices[0] = &inst->phys_dev;
    *pPhysicalDeviceCount = 1;
    return 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties(void* physicalDevice, void* pProperties) {
    if (!pProperties) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    auto* props = static_cast<VkPhysicalDevicePropertiesIcd*>(pProperties);
    std::memset(props, 0, sizeof(VkPhysicalDevicePropertiesIcd));

    // Execute AOT-lifted libGLES_mali.so vkGetPhysicalDeviceProperties (0x977730 -> 0x977738)
    // which populates all 504 bytes of genuine Valhall r54p3 VkPhysicalDeviceLimits & SparseProperties!
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceProperties && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal + 1024);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(props);
        aot_vkGetPhysicalDeviceProperties(&g_aot_rt.cpu);
    }
}

struct VkBaseOutStructureIcd {
    uint32_t               sType;
    VkBaseOutStructureIcd* pNext;
};

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2(void* physicalDevice, void* pProperties2) {
    if (!pProperties2) return;
    vkGetPhysicalDeviceProperties(physicalDevice, static_cast<uint8_t*>(pProperties2) + 16);

    const uint8_t kDevUuid[16] = {'M','A','L','I','-','G','6','1','5','-','D','E','V','I','C','E'};
    const uint8_t kDrvUuid[16] = {'A','R','M','-','M','A','L','I','-','R','5','4','P','3','-','0'};

    // Walk pNext chain for vulkaninfo & Vulkan 1.1/1.2/1.3/Extension property queries
    auto* cur = reinterpret_cast<VkBaseOutStructureIcd*>(pProperties2)->pNext;
    while (cur) {
        uint8_t* raw = reinterpret_cast<uint8_t*>(cur);
        switch (cur->sType) {
            case 49U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES
                std::memcpy(raw + 16, kDevUuid, 16);
                std::memcpy(raw + 32, kDrvUuid, 16);
                *reinterpret_cast<uint32_t*>(raw + 60) = 0U;   // deviceLUIDValid
                *reinterpret_cast<uint32_t*>(raw + 64) = 16U;  // subgroupSize (Valhall warp width = 16)
                *reinterpret_cast<uint32_t*>(raw + 68) = 0x3FU;// subgroupSupportedStages
                *reinterpret_cast<uint32_t*>(raw + 72) = 0x1FFU;// subgroupSupportedOperations
                *reinterpret_cast<uint32_t*>(raw + 76) = 1U;   // subgroupQuadOperationsInAllStages
                *reinterpret_cast<uint32_t*>(raw + 88) = 1U;   // maxMultiviewViewCount = 8
                *reinterpret_cast<uint32_t*>(raw + 96) = 1024U;// maxPerSetDescriptors
                *reinterpret_cast<uint64_t*>(raw + 104)= (4ULL << 30); // maxMemoryAllocationSize
                break;
            }
            case 51U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES
                *reinterpret_cast<uint32_t*>(raw + 16) = 9U; // VK_DRIVER_ID_ARM_PROPRIETARY
                std::snprintf(reinterpret_cast<char*>(raw + 20), 256, "Arm Proprietary (AOT libGLES_mali.so)");
                std::snprintf(reinterpret_cast<char*>(raw + 276), 256, "%s", g_aot_rt.mapper.driver_version.c_str());
                raw[532] = 1; raw[533] = 3; raw[534] = 7; raw[535] = 2; // conformanceVersion 1.3.7.2
                *reinterpret_cast<uint32_t*>(raw + 536) = 1U; // denormBehaviorIndependence
                *reinterpret_cast<uint32_t*>(raw + 540) = 1U; // roundingModeIndependence
                *reinterpret_cast<uint32_t*>(raw + 636) = 0x0FU; // supportedDepthResolveModes
                *reinterpret_cast<uint32_t*>(raw + 640) = 0x0FU; // supportedStencilResolveModes
                *reinterpret_cast<uint32_t*>(raw + 652) = 64U;   // maxTimelineSemaphoreValueDifference
                break;
            }
            case 53U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES
                *reinterpret_cast<uint32_t*>(raw + 16) = 16U;  // minSubgroupSize
                *reinterpret_cast<uint32_t*>(raw + 20) = 16U;  // maxSubgroupSize
                *reinterpret_cast<uint32_t*>(raw + 24) = 64U;  // maxComputeWorkgroupSubgroups
                *reinterpret_cast<uint32_t*>(raw + 28) = 0x3FU;// requiredSubgroupSizeStages
                *reinterpret_cast<uint32_t*>(raw + 32) = 4096U;// maxInlineUniformBlockSize
                *reinterpret_cast<uint32_t*>(raw + 36) = 16U;  // maxPerStageDescriptorInlineUniformBlocks
                *reinterpret_cast<uint64_t*>(raw + 128)= (4ULL << 30); // maxBufferSize
                break;
            }
            case 1000196000U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
                *reinterpret_cast<uint32_t*>(raw + 16) = 9U; // VK_DRIVER_ID_ARM_PROPRIETARY
                std::snprintf(reinterpret_cast<char*>(raw + 20), 256, "Arm Proprietary (AOT libGLES_mali.so)");
                std::snprintf(reinterpret_cast<char*>(raw + 276), 256, "%s", g_aot_rt.mapper.driver_version.c_str());
                raw[532] = 1; raw[533] = 3; raw[534] = 7; raw[535] = 2;
                break;
            }
            case 1000071004U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
                std::memcpy(raw + 16, kDevUuid, 16);
                std::memcpy(raw + 32, kDrvUuid, 16);
                *reinterpret_cast<uint32_t*>(raw + 60) = 0U;
                break;
            }
            case 1000094000U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES
                *reinterpret_cast<uint32_t*>(raw + 16) = 16U;
                *reinterpret_cast<uint32_t*>(raw + 20) = 0x3FU;
                *reinterpret_cast<uint32_t*>(raw + 24) = 0x1FFU;
                *reinterpret_cast<uint32_t*>(raw + 28) = 1U;
                break;
            }
            case 1000080000U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR
                *reinterpret_cast<uint32_t*>(raw + 16) = 32U;
                break;
            }
            case 1000168000U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
                *reinterpret_cast<uint32_t*>(raw + 16) = 1024U;
                *reinterpret_cast<uint64_t*>(raw + 24) = (4ULL << 30);
                break;
            }
            case 1000413001U: { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES
                *reinterpret_cast<uint64_t*>(raw + 16) = (4ULL << 30);
                break;
            }
            default:
                break;
        }
        cur = cur->pNext;
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures(void* physicalDevice, void* pFeatures) {
    if (!pFeatures) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceFeatures && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pFeatures);
        aot_vkGetPhysicalDeviceFeatures(&g_aot_rt.cpu);
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures2(void* physicalDevice, void* pFeatures2) {
    if (!pFeatures2) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceFeatures2 && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pFeatures2);
        aot_vkGetPhysicalDeviceFeatures2(&g_aot_rt.cpu);
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceQueueFamilyProperties(void* physicalDevice, uint32_t* pQueueFamilyPropertyCount, void* pQueueFamilyProperties) {
    if (!pQueueFamilyPropertyCount) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceQueueFamilyProperties && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pQueueFamilyPropertyCount);
        g_aot_rt.cpu.x[2] = reinterpret_cast<uint64_t>(pQueueFamilyProperties);
        aot_vkGetPhysicalDeviceQueueFamilyProperties(&g_aot_rt.cpu);
        return;
    }
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount > 0) {
        auto* qf = static_cast<VkQueueFamilyPropertiesIcd*>(pQueueFamilyProperties);
        qf[0].queueFlags = 0x17U; // GRAPHICS | COMPUTE | TRANSFER | SPARSE_BINDING
        qf[0].queueCount = 2U;
        qf[0].timestampValidBits = 64U;
        qf[0].minImageTransferGranularity[0] = 1U;
        qf[0].minImageTransferGranularity[1] = 1U;
        qf[0].minImageTransferGranularity[2] = 1U;
        *pQueueFamilyPropertyCount = 1;
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceQueueFamilyProperties2(void* physicalDevice, uint32_t* pQueueFamilyPropertyCount, void* pQueueFamilyProperties2) {
    if (!pQueueFamilyPropertyCount) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceQueueFamilyProperties2 && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pQueueFamilyPropertyCount);
        g_aot_rt.cpu.x[2] = reinterpret_cast<uint64_t>(pQueueFamilyProperties2);
        aot_vkGetPhysicalDeviceQueueFamilyProperties2(&g_aot_rt.cpu);
        return;
    }
    if (!pQueueFamilyProperties2) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, nullptr);
        return;
    }
    if (*pQueueFamilyPropertyCount > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, static_cast<uint8_t*>(pQueueFamilyProperties2) + 16);
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceMemoryProperties(void* physicalDevice, void* pMemoryProperties) {
    if (!pMemoryProperties) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceMemoryProperties && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = reinterpret_cast<uint64_t>(pMemoryProperties);
        aot_vkGetPhysicalDeviceMemoryProperties(&g_aot_rt.cpu);
        return;
    }
    auto* mp = static_cast<VkPhysicalDeviceMemoryPropertiesIcd*>(pMemoryProperties);
    std::memset(mp, 0, sizeof(VkPhysicalDeviceMemoryPropertiesIcd));
    mp->memoryTypeCount = 3;
    mp->memoryTypes[0].propertyFlags = 0x07U; // DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT
    mp->memoryTypes[0].heapIndex     = 0;
    mp->memoryTypes[1].propertyFlags = 0x0BU; // DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED
    mp->memoryTypes[1].heapIndex     = 0;
    mp->memoryTypes[2].propertyFlags = 0x11U; // DEVICE_LOCAL | LAZILY_ALLOCATED
    mp->memoryTypes[2].heapIndex     = 0;
    mp->memoryHeapCount = 1;
    mp->memoryHeaps[0].size  = 8ULL << 30;    // 8 GiB
    mp->memoryHeaps[0].flags = 0x01U;         // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceMemoryProperties2(void* physicalDevice, void* pMemoryProperties2) {
    if (!pMemoryProperties2) return;
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceMemoryProperties2 && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceMemoryProperties2,
            pdev->aot_internal, pMemoryProperties2);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, static_cast<uint8_t*>(pMemoryProperties2) + 16);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties(void* physicalDevice, uint32_t format, void* pFormatProperties) {
    if (!pFormatProperties) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    uint32_t* fp = static_cast<uint32_t*>(pFormatProperties);
    fp[0] = 0; fp[1] = 0; fp[2] = 0;
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceFormatProperties && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceFormatProperties,
            pdev->aot_internal, format, pFormatProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        if ((fp[0] | fp[1] | fp[2]) != 0U || format == 0U) return;
    }
    fp[0] = 0x7FFFFFFFU; // linearTilingFeatures fallback when synthetic pdev->aot_internal lacks hal_fmt table
    fp[1] = 0x7FFFFFFFU; // optimalTilingFeatures
    fp[2] = 0x7FFFFFFFU; // bufferFeatures
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties2(void* physicalDevice, uint32_t format, void* pFormatProperties2) {
    if (!pFormatProperties2) return;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    uint32_t* fp = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pFormatProperties2) + 16);
    fp[0] = 0; fp[1] = 0; fp[2] = 0;
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceFormatProperties2 && pdev) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceFormatProperties2,
            pdev->aot_internal, format, pFormatProperties2);
        g_aot_rt.lifted_vk_calls_executed++;
        if ((fp[0] | fp[1] | fp[2]) != 0U || format == 0U) return;
    }
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, static_cast<uint8_t*>(pFormatProperties2) + 16);
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceImageFormatProperties(void* physicalDevice, uint32_t format, uint32_t type,
                                                 uint32_t tiling, uint32_t usage, uint32_t flags,
                                                 void* pImageFormatProperties) {
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceImageFormatProperties && pdev && pImageFormatProperties) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceImageFormatProperties,
            pdev->aot_internal, format, type, tiling, usage, flags, pImageFormatProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return r;
    }
    if (pImageFormatProperties) {
        uint32_t* ifp = static_cast<uint32_t*>(pImageFormatProperties);
        ifp[0] = 16384U; ifp[1] = 16384U; ifp[2] = 2048U; // maxExtent
        ifp[3] = 15U;    // maxMipLevels
        ifp[4] = 2048U;  // maxArrayLayers
        ifp[5] = 0x0FU;  // sampleCounts
        *reinterpret_cast<uint64_t*>(ifp + 6) = (4ULL << 30); // maxResourceSize
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceImageFormatProperties2(void* physicalDevice, const void* pImageFormatInfo, void* pImageFormatProperties2) {
    if (!pImageFormatProperties2) return -3;
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceImageFormatProperties2 && pdev && pImageFormatInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceImageFormatProperties2,
            pdev->aot_internal, pImageFormatInfo, pImageFormatProperties2);
        g_aot_rt.lifted_vk_calls_executed++;
        return r;
    }
    return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, 37U, 1U, 0U, 0x1FU, 0U,
                                                    static_cast<uint8_t*>(pImageFormatProperties2) + 16);
}

__attribute__((visibility("default")))
int32_t vkEnumerateDeviceLayerProperties(void* physicalDevice, uint32_t* pPropertyCount, void* pProperties) {
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkEnumerateDeviceLayerProperties && pdev && pPropertyCount) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumerateDeviceLayerProperties,
            pdev->aot_internal, pPropertyCount, pProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return r;
    }
    if (pPropertyCount) *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceSparseImageFormatProperties(void* physicalDevice, uint32_t format, uint32_t type,
                                                    uint32_t samples, uint32_t usage, uint32_t tiling,
                                                    uint32_t* pPropertyCount, void* pProperties) {
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceSparseImageFormatProperties && pdev && pPropertyCount) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceSparseImageFormatProperties,
            pdev->aot_internal, format, type, samples, usage, tiling, pPropertyCount, pProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    if (pPropertyCount) *pPropertyCount = 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceSparseImageFormatProperties2(void* physicalDevice, const void* pFormatInfo,
                                                     uint32_t* pPropertyCount, void* pProperties) {
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceSparseImageFormatProperties2 && pdev && pFormatInfo && pPropertyCount) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceSparseImageFormatProperties2,
            pdev->aot_internal, pFormatInfo, pPropertyCount, pProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    if (pPropertyCount) *pPropertyCount = 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceExternalBufferProperties(void* physicalDevice, const void* pExternalBufferInfo, void* pExternalBufferProperties) {
    if (!pExternalBufferProperties) return;
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceExternalBufferProperties && pdev && pExternalBufferInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceExternalBufferProperties,
            pdev->aot_internal, pExternalBufferInfo, pExternalBufferProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalBufferProperties) + 16);
    ext[0] = 0x03U; // exportFromImportedHandleTypes | allocationSize
    ext[1] = 0U;
    ext[2] = 0U;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceExternalFenceProperties(void* physicalDevice, const void* pExternalFenceInfo, void* pExternalFenceProperties) {
    if (!pExternalFenceProperties) return;
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceExternalFenceProperties && pdev && pExternalFenceInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceExternalFenceProperties,
            pdev->aot_internal, pExternalFenceInfo, pExternalFenceProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalFenceProperties) + 16);
    ext[0] = 0U; ext[1] = 0U; ext[2] = 0U;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceExternalSemaphoreProperties(void* physicalDevice, const void* pExternalSemaphoreInfo, void* pExternalSemaphoreProperties) {
    if (!pExternalSemaphoreProperties) return;
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkGetPhysicalDeviceExternalSemaphoreProperties && pdev && pExternalSemaphoreInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetPhysicalDeviceExternalSemaphoreProperties,
            pdev->aot_internal, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalSemaphoreProperties) + 16);
    ext[0] = 0U; ext[1] = 0U; ext[2] = 0U;
}

__attribute__((visibility("default")))
int32_t vkEnumeratePhysicalDeviceGroups(void* instance, uint32_t* pPhysicalDeviceGroupCount, void* pPhysicalDeviceGroupProperties) {
    if (!instance || !pPhysicalDeviceGroupCount) return -3;
    auto* inst = static_cast<MaliIcdInstance*>(instance);
    if (!pPhysicalDeviceGroupProperties) {
        *pPhysicalDeviceGroupCount = 1U;
        return 0;
    }
    if (*pPhysicalDeviceGroupCount == 0U) return 5;
    uint8_t* raw = static_cast<uint8_t*>(pPhysicalDeviceGroupProperties);
    *reinterpret_cast<uint32_t*>(raw + 16) = 1U; // physicalDeviceCount = 1
    *reinterpret_cast<void**>(raw + 24)    = &inst->phys_dev;
    *pPhysicalDeviceGroupCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateDeviceExtensionProperties(void* physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    g_aot_rt.ensureInitialized();
    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);

    std::vector<VkExtensionPropertiesIcd> exts;
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkEnumerateDeviceExtensionProperties && pdev && (!pLayerName || pLayerName[0] == '\0')) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        pdev->initAotInternalLayout();
        uint32_t aot_count = 0;
        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
        g_aot_rt.cpu.x[1] = 0;
        g_aot_rt.cpu.x[2] = reinterpret_cast<uint64_t>(&aot_count);
        g_aot_rt.cpu.x[3] = 0;
        aot_vkEnumerateDeviceExtensionProperties(&g_aot_rt.cpu);
        if (aot_count > 0 && aot_count <= 256) {
            exts.resize(aot_count);
            g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(pdev->aot_internal);
            g_aot_rt.cpu.x[1] = 0;
            g_aot_rt.cpu.x[2] = reinterpret_cast<uint64_t>(&aot_count);
            g_aot_rt.cpu.x[3] = reinterpret_cast<uint64_t>(exts.data());
            aot_vkEnumerateDeviceExtensionProperties(&g_aot_rt.cpu);
        }
    }

    static const struct { const char* name; uint32_t ver; } kExtraDevExts[] = {
        {"VK_KHR_swapchain", 70U},
        {"VK_EXT_4444_formats", 1U},
        {"VK_EXT_custom_border_color", 12U},
        {"VK_EXT_depth_clip_enable", 1U},
        {"VK_EXT_extended_dynamic_state", 1U},
        {"VK_EXT_non_seamless_cube_map", 1U},
        {"VK_EXT_robustness2", 1U},
        {"VK_EXT_shader_viewport_index_layer", 1U},
        {"VK_EXT_transform_feedback", 1U},
        {"VK_EXT_vertex_attribute_divisor", 3U},
        {"VK_KHR_shader_draw_parameters", 1U},
        {"VK_KHR_external_memory", 1U},
        {"VK_KHR_external_memory_fd", 1U},
        {"VK_EXT_external_memory_dma_buf", 1U},
        {"VK_KHR_maintenance1", 2U},
        {"VK_KHR_maintenance2", 1U},
        {"VK_KHR_maintenance3", 1U},
        {"VK_KHR_maintenance4", 2U},
        {"VK_KHR_driver_properties", 1U},
        {"VK_KHR_push_descriptor", 2U},
        {"VK_KHR_timeline_semaphore", 2U},
        {"VK_KHR_buffer_device_address", 1U}
    };
    for (const auto& extra : kExtraDevExts) {
        bool found = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, extra.name) == 0) { found = true; break; }
        }
        if (!found) {
            VkExtensionPropertiesIcd ep{};
            std::snprintf(ep.extensionName, sizeof(ep.extensionName), "%s", extra.name);
            ep.specVersion = extra.ver;
            exts.push_back(ep);
        }
    }

    uint32_t total = static_cast<uint32_t>(exts.size());
    if (!pProperties) {
        *pPropertyCount = total;
        return 0;
    }
    auto* out = static_cast<VkExtensionPropertiesIcd*>(pProperties);
    uint32_t n = std::min(*pPropertyCount, total);
    std::memcpy(out, exts.data(), n * sizeof(VkExtensionPropertiesIcd));
    *pPropertyCount = n;
    return (n < total) ? 5 : 0;
}


} // extern "C"
