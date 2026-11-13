#pragma once
// ============================================================================
// mali0_emu_lvp.hpp
// Host Vulkan (Mesa Lavapipe / llvmpipe) Compute & 3D Graphics (vkcube) Backend
// Executes lifted Valhall & Bifrost SPIR-V 1.3 shaders against /dev/mali0 BOs
// ============================================================================
#include "mali0_emu_uapi.h"
#include "mali0_emu_isa.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <algorithm>

class HostLvpipeVulkanExecutor {
public:
    static constexpr size_t kBufferCapacityBytes = 4 * 1024 * 1024; // 4 MiB (supports up to 1024x1024 RGBA8 WSI surfaces)

    typedef void (*PFN_vkVoidFunction)(void);
    typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
    typedef int32_t (*PFN_vkCreateInstance)(const void*, const void*, void**);
    typedef int32_t (*PFN_vkEnumeratePhysicalDevices)(void*, uint32_t*, void**);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(void*, void*);
    typedef int32_t (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    typedef void (*PFN_vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
    typedef int32_t (*PFN_vkCreateShaderModule)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyShaderModule)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateDescriptorSetLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreatePipelineLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreateComputePipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyPipeline)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateBuffer)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateMemory)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkBindBufferMemory)(void*, uint64_t, uint64_t, uint64_t);
    typedef int32_t (*PFN_vkMapMemory)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**);
    typedef int32_t (*PFN_vkCreateDescriptorPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateDescriptorSets)(void*, const void*, uint64_t*);
    typedef void (*PFN_vkUpdateDescriptorSets)(void*, uint32_t, const void*, uint32_t, const void*);
    typedef int32_t (*PFN_vkCreateCommandPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateCommandBuffers)(void*, const void*, void**);
    typedef int32_t (*PFN_vkResetCommandBuffer)(void*, uint32_t);
    typedef int32_t (*PFN_vkBeginCommandBuffer)(void*, const void*);
    typedef void (*PFN_vkCmdBindPipeline)(void*, uint32_t, uint64_t);
    typedef void (*PFN_vkCmdBindDescriptorSets)(void*, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t*, uint32_t, const uint32_t*);
    typedef void (*PFN_vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t);
    typedef int32_t (*PFN_vkEndCommandBuffer)(void*);
    typedef int32_t (*PFN_vkQueueSubmit)(void*, uint32_t, const void*, uint64_t);
    typedef int32_t (*PFN_vkQueueWaitIdle)(void*);

    void*    vk_lib = nullptr;
    void*    instance = nullptr;
    void*    phys_dev = nullptr;
    void*    device = nullptr;
    void*    queue = nullptr;
    uint64_t desc_set_layout = 0;
    uint64_t pipeline_layout = 0;
    uint64_t desc_pool = 0;
    uint64_t desc_set = 0;
    uint64_t cmd_pool = 0;
    void*    cmd_buf = nullptr;
    uint64_t buf_in = 0, buf_out = 0;
    uint64_t mem_in = 0, mem_out = 0;
    void*    mapped_in = nullptr;
    void*    mapped_out = nullptr;
    std::string device_name;
    std::string backend_library_used;
    std::string backend_mode_requested = "auto";
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    uint32_t dispatches_completed = 0;
    uint32_t graphics_passes_completed = 0;

    bool tryOpenBackendIcdLibrary(const char* so_path) {
        void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) return false;
        auto fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vk_icdGetInstanceProcAddr"));
        if (!fn) {
            fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        }
        if (!fn) {
            dlclose(handle);
            return false;
        }
        struct VkApplicationInfo {
            uint32_t sType; const void* pNext; const char* pAppName; uint32_t appVer;
            const char* pEngineName; uint32_t engineVer; uint32_t apiVersion;
        } appInfo = {0, nullptr, "mali0_emu_lvp_harness", 1, "mali0_emu_lvp", 1, (1U << 22) | (3U << 12)};
        struct VkInstanceCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
            uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE;
        } ici = {1, nullptr, 0, &appInfo, 0, nullptr, 0, nullptr};

        auto createInst = reinterpret_cast<PFN_vkCreateInstance>(fn(nullptr, "vkCreateInstance"));
        void* test_inst = nullptr;
        if (!createInst || createInst(&ici, nullptr, &test_inst) != 0 || !test_inst) {
            dlclose(handle);
            return false;
        }
        auto enumPD = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fn(test_inst, "vkEnumeratePhysicalDevices"));
        uint32_t pdCount = 1;
        void* test_pd = nullptr;
        if (!enumPD || enumPD(test_inst, &pdCount, &test_pd) != 0 || pdCount == 0 || !test_pd) {
            dlclose(handle);
            return false;
        }
        auto getPropsTest = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(fn(test_inst, "vkGetPhysicalDeviceProperties"));
        if (getPropsTest && std::strstr(so_path, "libvulkan.so") != nullptr) {
            uint8_t testProps[1024]{};
            getPropsTest(test_pd, testProps);
            const char* pdName = reinterpret_cast<const char*>(testProps + 20);
            if (std::strstr(pdName, "Mali") != nullptr || std::strstr(pdName, "AOT") != nullptr) {
                dlclose(handle);
                return false;
            }
        }
        vk_lib   = handle;
        gipa     = fn;
        instance = test_inst;
        phys_dev = test_pd;
        backend_library_used = so_path;
        return true;
    }

    bool init() {
        if (device) return true;
        static thread_local bool s_in_lvp_init = false;
        if (s_in_lvp_init) return false;
        struct ReentrancyGuard {
            bool& flag;
            ReentrancyGuard(bool& f) : flag(f) { flag = true; }
            ~ReentrancyGuard() { flag = false; }
        } guard(s_in_lvp_init);
        const char* env_backend = getenv("MALI0_EMU_BACKEND");
        backend_mode_requested = env_backend ? env_backend : "llvmpipe";

        // 1. If user specified an explicit shared library path in MALI0_EMU_BACKEND_LIB or MALI0_EMU_BACKEND
        const char* custom_lib = getenv("MALI0_EMU_BACKEND_LIB");
        if (custom_lib && tryOpenBackendIcdLibrary(custom_lib)) {
            // opened custom backend library
        } else if (backend_mode_requested == "host" || backend_mode_requested == "auto") {
            // Probe host hardware Vulkan ICDs first (RADV, ANV, NVIDIA), then fall back to Mesa Lavapipe
            const char* host_candidates[] = {
                "/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
                "/usr/lib/libvulkan_radeon.so",
                "/usr/lib64/libvulkan_radeon.so",
                "/usr/lib/x86_64-linux-gnu/libvulkan_intel.so",
                "/usr/lib/libvulkan_intel.so",
                "/usr/lib64/libvulkan_intel.so",
                "/usr/lib/x86_64-linux-gnu/libvulkan_intel_hasvk.so",
                "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
                "/usr/lib/libvulkan_lvp.so",
                "/usr/lib64/libvulkan_lvp.so",
                "libvulkan_lvp.so"
            };
            for (const char* cand : host_candidates) {
                if (tryOpenBackendIcdLibrary(cand)) break;
            }
        } else {
            // Default / "llvmpipe" / "lvp": probe all distro paths (Debian/Ubuntu, Arch, Fedora) directly
            // without clobbering the process-wide VK_ICD_FILENAMES environment variable!
            const char* lvp_candidates[] = {
                "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
                "/usr/lib/libvulkan_lvp.so",
                "/usr/lib64/libvulkan_lvp.so",
                "libvulkan_lvp.so"
            };
            bool opened_lvp = false;
            for (const char* cand : lvp_candidates) {
                if (tryOpenBackendIcdLibrary(cand)) {
                    opened_lvp = true;
                    break;
                }
            }
            if (!opened_lvp) {
                const char* lvp_json = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json";
                if (access(lvp_json, R_OK) != 0) return false;
                const char* prev_icd = getenv("VK_ICD_FILENAMES");
                const char* prev_drv = getenv("VK_DRIVER_FILES");
                std::string saved_icd = prev_icd ? prev_icd : "";
                std::string saved_drv = prev_drv ? prev_drv : "";
                setenv("VK_ICD_FILENAMES", lvp_json, 1);
                setenv("VK_DRIVER_FILES", lvp_json, 1);
                bool ok = tryOpenBackendIcdLibrary("libvulkan.so.1");
                if (!saved_icd.empty()) {
                    setenv("VK_ICD_FILENAMES", saved_icd.c_str(), 1);
                } else {
                    unsetenv("VK_ICD_FILENAMES");
                }
                if (!saved_drv.empty()) {
                    setenv("VK_DRIVER_FILES", saved_drv.c_str(), 1);
                } else {
                    unsetenv("VK_DRIVER_FILES");
                }
                if (!ok) return false;
            }
        }
        if (!vk_lib || !gipa || !instance || !phys_dev) return false;



        uint8_t rawProps[1024]{};
        auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
        if (getProps) {
            getProps(phys_dev, rawProps);
            device_name = reinterpret_cast<const char*>(rawProps + 20);
        }

        float qPrio = 1.0f;
        struct VkDeviceQueueCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx; uint32_t qCount; const float* pPrio;
        } qci = {2, nullptr, 0, 0, 1, &qPrio};
        struct VkDeviceCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t qciCount; const void* pQci;
            uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE; const void* pFeatures;
        } dci = {3, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};

        auto createDev = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
        if (!createDev || createDev(phys_dev, &dci, nullptr, &device) != 0) return false;

        auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"));
        getQueue(device, 0, 0, &queue);

        struct VkDescriptorSetLayoutBinding {
            uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount; uint32_t stageFlags; const void* pImm;
        } bindings[2] = {
            {0, 7 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */, 1, 0x20 /* VK_SHADER_STAGE_COMPUTE_BIT */, nullptr},
            {1, 7 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */, 1, 0x20 /* VK_SHADER_STAGE_COMPUTE_BIT */, nullptr}
        };
        struct VkDescriptorSetLayoutCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t bindingCount; const void* pBindings;
        } dslci = {32, nullptr, 0, 2, bindings};
        if (reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(gipa(instance, "vkCreateDescriptorSetLayout"))(device, &dslci, nullptr, &desc_set_layout) != 0)
            return false;

        struct VkPipelineLayoutCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t setLayoutCount; const uint64_t* pSetLayouts;
            uint32_t pushConstantRangeCount; const void* pPushConstantRanges;
        } plci = {30, nullptr, 0, 1, &desc_set_layout, 0, nullptr};
        if (reinterpret_cast<PFN_vkCreatePipelineLayout>(gipa(instance, "vkCreatePipelineLayout"))(device, &plci, nullptr, &pipeline_layout) != 0)
            return false;

        struct VkBufferCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sharingMode;
            uint32_t qfiCount; const uint32_t* pQfi;
        } bci = {12, nullptr, 0, kBufferCapacityBytes, 0x80U | 0x40U | 0x20U | 0x02U | 0x01U /* STORAGE |INDEX | VERTEX | TRANSFER_DST | TRANSFER_SRC */, 0, 0, nullptr};
        auto createBuf = reinterpret_cast<PFN_vkCreateBuffer>(gipa(instance, "vkCreateBuffer"));
        createBuf(device, &bci, nullptr, &buf_in);
        createBuf(device, &bci, nullptr, &buf_out);

        struct VkMemoryAllocateInfo {
            uint32_t sType; const void* pNext; uint64_t allocationSize; uint32_t memoryTypeIndex;
        } mai = {5, nullptr, kBufferCapacityBytes, 0};
        auto allocMem = reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"));
        allocMem(device, &mai, nullptr, &mem_in);
        allocMem(device, &mai, nullptr, &mem_out);

        auto bindBuf = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"));
        bindBuf(device, buf_in, mem_in, 0);
        bindBuf(device, buf_out, mem_out, 0);

        auto mapMem = reinterpret_cast<PFN_vkMapMemory>(gipa(instance, "vkMapMemory"));
        mapMem(device, mem_in, 0, kBufferCapacityBytes, 0, &mapped_in);
        mapMem(device, mem_out, 0, kBufferCapacityBytes, 0, &mapped_out);

        struct VkDescriptorPoolSize { uint32_t type; uint32_t count; } poolSz = {7, 2};
        struct VkDescriptorPoolCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t maxSets; uint32_t poolSizeCount; const void* pPoolSizes;
        } dpci = {33, nullptr, 0, 1, 1, &poolSz};
        reinterpret_cast<PFN_vkCreateDescriptorPool>(gipa(instance, "vkCreateDescriptorPool"))(device, &dpci, nullptr, &desc_pool);

        struct VkDescriptorSetAllocateInfo {
            uint32_t sType; const void* pNext; uint64_t descriptorPool; uint32_t descriptorSetCount; const uint64_t* pSetLayouts;
        } dsai = {34, nullptr, desc_pool, 1, &desc_set_layout};
        reinterpret_cast<PFN_vkAllocateDescriptorSets>(gipa(instance, "vkAllocateDescriptorSets"))(device, &dsai, &desc_set);

        struct VkDescriptorBufferInfo { uint64_t buffer; uint64_t offset; uint64_t range; } dbi[2] = {
            {buf_in, 0, kBufferCapacityBytes}, {buf_out, 0, kBufferCapacityBytes}
        };
        struct VkWriteDescriptorSet {
            uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBinding; uint32_t dstArrayElement;
            uint32_t descriptorCount; uint32_t descriptorType; const void* pImg; const void* pBuf; const void* pTex;
        } wds[2] = {
            {35, nullptr, desc_set, 0, 0, 1, 7, nullptr, &dbi[0], nullptr},
            {35, nullptr, desc_set, 1, 0, 1, 7, nullptr, &dbi[1], nullptr}
        };
        reinterpret_cast<PFN_vkUpdateDescriptorSets>(gipa(instance, "vkUpdateDescriptorSets"))(device, 2, wds, 0, nullptr);

        struct VkCommandPoolCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx;
        } cpoci = {39, nullptr, 0x02 /* VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */, 0};
        reinterpret_cast<PFN_vkCreateCommandPool>(gipa(instance, "vkCreateCommandPool"))(device, &cpoci, nullptr, &cmd_pool);

        struct VkCommandBufferAllocateInfo {
            uint32_t sType; const void* pNext; uint64_t commandPool; uint32_t level; uint32_t count;
        } cbai = {40, nullptr, cmd_pool, 0, 1};
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd_buf);
        return true;
    }

    // Validate a lifted SPIR-V 1.3 Shader Module (Vertex, Fragment, or Compute) on Mesa Lavapipe
    bool validateShaderModuleOnLvpipe(const std::vector<uint32_t>& spv_words) {
        if (!init() || spv_words.empty()) return false;
        struct VkShaderModuleCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
        } smci = {16, nullptr, 0, spv_words.size() * sizeof(uint32_t), spv_words.data()};
        uint64_t shader_mod = 0;
        auto createSM = reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"));
        int32_t rc = createSM(device, &smci, nullptr, &shader_mod);
        if (rc == 0 && shader_mod != 0) {
            reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
            return true;
        }
        return false;
    }

    // Compile lifted SPIR-V module & dispatch on host Mesa Lavapipe against kbase BO memory
    bool executeLiftedSpirv(const std::vector<uint32_t>& spv_words,
                            const void* kbase_in_bo_ptr, void* kbase_out_bo_ptr,
                            size_t copy_bytes = 64,
                            uint32_t wg_x = 1, uint32_t wg_y = 1, uint32_t wg_z = 1) {
        if (!init()) return false;
        size_t clamped_bytes = std::min<size_t>(copy_bytes, kBufferCapacityBytes);
        if (kbase_in_bo_ptr && mapped_in) {
            std::memcpy(mapped_in, kbase_in_bo_ptr, clamped_bytes);
        }
        if (mapped_out) {
            std::memset(mapped_out, 0, clamped_bytes);
        }

        struct VkShaderModuleCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
        } smci = {16, nullptr, 0, spv_words.size() * sizeof(uint32_t), spv_words.data()};

        uint64_t shader_mod = 0;
        auto createSM = reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"));
        int32_t smrc = createSM(device, &smci, nullptr, &shader_mod);
        if (smrc != 0 || !shader_mod) { fprintf(stderr, "[LVP] smrc=%d\n", smrc); return false; }

        struct VkComputePipelineCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags;
            struct {
                uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec;
            } stage;
            uint64_t layout; uint64_t basePipelineHandle; int32_t basePipelineIndex;
        } cpci = {29, nullptr, 0, {18, nullptr, 0, 0x20, shader_mod, "main", nullptr}, pipeline_layout, 0, -1};

        uint64_t pipeline = 0;
        auto createPipe = reinterpret_cast<PFN_vkCreateComputePipelines>(gipa(instance, "vkCreateComputePipelines"));
        int32_t prc = createPipe(device, 0, 1, &cpci, nullptr, &pipeline);
        if (prc != 0 || !pipeline) { fprintf(stderr, "[LVP] prc=%d\n", prc);
            reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
            return false;
        }

        reinterpret_cast<PFN_vkResetCommandBuffer>(gipa(instance, "vkResetCommandBuffer"))(cmd_buf, 0);
        struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
        reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);
        reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"))(cmd_buf, 1 /* VK_PIPELINE_BIND_POINT_COMPUTE */, pipeline);
        reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gipa(instance, "vkCmdBindDescriptorSets"))(cmd_buf, 1, pipeline_layout, 0, 1, &desc_set, 0, nullptr);
        uint32_t max_wg_x = static_cast<uint32_t>(kBufferCapacityBytes / 16);
        uint32_t safe_wg_x = (wg_x == 0) ? 1 : ((wg_x > max_wg_x) ? max_wg_x : wg_x);
        reinterpret_cast<PFN_vkCmdDispatch>(gipa(instance, "vkCmdDispatch"))(cmd_buf, safe_wg_x, wg_y ? wg_y : 1, wg_z ? wg_z : 1);
        reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);

        struct VkSubmitInfo {
            uint32_t sType; const void* pNext; uint32_t waitSemCount; const uint64_t* pWaitSems; const uint32_t* pWaitStages;
            uint32_t cmdBufCount; void* const* pCmdBufs; uint32_t sigSemCount; const uint64_t* pSigSems;
        } si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};

        int32_t src = reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0);
        reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);

        if (src == 0 && kbase_out_bo_ptr && mapped_out) {
            std::memcpy(kbase_out_bo_ptr, mapped_out, clamped_bytes);
            if (copy_bytes > clamped_bytes && kbase_in_bo_ptr) {
                std::memcpy(static_cast<uint8_t*>(kbase_out_bo_ptr) + clamped_bytes,
                            static_cast<const uint8_t*>(kbase_in_bo_ptr) + clamped_bytes,
                            copy_bytes - clamped_bytes);
            }
            dispatches_completed++;
        }

        reinterpret_cast<PFN_vkDestroyPipeline>(gipa(instance, "vkDestroyPipeline"))(device, pipeline, nullptr);
        reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
        return src == 0;
    }

    // Execute a full Host Vulkan Graphics Pipeline (VkRenderPass + VkFramebuffer + VkGraphicsPipeline +
    // vkCmdBeginRenderPass + vkCmdBindVertexBuffers + vkCmdBindIndexBuffer + vkCmdDrawIndexed +
    // vkCmdEndRenderPass + vkCmdCopyImageToBuffer) on the host Vulkan driver (llvmpipe / radv)
    bool executeLiftedGraphicsPipelineOnLvpipe(const std::vector<uint32_t>& vs_spirv,
                                               const std::vector<uint32_t>& fs_spirv,
                                               uint32_t width, uint32_t height,
                                               const void* clip_vertices_32b, uint32_t vertex_count,
                                               const uint32_t* indices, uint32_t index_count) {
        if (!init() || vs_spirv.empty() || fs_spirv.empty() || width == 0 || height == 0) return false;

        typedef int32_t (*PFN_vkCreateRenderPass)(void*, const void*, const void*, uint64_t*);
        typedef void    (*PFN_vkDestroyRenderPass)(void*, uint64_t, const void*);
        typedef int32_t (*PFN_vkCreateImage)(void*, const void*, const void*, uint64_t*);
        typedef void    (*PFN_vkDestroyImage)(void*, uint64_t, const void*);
        typedef void    (*PFN_vkGetImageMemoryRequirements)(void*, uint64_t, void*);
        typedef int32_t (*PFN_vkBindImageMemory)(void*, uint64_t, uint64_t, uint64_t);
        typedef int32_t (*PFN_vkCreateImageView)(void*, const void*, const void*, uint64_t*);
        typedef void    (*PFN_vkDestroyImageView)(void*, uint64_t, const void*);
        typedef int32_t (*PFN_vkCreateFramebuffer)(void*, const void*, const void*, uint64_t*);
        typedef void    (*PFN_vkDestroyFramebuffer)(void*, uint64_t, const void*);
        typedef int32_t (*PFN_vkCreateGraphicsPipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
        typedef void    (*PFN_vkFreeMemory)(void*, uint64_t, const void*);
        typedef void    (*PFN_vkCmdBeginRenderPass)(void*, const void*, uint32_t);
        typedef void    (*PFN_vkCmdEndRenderPass)(void*);
        typedef void    (*PFN_vkCmdBindVertexBuffers)(void*, uint32_t, uint32_t, const uint64_t*, const uint64_t*);
        typedef void    (*PFN_vkCmdBindIndexBuffer)(void*, uint64_t, uint64_t, uint32_t);
        typedef void    (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
        typedef void    (*PFN_vkCmdCopyImageToBuffer)(void*, uint64_t, uint32_t, uint64_t, uint32_t, const void*);

        struct VkShaderModuleCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
        };
        VkShaderModuleCreateInfo vs_ci = {16, nullptr, 0, vs_spirv.size() * 4, vs_spirv.data()};
        VkShaderModuleCreateInfo fs_ci = {16, nullptr, 0, fs_spirv.size() * 4, fs_spirv.data()};
        uint64_t vs_mod = 0, fs_mod = 0;
        auto createSM = reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"));
        auto destroySM = reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"));
        if (createSM(device, &vs_ci, nullptr, &vs_mod) != 0 || !vs_mod) return false;
        if (createSM(device, &fs_ci, nullptr, &fs_mod) != 0 || !fs_mod) {
            destroySM(device, vs_mod, nullptr);
            return false;
        }

        // 1. Create offscreen color attachment VkImage + VkDeviceMemory + VkImageView
        struct VkImageCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
            uint32_t extent[3]; uint32_t mipLevels; uint32_t arrayLayers; uint32_t samples;
            uint32_t tiling; uint32_t usage; uint32_t sharingMode; uint32_t qfiCount; const uint32_t* pQfi;
            uint32_t initialLayout;
        } img_ci = {14, nullptr, 0, 1 /* 2D */, 37 /* R8G8B8A8_UNORM */, {width, height, 1}, 1, 1, 1,
                    0 /* OPTIMAL */, 0x10U | 0x01U /* COLOR_ATTACHMENT | TRANSFER_SRC */, 0, 0, nullptr, 0};
        uint64_t color_img = 0, color_mem = 0, color_view = 0;
        if (reinterpret_cast<PFN_vkCreateImage>(gipa(instance, "vkCreateImage"))(device, &img_ci, nullptr, &color_img) != 0) {
            destroySM(device, vs_mod, nullptr); destroySM(device, fs_mod, nullptr);
            return false;
        }
        struct VkMemoryRequirements { uint64_t size; uint64_t alignment; uint32_t memoryTypeBits; uint32_t pad; } mreq{};
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gipa(instance, "vkGetImageMemoryRequirements"))(device, color_img, &mreq);
        struct VkMemoryAllocateInfo { uint32_t sType; const void* pNext; uint64_t size; uint32_t typeIdx; } mai = {
            5, nullptr, std::max<uint64_t>(mreq.size, 65536ULL), 0
        };
        reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"))(device, &mai, nullptr, &color_mem);
        reinterpret_cast<PFN_vkBindImageMemory>(gipa(instance, "vkBindImageMemory"))(device, color_img, color_mem, 0);

        struct VkImageViewCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint64_t image; uint32_t viewType; uint32_t format;
            uint32_t components[4]; uint32_t subresourceRange[5];
        } iv_ci = {15, nullptr, 0, color_img, 1 /* 2D */, 37, {0, 0, 0, 0}, {1 /* COLOR */, 0, 1, 0, 1}};
        reinterpret_cast<PFN_vkCreateImageView>(gipa(instance, "vkCreateImageView"))(device, &iv_ci, nullptr, &color_view);

        // 2. Create VkRenderPass & VkFramebuffer
        struct VkAttachmentDescription {
            uint32_t flags; uint32_t format; uint32_t samples; uint32_t loadOp; uint32_t storeOp;
            uint32_t stencilLoadOp; uint32_t stencilStoreOp; uint32_t initialLayout; uint32_t finalLayout;
        } att_desc = {0, 37, 1, 1 /* CLEAR */, 0 /* STORE */, 2, 1, 0 /* UNDEFINED */, 6 /* TRANSFER_SRC_OPTIMAL */};
        struct VkAttachmentReference { uint32_t attachment; uint32_t layout; } color_ref = {0, 2 /* COLOR_ATTACHMENT_OPTIMAL */};
        struct VkSubpassDescription {
            uint32_t flags; uint32_t bindPoint; uint32_t inCount; const void* pIn;
            uint32_t colorCount; const void* pColor; const void* pResolve; const void* pDepth;
            uint32_t presCount; const void* pPres;
        } subpass = {0, 0 /* GRAPHICS */, 0, nullptr, 1, &color_ref, nullptr, nullptr, 0, nullptr};
        struct VkRenderPassCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t attCount; const void* pAtts;
            uint32_t subpassCount; const void* pSubpasses; uint32_t depCount; const void* pDeps;
        } rp_ci = {38, nullptr, 0, 1, &att_desc, 1, &subpass, 0, nullptr};
        uint64_t render_pass = 0, framebuffer = 0;
        reinterpret_cast<PFN_vkCreateRenderPass>(gipa(instance, "vkCreateRenderPass"))(device, &rp_ci, nullptr, &render_pass);

        struct VkFramebufferCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint64_t renderPass;
            uint32_t attCount; const uint64_t* pAtts; uint32_t width; uint32_t height; uint32_t layers;
        } fb_ci = {37, nullptr, 0, render_pass, 1, &color_view, width, height, 1};
        reinterpret_cast<PFN_vkCreateFramebuffer>(gipa(instance, "vkCreateFramebuffer"))(device, &fb_ci, nullptr, &framebuffer);

        // 3. Create VkGraphicsPipeline from lifted vs_mod and fs_mod
        struct StageCI {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec;
        } stages[2] = {
            {18, nullptr, 0, 0x01 /* VERTEX */,   vs_mod, "main", nullptr},
            {18, nullptr, 0, 0x10 /* FRAGMENT */, fs_mod, "main", nullptr}
        };
        struct VibDesc { uint32_t binding; uint32_t stride; uint32_t inputRate; } vib = {0, 32, 0};
        struct ViaDesc { uint32_t location; uint32_t binding; uint32_t format; uint32_t offset; } via[2] = {
            {0, 0, 109 /* R32G32B32A32_SFLOAT */, 0},
            {1, 0, 109 /* R32G32B32A32_SFLOAT */, 16}
        };
        struct ViState {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t bindCount; const void* pBinds;
            uint32_t attrCount; const void* pAttrs;
        } vi_state = {19, nullptr, 0, 1, &vib, 2, via};
        struct IaState { uint32_t sType; const void* pNext; uint32_t flags; uint32_t topology; uint32_t restart; } ia_state = {20, nullptr, 0, 3 /* TRIANGLE_LIST */, 0};
        struct Viewport { float x, y, w, h, minD, maxD; } vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
        struct Rect2D { int32_t x, y; uint32_t w, h; } sc = {0, 0, width, height};
        struct VpState {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t vpCount; const void* pVps; uint32_t scCount; const void* pScs;
        } vp_state = {22, nullptr, 0, 1, &vp, 1, &sc};
        struct RastState {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t depthClamp; uint32_t discard;
            uint32_t polyMode; uint32_t cullMode; uint32_t frontFace; uint32_t depthBias;
            float biasConst, biasClamp, biasSlope, lineWidth;
        } rast_state = {23, nullptr, 0, 0, 0, 0, 0 /* CULL_NONE */, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f};
        struct MsState {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t samples; uint32_t sampleShading;
            float minSampleShading; const void* pMask; uint32_t alphaToCov; uint32_t alphaToOne;
        } ms_state = {24, nullptr, 0, 1, 0, 1.0f, nullptr, 0, 0};
        struct BlendAtt {
            uint32_t blendEnable; uint32_t srcCol, dstCol, colOp, srcAlpha, dstAlpha, alphaOp, colorWriteMask;
        } blend_att = {0, 1, 0, 0, 1, 0, 0, 0xFU};
        struct BlendState {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t logicOpEnable; uint32_t logicOp;
            uint32_t attCount; const void* pAtts; float blendConstants[4];
        } blend_state = {26, nullptr, 0, 0, 0, 1, &blend_att, {0, 0, 0, 0}};

        struct VkGraphicsPipelineCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint32_t stageCount; const void* pStages;
            const void* pVertexInputState; const void* pInputAssemblyState; const void* pTessellationState;
            const void* pViewportState; const void* pRasterizationState; const void* pMultisampleState;
            const void* pDepthStencilState; const void* pColorBlendState; const void* pDynamicState;
            uint64_t layout; uint64_t renderPass; uint32_t subpass; uint64_t basePipe; int32_t baseIdx;
        } gp_ci = {28, nullptr, 0, 2, stages, &vi_state, &ia_state, nullptr, &vp_state, &rast_state,
                   &ms_state, nullptr, &blend_state, nullptr, pipeline_layout, render_pass, 0, 0, -1};

        uint64_t gfx_pipe = 0;
        int32_t gprc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(instance, "vkCreateGraphicsPipelines"))(
            device, 0, 1, &gp_ci, nullptr, &gfx_pipe);

        bool draw_ok = false;
        if (gprc == 0 && gfx_pipe != 0 && mapped_in) {
            size_t vbo_bytes = std::min<size_t>(vertex_count * 32ULL, 16384ULL);
            size_t ibo_off   = 16384ULL;
            size_t ibo_bytes = std::min<size_t>(index_count * 4ULL, 16384ULL);
            std::memcpy(mapped_in, clip_vertices_32b, vbo_bytes);
            std::memcpy( static_cast<uint8_t*>(mapped_in) + ibo_off, indices, ibo_bytes);

            reinterpret_cast<PFN_vkResetCommandBuffer>(gipa(instance, "vkResetCommandBuffer"))(cmd_buf, 0);
            struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
            reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);

            struct ClearVal { float c[4]; } clear_val = {{0.06f, 0.08f, 0.10f, 1.0f}};
            struct VkRenderPassBeginInfo {
                uint32_t sType; const void* pNext; uint64_t renderPass; uint64_t framebuffer;
                int32_t rx, ry; uint32_t rw, rh; uint32_t clearCount; const void* pClears;
            } rpbi = {43, nullptr, render_pass, framebuffer, 0, 0, width, height, 1, &clear_val};

            reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(instance, "vkCmdBeginRenderPass"))(cmd_buf, &rpbi, 0);
            reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"))(cmd_buf, 0 /* GRAPHICS */, gfx_pipe);
            uint64_t vbo_offset = 0;
            reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(instance, "vkCmdBindVertexBuffers"))(cmd_buf, 0, 1, &buf_in, &vbo_offset);
            reinterpret_cast<PFN_vkCmdBindIndexBuffer>(gipa(instance, "vkCmdBindIndexBuffer"))(cmd_buf, buf_in, ibo_off, 1 /* UINT32 */);
            reinterpret_cast<PFN_vkCmdDrawIndexed>(gipa(instance, "vkCmdDrawIndexed"))(cmd_buf, index_count, 1, 0, 0, 0);
            reinterpret_cast<PFN_vkCmdEndRenderPass>(gipa(instance, "vkCmdEndRenderPass"))(cmd_buf);

            struct VkBufferImageCopy {
                uint64_t bufferOffset; uint32_t bufferRowLength; uint32_t bufferImageHeight;
                uint32_t aspectMask, mipLevel, baseLayer, layerCount;
                int32_t ox, oy, oz; uint32_t ew, eh, ed;
            } bic = {0, 0, 0, 1 /* COLOR */, 0, 0, 1, 0, 0, 0, width, height, 1};
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(gipa(instance, "vkCmdCopyImageToBuffer"))(
                cmd_buf, color_img, 6 /* TRANSFER_SRC_OPTIMAL */, buf_out, 1, &bic);
            reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);

            struct VkSubmitInfo {
                uint32_t sType; const void* pNext; uint32_t waitSemCount; const uint64_t* pWaitSems; const uint32_t* pWaitStages;
                uint32_t cmdBufCount; void* const* pCmdBufs; uint32_t sigSemCount; const uint64_t* pSigSems;
            } si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};
            if (reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0) == 0) {
                reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);
                draw_ok = true;
            }
            reinterpret_cast<PFN_vkDestroyPipeline>(gipa(instance, "vkDestroyPipeline"))(device, gfx_pipe, nullptr);
        }

        reinterpret_cast<PFN_vkDestroyFramebuffer>(gipa(instance, "vkDestroyFramebuffer"))(device, framebuffer, nullptr);
        reinterpret_cast<PFN_vkDestroyRenderPass>(gipa(instance, "vkDestroyRenderPass"))(device, render_pass, nullptr);
        reinterpret_cast<PFN_vkDestroyImageView>(gipa(instance, "vkDestroyImageView"))(device, color_view, nullptr);
        reinterpret_cast<PFN_vkDestroyImage>(gipa(instance, "vkDestroyImage"))(device, color_img, nullptr);
        reinterpret_cast<PFN_vkFreeMemory>(gipa(instance, "vkFreeMemory"))(device, color_mem, nullptr);
        destroySM(device, vs_mod, nullptr);
        destroySM(device, fs_mod, nullptr);
        return draw_ok;
    }

};
