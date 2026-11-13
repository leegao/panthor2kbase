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
    std::vector<VkExtensionProperties> exts;
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
        exts.erase(std::remove_if(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
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
            VkExtensionProperties ep{};
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
    std::memcpy(pProperties, exts.data(), n * sizeof(VkExtensionProperties));
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
    auto* props = static_cast<VkPhysicalDeviceProperties*>(pProperties);
    std::memset(props, 0, sizeof(VkPhysicalDeviceProperties));

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

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2(void* physicalDevice, void* pProperties2) {
    if (!pProperties2) return;
    vkGetPhysicalDeviceProperties(physicalDevice, static_cast<uint8_t*>(pProperties2) + 16);

    const uint8_t kDevUuid[16] = {'M','A','L','I','-','G','6','1','5','-','D','E','V','I','C','E'};
    const uint8_t kDrvUuid[16] = {'A','R','M','-','M','A','L','I','-','R','5','4','P','3','-','0'};

    // Walk pNext chain for vulkaninfo & Vulkan 1.1/1.2/1.3/Extension property queries
    auto* cur = reinterpret_cast<VkBaseOutStructure*>(pProperties2)->pNext;
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
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    g_aot_rt.ensureInitialized();
    auto* pdev = reinterpret_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkEnumerateDeviceExtensionProperties && pdev) {
        if (pLayerName && pLayerName[0] != '\0') {
            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
            pdev->initAotInternalLayout();
            VkResult r = mali_aapcs64::invokeLiftedAapcs64<VkResult>(
                g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumerateDeviceExtensionProperties,
                pdev->aot_internal, pLayerName, pPropertyCount, pProperties);
            g_aot_rt.lifted_vk_calls_executed++;
            return r;
        }
        std::vector<VkExtensionProperties> exts;
        {
            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
            pdev->initAotInternalLayout();
            uint32_t aot_count = 0;
            VkResult r = mali_aapcs64::invokeLiftedAapcs64<VkResult>(
                g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumerateDeviceExtensionProperties,
                pdev->aot_internal, pLayerName, &aot_count, static_cast<VkExtensionProperties*>(nullptr));
            g_aot_rt.lifted_vk_calls_executed++;
            if (r == VK_SUCCESS && aot_count > 0) {
                exts.resize(aot_count);
                r = mali_aapcs64::invokeLiftedAapcs64<VkResult>(
                    g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumerateDeviceExtensionProperties,
                    pdev->aot_internal, pLayerName, &aot_count, exts.data());
                g_aot_rt.lifted_vk_calls_executed++;
                if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
                    exts.resize(aot_count);
                } else {
                    exts.clear();
                }
            }
        }
        bool has_swapchain = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true;
                break;
            }
        }
        if (!has_swapchain) {
            VkExtensionProperties swapchain_ext{};
            std::snprintf(swapchain_ext.extensionName, sizeof(swapchain_ext.extensionName), "%s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            swapchain_ext.specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
            exts.push_back(swapchain_ext);
        }
        if (!pProperties) {
            *pPropertyCount = static_cast<uint32_t>(exts.size());
            return VK_SUCCESS;
        }
        uint32_t to_copy = std::min(*pPropertyCount, static_cast<uint32_t>(exts.size()));
        if (to_copy > 0) {
            std::memcpy(pProperties, exts.data(), to_copy * sizeof(VkExtensionProperties));
        }
        *pPropertyCount = to_copy;
        return (to_copy < exts.size()) ? VK_INCOMPLETE : VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

#include "generated/mali_physdev_wrappers_autogen.inc"

} // extern "C"
