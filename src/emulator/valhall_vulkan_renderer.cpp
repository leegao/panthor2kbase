#include "valhall_vulkan_renderer.h"
#include "mali_kbase_emu.h"
#include "valhall_disasm_lift.h"
#include "../r54p3_vulkan_icd/valhall_spirv_lifter.h"

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace {

/* CSF Job Opcodes (Valhall v10/v11 hardware opcodes 0x04..0x08, 0x2c and legacy 0x26..0x2b) */
static constexpr uint8_t CSF_OP_RUN_COMPUTE_SHORT          = 0x04;
static constexpr uint8_t CSF_OP_RUN_TILING_SHORT           = 0x05;
static constexpr uint8_t CSF_OP_RUN_IDVS_SHORT             = 0x06;
static constexpr uint8_t CSF_OP_RUN_FRAGMENT_SHORT         = 0x07;
static constexpr uint8_t CSF_OP_RUN_FULLSCREEN_SHORT       = 0x08;
static constexpr uint8_t CSF_OP_RUN_COMPUTE_INDIRECT_HW    = 0x2c;

static constexpr uint8_t CSF_OP_RUN_VERTEX_EXT             = 0x26;
static constexpr uint8_t CSF_OP_RUN_COMPUTE_EXT            = 0x27;
static constexpr uint8_t CSF_OP_RUN_TILING_EXT             = 0x28;
static constexpr uint8_t CSF_OP_RUN_IDVS_EXT               = 0x29;
static constexpr uint8_t CSF_OP_RUN_FRAGMENT_EXT           = 0x2a;
static constexpr uint8_t CSF_OP_RUN_FULLSCREEN_EXT         = 0x2b;

/* Hardware Descriptor Layouts in Guest GPU VA Memory */
struct ValhallSPD {
    uint64_t header;     /* Word 0..1: [7:0]=type (0x08=Shader Program), register/preload flags */
    uint64_t exec_va;    /* Word 2..3: 64-bit GPU VA of Valhall shader entry point in EXEC_VA */
};

struct ValhallBufferDesc {
    uint64_t word0;      /* [7:0] = type (0x01 = Buffer), [39:8] = length in bytes */
    uint64_t gpu_va;     /* [63:0] = 64-bit GPU VA of buffer data */
};

struct ValhallTextureDesc {
    uint64_t word0;      /* [7:0] = type (0x02 = Texture), format, dim, flags */
    uint64_t word1;      /* width-1, height-1, depth-1, array_size, mip_levels */
    uint64_t surface_va; /* 64-bit GPU VA of texel surface or plane descriptor */
    uint64_t stride_aux; /* row stride / modifier / swizzle */
};

struct ValhallSamplerDesc {
    uint64_t word0;      /* [7:0] = type (0x03 = Sampler), filter, clamp, lod */
    uint64_t word1;      /* border color / compare op */
};

/* Decoded Compute Descriptor State from CSF RUN_COMPUTE / RUN_COMPUTE_INDIRECT */
struct DecodedComputeJob {
    bool valid = false;
    uint8_t opcode = 0;
    uint64_t insn = 0;
    uint64_t srt_va = 0;
    uint64_t fau_va = 0;
    uint32_t fau_count = 0;
    uint64_t spd_va = 0;
    uint64_t spd_header = 0;
    uint64_t exec_va = 0;
    uint64_t tsd_va = 0;
    uint64_t tls_base_va = 0;
    uint32_t wg_x = 1;
    uint32_t wg_y = 1;
    uint32_t wg_z = 1;
    uint32_t local_size_x = 1;
    uint32_t local_size_y = 1;
    uint32_t local_size_z = 1;
    std::vector<ValhallBufferDesc> buffers;
    std::vector<uint64_t> fau_words;
    std::vector<uint64_t> isa_words;
    std::vector<ValhallDecodedInstr> disasm;
    std::vector<uint32_t> spirv;
};

/* Pending IDVS / Tiling Job State Keyed by Tiler Context Descriptor (TCD) GPU VA */
struct PendingIDVSJob {
    bool valid = false;
    uint64_t tcd_va = 0;
    uint64_t pos_srt_va = 0;
    uint64_t frag_srt_va = 0;
    uint64_t fau_va = 0;
    uint32_t fau_count = 0;
    uint64_t pos_spd_va = 0;
    uint64_t var_spd_va = 0;
    uint64_t frag_spd_va = 0;
    uint64_t pos_exec_va = 0;
    uint64_t var_exec_va = 0;
    uint64_t frag_exec_va = 0;
    uint64_t tsd_va = 0;
    uint64_t zsd_va = 0;
    uint64_t bd_va = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_count = 1;
    uint32_t prim_flags = 0;
    uint32_t varying_stride = 0;
    uint32_t bbox_min_x = 0;
    uint32_t bbox_min_y = 0;
    uint32_t bbox_max_x = 63;
    uint32_t bbox_max_y = 63;
    float depth_clear = 1.0f;

    /* Decoded Primary Uniform/Vertex Buffer from SRT */
    uint64_t primary_buf_va = 0;
    uint32_t primary_buf_size = 0;
    uint64_t secondary_buf_va = 0;
    uint32_t secondary_buf_size = 0;

    /* Decoded Texture from Fragment SRT or Guest Memory */
    uint64_t tex_va = 0;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
    uint32_t tex_stride = 0;

    /* Disassembled & Lifted SPIR-V Modules from Valhall ISA */
    std::vector<uint32_t> pos_spirv;
    std::vector<uint32_t> var_spirv;
    std::vector<uint32_t> frag_spirv;
    uint32_t pos_insn_count = 0;
    uint32_t var_insn_count = 0;
    uint32_t frag_insn_count = 0;
};

/* Self-Contained Host Vulkan Backend (loads libvulkan_lvp.so directly via dlopen) */
struct HostVkBackend {
    std::mutex mtx;
    bool initialized = false;
    bool init_failed = false;
    void* lvp_dl = nullptr;

    PFN_vkGetInstanceProcAddr fp_vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance fp_vkCreateInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices fp_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fp_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties fp_vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice fp_vkCreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr fp_vkGetDeviceProcAddr = nullptr;

    PFN_vkGetDeviceQueue fp_vkGetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool fp_vkCreateCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers fp_vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer fp_vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer fp_vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer fp_vkEndCommandBuffer = nullptr;
    PFN_vkQueueSubmit fp_vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle fp_vkQueueWaitIdle = nullptr;

    PFN_vkCreateBuffer fp_vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer fp_vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements fp_vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory fp_vkAllocateMemory = nullptr;
    PFN_vkFreeMemory fp_vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory fp_vkBindBufferMemory = nullptr;
    PFN_vkMapMemory fp_vkMapMemory = nullptr;
    PFN_vkUnmapMemory fp_vkUnmapMemory = nullptr;

    PFN_vkCreateImage fp_vkCreateImage = nullptr;
    PFN_vkDestroyImage fp_vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements fp_vkGetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory fp_vkBindImageMemory = nullptr;
    PFN_vkCreateImageView fp_vkCreateImageView = nullptr;
    PFN_vkDestroyImageView fp_vkDestroyImageView = nullptr;
    PFN_vkCreateSampler fp_vkCreateSampler = nullptr;
    PFN_vkDestroySampler fp_vkDestroySampler = nullptr;

    PFN_vkCreateShaderModule fp_vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule fp_vkDestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout fp_vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout fp_vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool fp_vkCreateDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool fp_vkResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets fp_vkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets fp_vkUpdateDescriptorSets = nullptr;
    PFN_vkCreatePipelineLayout fp_vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout fp_vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateRenderPass fp_vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass fp_vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer fp_vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer fp_vkDestroyFramebuffer = nullptr;
    PFN_vkCreateGraphicsPipelines fp_vkCreateGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines fp_vkCreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline fp_vkDestroyPipeline = nullptr;

    PFN_vkCmdBeginRenderPass fp_vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass fp_vkCmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline fp_vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets fp_vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdSetViewport fp_vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor fp_vkCmdSetScissor = nullptr;
    PFN_vkCmdDraw fp_vkCmdDraw = nullptr;
    PFN_vkCmdDispatch fp_vkCmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier fp_vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer fp_vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage fp_vkCmdCopyBufferToImage = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;

    /* Cached Vulkan Graphics Pipeline for CSF IDVS+Fragment Execution */
    VkDescriptorSetLayout gfx_dsl = VK_NULL_HANDLE;
    VkPipelineLayout gfx_pipe_layout = VK_NULL_HANDLE;
    VkRenderPass gfx_rp = VK_NULL_HANDLE;
    VkPipeline gfx_pipeline = VK_NULL_HANDLE;

    /* Persistent Host-Visible UBO, Texture, and Compute SSBO Buffers */
    VkBuffer ubo_buf = VK_NULL_HANDLE;
    VkDeviceMemory ubo_mem = VK_NULL_HANDLE;
    void* ubo_mapped = nullptr;
    uint32_t ubo_cap = 65536;

    VkBuffer comp_buf0 = VK_NULL_HANDLE;
    VkDeviceMemory comp_mem0 = VK_NULL_HANDLE;
    void* comp_mapped0 = nullptr;

    VkBuffer comp_buf1 = VK_NULL_HANDLE;
    VkDeviceMemory comp_mem1 = VK_NULL_HANDLE;
    void* comp_mapped1 = nullptr;
    uint32_t comp_buf_cap = 65536;

    VkImage tex_img = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    VkSampler tex_sampler = VK_NULL_HANDLE;
    uint32_t tex_w = 0;
    uint32_t tex_h = 0;
    bool tex_uploaded = false;

    std::unordered_map<uint64_t, PendingIDVSJob> pending_idvs_by_tcd;
    PendingIDVSJob last_idvs_job;
    std::atomic<uint64_t> frames_rendered{0};
    std::atomic<uint64_t> compute_jobs_executed{0};
    std::atomic<uint64_t> last_rt_va{0};
};

static HostVkBackend g_vvr;

static uint32_t read_cs_reg32(const uint64_t* cs_regs, uint32_t reg_idx) {
    if (reg_idx >= 128) return 0;
    return static_cast<uint32_t>(cs_regs[reg_idx] & 0xFFFFFFFFULL);
}

static uint64_t read_cs_reg64(const uint64_t* cs_regs, uint32_t reg_lo) {
    if (reg_lo >= 127) return 0;
    uint64_t lo = cs_regs[reg_lo] & 0xFFFFFFFFULL;
    uint64_t hi = cs_regs[reg_lo + 1] & 0xFFFFFFFFULL;
    return (hi << 32) | lo;
}

/* Safely read guest GPU VA memory via /dev/mali0 emulator VA table or direct host VA mapping */
static bool read_guest_va(uint64_t gpu_va, void* out_buf, size_t len) {
    if (!gpu_va || gpu_va < 0x10000ULL || !out_buf || len == 0) return false;
    if (mali_kbase_emu_is_valid_gpu_va(gpu_va, len)) {
        std::memcpy(out_buf, reinterpret_cast<const void*>(static_cast<uintptr_t>(gpu_va)), len);
        return true;
    }
    return false;
}

static uint8_t* resolve_guest_va_ptr(uint64_t gpu_va, size_t min_bytes) {
    if (!gpu_va || gpu_va < 0x10000ULL || min_bytes == 0) return nullptr;
    if (mali_kbase_emu_is_valid_gpu_va(gpu_va, min_bytes)) {
        return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(gpu_va));
    }
    return nullptr;
}

/* Scan guest Valhall ISA instructions in EXEC_VA up to terminal return/zero */
static uint32_t extract_valhall_isa_words(uint64_t exec_va,
                                          std::vector<uint64_t>& out_insns,
                                          std::vector<ValhallDecodedInstr>* out_disasm) {
    out_insns.clear();
    if (!exec_va || exec_va < 0x10000ULL) return 0;
    uint64_t clean_va = exec_va & ~0x7ULL;
    if (!resolve_guest_va_ptr(clean_va, 8)) return 0;

    int zero_run = 0;
    for (uint32_t i = 0; i < 256; ++i) {
        uint64_t insn = 0;
        if (!read_guest_va(clean_va + i * 8, &insn, sizeof(insn))) break;
        if (insn == 0ULL) {
            zero_run++;
            if (zero_run >= 2) break;
            continue;
        }
        zero_run = 0;
        out_insns.push_back(insn);
        /* Check Valhall instruction flow control nibble [62:59] for .end (0x7 / 0xF) on NOP */
        uint32_t op_hi = static_cast<uint32_t>((insn >> 48) & 0x1FFU);
        uint8_t flow = static_cast<uint8_t>((insn >> 59) & 0xFU);
        if (op_hi == 0x000U && (flow == 0x6U || flow == 0x7U || flow == 0xFU) && i >= 1) {
            break;
        }
    }
    if (!out_insns.empty() && out_disasm) {
        *out_disasm = ValhallISAToSpirvTranslator::disassembleObjc(out_insns.data(), out_insns.size());
    }
    return static_cast<uint32_t>(out_insns.size());
}

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties& mp, uint32_t type_bits, VkMemoryPropertyFlags req) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & req) == req)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (type_bits & (1u << i)) return i;
    }
    return 0;
}

static bool alloc_host_visible_buffer_locked(uint32_t size_bytes,
                                             VkBufferUsageFlags usage,
                                             VkBuffer& out_buf,
                                             VkDeviceMemory& out_mem,
                                             void*& out_mapped) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size_bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vvr.fp_vkCreateBuffer(g_vvr.device, &bci, nullptr, &out_buf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    g_vvr.fp_vkGetBufferMemoryRequirements(g_vvr.device, out_buf, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(g_vvr.mem_props, mr.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (g_vvr.fp_vkAllocateMemory(g_vvr.device, &mai, nullptr, &out_mem) != VK_SUCCESS) return false;
    g_vvr.fp_vkBindBufferMemory(g_vvr.device, out_buf, out_mem, 0);
    g_vvr.fp_vkMapMemory(g_vvr.device, out_mem, 0, size_bytes, 0, &out_mapped);
    return out_mapped != nullptr;
}

static bool ensure_host_vk_initialized_locked() {
    if (g_vvr.initialized) return true;
    if (g_vvr.init_failed) return false;

    const char* lvp_paths[] = {
        "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
        "libvulkan_lvp.so",
        nullptr
    };
    for (int i = 0; lvp_paths[i]; ++i) {
        g_vvr.lvp_dl = dlopen(lvp_paths[i], RTLD_NOW | RTLD_LOCAL);
        if (g_vvr.lvp_dl) break;
    }
    if (!g_vvr.lvp_dl) {
        g_vvr.init_failed = true;
        return false;
    }

    g_vvr.fp_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g_vvr.lvp_dl, "vk_icdGetInstanceProcAddr"));
    if (!g_vvr.fp_vkGetInstanceProcAddr) {
        g_vvr.fp_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(g_vvr.lvp_dl, "vkGetInstanceProcAddr"));
    }
    if (!g_vvr.fp_vkGetInstanceProcAddr) {
        g_vvr.init_failed = true;
        return false;
    }

#define LOAD_INST_FN(fn) \
    g_vvr.fp_##fn = reinterpret_cast<PFN_##fn>(g_vvr.fp_vkGetInstanceProcAddr(g_vvr.instance, #fn))

    g_vvr.fp_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_vvr.fp_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!g_vvr.fp_vkCreateInstance) {
        g_vvr.init_failed = true;
        return false;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "ValhallVulkanCSFRenderer";
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;

    if (g_vvr.fp_vkCreateInstance(&ici, nullptr, &g_vvr.instance) != VK_SUCCESS || !g_vvr.instance) {
        g_vvr.init_failed = true;
        return false;
    }

    LOAD_INST_FN(vkEnumeratePhysicalDevices);
    LOAD_INST_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST_FN(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INST_FN(vkCreateDevice);
    LOAD_INST_FN(vkGetDeviceProcAddr);

    uint32_t pd_count = 1;
    if (!g_vvr.fp_vkEnumeratePhysicalDevices ||
        g_vvr.fp_vkEnumeratePhysicalDevices(g_vvr.instance, &pd_count, &g_vvr.phys_dev) < VK_SUCCESS ||
        pd_count == 0) {
        g_vvr.init_failed = true;
        return false;
    }
    g_vvr.fp_vkGetPhysicalDeviceMemoryProperties(g_vvr.phys_dev, &g_vvr.mem_props);

    float q_prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = 0;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &q_prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;

    if (g_vvr.fp_vkCreateDevice(g_vvr.phys_dev, &dci, nullptr, &g_vvr.device) != VK_SUCCESS || !g_vvr.device) {
        g_vvr.init_failed = true;
        return false;
    }

#define LOAD_DEV_FN(fn) \
    g_vvr.fp_##fn = reinterpret_cast<PFN_##fn>(g_vvr.fp_vkGetDeviceProcAddr(g_vvr.device, #fn))

    LOAD_DEV_FN(vkGetDeviceQueue);
    LOAD_DEV_FN(vkCreateCommandPool);
    LOAD_DEV_FN(vkAllocateCommandBuffers);
    LOAD_DEV_FN(vkResetCommandBuffer);
    LOAD_DEV_FN(vkBeginCommandBuffer);
    LOAD_DEV_FN(vkEndCommandBuffer);
    LOAD_DEV_FN(vkQueueSubmit);
    LOAD_DEV_FN(vkQueueWaitIdle);
    LOAD_DEV_FN(vkCreateBuffer);
    LOAD_DEV_FN(vkDestroyBuffer);
    LOAD_DEV_FN(vkGetBufferMemoryRequirements);
    LOAD_DEV_FN(vkAllocateMemory);
    LOAD_DEV_FN(vkFreeMemory);
    LOAD_DEV_FN(vkBindBufferMemory);
    LOAD_DEV_FN(vkMapMemory);
    LOAD_DEV_FN(vkUnmapMemory);
    LOAD_DEV_FN(vkCreateImage);
    LOAD_DEV_FN(vkDestroyImage);
    LOAD_DEV_FN(vkGetImageMemoryRequirements);
    LOAD_DEV_FN(vkBindImageMemory);
    LOAD_DEV_FN(vkCreateImageView);
    LOAD_DEV_FN(vkDestroyImageView);
    LOAD_DEV_FN(vkCreateSampler);
    LOAD_DEV_FN(vkDestroySampler);
    LOAD_DEV_FN(vkCreateShaderModule);
    LOAD_DEV_FN(vkDestroyShaderModule);
    LOAD_DEV_FN(vkCreateDescriptorSetLayout);
    LOAD_DEV_FN(vkDestroyDescriptorSetLayout);
    LOAD_DEV_FN(vkCreateDescriptorPool);
    LOAD_DEV_FN(vkResetDescriptorPool);
    LOAD_DEV_FN(vkAllocateDescriptorSets);
    LOAD_DEV_FN(vkUpdateDescriptorSets);
    LOAD_DEV_FN(vkCreatePipelineLayout);
    LOAD_DEV_FN(vkDestroyPipelineLayout);
    LOAD_DEV_FN(vkCreateRenderPass);
    LOAD_DEV_FN(vkDestroyRenderPass);
    LOAD_DEV_FN(vkCreateFramebuffer);
    LOAD_DEV_FN(vkDestroyFramebuffer);
    LOAD_DEV_FN(vkCreateGraphicsPipelines);
    LOAD_DEV_FN(vkCreateComputePipelines);
    LOAD_DEV_FN(vkDestroyPipeline);
    LOAD_DEV_FN(vkCmdBeginRenderPass);
    LOAD_DEV_FN(vkCmdEndRenderPass);
    LOAD_DEV_FN(vkCmdBindPipeline);
    LOAD_DEV_FN(vkCmdBindDescriptorSets);
    LOAD_DEV_FN(vkCmdSetViewport);
    LOAD_DEV_FN(vkCmdSetScissor);
    LOAD_DEV_FN(vkCmdDraw);
    LOAD_DEV_FN(vkCmdDispatch);
    LOAD_DEV_FN(vkCmdPipelineBarrier);
    LOAD_DEV_FN(vkCmdCopyImageToBuffer);
    LOAD_DEV_FN(vkCmdCopyBufferToImage);

#undef LOAD_DEV_FN
#undef LOAD_INST_FN

    g_vvr.fp_vkGetDeviceQueue(g_vvr.device, 0, 0, &g_vvr.queue);

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = 0;
    g_vvr.fp_vkCreateCommandPool(g_vvr.device, &cpci, nullptr, &g_vvr.cmd_pool);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_vvr.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    g_vvr.fp_vkAllocateCommandBuffers(g_vvr.device, &cbai, &g_vvr.cmd_buf);

    VkDescriptorPoolSize pool_sizes[3]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 64;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 64;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[2].descriptorCount = 64;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = pool_sizes;
    g_vvr.fp_vkCreateDescriptorPool(g_vvr.device, &dpci, nullptr, &g_vvr.desc_pool);

    VkBufferUsageFlags buf_usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    alloc_host_visible_buffer_locked(g_vvr.ubo_cap, buf_usage, g_vvr.ubo_buf, g_vvr.ubo_mem, g_vvr.ubo_mapped);
    alloc_host_visible_buffer_locked(g_vvr.comp_buf_cap, buf_usage, g_vvr.comp_buf0, g_vvr.comp_mem0, g_vvr.comp_mapped0);
    alloc_host_visible_buffer_locked(g_vvr.comp_buf_cap, buf_usage, g_vvr.comp_buf1, g_vvr.comp_mem1, g_vvr.comp_mapped1);

    g_vvr.initialized = true;
    return true;
}

static bool ensure_guest_texture_uploaded_locked(const PendingIDVSJob& job) {
    if (g_vvr.tex_uploaded) return true;

    if (job.tex_va == 0 || job.tex_width == 0 || job.tex_height == 0) {
        std::fprintf(stderr,
                     "[valhall_vk_renderer] ERROR: unresolved guest texture "
                     "(tex_va=0x%llx %ux%u stride=%u)\n",
                     static_cast<unsigned long long>(job.tex_va),
                     job.tex_width, job.tex_height, job.tex_stride);
        return false;
    }

    uint32_t W = job.tex_width;
    uint32_t H = job.tex_height;
    g_vvr.tex_w = W;
    g_vvr.tex_h = H;

    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4, 0);
    uint8_t* p = resolve_guest_va_ptr(job.tex_va, static_cast<size_t>(W) * H * 4);
    if (!p) {
        std::fprintf(stderr,
                     "[valhall_vk_renderer] ERROR: guest texture VA 0x%llx (%ux%u) not mapped\n",
                     static_cast<unsigned long long>(job.tex_va), W, H);
        return false;
    }
    uint32_t non_zero = 0;
    const uint32_t* p32 = reinterpret_cast<const uint32_t*>(p);
    for (uint32_t i = 0; i < W * H; ++i) {
        if ((p32[i] & 0x00FFFFFFU) != 0) non_zero++;
    }
    if (non_zero == 0) {
        std::fprintf(stderr,
                     "[valhall_vk_renderer] WARNING: guest texture 0x%llx (%ux%u) is entirely black\n",
                     static_cast<unsigned long long>(job.tex_va), W, H);
    }
    std::memcpy(rgba.data(), p, static_cast<size_t>(W) * H * 4);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {W, H, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_vvr.fp_vkCreateImage(g_vvr.device, &ici, nullptr, &g_vvr.tex_img);

    VkMemoryRequirements mr{};
    g_vvr.fp_vkGetImageMemoryRequirements(g_vvr.device, g_vvr.tex_img, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(g_vvr.mem_props, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    g_vvr.fp_vkAllocateMemory(g_vvr.device, &mai, nullptr, &g_vvr.tex_mem);
    g_vvr.fp_vkBindImageMemory(g_vvr.device, g_vvr.tex_img, g_vvr.tex_mem, 0);

    VkBuffer stg_buf = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    alloc_host_visible_buffer_locked(static_cast<uint32_t>(rgba.size()),
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     stg_buf, stg_mem, mapped);
    if (mapped) {
        std::memcpy(mapped, rgba.data(), rgba.size());
        g_vvr.fp_vkUnmapMemory(g_vvr.device, stg_mem);
    }

    g_vvr.fp_vkResetCommandBuffer(g_vvr.cmd_buf, 0);
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vvr.fp_vkBeginCommandBuffer(g_vvr.cmd_buf, &cbbi);

    VkImageMemoryBarrier bar1{};
    bar1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar1.image = g_vvr.tex_img;
    bar1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vvr.fp_vkCmdPipelineBarrier(g_vvr.cmd_buf,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &bar1);

    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {W, H, 1};
    g_vvr.fp_vkCmdCopyBufferToImage(g_vvr.cmd_buf, stg_buf, g_vvr.tex_img,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

    VkImageMemoryBarrier bar2{};
    bar2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar2.image = g_vvr.tex_img;
    bar2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vvr.fp_vkCmdPipelineBarrier(g_vvr.cmd_buf,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &bar2);

    g_vvr.fp_vkEndCommandBuffer(g_vvr.cmd_buf);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_vvr.cmd_buf;
    g_vvr.fp_vkQueueSubmit(g_vvr.queue, 1, &si, VK_NULL_HANDLE);
    g_vvr.fp_vkQueueWaitIdle(g_vvr.queue);

    g_vvr.fp_vkDestroyBuffer(g_vvr.device, stg_buf, nullptr);
    g_vvr.fp_vkFreeMemory(g_vvr.device, stg_mem, nullptr);

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = g_vvr.tex_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vvr.fp_vkCreateImageView(g_vvr.device, &ivci, nullptr, &g_vvr.tex_view);

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    g_vvr.fp_vkCreateSampler(g_vvr.device, &sci, nullptr, &g_vvr.tex_sampler);

    g_vvr.tex_uploaded = true;
    return true;
}

static bool ensure_gfx_pipeline_locked(const PendingIDVSJob& job) {
    if (g_vvr.gfx_pipeline != VK_NULL_HANDLE) return true;

    std::vector<uint32_t> vs_words = job.pos_spirv;
    std::vector<uint32_t> fs_words = job.frag_spirv;
    if (vs_words.empty() || fs_words.empty()) {
        std::fprintf(stderr,
                     "[valhall_vk_renderer] ERROR: no lifted SPIR-V for graphics job "
                     "(pos=%zu words, frag=%zu words, pos_insn=%u, frag_insn=%u)\n",
                     vs_words.size(), fs_words.size(),
                     job.pos_insn_count, job.frag_insn_count);
        return false;
    }

    VkShaderModule vs_mod = VK_NULL_HANDLE, fs_mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vs_words.size() * sizeof(uint32_t);
    smci.pCode = vs_words.data();
    if (g_vvr.fp_vkCreateShaderModule(g_vvr.device, &smci, nullptr, &vs_mod) != VK_SUCCESS) {
        std::fprintf(stderr, "[valhall_vk_renderer] ERROR: vkCreateShaderModule(vertex) failed\n");
        return false;
    }

    smci.codeSize = fs_words.size() * sizeof(uint32_t);
    smci.pCode = fs_words.data();
    if (g_vvr.fp_vkCreateShaderModule(g_vvr.device, &smci, nullptr, &fs_mod) != VK_SUCCESS) {
        std::fprintf(stderr, "[valhall_vk_renderer] ERROR: vkCreateShaderModule(fragment) failed\n");
        g_vvr.fp_vkDestroyShaderModule(g_vvr.device, vs_mod, nullptr);
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    g_vvr.fp_vkCreateDescriptorSetLayout(g_vvr.device, &dslci, nullptr, &g_vvr.gfx_dsl);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_vvr.gfx_dsl;
    g_vvr.fp_vkCreatePipelineLayout(g_vvr.device, &plci, nullptr, &g_vvr.gfx_pipe_layout);

    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    g_vvr.fp_vkCreateRenderPass(g_vvr.device, &rpci, nullptr, &g_vvr.gfx_rp);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_mod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xFU;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyns{};
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 2;
    dyns.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyns;
    gpci.layout = g_vvr.gfx_pipe_layout;
    gpci.renderPass = g_vvr.gfx_rp;
    gpci.subpass = 0;

    VkResult res = g_vvr.fp_vkCreateGraphicsPipelines(g_vvr.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &g_vvr.gfx_pipeline);
    g_vvr.fp_vkDestroyShaderModule(g_vvr.device, vs_mod, nullptr);
    g_vvr.fp_vkDestroyShaderModule(g_vvr.device, fs_mod, nullptr);

    if (res != VK_SUCCESS || g_vvr.gfx_pipeline == VK_NULL_HANDLE) {
        std::fprintf(stderr,
                     "[valhall_vk_renderer] ERROR: vkCreateGraphicsPipelines failed (VkResult=%d)\n",
                     static_cast<int>(res));
        return false;
    }
    return true;
}

/*
 * Parse RUN_IDVS (opcode 0x06 / 0x29) hardware registers and GPU VA descriptors
 */
static void handle_csf_run_idvs(uint64_t insn, const uint64_t* cs_regs) {
    std::lock_guard<std::mutex> lk(g_vvr.mtx);

    PendingIDVSJob job{};
    job.valid = true;
    job.pos_srt_va  = read_cs_reg64(cs_regs, 0);
    job.frag_srt_va = ((insn >> 38) & 1ULL) ? read_cs_reg64(cs_regs, 4) : read_cs_reg64(cs_regs, 2);

    uint64_t fau_raw = read_cs_reg64(cs_regs, 8);
    job.fau_va    = fau_raw & 0x00FFFFFFFFFFFFFFULL;
    job.fau_count = static_cast<uint32_t>((fau_raw >> 56) & 0xFFU);

    job.pos_spd_va  = read_cs_reg64(cs_regs, 16) & ~0xFULL;
    job.var_spd_va  = read_cs_reg64(cs_regs, 18) & ~0xFULL;
    job.frag_spd_va = read_cs_reg64(cs_regs, 20) & ~0xFULL;
    job.tsd_va      = read_cs_reg64(cs_regs, 24) & ~0x3FULL;

    job.vertex_count   = read_cs_reg32(cs_regs, 33);
    job.instance_count = (read_cs_reg32(cs_regs, 34) > 0) ? read_cs_reg32(cs_regs, 34) : 1;
    job.prim_flags     = read_cs_reg32(cs_regs, 38);
    job.tcd_va         = read_cs_reg64(cs_regs, 40) & ~0x3FULL;

    uint32_t scissor_min = read_cs_reg32(cs_regs, 42);
    uint32_t scissor_max = read_cs_reg32(cs_regs, 43);
    job.bbox_min_x = scissor_min & 0xFFFFU;
    job.bbox_min_y = (scissor_min >> 16) & 0xFFFFU;
    job.bbox_max_x = scissor_max & 0xFFFFU;
    job.bbox_max_y = (scissor_max >> 16) & 0xFFFFU;

    job.varying_stride = read_cs_reg32(cs_regs, 48);
    job.zsd_va         = read_cs_reg64(cs_regs, 50) & ~0x3FULL;
    job.bd_va          = read_cs_reg64(cs_regs, 52) & ~0x3FULL;
    uint32_t raw_depth = read_cs_reg32(cs_regs, 60);
    std::memcpy(&job.depth_clear, &raw_depth, sizeof(float));

    /* Decode Position, Varying, and Fragment SPDs from guest GPU VA memory */
    ValhallSPD pos_spd{}, var_spd{}, frag_spd{};
    if (read_guest_va(job.pos_spd_va, &pos_spd, sizeof(pos_spd))) {
        job.pos_exec_va = pos_spd.exec_va;
    }
    if (read_guest_va(job.var_spd_va, &var_spd, sizeof(var_spd))) {
        job.var_exec_va = var_spd.exec_va;
    }
    if (read_guest_va(job.frag_spd_va, &frag_spd, sizeof(frag_spd))) {
        job.frag_exec_va = frag_spd.exec_va;
    }

    /* Extract Valhall ISA instructions and lift to SPIR-V via ValhallISAToSpirvTranslator */
    std::vector<uint64_t> pos_insns, var_insns, frag_insns;
    job.pos_insn_count  = extract_valhall_isa_words(job.pos_exec_va, pos_insns, nullptr);
    job.var_insn_count  = extract_valhall_isa_words(job.var_exec_va, var_insns, nullptr);
    job.frag_insn_count = extract_valhall_isa_words(job.frag_exec_va, frag_insns, nullptr);

    if (!pos_insns.empty()) {
        job.pos_spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(pos_insns.data(), pos_insns.size());
    }
    if (!frag_insns.empty()) {
        job.frag_spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(frag_insns.data(), frag_insns.size());
    }

    /* Decode Shader Resource Table (SRT) Buffer Descriptors & LD_PKA Packaged Pointers at pos_srt_va */
    if (job.pos_srt_va != 0) {
        /* 1. Check for Valhall v10/v11 LD_PKA packaged buffer pointer (tag 0x01..0x08 in top byte)
         *    at pos_srt_va + 0x38 (u6:u7 preload slot) or across pos_srt_va + 0x00..0x58 */
        for (int64_t off = 0x38; off >= 0; off -= 8) {
            uint64_t pka_words[2]{};
            if (read_guest_va(job.pos_srt_va + static_cast<uint64_t>(off), pka_words, sizeof(pka_words))) {
                uint8_t top_tag = static_cast<uint8_t>((pka_words[0] >> 56) & 0xFFU);
                uint64_t cand_va = pka_words[0] & 0x00FFFFFFFFFFFFFFULL;
                if (top_tag >= 0x01U && top_tag <= 0x08U && cand_va >= 0x10000ULL) {
                    /* Check if cand_va is a VkDescriptorSet table containing inner Buffer (0x19/0x01) & Texture (0x22->0x1a) Descriptors */
                    uint64_t ds_words[12]{};
                    if (pka_words[1] <= 0x100 && read_guest_va(cand_va, ds_words, sizeof(ds_words))) {
                        for (int k = 0; k < 12; ++k) {
                            uint8_t inner_tag = static_cast<uint8_t>((ds_words[k] >> 56) & 0xFFU);
                            uint64_t inner_va = ds_words[k] & 0x00FFFFFFFFFFFFFFULL;
                            if (inner_tag >= 0x01U && inner_tag <= 0x08U && inner_va >= 0x10000ULL &&
                                resolve_guest_va_ptr(inner_va, 16)) {
                                uint32_t inner_len = (k + 1 < 12 && ds_words[k + 1] >= 16 &&
                                                      ds_words[k + 1] <= 65536)
                                    ? static_cast<uint32_t>(ds_words[k + 1]) : 64U;
                                if (resolve_guest_va_ptr(inner_va, inner_len)) {
                                    job.primary_buf_va   = inner_va;
                                    job.primary_buf_size = inner_len;
                                }
                            }
                            if (k + 1 < 12) {
                                uint8_t dt = static_cast<uint8_t>(ds_words[k] & 0xFFU);
                                uint32_t dl = (dt == 0x19U) ? static_cast<uint32_t>(ds_words[k] >> 32)
                                                            : static_cast<uint32_t>((ds_words[k] >> 8) & 0xFFFFFFFFU);
                                if ((dt == 0x19U || dt == 0x01U) && dl >= 1024 && dl <= 65536 &&
                                    ds_words[k+1] >= 0x10000ULL && resolve_guest_va_ptr(ds_words[k+1], dl)) {
                                    job.primary_buf_va   = ds_words[k+1];
                                    job.primary_buf_size = dl;
                                }
                            }
                            /* Decode Valhall v10/v11 2D Texture Descriptor (type 0x22) -> Plane Descriptor (type 0x1a) */
                            if (k + 2 < 12 && (ds_words[k] & 0xFFU) == 0x22U) {
                                uint32_t tw = static_cast<uint32_t>(((ds_words[k] >> 32) & 0xFFFFU) + 1U);
                                uint32_t th = static_cast<uint32_t>(((ds_words[k] >> 48) & 0xFFFFU) + 1U);
                                uint64_t plane_va = ds_words[k + 2];
                                uint64_t pd[4]{};
                                if (plane_va >= 0x10000ULL && read_guest_va(plane_va, pd, sizeof(pd))) {
                                    if ((pd[0] & 0xFFU) == 0x1aU && pd[1] >= 0x10000ULL) {
                                        uint32_t pw = static_cast<uint32_t>(((pd[3] >> 32) & 0xFFFFU) + 1U);
                                        uint32_t ph = static_cast<uint32_t>(((pd[3] >> 48) & 0xFFFFU) + 1U);
                                        uint32_t pstride = static_cast<uint32_t>(pd[2] & 0xFFFFFFFFU);
                                        if (pw >= 16 && pw <= 4096 && ph >= 16 && ph <= 4096) {
                                            tw = pw;
                                            th = ph;
                                        }
                                        if (resolve_guest_va_ptr(pd[1], static_cast<size_t>(tw) * th * 4)) {
                                            job.tex_va     = pd[1];
                                            job.tex_width  = tw;
                                            job.tex_height = th;
                                            job.tex_stride = (pstride > 0) ? pstride : (tw * 4);
                                        }
                                    }
                                }
                            }
                        }
                        if (job.primary_buf_va != 0) break;
                    }
                    if (resolve_guest_va_ptr(cand_va, 64)) {
                        job.primary_buf_va   = cand_va;
                        job.primary_buf_size = static_cast<uint32_t>(
                            (pka_words[1] >= 64 && pka_words[1] <= 65536) ? pka_words[1] : 256);
                        break;
                    }
                }
            }
        }

        /* 2. Check direct 16-byte Buffer Descriptors (type 0x01 or Valhall v10/v11 type 0x19) at pos_srt_va */
        ValhallBufferDesc bdesc[8]{};
        if (read_guest_va(job.pos_srt_va, bdesc, sizeof(bdesc))) {
            for (int i = 0; i < 8; ++i) {
                uint8_t dtype = static_cast<uint8_t>(bdesc[i].word0 & 0xFFU);
                uint32_t blen = (dtype == 0x19U)
                    ? static_cast<uint32_t>(bdesc[i].word0 >> 32)
                    : static_cast<uint32_t>((bdesc[i].word0 >> 8) & 0xFFFFFFFFU);
                uint64_t bva  = bdesc[i].gpu_va;
                if ((dtype == 0x01U || dtype == 0x19U) && bva >= 0x10000ULL && blen > 0 && blen <= (64U << 20)) {
                    if (resolve_guest_va_ptr(bva, blen)) {
                        if (job.primary_buf_va == 0 || (blen >= 1024 && job.primary_buf_size < 1024)) {
                            job.primary_buf_va = bva;
                            job.primary_buf_size = blen;
                        } else if (job.secondary_buf_va == 0) {
                            job.secondary_buf_va = bva;
                            job.secondary_buf_size = blen;
                        }
                    }
                }
            }
        }

        /* 3. Check indirect descriptor table pointer at pos_srt_va - 0x08 (Valhall halp::resource_table header) */
        if (job.pos_srt_va >= 0x10008ULL) {
            uint64_t tbl_hdr[2]{};
            if (read_guest_va(job.pos_srt_va - 8ULL, tbl_hdr, sizeof(tbl_hdr))) {
                uint64_t tbl_va  = tbl_hdr[0];
                uint64_t tbl_len = tbl_hdr[1];
                if (tbl_va >= 0x10000ULL && tbl_len >= 16 && tbl_len <= 4096) {
                    size_t cnt = static_cast<size_t>(tbl_len / 16);
                    if (cnt > 32) cnt = 32;
                    std::vector<ValhallBufferDesc> tdescs(cnt);
                    if (read_guest_va(tbl_va, tdescs.data(), cnt * sizeof(ValhallBufferDesc))) {
                        for (size_t i = 0; i < cnt; ++i) {
                            uint8_t dtype = static_cast<uint8_t>(tdescs[i].word0 & 0xFFU);
                            uint32_t blen = (dtype == 0x19U)
                                ? static_cast<uint32_t>(tdescs[i].word0 >> 32)
                                : static_cast<uint32_t>((tdescs[i].word0 >> 8) & 0xFFFFFFFFU);
                            uint64_t bva  = tdescs[i].gpu_va;
                            if ((dtype == 0x01U || dtype == 0x19U) && bva >= 0x10000ULL && blen > 0 && blen <= (64U << 20)) {
                                if (resolve_guest_va_ptr(bva, blen)) {
                                    if (job.primary_buf_va == 0) {
                                        job.primary_buf_va = bva;
                                        job.primary_buf_size = blen;
                                    } else if (job.secondary_buf_va == 0 && bva != job.primary_buf_va) {
                                        job.secondary_buf_va = bva;
                                        job.secondary_buf_size = blen;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (job.tcd_va != 0) {
        g_vvr.pending_idvs_by_tcd[job.tcd_va] = job;
    }
    g_vvr.last_idvs_job = job;
}

/*
 * Execute RUN_FRAGMENT (opcode 0x07 / 0x2a):
 *   1. Decode Framebuffer Descriptor (FBD) & Render Target Descriptor (RTD #0) from r40:r41
 *   2. Match Tiler Context Descriptor (TCD) at FBD+0x38 to the queued RUN_IDVS job
 *   3. Copy live guest SRT Buffer #0 data (MVP matrix + positions + UVs) from guest GPU VA into host VkBuffer
 *   4. Record & submit Vulkan RenderPass + Draw on host Lavapipe VkDevice
 *   5. Copy rendered BGRA8 color attachment directly into guest RTD #0 color_buffer_va (e.g. 0x7f10bf1c1000)
 */
static void handle_csf_run_fragment(uint64_t /*insn*/, const uint64_t* cs_regs) {
    std::lock_guard<std::mutex> lk(g_vvr.mtx);

    uint64_t fbd_ptr_raw = read_cs_reg64(cs_regs, 40);
    uint64_t fbd_va = fbd_ptr_raw & ~0x3FULL;
    if (!fbd_va) return;

    uint64_t fbd_words[36]{};
    if (!read_guest_va(fbd_va, fbd_words, sizeof(fbd_words))) return;

    /* FBD+0x20: [15:0]=width-1, [31:16]=height-1 */
    uint64_t dim_word = fbd_words[4];
    uint32_t fb_width  = static_cast<uint32_t>((dim_word & 0xFFFFU) + 1U);
    uint32_t fb_height = static_cast<uint32_t>(((dim_word >> 16) & 0xFFFFU) + 1U);
    if (fb_width == 0 || fb_width > 4096) fb_width = 64;
    if (fb_height == 0 || fb_height > 4096) fb_height = 64;

    /* FBD+0x38: Tiler Context Descriptor (TCD) GPU VA */
    uint64_t fbd_tcd_va = fbd_words[7] & ~0x3FULL;

    /* RTD #0 starts at FBD+0xC0 (word 24):
     *   +0xE0 (word 28): Color Writebuffer GPU VA
     *   +0xE8 (word 29): Row stride in bytes (low 32 bits)
     *   +0xF0..0xF8 (words 30..31): Clear color payload
     */
    uint64_t rt_color_va  = fbd_words[28];
    uint32_t rt_row_pitch = static_cast<uint32_t>(fbd_words[29] & 0xFFFFFFFFU);
    if (rt_row_pitch == 0) rt_row_pitch = fb_width * 4;

    uint8_t* guest_rt_ptr = resolve_guest_va_ptr(rt_color_va, static_cast<size_t>(rt_row_pitch) * fb_height);
    if (!guest_rt_ptr) return;

    /* Match the pending IDVS job for this TCD */
    PendingIDVSJob job = g_vvr.last_idvs_job;
    auto it = g_vvr.pending_idvs_by_tcd.find(fbd_tcd_va);
    if (it != g_vvr.pending_idvs_by_tcd.end()) {
        job = it->second;
    }
    if (!job.valid) return;

    if (!ensure_host_vk_initialized_locked()) return;
    if (!ensure_guest_texture_uploaded_locked(job)) return;
    if (!ensure_gfx_pipeline_locked(job)) return;

    /* Copy live guest uniform/vertex buffer from job.primary_buf_va into host UBO */
    if (job.primary_buf_va != 0 && job.primary_buf_size > 0 && g_vvr.ubo_mapped) {
        uint32_t copy_len = (job.primary_buf_size < g_vvr.ubo_cap) ? job.primary_buf_size : g_vvr.ubo_cap;
        read_guest_va(job.primary_buf_va, g_vvr.ubo_mapped, copy_len);
    }

    /* Create per-frame target color & depth images + readback buffer */
    VkImage color_img = VK_NULL_HANDLE, depth_img = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkBuffer read_buf = VK_NULL_HANDLE;
    VkDeviceMemory read_mem = VK_NULL_HANDLE;

    VkImageCreateInfo cici{};
    cici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cici.imageType = VK_IMAGE_TYPE_2D;
    cici.format = VK_FORMAT_B8G8R8A8_UNORM;
    cici.extent = {fb_width, fb_height, 1};
    cici.mipLevels = 1;
    cici.arrayLayers = 1;
    cici.samples = VK_SAMPLE_COUNT_1_BIT;
    cici.tiling = VK_IMAGE_TILING_OPTIMAL;
    cici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    g_vvr.fp_vkCreateImage(g_vvr.device, &cici, nullptr, &color_img);

    VkMemoryRequirements cmr{};
    g_vvr.fp_vkGetImageMemoryRequirements(g_vvr.device, color_img, &cmr);
    VkMemoryAllocateInfo cmai{};
    cmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    cmai.allocationSize = cmr.size;
    cmai.memoryTypeIndex = find_mem_type(g_vvr.mem_props, cmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    g_vvr.fp_vkAllocateMemory(g_vvr.device, &cmai, nullptr, &color_mem);
    g_vvr.fp_vkBindImageMemory(g_vvr.device, color_img, color_mem, 0);

    VkImageViewCreateInfo civci{};
    civci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    civci.image = color_img;
    civci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    civci.format = VK_FORMAT_B8G8R8A8_UNORM;
    civci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vvr.fp_vkCreateImageView(g_vvr.device, &civci, nullptr, &color_view);

    VkImageCreateInfo dici = cici;
    dici.format = VK_FORMAT_D32_SFLOAT;
    dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    g_vvr.fp_vkCreateImage(g_vvr.device, &dici, nullptr, &depth_img);

    VkMemoryRequirements dmr{};
    g_vvr.fp_vkGetImageMemoryRequirements(g_vvr.device, depth_img, &dmr);
    VkMemoryAllocateInfo dmai{};
    dmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dmai.allocationSize = dmr.size;
    dmai.memoryTypeIndex = find_mem_type(g_vvr.mem_props, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    g_vvr.fp_vkAllocateMemory(g_vvr.device, &dmai, nullptr, &depth_mem);
    g_vvr.fp_vkBindImageMemory(g_vvr.device, depth_img, depth_mem, 0);

    VkImageViewCreateInfo divci{};
    divci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    divci.image = depth_img;
    divci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    divci.format = VK_FORMAT_D32_SFLOAT;
    divci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    g_vvr.fp_vkCreateImageView(g_vvr.device, &divci, nullptr, &depth_view);

    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = g_vvr.gfx_rp;
    fbci.attachmentCount = 2;
    fbci.pAttachments = fb_views;
    fbci.width = fb_width;
    fbci.height = fb_height;
    fbci.layers = 1;
    g_vvr.fp_vkCreateFramebuffer(g_vvr.device, &fbci, nullptr, &fb);

    VkDeviceSize rb_bytes = static_cast<VkDeviceSize>(fb_width) * fb_height * 4;
    VkBufferCreateInfo rbci{};
    rbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    rbci.size = rb_bytes;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    g_vvr.fp_vkCreateBuffer(g_vvr.device, &rbci, nullptr, &read_buf);
    VkMemoryRequirements rbmr{};
    g_vvr.fp_vkGetBufferMemoryRequirements(g_vvr.device, read_buf, &rbmr);
    VkMemoryAllocateInfo rbmai{};
    rbmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    rbmai.allocationSize = rbmr.size;
    rbmai.memoryTypeIndex = find_mem_type(g_vvr.mem_props, rbmr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_vvr.fp_vkAllocateMemory(g_vvr.device, &rbmai, nullptr, &read_mem);
    g_vvr.fp_vkBindBufferMemory(g_vvr.device, read_buf, read_mem, 0);

    /* Allocate and update descriptor set */
    g_vvr.fp_vkResetDescriptorPool(g_vvr.device, g_vvr.desc_pool, 0);
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_vvr.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_vvr.gfx_dsl;
    g_vvr.fp_vkAllocateDescriptorSets(g_vvr.device, &dsai, &ds);

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = g_vvr.ubo_buf;
    dbi.offset = 0;
    dbi.range = (job.primary_buf_size > 0) ? job.primary_buf_size : 64;

    VkDescriptorImageInfo dii{};
    dii.sampler = g_vvr.tex_sampler;
    dii.imageView = g_vvr.tex_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &dbi;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &dii;

    g_vvr.fp_vkUpdateDescriptorSets(g_vvr.device, 2, writes, 0, nullptr);

    /* Decode clear color from RTD+0x30 (fbd_words[30]) */
    uint32_t raw_clear = static_cast<uint32_t>(fbd_words[30] & 0xFFFFFFFFU);
    float clear_r = static_cast<float>((raw_clear >> 16) & 0xFFU) / 255.0f;
    float clear_g = static_cast<float>((raw_clear >> 8)  & 0xFFU) / 255.0f;
    float clear_b = static_cast<float>((raw_clear >> 0)  & 0xFFU) / 255.0f;
    float clear_a = static_cast<float>((raw_clear >> 24) & 0xFFU) / 255.0f;

    g_vvr.fp_vkResetCommandBuffer(g_vvr.cmd_buf, 0);
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vvr.fp_vkBeginCommandBuffer(g_vvr.cmd_buf, &cbbi);

    VkClearValue clear_vals[2]{};
    clear_vals[0].color = {{clear_r, clear_g, clear_b, clear_a}};
    clear_vals[1].depthStencil = {job.depth_clear, 0};

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = g_vvr.gfx_rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = {fb_width, fb_height};
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clear_vals;

    g_vvr.fp_vkCmdBeginRenderPass(g_vvr.cmd_buf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    g_vvr.fp_vkCmdBindPipeline(g_vvr.cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vvr.gfx_pipeline);
    g_vvr.fp_vkCmdBindDescriptorSets(g_vvr.cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      g_vvr.gfx_pipe_layout, 0, 1, &ds, 0, nullptr);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {fb_width, fb_height}};
    g_vvr.fp_vkCmdSetViewport(g_vvr.cmd_buf, 0, 1, &vp);
    g_vvr.fp_vkCmdSetScissor(g_vvr.cmd_buf, 0, 1, &scissor);

    uint32_t vtx_cnt = (job.vertex_count > 0) ? job.vertex_count : 36;
    uint32_t inst_cnt = (job.instance_count > 0) ? job.instance_count : 1;
    g_vvr.fp_vkCmdDraw(g_vvr.cmd_buf, vtx_cnt, inst_cnt, 0, 0);
    g_vvr.fp_vkCmdEndRenderPass(g_vvr.cmd_buf);

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy_region.imageExtent = {fb_width, fb_height, 1};
    g_vvr.fp_vkCmdCopyImageToBuffer(g_vvr.cmd_buf, color_img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    read_buf, 1, &copy_region);

    g_vvr.fp_vkEndCommandBuffer(g_vvr.cmd_buf);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_vvr.cmd_buf;
    g_vvr.fp_vkQueueSubmit(g_vvr.queue, 1, &si, VK_NULL_HANDLE);
    g_vvr.fp_vkQueueWaitIdle(g_vvr.queue);

    /* Copy rendered BGRA8 pixels directly into the guest RTD #0 GPU VA buffer */
    void* rb_ptr = nullptr;
    if (g_vvr.fp_vkMapMemory(g_vvr.device, read_mem, 0, rb_bytes, 0, &rb_ptr) == VK_SUCCESS && rb_ptr) {
        const uint8_t* src_rows = static_cast<const uint8_t*>(rb_ptr);
        for (uint32_t y = 0; y < fb_height; ++y) {
            std::memcpy(guest_rt_ptr + static_cast<size_t>(y) * rt_row_pitch,
                        src_rows + static_cast<size_t>(y) * fb_width * 4,
                        fb_width * 4);
        }
        g_vvr.fp_vkUnmapMemory(g_vvr.device, read_mem);
    }

    uint64_t fnum = g_vvr.frames_rendered.fetch_add(1) + 1;
    g_vvr.last_rt_va.store(rt_color_va);

    const char* log_env = std::getenv("KBASE_EMU_LOG");
    if (log_env && log_env[0] && std::strcmp(log_env, "0") != 0) {
        uint32_t non_black = 0;
        const uint32_t* px = reinterpret_cast<const uint32_t*>(guest_rt_ptr);
        for (uint32_t i = 0; i < fb_width * fb_height; ++i) {
            if ((px[i] & 0x00FFFFFFU) != 0) non_black++;
        }
        std::fprintf(stderr,
                     "[valhall_vk_renderer] frame=%llu RUN_FRAGMENT fbd=0x%llx rt_va=0x%llx (%ux%u stride=%u) "
                     "pos_spd=0x%llx(%u insns) var_spd=0x%llx(%u insns) frag_spd=0x%llx(%u insns) "
                     "ubo=0x%llx(%u B) -> non_black=%u/%u\n",
                     static_cast<unsigned long long>(fnum),
                     static_cast<unsigned long long>(fbd_va),
                     static_cast<unsigned long long>(rt_color_va),
                     fb_width, fb_height, rt_row_pitch,
                     static_cast<unsigned long long>(job.pos_exec_va), job.pos_insn_count,
                     static_cast<unsigned long long>(job.var_exec_va), job.var_insn_count,
                     static_cast<unsigned long long>(job.frag_exec_va), job.frag_insn_count,
                     static_cast<unsigned long long>(job.primary_buf_va), job.primary_buf_size,
                     non_black, fb_width * fb_height);
    }

    g_vvr.fp_vkDestroyBuffer(g_vvr.device, read_buf, nullptr);
    g_vvr.fp_vkFreeMemory(g_vvr.device, read_mem, nullptr);
    g_vvr.fp_vkDestroyFramebuffer(g_vvr.device, fb, nullptr);
    g_vvr.fp_vkDestroyImageView(g_vvr.device, depth_view, nullptr);
    g_vvr.fp_vkDestroyImage(g_vvr.device, depth_img, nullptr);
    g_vvr.fp_vkFreeMemory(g_vvr.device, depth_mem, nullptr);
    g_vvr.fp_vkDestroyImageView(g_vvr.device, color_view, nullptr);
    g_vvr.fp_vkDestroyImage(g_vvr.device, color_img, nullptr);
    g_vvr.fp_vkFreeMemory(g_vvr.device, color_mem, nullptr);
}

/*
 * Parse & Execute RUN_COMPUTE (opcode 0x04 / 0x27) and RUN_COMPUTE_INDIRECT (opcode 0x2c):
 *
 *   1. Decodes Compute Shader Program Descriptor (SPD) from CS registers (r16:r17, r20:r21,
 *      or SPD selector in instruction bits [43:40]) and verifies hardware SPD header (type=0x08)
 *      and `exec_va` in the 32-bit EXEC_VA shader zone `[0x10000000, 0x70000000)`.
 *   2. Decodes Compute Shader Resource Table (SRT) from `r0:r1` (or `r2:r3`, `r4:r5`),
 *      extracting all 16-byte Valhall Buffer Descriptors (`type=0x01`, `length=word0[39:8]`,
 *      `gpu_va=word1`) as well as raw 64-bit GPU VA pointer tables.
 *   3. Decodes Fast Access Uniform (FAU) table from `r8:r9` (`fau_va = r8:r9 & 0x00FFFFFFFFFFFFFF`,
 *      `fau_count = r8:r9 >> 56`) which holds push constants, workgroup parameters, or 64-bit
 *      buffer device addresses pushed by the compute shader compiler.
 *   4. Decodes Thread Storage Descriptor (TSD) from `r24:r25` and workgroup grid dimensions
 *      (`wg_x, wg_y, wg_z`) from `r32..r39`.
 *   5. Disassembles and lifts the Valhall 64-bit compute ISA stream at `spd.exec_va` into a
 *      SPIR-V 1.3 `GLCompute` shader module via `ValhallISAToSpirvTranslator::translateObjcToSpirv`,
 *      also decoding Valhall `LD_BUFFER` / `ST_BUFFER` / `FMA.f32` / `FADD.f32` / `FMUL.f32`
 *      dataflow across the bound guest SSBO/UBO buffers.
 *   6. Creates a `VkShaderModule` + `VkComputePipeline` on the self-contained host Lavapipe
 *      `VkDevice`, binds the guest storage buffers, records and submits `vkCmdDispatch(wg_x, wg_y, wg_z)`,
 *      and writes the computed output buffer(s) directly back into guest `/dev/mali0` GPU VA memory.
 */
static void handle_csf_run_compute(uint8_t cs_opcode, uint64_t insn, const uint64_t* cs_regs) {
    std::lock_guard<std::mutex> lk(g_vvr.mtx);

    DecodedComputeJob cjob{};
    cjob.opcode = cs_opcode;
    cjob.insn   = insn;

    /* 1. Locate hardware Compute SPD (type=0x08, exec_va in [0x10000000, 0x70000000)) */
    const uint32_t candidate_spd_regs[] = {16, 20, 18, 22, 8, 2, 0};
    for (uint32_t reg_lo : candidate_spd_regs) {
        uint64_t cand_va = read_cs_reg64(cs_regs, reg_lo) & ~0xFULL;
        if (cand_va >= 0x10000ULL) {
            ValhallSPD spd{};
            if (read_guest_va(cand_va, &spd, sizeof(spd))) {
                if ((spd.header & 0xFFULL) == 0x08ULL &&
                    spd.exec_va >= 0x10000000ULL && spd.exec_va < 0x70000000ULL) {
                    cjob.spd_va = cand_va;
                    cjob.spd_header = spd.header;
                    cjob.exec_va = spd.exec_va;
                    break;
                }
            }
        }
    }
    /* Fallback: scan all even register pairs r00..r62 for a valid hardware Compute SPD */
    if (cjob.exec_va == 0) {
        for (uint32_t reg_lo = 0; reg_lo < 64; reg_lo += 2) {
            uint64_t cand_va = read_cs_reg64(cs_regs, reg_lo) & ~0xFULL;
            if (cand_va >= 0x10000ULL) {
                ValhallSPD spd{};
                if (read_guest_va(cand_va, &spd, sizeof(spd))) {
                    if ((spd.header & 0xFFULL) == 0x08ULL &&
                        spd.exec_va >= 0x10000000ULL && spd.exec_va < 0x70000000ULL) {
                        cjob.spd_va = cand_va;
                        cjob.spd_header = spd.header;
                        cjob.exec_va = spd.exec_va;
                        break;
                    }
                }
            }
        }
    }
    if (cjob.exec_va == 0) {
        /* Not a native Valhall hardware SPD (e.g., synthetic MALI_ICD_NATIVE_AOT=0 compute test) */
        return;
    }

    /* 2. Decode FAU (Fast Access Uniforms) from r8:r9 (or r10:r11) */
    uint64_t fau_raw = read_cs_reg64(cs_regs, 8);
    cjob.fau_va    = fau_raw & 0x00FFFFFFFFFFFFFFULL;
    cjob.fau_count = static_cast<uint32_t>((fau_raw >> 56) & 0xFFU);
    if (cjob.fau_va >= 0x10000ULL && cjob.fau_count > 0 && cjob.fau_count <= 64) {
        cjob.fau_words.resize(cjob.fau_count, 0);
        read_guest_va(cjob.fau_va, cjob.fau_words.data(), cjob.fau_count * sizeof(uint64_t));
    }

    /* 3. Decode Thread Storage Descriptor (TSD) from r24:r25 */
    cjob.tsd_va = read_cs_reg64(cs_regs, 24) & ~0x3FULL;
    if (cjob.tsd_va >= 0x10000ULL) {
        uint64_t tsd_words[4]{};
        if (read_guest_va(cjob.tsd_va, tsd_words, sizeof(tsd_words))) {
            cjob.tls_base_va = tsd_words[1];
        }
    }

    /* 4. Decode Workgroup Grid Dimensions (wg_x, wg_y, wg_z) from CSF registers */
    uint32_t r32 = read_cs_reg32(cs_regs, 32);
    uint32_t r33 = read_cs_reg32(cs_regs, 33);
    uint32_t r34 = read_cs_reg32(cs_regs, 34);
    uint32_t r35 = read_cs_reg32(cs_regs, 35);
    uint32_t r36 = read_cs_reg32(cs_regs, 36);
    uint32_t r37 = read_cs_reg32(cs_regs, 37);
    uint32_t r38 = read_cs_reg32(cs_regs, 38);
    uint32_t r39 = read_cs_reg32(cs_regs, 39);

    /* Support RUN_COMPUTE_INDIRECT (0x2c) where r32:r33 or r36:r37 holds the GPU VA of VkDispatchIndirectCommand {x, y, z} */
    if (cs_opcode == CSF_OP_RUN_COMPUTE_INDIRECT_HW) {
        for (uint32_t ireg : {32U, 36U, 28U}) {
            uint64_t ind_va = read_cs_reg64(cs_regs, ireg);
            uint32_t ind_wg[3]{};
            if (ind_va >= 0x10000ULL && read_guest_va(ind_va, ind_wg, sizeof(ind_wg))) {
                if (ind_wg[0] > 0 && ind_wg[0] <= 65535 &&
                    ind_wg[1] > 0 && ind_wg[1] <= 65535 &&
                    ind_wg[2] > 0 && ind_wg[2] <= 65535) {
                    cjob.wg_x = ind_wg[0];
                    cjob.wg_y = ind_wg[1];
                    cjob.wg_z = ind_wg[2];
                    break;
                }
            }
        }
    }
    if (cjob.wg_x == 1 && cjob.wg_y == 1 && cjob.wg_z == 1 &&
        r33 > 0 && r33 <= 65535 && r34 <= 65535 && r35 <= 65535) {
        cjob.wg_x = r33;
        cjob.wg_y = (r34 > 0) ? r34 : 1;
        cjob.wg_z = (r35 > 0) ? r35 : 1;
    } else if (r36 > 0 && r36 <= 65535) {
        cjob.wg_x = r36;
        cjob.wg_y = (r37 > 0 && r37 <= 65535) ? r37 : 1;
        cjob.wg_z = (r38 > 0 && r38 <= 65535) ? r38 : 1;
    } else if (r32 > 0 && r32 <= 65535) {
        cjob.wg_x = r32;
        cjob.wg_y = 1;
        cjob.wg_z = 1;
    }
    (void)r39;

    /* Helper lambda to normalize Valhall v9 (0x01), v10/v11 (0x19), or LD_PKA (0x0100...) buffer descriptors */
    auto try_push_buffer_desc = [&](uint64_t w0, uint64_t w1, uint64_t srt_origin) -> bool {
        uint8_t dtype = static_cast<uint8_t>(w0 & 0xFFU);
        if (dtype == 0x01U || dtype == 0x19U) {
            uint32_t blen = (dtype == 0x19U)
                ? static_cast<uint32_t>(w0 >> 32)
                : static_cast<uint32_t>((w0 >> 8) & 0xFFFFFFFFU);
            if (w1 >= 0x10000ULL && blen > 0 && blen <= (64U << 20) && resolve_guest_va_ptr(w1, blen)) {
                ValhallBufferDesc norm{(static_cast<uint64_t>(blen) << 8) | 0x01ULL, w1};
                if (cjob.srt_va == 0) cjob.srt_va = srt_origin;
                cjob.buffers.push_back(norm);
                return true;
            }
        }
        uint8_t top_tag = static_cast<uint8_t>((w0 >> 56) & 0xFFU);
        uint64_t pka_va = w0 & 0x00FFFFFFFFFFFFFFULL;
        if (top_tag >= 0x01U && top_tag <= 0x08U && pka_va >= 0x10000ULL && resolve_guest_va_ptr(pka_va, 16)) {
            uint32_t blen = (w1 >= 16 && w1 <= (64U << 20) && resolve_guest_va_ptr(pka_va, static_cast<size_t>(w1)))
                ? static_cast<uint32_t>(w1) : 64U;
            ValhallBufferDesc norm{(static_cast<uint64_t>(blen) << 8) | 0x01ULL, pka_va};
            if (cjob.srt_va == 0) cjob.srt_va = srt_origin;
            cjob.buffers.push_back(norm);
            return true;
        }
        return false;
    };

    /* 5. Decode Compute SRT (Shader Resource Table) from r0:r1, r2:r3, or r4:r5 */
    const uint32_t candidate_srt_regs[] = {0, 2, 4};
    for (uint32_t sreg : candidate_srt_regs) {
        uint64_t sva = read_cs_reg64(cs_regs, sreg) & ~0x7ULL;
        if (sva >= 0x10000ULL && sva != cjob.spd_va) {
            /* Check indirect table header at sva - 8 first (halp::resource_table layout) */
            if (sva >= 0x10008ULL) {
                uint64_t tbl_hdr[2]{};
                if (read_guest_va(sva - 8ULL, tbl_hdr, sizeof(tbl_hdr))) {
                    uint64_t tbl_va  = tbl_hdr[0];
                    uint64_t tbl_len = tbl_hdr[1];
                    if (tbl_va >= 0x10000ULL && tbl_len >= 16 && tbl_len <= 4096) {
                        size_t cnt = static_cast<size_t>(tbl_len / 16);
                        if (cnt > 16) cnt = 16;
                        std::vector<ValhallBufferDesc> tdescs(cnt);
                        if (read_guest_va(tbl_va, tdescs.data(), cnt * sizeof(ValhallBufferDesc))) {
                            for (size_t i = 0; i < cnt; ++i) {
                                try_push_buffer_desc(tdescs[i].word0, tdescs[i].gpu_va, tbl_va);
                            }
                        }
                    }
                }
            }
            ValhallBufferDesc raw_descs[8]{};
            if (read_guest_va(sva, raw_descs, sizeof(raw_descs))) {
                for (int i = 0; i < 8; ++i) {
                    if (try_push_buffer_desc(raw_descs[i].word0, raw_descs[i].gpu_va, sva)) {
                        continue;
                    }
                    if (raw_descs[i].word0 >= 0x100000000ULL &&
                        resolve_guest_va_ptr(raw_descs[i].word0, 16)) {
                        /* Direct 64-bit buffer address entry or nested descriptor table */
                        ValhallBufferDesc sub_descs[4]{};
                        if (read_guest_va(raw_descs[i].word0, sub_descs, sizeof(sub_descs))) {
                            bool found_sub = false;
                            for (int j = 0; j < 4; ++j) {
                                if (try_push_buffer_desc(sub_descs[j].word0, sub_descs[j].gpu_va, raw_descs[i].word0)) {
                                    found_sub = true;
                                }
                            }
                            if (!found_sub) {
                                ValhallBufferDesc synth{(64ULL << 8) | 0x01ULL, raw_descs[i].word0};
                                cjob.buffers.push_back(synth);
                            }
                        }
                    }
                }
            }
            if (!cjob.buffers.empty()) break;
        }
    }

    /* Also check FAU words for embedded 16-byte Buffer Descriptors, LD_PKA pointers, or 64-bit SSBO addresses */
    if (cjob.buffers.empty() && !cjob.fau_words.empty()) {
        for (size_t i = 0; i + 1 < cjob.fau_words.size(); i += 2) {
            try_push_buffer_desc(cjob.fau_words[i], cjob.fau_words[i + 1], cjob.fau_va);
        }
        for (size_t i = 0; i < cjob.fau_words.size(); ++i) {
            uint64_t w = cjob.fau_words[i];
            if (try_push_buffer_desc(w, 64, cjob.fau_va)) continue;
            if (w >= 0x100000000ULL && (w & 0x3ULL) == 0 && resolve_guest_va_ptr(w, 16)) {
                ValhallBufferDesc sub[4]{};
                bool added_sub = false;
                if (read_guest_va(w, sub, sizeof(sub))) {
                    for (int j = 0; j < 4; ++j) {
                        if (try_push_buffer_desc(sub[j].word0, sub[j].gpu_va, w)) {
                            added_sub = true;
                        }
                    }
                }
                if (!added_sub) {
                    ValhallBufferDesc bd{(64ULL << 8) | 0x01ULL, w};
                    cjob.buffers.push_back(bd);
                }
            }
        }
    }

    /* 6. Disassemble & Lift Valhall Compute ISA at cjob.exec_va to SPIR-V 1.3 */
    extract_valhall_isa_words(cjob.exec_va, cjob.isa_words, &cjob.disasm);
    if (cjob.isa_words.empty()) return;

    cjob.spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(
        cjob.isa_words.data(), cjob.isa_words.size(), nullptr);
    if (cjob.spirv.empty()) return;

    cjob.valid = true;

    /* 7. Execute lifted Compute SPIR-V on self-contained Host Vulkan Device (Lavapipe) */
    if (!ensure_host_vk_initialized_locked()) return;

    VkShaderModule cs_mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = cjob.spirv.size() * sizeof(uint32_t);
    smci.pCode = cjob.spirv.data();
    if (g_vvr.fp_vkCreateShaderModule(g_vvr.device, &smci, nullptr, &cs_mod) != VK_SUCCESS) {
        return;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    g_vvr.fp_vkCreateDescriptorSetLayout(g_vvr.device, &dslci, nullptr, &dsl);

    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    g_vvr.fp_vkCreatePipelineLayout(g_vvr.device, &plci, nullptr, &pipe_layout);

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = cs_mod;
    cpci.stage.pName = "main";
    cpci.layout = pipe_layout;

    VkPipeline comp_pipe = VK_NULL_HANDLE;
    VkResult pres = g_vvr.fp_vkCreateComputePipelines(g_vvr.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &comp_pipe);

    if (pres == VK_SUCCESS && comp_pipe != VK_NULL_HANDLE) {
        /* Stage guest SSBOs into host-visible compute buffers */
        uint64_t in_va = 0, out_va = 0;
        uint32_t in_len = 64, out_len = 64;
        if (!cjob.buffers.empty()) {
            in_va  = cjob.buffers[0].gpu_va;
            in_len = static_cast<uint32_t>((cjob.buffers[0].word0 >> 8) & 0xFFFFFFFFU);
            if (in_len == 0 || in_len > g_vvr.comp_buf_cap) in_len = 64;
            if (cjob.buffers.size() >= 2) {
                out_va  = cjob.buffers[1].gpu_va;
                out_len = static_cast<uint32_t>((cjob.buffers[1].word0 >> 8) & 0xFFFFFFFFU);
                if (out_len == 0 || out_len > g_vvr.comp_buf_cap) out_len = in_len;
            } else {
                out_va  = in_va;
                out_len = in_len;
            }
        }

        if (in_va && g_vvr.comp_mapped0) {
            read_guest_va(in_va, g_vvr.comp_mapped0, in_len);
        }
        if (out_va && g_vvr.comp_mapped1) {
            read_guest_va(out_va, g_vvr.comp_mapped1, out_len);
        }

        g_vvr.fp_vkResetDescriptorPool(g_vvr.device, g_vvr.desc_pool, 0);
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_vvr.desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        g_vvr.fp_vkAllocateDescriptorSets(g_vvr.device, &dsai, &ds);

        VkDescriptorBufferInfo dbi0{g_vvr.comp_buf0, 0, in_len};
        VkDescriptorBufferInfo dbi1{g_vvr.comp_buf1, 0, out_len};

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = ds;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &dbi0;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = ds;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &dbi1;
        g_vvr.fp_vkUpdateDescriptorSets(g_vvr.device, 2, writes, 0, nullptr);

        g_vvr.fp_vkResetCommandBuffer(g_vvr.cmd_buf, 0);
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_vvr.fp_vkBeginCommandBuffer(g_vvr.cmd_buf, &cbbi);
        g_vvr.fp_vkCmdBindPipeline(g_vvr.cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe);
        g_vvr.fp_vkCmdBindDescriptorSets(g_vvr.cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                          pipe_layout, 0, 1, &ds, 0, nullptr);
        g_vvr.fp_vkCmdDispatch(g_vvr.cmd_buf, cjob.wg_x, cjob.wg_y, cjob.wg_z);
        g_vvr.fp_vkEndCommandBuffer(g_vvr.cmd_buf);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_vvr.cmd_buf;
        g_vvr.fp_vkQueueSubmit(g_vvr.queue, 1, &si, VK_NULL_HANDLE);
        g_vvr.fp_vkQueueWaitIdle(g_vvr.queue);

        /* Write back computed SSBO output into guest GPU VA memory */
        if (out_va && g_vvr.comp_mapped1) {
            uint8_t* guest_out = resolve_guest_va_ptr(out_va, out_len);
            if (guest_out) {
                std::memcpy(guest_out, g_vvr.comp_mapped1, out_len);
            }
        }

        uint64_t cnum = g_vvr.compute_jobs_executed.fetch_add(1) + 1;
        const char* log_env = std::getenv("KBASE_EMU_LOG");
        if (log_env && log_env[0] && std::strcmp(log_env, "0") != 0) {
            std::fprintf(stderr,
                         "[valhall_vk_renderer] compute=%llu RUN_COMPUTE spd=0x%llx exec_va=0x%llx(%zu insns, spv=%zu words) "
                         "srt=0x%llx(bufs=%zu) fau=0x%llx(cnt=%u) wg=(%u,%u,%u) in=0x%llx out=0x%llx\n",
                         static_cast<unsigned long long>(cnum),
                         static_cast<unsigned long long>(cjob.spd_va),
                         static_cast<unsigned long long>(cjob.exec_va),
                         cjob.isa_words.size(),
                         cjob.spirv.size(),
                         static_cast<unsigned long long>(cjob.srt_va),
                         cjob.buffers.size(),
                         static_cast<unsigned long long>(cjob.fau_va),
                         cjob.fau_count,
                         cjob.wg_x, cjob.wg_y, cjob.wg_z,
                         static_cast<unsigned long long>(in_va),
                         static_cast<unsigned long long>(out_va));
        }

        g_vvr.fp_vkDestroyPipeline(g_vvr.device, comp_pipe, nullptr);
    }

    g_vvr.fp_vkDestroyPipelineLayout(g_vvr.device, pipe_layout, nullptr);
    g_vvr.fp_vkDestroyDescriptorSetLayout(g_vvr.device, dsl, nullptr);
    g_vvr.fp_vkDestroyShaderModule(g_vvr.device, cs_mod, nullptr);
}

/* MALI_EMU_HEADLESS=1 explicitly disables the CSF-hooked Vulkan renderer. */
static bool renderer_disabled() {
    const char* env = std::getenv("MALI_EMU_HEADLESS");
    return env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
}

} // namespace

extern "C" {

void valhall_vk_renderer_init(void) {
    if (renderer_disabled()) return;
    std::lock_guard<std::mutex> lk(g_vvr.mtx);
    ensure_host_vk_initialized_locked();
}

void valhall_vk_renderer_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_vvr.mtx);
}

void valhall_vk_csf_job_hook(uint8_t cs_opcode,
                             uint64_t insn,
                             const uint64_t* cs_regs,
                             void* /*user_data*/) {
    if (!cs_regs) return;
    if (renderer_disabled()) return;
    switch (cs_opcode) {
        case CSF_OP_RUN_IDVS_SHORT:
        case CSF_OP_RUN_IDVS_EXT:
        case CSF_OP_RUN_TILING_SHORT:
        case CSF_OP_RUN_TILING_EXT:
        case CSF_OP_RUN_VERTEX_EXT:
            handle_csf_run_idvs(insn, cs_regs);
            break;
        case CSF_OP_RUN_FRAGMENT_SHORT:
        case CSF_OP_RUN_FRAGMENT_EXT:
        case CSF_OP_RUN_FULLSCREEN_SHORT:
        case CSF_OP_RUN_FULLSCREEN_EXT:
            handle_csf_run_fragment(insn, cs_regs);
            break;
        case CSF_OP_RUN_COMPUTE_SHORT:
        case CSF_OP_RUN_COMPUTE_EXT:
        case CSF_OP_RUN_COMPUTE_INDIRECT_HW:
            handle_csf_run_compute(cs_opcode, insn, cs_regs);
            break;
        default:
            break;
    }
}

uint64_t valhall_vk_renderer_get_frame_count(void) {
    return g_vvr.frames_rendered.load();
}

uint64_t valhall_vk_renderer_get_compute_count(void) {
    return g_vvr.compute_jobs_executed.load();
}

uint64_t valhall_vk_renderer_get_last_render_target_va(void) {
    return g_vvr.last_rt_va.load();
}

} // extern "C"
