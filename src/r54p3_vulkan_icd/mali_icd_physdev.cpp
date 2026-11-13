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
int32_t vkCreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    if (!pInstance) return -3;
    if (!g_aot_rt.ensureInitialized()) return -3;

    auto* inst = new MaliIcdInstance();
    inst->loader_magic = ICD_LOADER_MAGIC;
    inst->sidecar_tag = MALI_SIDECAR_TAG_INSTANCE;
    inst->phys_dev.loader_magic = ICD_LOADER_MAGIC;
    inst->phys_dev.sidecar_tag = MALI_SIDECAR_TAG_PHYSDEV;
    inst->phys_dev.parent_instance = inst;

    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_vkCreateInstance && aot_vkEnumeratePhysicalDevices) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);

        std::vector<VkExtensionProperties> nat_exts;
        if (aot_vkEnumerateInstanceExtensionProperties) {
            uint32_t nat_cnt = 0;
            if (mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                    g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size,
                    aot_vkEnumerateInstanceExtensionProperties,
                    static_cast<const char*>(nullptr), &nat_cnt,
                    static_cast<VkExtensionProperties*>(nullptr)) == 0 &&
                nat_cnt > 0 && nat_cnt <= 256) {
                nat_exts.resize(nat_cnt);
                mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                    g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size,
                    aot_vkEnumerateInstanceExtensionProperties,
                    static_cast<const char*>(nullptr), &nat_cnt, nat_exts.data());
                nat_exts.resize(nat_cnt);
            }
        }

        VkInstanceCreateInfo aot_ci{};
        std::vector<const char*> filtered_exts;
        if (pCreateInfo) {
            aot_ci = *static_cast<const VkInstanceCreateInfo*>(pCreateInfo);
            if (aot_ci.enabledExtensionCount > 0 && aot_ci.ppEnabledExtensionNames) {
                filtered_exts.reserve(aot_ci.enabledExtensionCount);
                for (uint32_t i = 0; i < aot_ci.enabledExtensionCount; ++i) {
                    const char* ext_name = aot_ci.ppEnabledExtensionNames[i];
                    if (!ext_name) continue;
                    bool supported = false;
                    for (const auto& ne : nat_exts) {
                        if (std::strcmp(ne.extensionName, ext_name) == 0) {
                            supported = true;
                            break;
                        }
                    }
                    if (supported) filtered_exts.push_back(ext_name);
                }
                aot_ci.enabledExtensionCount = static_cast<uint32_t>(filtered_exts.size());
                aot_ci.ppEnabledExtensionNames = filtered_exts.empty() ? nullptr : filtered_exts.data();
            }
        } else {
            aot_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        }

        const VkAllocationCallbacks* sidecar_alloc = icdGetSidecarAllocator(
            static_cast<const VkAllocationCallbacks*>(pAllocator));
        void* aot_inst = nullptr;
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkCreateInstance,
            &aot_ci, sidecar_alloc, &aot_inst);
        g_aot_rt.lifted_vk_calls_executed++;
        if (r == 0 && aot_inst != nullptr) {
            icdSetSidecar(aot_inst, MALI_SIDECAR_TAG_INSTANCE, inst);
            inst->native_instance = aot_inst;

            uint32_t pd_count = 0;
            int32_t pr = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumeratePhysicalDevices,
                aot_inst, &pd_count, static_cast<void**>(nullptr));
            g_aot_rt.lifted_vk_calls_executed++;
            if (pr == 0 && pd_count > 0 && pd_count <= 16) {
                std::vector<void*> native_pds(pd_count, nullptr);
                pr = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                    g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkEnumeratePhysicalDevices,
                    aot_inst, &pd_count, native_pds.data());
                g_aot_rt.lifted_vk_calls_executed++;
                if ((pr == 0 || pr == 5) && pd_count > 0) {
                    for (uint32_t i = 0; i < pd_count; ++i) {
                        if (!native_pds[i]) continue;
                        MaliIcdPhysicalDevice* pd_sidecar = nullptr;
                        if (i == 0) {
                            pd_sidecar = &inst->phys_dev;
                        } else {
                            auto extra = std::make_unique<MaliIcdPhysicalDevice>();
                            pd_sidecar = extra.get();
                            inst->extra_phys_devs.push_back(std::move(extra));
                        }
                        pd_sidecar->loader_magic = ICD_LOADER_MAGIC;
                        pd_sidecar->sidecar_tag = MALI_SIDECAR_TAG_PHYSDEV;
                        pd_sidecar->parent_instance = inst;
                        pd_sidecar->native_phys_dev = native_pds[i];
                        icdSetSidecar(native_pds[i], MALI_SIDECAR_TAG_PHYSDEV, pd_sidecar);
                        inst->native_phys_devs.push_back(native_pds[i]);
                    }
                }
            }
            if (mali_icd_log_enabled()) {
                mali_icd_log("vkCreateInstance -> native=%p sidecar=%p phys_devs=%zu",
                             aot_inst, static_cast<void*>(inst), inst->native_phys_devs.size());
            }
            *pInstance = aot_inst;
            return 0;
        }
        if (mali_icd_log_enabled()) {
            mali_icd_log("vkCreateInstance: aot_vkCreateInstance returned %d (inst=%p); using fallback",
                         r, aot_inst);
        }
    }

    inst->phys_dev.initAotInternalLayout();
    *pInstance = inst;
    if (mali_icd_log_enabled() && pCreateInfo) {
        const auto* ci = static_cast<const VkInstanceCreateInfo*>(pCreateInfo);
        std::string exts_str;
        if (ci->enabledExtensionCount <= 64 && ci->ppEnabledExtensionNames) {
            for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
                if (ci->ppEnabledExtensionNames[i]) {
                    if (!exts_str.empty()) exts_str += ", ";
                    exts_str += ci->ppEnabledExtensionNames[i];
                }
            }
        }
        mali_icd_log("vkCreateInstance -> instance=%p driver_so='%s' lifted_vk_entrypoints=%lu enabled_exts(%u)=[%s]",
                     static_cast<void*>(inst),
                     g_aot_rt.mapper.so_path_loaded.empty() ? "standalone-builtin" : g_aot_rt.mapper.so_path_loaded.c_str(),
                     static_cast<unsigned long>(aot_get_lifted_vk_entrypoint_count()),
                     ci->enabledExtensionCount, exts_str.c_str());
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyInstance(void* instance, const void* pAllocator) {
    auto* inst = icdGetInstance(instance);
    if (!inst) return;
    void* aot_inst = inst->native_instance;
    if (aot_inst && !g_aot_rt.mapper.so_path_loaded.empty() && aot_vkDestroyInstance) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        const VkAllocationCallbacks* sidecar_alloc = icdGetSidecarAllocator(
            static_cast<const VkAllocationCallbacks*>(pAllocator));
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkDestroyInstance,
            aot_inst, sidecar_alloc);
        g_aot_rt.lifted_vk_calls_executed++;
    }
    delete inst;
}

__attribute__((visibility("default")))
int32_t vkEnumeratePhysicalDevices(void* instance, uint32_t* pPhysicalDeviceCount, void** pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return -3;
    auto* inst = icdGetInstance(instance);
    if (!inst) return -3;
    if (inst->native_instance && !inst->native_phys_devs.empty()) {
        uint32_t total = static_cast<uint32_t>(inst->native_phys_devs.size());
        if (!pPhysicalDevices) {
            *pPhysicalDeviceCount = total;
            return 0;
        }
        uint32_t n = std::min(*pPhysicalDeviceCount, total);
        for (uint32_t i = 0; i < n; ++i) {
            pPhysicalDevices[i] = inst->native_phys_devs[i];
        }
        *pPhysicalDeviceCount = n;
        return (n < total) ? 5 : 0; // VK_INCOMPLETE : VK_SUCCESS
    }
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return 0;
    }
    if (*pPhysicalDeviceCount == 0) return 5; // VK_INCOMPLETE
    pPhysicalDevices[0] = &inst->phys_dev;
    *pPhysicalDeviceCount = 1;
    return 0;
}

#include "generated/mali_physdev_wrappers_autogen.inc"

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties(void* physicalDevice, void* pProperties) {
    if (!pProperties) return;
    auto* props = static_cast<VkPhysicalDeviceProperties*>(pProperties);
    std::memset(props, 0, sizeof(VkPhysicalDeviceProperties));
    aot_forward_vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(physicalDevice), props);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2(void* physicalDevice, void* pProperties2) {
    if (!pProperties2) return;
    aot_forward_vkGetPhysicalDeviceProperties2(
        static_cast<VkPhysicalDevice>(physicalDevice),
        static_cast<VkPhysicalDeviceProperties2*>(pProperties2));
}

__attribute__((visibility("default")))
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName && pLayerName[0] != '\0') {
        return aot_forward_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }
    uint32_t aot_count = 0;
    VkResult r = aot_forward_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &aot_count, nullptr);
    if (r != VK_SUCCESS) return r;

    std::vector<VkExtensionProperties> exts(aot_count);
    if (aot_count > 0) {
        r = aot_forward_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &aot_count, exts.data());
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
            exts.resize(aot_count);
        } else {
            exts.clear();
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

} // extern "C"
