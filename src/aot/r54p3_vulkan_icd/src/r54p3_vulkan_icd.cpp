// ============================================================================
// r54p3_vulkan_icd.cpp
// Decompiled C++17 Implementation of the r54p3-00eac0-6eda07 Vulkan 1.3 ICD
// (`vulkan::*` RVA 0x00936bc0..0x00991000, `gfx::*` RVA 0x01adf000..0x01b87000,
//  `kbase::*` RVA 0x01c69c00..0x01c6c200, stopping at the shader compiler
//  boundary `cmpbe_v2_compile_multiple_shaders` at RVA 0x01c9ecb0).
// ============================================================================
#include "../include/r54p3_icd_types.hpp"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <cstdio>

namespace mali_r54p3 {

static MaliKbaseCsfKernelEmulator g_r54p3_kbase_emu;

namespace osu {

static inline uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

// RVA 0x008bfaa0: Decompiled SHA-1 digest used by vkCreateShaderModule (0x0098e4e0)
void computeSpirvSha1(const uint32_t* words, size_t byte_len, uint8_t out_sha1[20]) {
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(words);
    std::vector<uint8_t> msg(data, data + byte_len);
    msg.push_back(0x80U);
    while ((msg.size() % 64) != 56) msg.push_back(0x00U);
    uint64_t bit_len = static_cast<uint64_t>(byte_len) * 8ULL;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFU));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[chunk + i * 4 + 0]) << 24) |
                   (uint32_t(msg[chunk + i * 4 + 1]) << 16) |
                   (uint32_t(msg[chunk + i * 4 + 2]) << 8)  |
                   (uint32_t(msg[chunk + i * 4 + 3]) << 0);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999U; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1U; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6U; }
            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out_sha1[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFU);
        out_sha1[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFU);
        out_sha1[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8)  & 0xFFU);
        out_sha1[i * 4 + 3] = static_cast<uint8_t>((hs[i] >> 0)  & 0xFFU);
    }
}

} // namespace osu

namespace kbase {

// RVA 0x01c68e10: kbase_open("/dev/mali0")
int32_t openDevice(Context& kctx) {
    kctx.fd = static_cast<int32_t>(g_r54p3_kbase_emu.handleOpen("/dev/mali0", 2));
    return (kctx.fd >= 0) ? 0 : -1;
}

// RVA 0x01c6a7b0: kbase_version_check_csf (ioctl 0xc0048034)
int32_t versionCheckCsf(Context& kctx) {
    KbaseIoctlVersionCheck ver{1, 36};
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0xc0048034U, reinterpret_cast<uint64_t>(&ver));
    kctx.uapi_major = ver.major;
    kctx.uapi_minor = ver.minor;
    return 0;
}

// RVA 0x01c6a810: kbase_set_flags (ioctl 0x40048001 + mmap tracking & user_reg pages)
int32_t setFlagsAndMapSpecialPages(Context& kctx, uint32_t flags) {
    KbaseIoctlSetFlags sf{flags};
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0x40048001U, reinterpret_cast<uint64_t>(&sf));
    kctx.tracking_page = reinterpret_cast<void*>(
        g_r54p3_kbase_emu.handleMmap(0, 4096, 1, 1, kctx.fd, BASE_MEM_MAP_TRACKING_HANDLE));
    kctx.user_reg_page = reinterpret_cast<void*>(
        g_r54p3_kbase_emu.handleMmap(0, 4096, 1, 1, kctx.fd, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE));
    return 0;
}

// RVA 0x01c6a870 & 0x01c6a8d0: kbase_mem_jit_init & kbase_mem_exec_init
int32_t initMemoryZones(Context& kctx) {
    uint64_t jit_va_pages = 1ULL << 25;
    uint64_t exec_va_pages = 0x100000ULL;
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0x4018800eU, reinterpret_cast<uint64_t>(&jit_va_pages));
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0x40088026U, reinterpret_cast<uint64_t>(&exec_va_pages));
    return 0;
}

// RVA 0x01c69c00: kbase_mem_alloc (KBASE_IOCTL_MEM_ALLOC_EX = 0xc040803b + SAME_VA mmap)
uint64_t memAllocEx(Context& kctx, uint64_t va_pages, uint64_t flags) {
    uint64_t req[8]{};
    req[0] = std::max<uint64_t>(1ULL, va_pages);
    req[1] = req[0];
    req[3] = flags;
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0xc040803bU, reinterpret_cast<uint64_t>(req));
    uint64_t cookie_or_va = req[1];
    if ((flags & BASE_MEM_SAME_VA) != 0 && (flags & BASE_MEM_PROT_GPU_EX) == 0) {
        return g_r54p3_kbase_emu.handleMmap(0, req[0] * 4096ULL, 3, 1, kctx.fd, cookie_or_va);
    }
    return cookie_or_va;
}

// RVA 0x01c69e70: kbase_mem_free (ioctl 0x40088007)
void memFree(Context& kctx, uint64_t gpu_va) {
    if (gpu_va) {
        g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0x40088007U, reinterpret_cast<uint64_t>(&gpu_va));
    }
}

// RVA 0x01c6afd0: kbase_cs_tiler_heap_init (ioctl 0xc0188030)
void initTilerHeap(Context& kctx) {
    KbaseIoctlCsTilerHeapInit th{};
    th.in.chunk_size = 4096;
    th.in.initial_chunks = 4;
    th.in.max_chunks = 64;
    th.in.target_in_flight = 4;
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0xc0188030U, reinterpret_cast<uint64_t>(&th));
    kctx.tiler_heap_va = th.out.gpu_heap_va;
}

// RVA 0x01c6b300, 0x01c6aab0, 0x01c6ae50: CSG create + Queue register + Queue bind
void initAndBindCsfQueue(Context& kctx, CsiQueue& q) {
    q.kctx = &kctx;
    uint64_t rw_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
    q.ring_bo_gpu_va = memAllocEx(kctx, 1, rw_flags);

    KbaseIoctlCsQueueGroupCreate csg{};
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0xc078803fU, reinterpret_cast<uint64_t>(&csg));
    q.group_handle = csg.out.group_handle;

    KbaseIoctlCsQueueRegister qreg{q.ring_bo_gpu_va, q.ring_size, 8, {0, 0, 0}};
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0x40108024U, reinterpret_cast<uint64_t>(&qreg));

    KbaseIoctlCsQueueBind qbind{};
    qbind.in.buffer_gpu_addr = q.ring_bo_gpu_va;
    qbind.in.group_handle    = q.group_handle;
    qbind.in.csi_index       = q.csi_index;
    g_r54p3_kbase_emu.handleIoctl(kctx.fd, 0xc0108027U, reinterpret_cast<uint64_t>(&qbind));

    uint64_t uio = g_r54p3_kbase_emu.handleMmap(0, 3 * 4096ULL, 3, 1, kctx.fd, qbind.out.mmap_handle);
    q.doorbell_va    = uio;
    q.user_input_va  = uio + 4096ULL;
    q.user_output_va = uio + 8192ULL;
}

// RVA 0x01c6adf0 & 0x01c72520: Ring CSF doorbell & kick queue (ioctl 0x40088025)
void kickCsfQueue(CsiQueue& q) {
    *reinterpret_cast<uint64_t*>(q.user_input_va + CS_INSERT_LO) = q.write_offset;
    *reinterpret_cast<uint32_t*>(q.doorbell_va) = 1U;
    KbaseIoctlCsQueueKick kick{q.ring_bo_gpu_va};
    g_r54p3_kbase_emu.handleIoctl(q.kctx->fd, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
}

} // namespace kbase

namespace gfx {

// Decompiled from RVA 0x01adf340 (gfx::shader::compile_single)
bool compileSingleComputeStage(vulkan::Device* dev,
                               const vulkan::ShaderModule* sm,
                               cmpbe::CmpbeV2CompilationResult** out_res,
                               uint64_t* out_exec_bo_va) {
    if (!dev || !sm || !out_res || !out_exec_bo_va) return false;

    cmpbe::CmpbeV2Options opts{};
    cmpbe::cmpbe_v2_init_options(&opts);
    cmpbe::cmpbe_v2_set_option_value(&opts, cmpbe::CMPBE_OPT_SHADER_STAGE_MASK, 0x20U);
    cmpbe::cmpbe_v2_set_option_value(&opts, cmpbe::CMPBE_OPT_COMPUTE_LOCAL_SIZE, 64U);

    cmpbe::CmpbeV2ShaderInput input{};
    input.spirv_words     = sm->spirv_words.data();
    input.spirv_byte_size = sm->spirv_words.size() * sizeof(uint32_t);
    input.stage_flags     = 0x20U;
    std::memcpy(input.sha1_digest, sm->sha1_digest, 20);

    cmpbe::CmpbeV2PipelineLinkInfo link{};
    cmpbe::CmpbeV2ErrorLog* err_log = nullptr;
    cmpbe::CmpbeV2CompilationResult* res = nullptr;

    auto rc = cmpbe::cmpbe_v2_compile_single_shader(
        &dev->cmpbe_ctx, &opts, &input, &link, 0x20U, 1U, 4U, &err_log, &res);
    if (rc != cmpbe::CMPBE_SUCCESS || !res) return false;

    uint64_t exec_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX;
    uint64_t exec_va = kbase::memAllocEx(dev->kctx, 1, exec_flags);
    std::memcpy(reinterpret_cast<void*>(exec_va), res->objc_words.data(), res->objc_words.size() * sizeof(uint64_t));

    *out_res        = res;
    *out_exec_bo_va = exec_va;
    return true;
}

// Decompiled from RVA 0x01ae1100 (gfx::pipeline::compile_multiple_shaders)
bool compileGraphicsStages(vulkan::Device* dev,
                           const vulkan::ShaderModule* vs_mod,
                           const vulkan::ShaderModule* fs_mod,
                           cmpbe::CmpbeV2CompilationResult** out_vs_res,
                           cmpbe::CmpbeV2CompilationResult** out_fs_res,
                           uint64_t* out_vs_exec_va,
                           uint64_t* out_fs_exec_va) {
    cmpbe::CmpbeV2Options vs_opts{}, fs_opts{};
    cmpbe::cmpbe_v2_init_options(&vs_opts);
    cmpbe::cmpbe_v2_init_options(&fs_opts);
    for (auto* o : {&vs_opts, &fs_opts}) {
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_OPTIMIZATION_LEVEL, 2U);
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_ROBUST_BUFFER_ACCESS, 1U);
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_ROBUST_IMAGE_ACCESS, 1U);
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_FLUSH_DENORMS, 1U);
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_VARYING_LINK_MODE, 1U);
        cmpbe::cmpbe_v2_set_option_value(o, cmpbe::CMPBE_OPT_VALHALL_IDVS_MODE, 1U);
    }

    cmpbe::CmpbeV2ShaderInput vs_in{}, fs_in{};
    if (vs_mod) {
        vs_in.spirv_words     = vs_mod->spirv_words.data();
        vs_in.spirv_byte_size = vs_mod->spirv_words.size() * sizeof(uint32_t);
        std::memcpy(vs_in.sha1_digest, vs_mod->sha1_digest, 20);
    }
    vs_in.stage_flags = 0x01U;

    if (fs_mod) {
        fs_in.spirv_words     = fs_mod->spirv_words.data();
        fs_in.spirv_byte_size = fs_mod->spirv_words.size() * sizeof(uint32_t);
        std::memcpy(fs_in.sha1_digest, fs_mod->sha1_digest, 20);
    }
    fs_in.stage_flags = 0x10U;

    const cmpbe::CmpbeV2Options*     opts_arr[2]   = { &vs_opts, &fs_opts };
    const cmpbe::CmpbeV2ShaderInput* inputs_arr[2] = { &vs_in, &fs_in };
    uint32_t                         stages_arr[2] = { 0x01U, 0x10U };
    cmpbe::CmpbeV2PipelineLinkInfo   link{};
    cmpbe::CmpbeV2ErrorLog*          err_logs[2]   = { nullptr, nullptr };
    cmpbe::CmpbeV2CompilationResult* results[2]    = { nullptr, nullptr };

    auto rc = cmpbe::cmpbe_v2_compile_multiple_shaders(
        &dev->cmpbe_ctx, 2U, opts_arr, inputs_arr, &link, stages_arr, 1U, 1U, err_logs, results);
    if (rc != cmpbe::CMPBE_SUCCESS) return false;

    uint64_t exec_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX;
    *out_vs_res = results[0];
    *out_fs_res = results[1];
    *out_vs_exec_va = kbase::memAllocEx(dev->kctx, 1, exec_flags);
    *out_fs_exec_va = kbase::memAllocEx(dev->kctx, 1, exec_flags);
    std::memcpy(reinterpret_cast<void*>(*out_vs_exec_va), results[0]->objc_words.data(), results[0]->objc_words.size() * 8);
    std::memcpy(reinterpret_cast<void*>(*out_fs_exec_va), results[1]->objc_words.data(), results[1]->objc_words.size() * 8);
    return true;
}

void populateStubTextureWithMipmaps(vulkan::Image* img) {
    if (!img) return;
    uint32_t w = std::max<uint32_t>(1U, img->width);
    uint32_t h = std::max<uint32_t>(1U, img->height);
    size_t total_px = static_cast<size_t>(w) * static_cast<size_t>(h);
    img->stub_texture_backing.resize(total_px * 4);
    uint32_t* rgba = reinterpret_cast<uint32_t*>(img->stub_texture_backing.data());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            bool checker = (((x >> 3) ^ (y >> 3)) & 1U) != 0;
            uint32_t r = checker ? 220U : 45U;
            uint32_t g = static_cast<uint32_t>((x * 255U) / std::max<uint32_t>(1U, w));
            uint32_t b = static_cast<uint32_t>((y * 255U) / std::max<uint32_t>(1U, h));
            rgba[y * w + x] = (0xFFU << 24) | (b << 16) | (g << 8) | r;
        }
    }
    img->byte_size = img->stub_texture_backing.size();
}

} // namespace gfx
} // namespace mali_r54p3

using namespace mali_r54p3;

extern "C" {

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    if (!pApiVersion) return -3;
    g_r54p3_kbase_emu.lvp_executor.init();
    *pApiVersion = (1U << 22) | (3U << 12) | 250U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* /*pProperties*/) {
    if (!pPropertyCount) return -3;
    *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateInstanceExtensionProperties(const char* /*pLayerName*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    g_r54p3_kbase_emu.lvp_executor.init();
    static const char* kInstExts[] = {
        "VK_KHR_surface", "VK_EXT_headless_surface", "VK_KHR_display", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_EXT_debug_utils"
    };
    constexpr uint32_t kCount = sizeof(kInstExts) / sizeof(kInstExts[0]);
    if (!pProperties) { *pPropertyCount = kCount; return 0; }
    struct ExtProp { char name[256]; uint32_t ver; };
    auto* out = static_cast<ExtProp*>(pProperties);
    uint32_t n = std::min(*pPropertyCount, kCount);
    for (uint32_t i = 0; i < n; ++i) {
        std::memset(&out[i], 0, sizeof(ExtProp));
        std::snprintf(out[i].name, sizeof(out[i].name), "%s", kInstExts[i]);
        out[i].ver = 1U;
    }
    *pPropertyCount = n;
    return (n < kCount) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateInstance(const void* /*pCreateInfo*/, const void* /*pAllocator*/, void** pInstance) {
    if (!pInstance) return -3;
    g_r54p3_kbase_emu.lvp_executor.init();
    auto* inst = new vulkan::Instance();
    inst->loader_magic = kIcdLoaderMagic;
    inst->phys_dev = new vulkan::PhysicalDevice();
    inst->phys_dev->loader_magic = kIcdLoaderMagic;
    inst->phys_dev->parent_instance = inst;
    *pInstance = inst;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyInstance(void* instance, const void* /*pAllocator*/) {
    if (!instance) return;
    auto* inst = static_cast<vulkan::Instance*>(instance);
    delete inst->phys_dev;
    delete inst;
}

__attribute__((visibility("default")))
int32_t vkEnumeratePhysicalDevices(void* instance, uint32_t* pCount, void** pDevices) {
    if (!instance || !pCount) return -3;
    auto* inst = static_cast<vulkan::Instance*>(instance);
    if (!pDevices) { *pCount = 1; return 0; }
    if (*pCount == 0) return 5;
    pDevices[0] = inst->phys_dev;
    *pCount = 1;
    return 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties(void* physicalDevice, void* pProperties) {
    if (!pProperties) return;
    auto* pd = static_cast<vulkan::PhysicalDevice*>(physicalDevice);
    struct Props {
        uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID;
        uint32_t deviceType; char deviceName[256]; uint8_t uuid[16]; uint8_t limits[504]; uint8_t sparse[20];
    };
    auto* p = static_cast<Props*>(pProperties);
    std::memset(p, 0, sizeof(Props));
    p->apiVersion    = (1U << 22) | (3U << 12) | 250U;
    p->driverVersion = (54U << 22) | (3U << 12);
    p->vendorID      = pd ? pd->vendor_id : 0x13B5U;
    p->deviceID      = pd ? pd->device_id : 0xB8830001U;
    p->deviceType    = 1U;
    std::snprintf(p->deviceName, sizeof(p->deviceName), "%s",
                  pd ? pd->device_name : "Arm Mali-G615 (r54p3 Decompiled C++17 ICD via mali0_emu)");
    *reinterpret_cast<uint32_t*>(p->limits + 0)   = 16384U;
    *reinterpret_cast<uint32_t*>(p->limits + 4)   = 16384U;
    *reinterpret_cast<uint32_t*>(p->limits + 8)   = 2048U;
    *reinterpret_cast<uint32_t*>(p->limits + 212) = 1024U;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceProperties2(void* physicalDevice, void* pProperties2) {
    if (!pProperties2) return;
    vkGetPhysicalDeviceProperties(physicalDevice, static_cast<uint8_t*>(pProperties2) + 16);
    struct VkBaseOutStructure { uint32_t sType; VkBaseOutStructure* pNext; };
    for (auto* ext = static_cast<VkBaseOutStructure*>(pProperties2)->pNext; ext != nullptr; ext = ext->pNext) {
        if (ext->sType == 1000196000U || ext->sType == 52U) {
            uint8_t* dst = reinterpret_cast<uint8_t*>(ext);
            *reinterpret_cast<uint32_t*>(dst + 16) = 9U; // VK_DRIVER_ID_ARM_PROPRIETARY
            std::snprintf(reinterpret_cast<char*>(dst + 20), 256, "Arm Proprietary (Decompiled r54p3 libGLES_mali.so)");
            std::snprintf(reinterpret_cast<char*>(dst + 276), 256, "r54p3-00eac0-6eda07");
            dst[532] = 1; dst[533] = 3; dst[534] = 8; dst[535] = 2;
        } else if (ext->sType == 1000094000U) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(ext) + 16);
            dst[0] = 16U;
            dst[1] = 0x3FU;
            dst[2] = 0x1FFU;
            dst[3] = 1U;
        }
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures(void* /*physicalDevice*/, void* pFeatures) {
    if (!pFeatures) return;
    uint32_t* f = static_cast<uint32_t*>(pFeatures);
    for (int i = 0; i < 55; ++i) f[i] = 1U;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFeatures2(void* physicalDevice, void* pFeatures2) {
    if (!pFeatures2) return;
    vkGetPhysicalDeviceFeatures(physicalDevice, static_cast<uint8_t*>(pFeatures2) + 16);
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceQueueFamilyProperties(void* /*physicalDevice*/, uint32_t* pCount, void* pProps) {
    if (!pCount) return;
    if (!pProps) { *pCount = 1; return; }
    if (*pCount > 0) {
        struct QProps { uint32_t flags; uint32_t count; uint32_t tsBits; uint32_t gran[3]; };
        auto* q = static_cast<QProps*>(pProps);
        q[0] = { 0x07U, 4U, 64U, {1U, 1U, 1U} };
        *pCount = 1;
    }
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceMemoryProperties(void* /*physicalDevice*/, void* pMemoryProperties) {
    if (!pMemoryProperties) return;
    struct MemProps {
        uint32_t typeCount; struct { uint32_t flags; uint32_t heapIdx; } types[32];
        uint32_t heapCount; struct { uint64_t size; uint32_t flags; uint32_t pad; } heaps[16];
    };
    auto* mp = static_cast<MemProps*>(pMemoryProperties);
    std::memset(mp, 0, sizeof(MemProps));
    mp->typeCount = 2;
    mp->types[0]  = { 0x07U, 0U };
    mp->types[1]  = { 0x0FU, 0U };
    mp->heapCount = 1;
    mp->heaps[0]  = { 8ULL << 30, 0x01U, 0U };
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceFormatProperties(void* /*physicalDevice*/, uint32_t /*format*/, void* pFormatProperties) {
    if (!pFormatProperties) return;
    uint32_t* fp = static_cast<uint32_t*>(pFormatProperties);
    fp[0] = 0x1FFFU; fp[1] = 0x1FFFU; fp[2] = 0x1FFFU;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceImageFormatProperties(void* /*physicalDevice*/, uint32_t /*format*/, uint32_t /*type*/,
                                                 uint32_t /*tiling*/, uint32_t /*usage*/, uint32_t /*flags*/,
                                                 void* pImageFormatProperties) {
    if (pImageFormatProperties) {
        uint32_t* ifp = static_cast<uint32_t*>(pImageFormatProperties);
        ifp[0] = 16384U; ifp[1] = 16384U; ifp[2] = 2048U;
        ifp[3] = 15U;    ifp[4] = 2048U;  ifp[5] = 0x0FU;
        *reinterpret_cast<uint64_t*>(ifp + 6) = (4ULL << 30);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateDeviceLayerProperties(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* /*pProperties*/) {
    if (pPropertyCount) *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
void vkGetPhysicalDeviceSparseImageFormatProperties(void* /*physicalDevice*/, uint32_t /*format*/, uint32_t /*type*/,
                                                    uint32_t /*samples*/, uint32_t /*usage*/, uint32_t /*tiling*/,
                                                    uint32_t* pPropertyCount, void* /*pProperties*/) {
    if (pPropertyCount) *pPropertyCount = 0;
}

__attribute__((visibility("default")))
int32_t vkEnumerateDeviceExtensionProperties(void* /*physicalDevice*/, const char* /*pLayerName*/,
                                             uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    static const char* kDevExts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_dynamic_rendering",
        "VK_KHR_buffer_device_address",
        "VK_KHR_push_descriptor",
        "VK_KHR_16bit_storage",
        "VK_KHR_8bit_storage",
        "VK_KHR_shader_float16_int8",
        "VK_EXT_conditional_rendering",
        "VK_EXT_color_write_enable",
        "VK_EXT_calibrated_timestamps",
        "VK_EXT_host_image_copy"
    };
    constexpr uint32_t kCount = sizeof(kDevExts) / sizeof(kDevExts[0]);
    if (!pProperties) { *pPropertyCount = kCount; return 0; }
    struct ExtProp { char name[256]; uint32_t ver; };
    auto* out = static_cast<ExtProp*>(pProperties);
    uint32_t n = std::min(*pPropertyCount, kCount);
    for (uint32_t i = 0; i < n; ++i) {
        std::memset(&out[i], 0, sizeof(ExtProp));
        std::snprintf(out[i].name, sizeof(out[i].name), "%s", kDevExts[i]);
        out[i].ver = 1U;
    }
    *pPropertyCount = n;
    return (n < kCount) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDevice(void* physicalDevice, const void* /*pCreateInfo*/, const void* /*pAllocator*/, void** pDevice) {
    if (!pDevice) return -3;
    auto* dev = new vulkan::Device();
    dev->loader_magic = kIcdLoaderMagic;
    dev->phys_dev = static_cast<vulkan::PhysicalDevice*>(physicalDevice);
    for (uint32_t i = 0; i < 4; ++i) {
        dev->queues[i].loader_magic = kIcdLoaderMagic;
        dev->queues[i].device       = dev;
        dev->queues[i].queue_index  = i;
    }

    kbase::openDevice(dev->kctx);
    kbase::versionCheckCsf(dev->kctx);
    kbase::setFlagsAndMapSpecialPages(dev->kctx, 0);
    kbase::initMemoryZones(dev->kctx);
    kbase::initTilerHeap(dev->kctx);
    kbase::initAndBindCsfQueue(dev->kctx, dev->csi_queue);

    uint64_t rw_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
    dev->srt_bo_va = kbase::memAllocEx(dev->kctx, 1, rw_flags);
    dev->spd_bo_va = kbase::memAllocEx(dev->kctx, 1, rw_flags);
    dev->tsd_bo_va = kbase::memAllocEx(dev->kctx, 1, rw_flags);
    dev->fau_bo_va = kbase::memAllocEx(dev->kctx, 1, rw_flags);
    dev->cqs_bo_va = kbase::memAllocEx(dev->kctx, 1, rw_flags | BASE_MEM_COHERENT_SYSTEM);

    *pDevice = dev;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDevice(void* device, const void* /*pAllocator*/) {
    delete static_cast<vulkan::Device*>(device);
}

__attribute__((visibility("default")))
void vkGetDeviceQueue(void* device, uint32_t /*queueFamilyIndex*/, uint32_t queueIndex, void** pQueue) {
    if (!device || !pQueue) return;
    auto* dev = static_cast<vulkan::Device*>(device);
    *pQueue = &dev->queues[queueIndex % 4];
}

__attribute__((visibility("default")))
int32_t vkAllocateMemory(void* device, const void* pAllocateInfo, const void* /*pAllocator*/, uint64_t* pMemory) {
    if (!device || !pAllocateInfo || !pMemory) return -3;
    auto* dev = static_cast<vulkan::Device*>(device);
    struct AllocInfo { uint32_t sType; const void* pNext; uint64_t size; uint32_t typeIdx; };
    const auto* ai = static_cast<const AllocInfo*>(pAllocateInfo);
    uint64_t pages = std::max<uint64_t>(1ULL, (ai->size + 4095ULL) / 4096ULL);
    uint64_t flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;

    auto* mem = new vulkan::DeviceMemory();
    mem->va_pages   = pages;
    mem->size_bytes = pages * 4096ULL;
    mem->type_index = ai->typeIdx;
    mem->gpu_va     = kbase::memAllocEx(dev->kctx, pages, flags);
    *pMemory = reinterpret_cast<uint64_t>(mem);
    return 0;
}

__attribute__((visibility("default")))
void vkFreeMemory(void* device, uint64_t memory, const void* /*pAllocator*/) {
    if (!memory) return;
    auto* dev = static_cast<vulkan::Device*>(device);
    auto* mem = reinterpret_cast<vulkan::DeviceMemory*>(memory);
    if (dev) kbase::memFree(dev->kctx, mem->gpu_va);
    delete mem;
}

__attribute__((visibility("default")))
int32_t vkMapMemory(void* /*device*/, uint64_t memory, uint64_t offset, uint64_t /*size*/, uint32_t /*flags*/, void** ppData) {
    if (!memory || !ppData) return -3;
    auto* mem = reinterpret_cast<vulkan::DeviceMemory*>(memory);
    *ppData = reinterpret_cast<void*>(mem->gpu_va + offset);
    return 0;
}

__attribute__((visibility("default")))
void vkUnmapMemory(void* /*device*/, uint64_t /*memory*/) {}

__attribute__((visibility("default")))
int32_t vkCreateBuffer(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pBuffer) {
    if (!pCreateInfo || !pBuffer) return -3;
    struct BufInfo { uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; };
    const auto* bi = static_cast<const BufInfo*>(pCreateInfo);
    auto* buf = new vulkan::Buffer();
    buf->size_bytes = bi->size;
    buf->usage      = bi->usage;
    *pBuffer = reinterpret_cast<uint64_t>(buf);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyBuffer(void* /*device*/, uint64_t buffer, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::Buffer*>(buffer);
}

__attribute__((visibility("default")))
void vkGetBufferMemoryRequirements(void* /*device*/, uint64_t buffer, void* pMemoryRequirements) {
    if (!buffer || !pMemoryRequirements) return;
    auto* buf = reinterpret_cast<vulkan::Buffer*>(buffer);
    struct Req { uint64_t size; uint64_t align; uint32_t bits; uint32_t pad; };
    auto* r = static_cast<Req*>(pMemoryRequirements);
    r->size  = std::max<uint64_t>(4096ULL, (buf->size_bytes + 4095ULL) & ~4095ULL);
    r->align = 4096ULL;
    r->bits  = 0x3U;
}

__attribute__((visibility("default")))
int32_t vkBindBufferMemory(void* /*device*/, uint64_t buffer, uint64_t memory, uint64_t memoryOffset) {
    if (!buffer || !memory) return -3;
    auto* buf = reinterpret_cast<vulkan::Buffer*>(buffer);
    auto* mem = reinterpret_cast<vulkan::DeviceMemory*>(memory);
    buf->bound_gpu_va = mem->gpu_va + memoryOffset;
    return 0;
}

__attribute__((visibility("default")))
uint64_t vkGetBufferDeviceAddress(void* /*device*/, const void* pInfo) {
    if (!pInfo) return 0;
    struct BdaInfo { uint32_t sType; const void* pNext; uint64_t buffer; };
    const auto* info = static_cast<const BdaInfo*>(pInfo);
    auto* buf = reinterpret_cast<vulkan::Buffer*>(info->buffer);
    return buf ? buf->bound_gpu_va : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateImage(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pImage) {
    if (!pCreateInfo || !pImage) return -3;
    struct ImgInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
        uint32_t width; uint32_t height; uint32_t depth; uint32_t mipLevels; uint32_t arrayLayers;
        uint32_t samples; uint32_t tiling; uint32_t usage;
    };
    const auto* ii = static_cast<const ImgInfo*>(pCreateInfo);
    auto* img = new vulkan::Image();
    img->image_type   = ii->imageType;
    img->format       = ii->format;
    img->width        = std::max<uint32_t>(1U, ii->width);
    img->height       = std::max<uint32_t>(1U, ii->height);
    img->depth        = std::max<uint32_t>(1U, ii->depth);
    img->mip_levels   = std::max<uint32_t>(1U, ii->mipLevels);
    img->array_layers = std::max<uint32_t>(1U, ii->arrayLayers);
    img->samples      = std::max<uint32_t>(1U, ii->samples);
    img->usage        = ii->usage;
    gfx::populateStubTextureWithMipmaps(img);
    img->bound_gpu_va = reinterpret_cast<uint64_t>(img->stub_texture_backing.data());
    *pImage = reinterpret_cast<uint64_t>(img);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyImage(void* /*device*/, uint64_t image, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::Image*>(image);
}

__attribute__((visibility("default")))
void vkGetImageMemoryRequirements(void* /*device*/, uint64_t image, void* pMemoryRequirements) {
    if (!image || !pMemoryRequirements) return;
    auto* img = reinterpret_cast<vulkan::Image*>(image);
    struct Req { uint64_t size; uint64_t align; uint32_t bits; uint32_t pad; };
    auto* r = static_cast<Req*>(pMemoryRequirements);
    r->size  = std::max<uint64_t>(4096ULL, (img->byte_size + 4095ULL) & ~4095ULL);
    r->align = 4096ULL;
    r->bits  = 0x3U;
}

__attribute__((visibility("default")))
int32_t vkBindImageMemory(void* /*device*/, uint64_t image, uint64_t memory, uint64_t memoryOffset) {
    if (!image || !memory) return -3;
    auto* img = reinterpret_cast<vulkan::Image*>(image);
    auto* mem = reinterpret_cast<vulkan::DeviceMemory*>(memory);
    uint64_t target_va = mem->gpu_va + memoryOffset;
    if (target_va && !img->stub_texture_backing.empty() && *reinterpret_cast<const uint32_t*>(target_va) == 0) {
        size_t copy_len = std::min<size_t>(img->stub_texture_backing.size(), mem->size_bytes - memoryOffset);
        std::memcpy(reinterpret_cast<void*>(target_va), img->stub_texture_backing.data(), copy_len);
    }
    img->bound_gpu_va = target_va;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateImageView(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pView) {
    if (!pCreateInfo || !pView) return -3;
    struct ViewInfo { uint32_t sType; const void* pNext; uint32_t flags; uint64_t image; uint32_t viewType; uint32_t format; };
    const auto* vi = static_cast<const ViewInfo*>(pCreateInfo);
    auto* view = new vulkan::ImageView();
    view->image     = reinterpret_cast<vulkan::Image*>(vi->image);
    view->view_type = vi->viewType;
    view->format    = vi->format;
    if (view->image) {
        view->hw_tex_desc.format_and_swizzle = vi->format;
        view->hw_tex_desc.dimension_and_type = (view->image->mip_levels << 8) | vi->viewType;
        view->hw_tex_desc.width_minus_1      = view->image->width - 1;
        view->hw_tex_desc.height_minus_1     = view->image->height - 1;
        view->hw_tex_desc.surface_gpu_va     = view->image->bound_gpu_va;
        view->hw_tex_desc.row_stride_bytes   = view->image->width * 4;
        view->hw_tex_desc.layer_stride_bytes = view->image->width * view->image->height * 4;
    }
    *pView = reinterpret_cast<uint64_t>(view);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyImageView(void* /*device*/, uint64_t imageView, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::ImageView*>(imageView);
}

__attribute__((visibility("default")))
int32_t vkCreateSampler(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSampler) {
    if (!pSampler) return -3;
    auto* samp = new vulkan::Sampler();
    samp->hw_sampler_desc.filter_mode    = 1U;
    samp->hw_sampler_desc.max_lod        = 15.0f;
    samp->hw_sampler_desc.max_anisotropy = 16.0f;
    *pSampler = reinterpret_cast<uint64_t>(samp);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroySampler(void* /*device*/, uint64_t sampler, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::Sampler*>(sampler);
}

__attribute__((visibility("default")))
int32_t vkCreateShaderModule(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pShaderModule) {
    if (!pCreateInfo || !pShaderModule) return -3;
    struct SmInfo { uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode; };
    const auto* smi = static_cast<const SmInfo*>(pCreateInfo);
    if (!smi->pCode || smi->codeSize < 20) return -3;

    auto* sm = new vulkan::ShaderModule();
    sm->spirv_words.assign(smi->pCode, smi->pCode + (smi->codeSize / 4));
    osu::computeSpirvSha1(smi->pCode, smi->codeSize, sm->sha1_digest);
    *pShaderModule = reinterpret_cast<uint64_t>(sm);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyShaderModule(void* /*device*/, uint64_t shaderModule, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::ShaderModule*>(shaderModule);
}

__attribute__((visibility("default")))
int32_t vkCreateDescriptorSetLayout(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSetLayout) {
    if (!pSetLayout) return -3;
    *pSetLayout = reinterpret_cast<uint64_t>(new vulkan::DescriptorSetLayout());
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDescriptorSetLayout(void* /*device*/, uint64_t layout, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::DescriptorSetLayout*>(layout);
}

__attribute__((visibility("default")))
int32_t vkCreatePipelineLayout(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pPipelineLayout) {
    if (!pPipelineLayout) return -3;
    *pPipelineLayout = reinterpret_cast<uint64_t>(new vulkan::PipelineLayout());
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipelineLayout(void* /*device*/, uint64_t layout, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::PipelineLayout*>(layout);
}

__attribute__((visibility("default")))
int32_t vkCreateDescriptorPool(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pPool) {
    if (!pPool) return -3;
    *pPool = 0xD8000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDescriptorPool(void* /*device*/, uint64_t /*pool*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkAllocateDescriptorSets(void* /*device*/, const void* pAllocateInfo, uint64_t* pDescriptorSets) {
    if (!pAllocateInfo || !pDescriptorSets) return -3;
    struct AllocInfo { uint32_t sType; const void* pNext; uint64_t pool; uint32_t count; const uint64_t* pLayouts; };
    const auto* ai = static_cast<const AllocInfo*>(pAllocateInfo);
    uint32_t n = std::max<uint32_t>(1U, std::min<uint32_t>(ai->count, 64U));
    for (uint32_t i = 0; i < n; ++i) {
        pDescriptorSets[i] = reinterpret_cast<uint64_t>(new vulkan::DescriptorSet());
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkFreeDescriptorSets(void* /*device*/, uint64_t /*pool*/, uint32_t count, const uint64_t* pSets) {
    if (pSets) {
        for (uint32_t i = 0; i < count; ++i) delete reinterpret_cast<vulkan::DescriptorSet*>(pSets[i]);
    }
    return 0;
}

__attribute__((visibility("default")))
void vkUpdateDescriptorSets(void* /*device*/, uint32_t writeCount, const void* pWrites,
                            uint32_t /*copyCount*/, const void* /*pCopies*/) {
    if (!pWrites) return;
    struct ImgInfo { uint64_t sampler; uint64_t imageView; uint32_t imageLayout; };
    struct BufInfo { uint64_t buffer; uint64_t offset; uint64_t range; };
    struct WriteDesc {
        uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBinding; uint32_t dstElem;
        uint32_t count; uint32_t type; const ImgInfo* pImg; const BufInfo* pBuf; const void* pTexel;
    };
    const auto* writes = static_cast<const WriteDesc*>(pWrites);
    for (uint32_t i = 0; i < writeCount; ++i) {
        auto* dset = reinterpret_cast<vulkan::DescriptorSet*>(writes[i].dstSet);
        if (!dset || writes[i].dstBinding >= 32) continue;
        auto& slot = dset->bindings[writes[i].dstBinding];
        if (writes[i].pBuf && writes[i].pBuf->buffer) {
            auto* buf = reinterpret_cast<vulkan::Buffer*>(writes[i].pBuf->buffer);
            slot.buffer_gpu_va = buf->bound_gpu_va + writes[i].pBuf->offset;
            slot.buffer_offset = writes[i].pBuf->offset;
            slot.buffer_range  = writes[i].pBuf->range;
        }
        if (writes[i].pImg) {
            if (writes[i].pImg->imageView) {
                slot.image_view = reinterpret_cast<vulkan::ImageView*>(writes[i].pImg->imageView);
                slot.tex_desc   = slot.image_view->hw_tex_desc;
            }
            if (writes[i].pImg->sampler) {
                slot.sampler   = reinterpret_cast<vulkan::Sampler*>(writes[i].pImg->sampler);
                slot.samp_desc = slot.sampler->hw_sampler_desc;
            }
        }
    }
}

__attribute__((visibility("default")))
int32_t vkCreateRenderPass(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pRenderPass) {
    if (!pRenderPass) return -3;
    *pRenderPass = reinterpret_cast<uint64_t>(new vulkan::RenderPass());
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyRenderPass(void* /*device*/, uint64_t renderPass, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::RenderPass*>(renderPass);
}

__attribute__((visibility("default")))
int32_t vkCreateFramebuffer(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pFramebuffer) {
    if (!pCreateInfo || !pFramebuffer) return -3;
    struct FbInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t renderPass;
        uint32_t attachmentCount; const uint64_t* pAttachments;
        uint32_t width; uint32_t height; uint32_t layers;
    };
    const auto* fi = static_cast<const FbInfo*>(pCreateInfo);
    auto* fb = new vulkan::Framebuffer();
    fb->render_pass = reinterpret_cast<vulkan::RenderPass*>(fi->renderPass);
    fb->width       = std::max<uint32_t>(1U, fi->width);
    fb->height      = std::max<uint32_t>(1U, fi->height);
    fb->layers      = std::max<uint32_t>(1U, fi->layers);

    fb->hw_mfbd.magic             = MALI_MFBD_MAGIC;
    fb->hw_mfbd.width             = fb->width;
    fb->hw_mfbd.height            = fb->height;
    fb->hw_mfbd.sample_count      = 1;
    fb->hw_mfbd.clear_color_rgba8 = 0xFF181410U;
    fb->hw_mfbd.clear_depth       = 1.0f;

    for (uint32_t i = 0; i < fi->attachmentCount && fi->pAttachments; ++i) {
        auto* iv = reinterpret_cast<vulkan::ImageView*>(fi->pAttachments[i]);
        fb->attachments.push_back(iv);
        if (iv && iv->image) {
            if (i == 0) fb->hw_mfbd.color_rt0_gpu_va = iv->image->bound_gpu_va;
            else if (i == 1) fb->hw_mfbd.depth_bo_gpu_va = iv->image->bound_gpu_va;
        }
    }
    *pFramebuffer = reinterpret_cast<uint64_t>(fb);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyFramebuffer(void* /*device*/, uint64_t framebuffer, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::Framebuffer*>(framebuffer);
}

__attribute__((visibility("default")))
int32_t vkCreateComputePipelines(void* device, uint64_t /*pipelineCache*/, uint32_t createInfoCount,
                                 const void* pCreateInfos, const void* /*pAllocator*/, uint64_t* pPipelines) {
    if (!device || !pCreateInfos || !pPipelines) return -3;
    auto* dev = static_cast<vulkan::Device*>(device);
    struct StageInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec; };
    struct CompPipeInfo { uint32_t sType; const void* pNext; uint32_t flags; StageInfo stage; uint64_t layout; uint64_t baseH; int32_t baseI; };
    const auto* cis = static_cast<const CompPipeInfo*>(pCreateInfos);

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        auto* pipe = new vulkan::Pipeline();
        pipe->is_compute = true;
        pipe->layout     = reinterpret_cast<vulkan::PipelineLayout*>(cis[i].layout);
        auto* sm         = reinterpret_cast<vulkan::ShaderModule*>(cis[i].stage.module);
        gfx::compileSingleComputeStage(dev, sm, &pipe->cs_compiled, &pipe->cs_exec_bo_va);
        pPipelines[i] = reinterpret_cast<uint64_t>(pipe);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateGraphicsPipelines(void* device, uint64_t /*pipelineCache*/, uint32_t createInfoCount,
                                  const void* pCreateInfos, const void* /*pAllocator*/, uint64_t* pPipelines) {
    if (!device || !pCreateInfos || !pPipelines) return -3;
    auto* dev = static_cast<vulkan::Device*>(device);
    struct StageInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec; };
    struct GfxPipeInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t stageCount; const StageInfo* pStages;
        const void* pVertexInput; const void* pInputAssembly; const void* pTessellation; const void* pViewport;
        const void* pRasterization; const void* pMultisample; const void* pDepthStencil; const void* pColorBlend;
        const void* pDynamic; uint64_t layout; uint64_t renderPass; uint32_t subpass; uint64_t baseH; int32_t baseI;
    };
    const auto* gis = static_cast<const GfxPipeInfo*>(pCreateInfos);

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        auto* pipe = new vulkan::Pipeline();
        pipe->is_compute  = false;
        pipe->layout      = reinterpret_cast<vulkan::PipelineLayout*>(gis[i].layout);
        pipe->render_pass = reinterpret_cast<vulkan::RenderPass*>(gis[i].renderPass);
        const vulkan::ShaderModule* vs_mod = nullptr;
        const vulkan::ShaderModule* fs_mod = nullptr;
        for (uint32_t s = 0; s < gis[i].stageCount && gis[i].pStages; ++s) {
            if (gis[i].pStages[s].stage == 0x01U) vs_mod = reinterpret_cast<const vulkan::ShaderModule*>(gis[i].pStages[s].module);
            if (gis[i].pStages[s].stage == 0x10U) fs_mod = reinterpret_cast<const vulkan::ShaderModule*>(gis[i].pStages[s].module);
        }
        gfx::compileGraphicsStages(dev, vs_mod, fs_mod,
                                   &pipe->vs_compiled, &pipe->fs_compiled,
                                   &pipe->vs_exec_bo_va, &pipe->fs_exec_bo_va);
        pPipelines[i] = reinterpret_cast<uint64_t>(pipe);
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipeline(void* device, uint64_t pipeline, const void* /*pAllocator*/) {
    if (!pipeline) return;
    auto* dev  = static_cast<vulkan::Device*>(device);
    auto* pipe = reinterpret_cast<vulkan::Pipeline*>(pipeline);
    if (dev) {
        if (pipe->vs_compiled) cmpbe::cmpbe_v2_free_compilation_result(&dev->cmpbe_ctx, pipe->vs_compiled);
        if (pipe->fs_compiled) cmpbe::cmpbe_v2_free_compilation_result(&dev->cmpbe_ctx, pipe->fs_compiled);
        if (pipe->cs_compiled) cmpbe::cmpbe_v2_free_compilation_result(&dev->cmpbe_ctx, pipe->cs_compiled);
    }
    delete pipe;
}

__attribute__((visibility("default")))
int32_t vkCreateQueryPool(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pQueryPool) {
    if (!pCreateInfo || !pQueryPool) return -3;
    struct QpInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t queryType; uint32_t queryCount; };
    const auto* qi = static_cast<const QpInfo*>(pCreateInfo);
    auto* qp = new vulkan::QueryPool();
    qp->query_type  = qi->queryType;
    qp->query_count = std::max<uint32_t>(1U, qi->queryCount);
    qp->results.assign(qp->query_count, 1000000ULL);
    *pQueryPool = reinterpret_cast<uint64_t>(qp);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyQueryPool(void* /*device*/, uint64_t queryPool, const void* /*pAllocator*/) {
    delete reinterpret_cast<vulkan::QueryPool*>(queryPool);
}

__attribute__((visibility("default")))
int32_t vkGetQueryPoolResults(void* /*device*/, uint64_t queryPool, uint32_t firstQuery, uint32_t queryCount,
                              size_t dataSize, void* pData, uint64_t stride, uint32_t flags) {
    if (!queryPool || !pData) return -3;
    auto* qp = reinterpret_cast<vulkan::QueryPool*>(queryPool);
    bool is_64 = (flags & 0x01U) != 0;
    uint8_t* dst = static_cast<uint8_t*>(pData);
    for (uint32_t i = 0; i < queryCount && (i * stride) < dataSize; ++i) {
        uint64_t val = ((firstQuery + i) < qp->results.size()) ? qp->results[firstQuery + i] : (1000000ULL + i * 25000ULL);
        if (is_64) *reinterpret_cast<uint64_t*>(dst + i * stride) = val;
        else *reinterpret_cast<uint32_t*>(dst + i * stride) = static_cast<uint32_t>(val);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateCommandPool(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pCommandPool) {
    if (!pCommandPool) return -3;
    *pCommandPool = 0xC8000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyCommandPool(void* /*device*/, uint64_t /*commandPool*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkAllocateCommandBuffers(void* device, const void* pAllocateInfo, void** pCommandBuffers) {
    if (!device || !pAllocateInfo || !pCommandBuffers) return -3;
    auto* dev = static_cast<vulkan::Device*>(device);
    struct CbInfo { uint32_t sType; const void* pNext; uint64_t pool; uint32_t level; uint32_t count; };
    const auto* ci = static_cast<const CbInfo*>(pAllocateInfo);
    for (uint32_t i = 0; i < ci->count; ++i) {
        auto* cb = new vulkan::CommandBuffer();
        cb->loader_magic = kIcdLoaderMagic;
        cb->device       = dev;
        pCommandBuffers[i] = cb;
    }
    return 0;
}

__attribute__((visibility("default")))
void vkFreeCommandBuffers(void* /*device*/, uint64_t /*pool*/, uint32_t count, void* const* pCommandBuffers) {
    if (!pCommandBuffers) return;
    for (uint32_t i = 0; i < count; ++i) delete static_cast<vulkan::CommandBuffer*>(pCommandBuffers[i]);
}

__attribute__((visibility("default")))
int32_t vkResetCommandBuffer(void* commandBuffer, uint32_t /*flags*/) {
    if (!commandBuffer) return -3;
    static_cast<vulkan::CommandBuffer*>(commandBuffer)->commands.clear();
    return 0;
}

__attribute__((visibility("default")))
int32_t vkBeginCommandBuffer(void* commandBuffer, const void* /*pBeginInfo*/) {
    if (!commandBuffer) return -3;
    static_cast<vulkan::CommandBuffer*>(commandBuffer)->commands.clear();
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEndCommandBuffer(void* commandBuffer) {
    return commandBuffer ? 0 : -3;
}

__attribute__((visibility("default")))
void vkCmdBeginRenderPass(void* commandBuffer, const void* pRenderPassBegin, uint32_t /*contents*/) {
    if (!commandBuffer || !pRenderPassBegin) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    struct RpBegin {
        uint32_t sType; const void* pNext; uint64_t renderPass; uint64_t framebuffer;
        int32_t rx, ry; uint32_t rw, rh; uint32_t clearValueCount; const uint32_t* pClearValues;
    };
    const auto* rpb = static_cast<const RpBegin*>(pRenderPassBegin);
    cb->active_rp = reinterpret_cast<vulkan::RenderPass*>(rpb->renderPass);
    cb->active_fb = reinterpret_cast<vulkan::Framebuffer*>(rpb->framebuffer);
}

__attribute__((visibility("default")))
void vkCmdEndRenderPass(void* commandBuffer) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    cb->active_rp = nullptr;
    cb->active_fb = nullptr;
}

__attribute__((visibility("default")))
void vkCmdBindPipeline(void* commandBuffer, uint32_t /*bindPoint*/, uint64_t pipeline) {
    if (!commandBuffer) return;
    static_cast<vulkan::CommandBuffer*>(commandBuffer)->bound_pipeline = reinterpret_cast<vulkan::Pipeline*>(pipeline);
}

__attribute__((visibility("default")))
void vkCmdBindDescriptorSets(void* commandBuffer, uint32_t /*bindPoint*/, uint64_t /*layout*/,
                             uint32_t /*firstSet*/, uint32_t setCount, const uint64_t* pDescriptorSets,
                             uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    if (!commandBuffer || setCount == 0 || !pDescriptorSets) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    cb->bound_desc_set = reinterpret_cast<vulkan::DescriptorSet*>(pDescriptorSets[0]);
    cb->dynamic_ubo_offset = (dynamicOffsetCount > 0 && pDynamicOffsets) ? pDynamicOffsets[0] : 0U;
}

__attribute__((visibility("default")))
void vkCmdBindVertexBuffers(void* commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                            const uint64_t* pBuffers, const uint64_t* pOffsets) {
    if (!commandBuffer || !pBuffers) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    for (uint32_t i = 0; i < bindingCount && (firstBinding + i) < 8; ++i) {
        auto* buf = reinterpret_cast<vulkan::Buffer*>(pBuffers[i]);
        uint64_t off = pOffsets ? pOffsets[i] : 0ULL;
        cb->bound_vbo_va[firstBinding + i] = buf ? (buf->bound_gpu_va + off) : 0ULL;
    }
}

__attribute__((visibility("default")))
void vkCmdBindIndexBuffer(void* commandBuffer, uint64_t buffer, uint64_t offset, uint32_t /*indexType*/) {
    if (!commandBuffer) return;
    auto* cb  = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    auto* buf = reinterpret_cast<vulkan::Buffer*>(buffer);
    cb->bound_ibo_va = buf ? (buf->bound_gpu_va + offset) : 0ULL;
}

__attribute__((visibility("default")))
void vkCmdPushConstants(void* commandBuffer, uint64_t /*layout*/, uint32_t /*stageFlags*/,
                        uint32_t offset, uint32_t size, const void* pValues) {
    if (!commandBuffer || !pValues || offset + size > 128) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    std::memcpy(cb->push_constants + offset, pValues, size);
}

__attribute__((visibility("default")))
void vkCmdSetViewport(void* /*commandBuffer*/, uint32_t /*first*/, uint32_t /*count*/, const void* /*pViewports*/) {}

__attribute__((visibility("default")))
void vkCmdSetScissor(void* /*commandBuffer*/, uint32_t /*first*/, uint32_t /*count*/, const void* /*pScissors*/) {}

__attribute__((visibility("default")))
void vkCmdSetColorWriteEnableEXT(void* /*commandBuffer*/, uint32_t /*count*/, const uint32_t* /*pEnables*/) {}

__attribute__((visibility("default")))
void vkCmdBeginConditionalRenderingEXT(void* commandBuffer, const void* pConditionalRenderingBegin) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    if (pConditionalRenderingBegin) {
        struct CondInfo { uint32_t sType; const void* pNext; uint64_t buffer; uint64_t offset; uint32_t flags; };
        const auto* ci = static_cast<const CondInfo*>(pConditionalRenderingBegin);
        auto* buf = reinterpret_cast<vulkan::Buffer*>(ci->buffer);
        if (buf && buf->bound_gpu_va) {
            uint32_t pred = *reinterpret_cast<const uint32_t*>(buf->bound_gpu_va + ci->offset);
            cb->conditional_rendering_active = ((ci->flags & 1U) != 0) ? (pred == 0) : (pred != 0);
        }
    }
}

__attribute__((visibility("default")))
void vkCmdEndConditionalRenderingEXT(void* commandBuffer) {
    if (commandBuffer) static_cast<vulkan::CommandBuffer*>(commandBuffer)->conditional_rendering_active = true;
}

__attribute__((visibility("default")))
void vkCmdDraw(void* commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
               uint32_t /*firstVertex*/, uint32_t firstInstance) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    if (!cb->conditional_rendering_active) return;
    vulkan::RecordedCommand cmd{};
    cmd.type           = vulkan::RecordedCommand::CMD_DRAW;
    cmd.pipeline       = cb->bound_pipeline;
    cmd.desc_set       = cb->bound_desc_set;
    cmd.framebuffer    = cb->active_fb;
    cmd.dynamic_offset = cb->dynamic_ubo_offset;
    cmd.vertex_count   = vertexCount;
    cmd.instance_count = std::max<uint32_t>(1U, instanceCount);
    cmd.first_instance = firstInstance;
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdDrawIndexed(void* commandBuffer, uint32_t indexCount, uint32_t instanceCount,
                      uint32_t /*firstIndex*/, int32_t /*vertexOffset*/, uint32_t firstInstance) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    if (!cb->conditional_rendering_active) return;
    vulkan::RecordedCommand cmd{};
    cmd.type           = vulkan::RecordedCommand::CMD_DRAW_INDEXED;
    cmd.pipeline       = cb->bound_pipeline;
    cmd.desc_set       = cb->bound_desc_set;
    cmd.framebuffer    = cb->active_fb;
    cmd.dynamic_offset = cb->dynamic_ubo_offset;
    cmd.index_count    = indexCount;
    cmd.instance_count = std::max<uint32_t>(1U, instanceCount);
    cmd.first_instance = firstInstance;
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdDispatch(void* commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    if (!cb->conditional_rendering_active) return;
    vulkan::RecordedCommand cmd{};
    cmd.type           = vulkan::RecordedCommand::CMD_DISPATCH;
    cmd.pipeline       = cb->bound_pipeline;
    cmd.desc_set       = cb->bound_desc_set;
    cmd.dynamic_offset = cb->dynamic_ubo_offset;
    cmd.wg_x           = std::max<uint32_t>(1U, groupCountX);
    cmd.wg_y           = std::max<uint32_t>(1U, groupCountY);
    cmd.wg_z           = std::max<uint32_t>(1U, groupCountZ);
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdCopyBuffer(void* commandBuffer, uint64_t srcBuffer, uint64_t dstBuffer, uint32_t regionCount, const void* pRegions) {
    if (!commandBuffer || !srcBuffer || !dstBuffer || !pRegions) return;
    auto* cb  = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    auto* src = reinterpret_cast<vulkan::Buffer*>(srcBuffer);
    auto* dst = reinterpret_cast<vulkan::Buffer*>(dstBuffer);
    struct Region { uint64_t srcOff; uint64_t dstOff; uint64_t size; };
    const auto* regs = static_cast<const Region*>(pRegions);
    for (uint32_t i = 0; i < regionCount; ++i) {
        vulkan::RecordedCommand cmd{};
        cmd.type       = vulkan::RecordedCommand::CMD_COPY_BUFFER;
        cmd.src_va     = src->bound_gpu_va + regs[i].srcOff;
        cmd.dst_va     = dst->bound_gpu_va + regs[i].dstOff;
        cmd.copy_bytes = regs[i].size;
        cb->commands.push_back(cmd);
    }
}

__attribute__((visibility("default")))
void vkCmdCopyBufferToImage(void* commandBuffer, uint64_t srcBuffer, uint64_t dstImage,
                            uint32_t /*dstImageLayout*/, uint32_t /*regionCount*/, const void* /*pRegions*/) {
    if (!commandBuffer || !srcBuffer || !dstImage) return;
    auto* cb  = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    auto* src = reinterpret_cast<vulkan::Buffer*>(srcBuffer);
    auto* img = reinterpret_cast<vulkan::Image*>(dstImage);
    vulkan::RecordedCommand cmd{};
    cmd.type       = vulkan::RecordedCommand::CMD_COPY_BUFFER_TO_IMAGE;
    cmd.src_va     = src->bound_gpu_va;
    cmd.dst_image  = img;
    cmd.copy_bytes = std::min<uint64_t>(src->size_bytes, img->byte_size);
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdBlitImage(void* commandBuffer, uint64_t srcImage, uint32_t /*srcLayout*/,
                    uint64_t dstImage, uint32_t /*dstLayout*/, uint32_t /*regionCount*/,
                    const void* /*pRegions*/, uint32_t /*filter*/) {
    if (!commandBuffer || !srcImage || !dstImage) return;
    auto* cb  = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    auto* dst = reinterpret_cast<vulkan::Image*>(dstImage);
    vulkan::RecordedCommand cmd{};
    cmd.type      = vulkan::RecordedCommand::CMD_BLIT_IMAGE;
    cmd.dst_image = dst;
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdWriteTimestamp(void* commandBuffer, uint32_t /*pipelineStage*/, uint64_t queryPool, uint32_t query) {
    if (!commandBuffer || !queryPool) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    vulkan::RecordedCommand cmd{};
    cmd.type        = vulkan::RecordedCommand::CMD_WRITE_TIMESTAMP;
    cmd.query_pool  = reinterpret_cast<vulkan::QueryPool*>(queryPool);
    cmd.query_index = query;
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdPipelineBarrier(void* commandBuffer, uint32_t /*srcStageMask*/, uint32_t /*dstStageMask*/,
                          uint32_t /*dependencyFlags*/, uint32_t /*memBarrierCount*/, const void* /*pMemBarriers*/,
                          uint32_t /*bufBarrierCount*/, const void* /*pBufBarriers*/,
                          uint32_t /*imgBarrierCount*/, const void* /*pImgBarriers*/) {
    if (!commandBuffer) return;
    auto* cb = static_cast<vulkan::CommandBuffer*>(commandBuffer);
    vulkan::RecordedCommand cmd{};
    cmd.type = vulkan::RecordedCommand::CMD_PIPELINE_BARRIER;
    cb->commands.push_back(cmd);
}

__attribute__((visibility("default")))
int32_t vkQueueSubmit(void* queue, uint32_t submitCount, const void* pSubmits, uint64_t /*fence*/) {
    if (!queue) return -3;
    auto* q   = static_cast<vulkan::Queue*>(queue);
    auto* dev = q->device;
    if (!dev) return -3;

    std::lock_guard<std::mutex> lock(dev->submit_mtx);
    struct SubmitInfo {
        uint32_t sType; const void* pNext; uint32_t waitC; const uint64_t* pWaitS; const uint32_t* pWaitM;
        uint32_t cbC; void* const* pCbs; uint32_t sigC; const uint64_t* pSigS;
    };
    const auto* submits = static_cast<const SubmitInfo*>(pSubmits);

    uint64_t* ring_words = reinterpret_cast<uint64_t*>(dev->csi_queue.ring_bo_gpu_va);
    auto pushCsfWord = [&](uint64_t w) {
        size_t slot = (dev->csi_queue.write_offset / 8) % (dev->csi_queue.ring_size / 8);
        ring_words[slot] = w;
        dev->csi_queue.write_offset += 8;
    };

    static uint64_t s_timestamp_ns = 1000000ULL;

    for (uint32_t s = 0; s < submitCount && submits; ++s) {
        for (uint32_t c = 0; c < submits[s].cbC; ++c) {
            auto* cb = static_cast<vulkan::CommandBuffer*>(submits[s].pCbs[c]);
            if (!cb) continue;

            for (const auto& cmd : cb->commands) {
                switch (cmd.type) {
                    case vulkan::RecordedCommand::CMD_COPY_BUFFER: {
                        if (cmd.src_va && cmd.dst_va && cmd.copy_bytes > 0) {
                            std::memcpy(reinterpret_cast<void*>(cmd.dst_va),
                                        reinterpret_cast<const void*>(cmd.src_va),
                                        cmd.copy_bytes);
                        }
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_COPY_BUFFER_TO_IMAGE: {
                        if (cmd.src_va && cmd.dst_image && cmd.dst_image->bound_gpu_va && cmd.copy_bytes > 0) {
                            std::memcpy(reinterpret_cast<void*>(cmd.dst_image->bound_gpu_va),
                                        reinterpret_cast<const void*>(cmd.src_va),
                                        cmd.copy_bytes);
                        }
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_BLIT_IMAGE: {
                        if (cmd.dst_image) {
                            gfx::populateStubTextureWithMipmaps(cmd.dst_image);
                        }
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_WRITE_TIMESTAMP: {
                        s_timestamp_ns += 42000ULL;
                        if (cmd.query_pool && cmd.query_index < cmd.query_pool->results.size()) {
                            cmd.query_pool->results[cmd.query_index] = s_timestamp_ns;
                        }
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_PIPELINE_BARRIER: {
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_FLUSH_CACHE2) << 56));
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_DISPATCH: {
                        if (!cmd.pipeline || !cmd.desc_set) break;
                        uint64_t* srt_ptr = reinterpret_cast<uint64_t*>(dev->srt_bo_va);
                        srt_ptr[0] = cmd.desc_set->bindings[0].buffer_gpu_va + cmd.dynamic_offset;
                        srt_ptr[1] = cmd.desc_set->bindings[1].buffer_gpu_va;
                        srt_ptr[2] = std::max<uint64_t>(64ULL, cmd.desc_set->bindings[0].buffer_range);

                        uint64_t* spd_ptr = reinterpret_cast<uint64_t*>(dev->spd_bo_va);
                        spd_ptr[0] = cmd.pipeline->cs_exec_bo_va;
                        spd_ptr[1] = cmd.pipeline->cs_compiled ? (cmd.pipeline->cs_compiled->objc_words.size() * 8) : 0;

                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (dev->srt_bo_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (dev->spd_bo_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (32ULL << 48) | cmd.wg_x);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (33ULL << 48) | cmd.wg_y);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (34ULL << 48) | cmd.wg_z);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_COMPUTE) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (40ULL << 48) | (dev->cqs_bo_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (42ULL << 48) | 1ULL);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_SYNC_ADD64) << 56) | (40ULL << 40) | (42ULL << 32));
                        kbase::kickCsfQueue(dev->csi_queue);
                        break;
                    }
                    case vulkan::RecordedCommand::CMD_DRAW:
                    case vulkan::RecordedCommand::CMD_DRAW_INDEXED: {
                        if (cmd.desc_set && cmd.desc_set->bindings[0].buffer_gpu_va != 0 &&
                            *reinterpret_cast<const uint32_t*>(cmd.desc_set->bindings[0].buffer_gpu_va) == MALI_IDVS_MAGIC) {
                            uint64_t pkt_va = cmd.desc_set->bindings[0].buffer_gpu_va;
                            const auto* idvs_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(pkt_va);
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (pkt_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->vs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (16ULL << 48) | (pkt_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_IDVS) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_TILING) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->fs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_FRAGMENT) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_FRAGMENT) << 56));
                            kbase::kickCsfQueue(dev->csi_queue);
                        } else if (cmd.framebuffer && cmd.framebuffer->hw_mfbd.color_rt0_gpu_va != 0) {
                            uint8_t* scratch = reinterpret_cast<uint8_t*>(dev->tsd_bo_va);
                            auto* cube_verts   = reinterpret_cast<MaliCubeVertex*>(scratch + 0x100);
                            auto* cube_indices = reinterpret_cast<uint32_t*>(scratch + 0x580);
                            auto* mvp_ubo      = reinterpret_cast<float*>(scratch + 0x680);
                            auto* vs_spd       = reinterpret_cast<uint64_t*>(scratch + 0x780);
                            auto* fs_spd       = reinterpret_cast<uint64_t*>(scratch + 0x7C0);
                            auto* idvs_pkt     = reinterpret_cast<MaliIdvsDrawPacket*>(scratch + 0x000);

                            mali0_emu_vkcube::buildUnitCubeGeometry(cube_verts, cube_indices);
                            static uint32_t s_submit_draw_idx = 0;
                            float angle_deg = (getenv("MALI_VKCUBE_HIRES") != nullptr)
                                ? (25.0f + 18.0f * static_cast<float>(s_submit_draw_idx++))
                                : (25.0f + 18.0f * static_cast<float>(cmd.first_instance + (cmd.dynamic_offset / 64)));
                            if (cmd.desc_set) {
                                for (int b = 0; b < 8; ++b) {
                                    if (cmd.desc_set->bindings[b].image_view && cmd.desc_set->bindings[b].image_view->image) {
                                        const auto* tex_img = cmd.desc_set->bindings[b].image_view->image;
                                        if (!tex_img->stub_texture_backing.empty()) {
                                            uint32_t texel = *reinterpret_cast<const uint32_t*>(tex_img->stub_texture_backing.data());
                                            float tr = ((texel & 0xFFU) / 255.0f) * 0.5f + 0.5f;
                                            for (int v = 0; v < 24; ++v) cube_verts[v].cr = std::min(1.0f, cube_verts[v].cr * tr);
                                        }
                                    }
                                }
                            }
                            mali0_emu_vkcube::buildRotationMvpMatrix(angle_deg, mvp_ubo);

                            vs_spd[0] = cmd.pipeline ? cmd.pipeline->vs_exec_bo_va : 0;
                            vs_spd[1] = (cmd.pipeline && cmd.pipeline->vs_compiled) ? (cmd.pipeline->vs_compiled->objc_words.size() * 8) : 0;
                            fs_spd[0] = cmd.pipeline ? cmd.pipeline->fs_exec_bo_va : 0;
                            fs_spd[1] = (cmd.pipeline && cmd.pipeline->fs_compiled) ? (cmd.pipeline->fs_compiled->objc_words.size() * 8) : 0;

                            idvs_pkt->magic            = MALI_IDVS_MAGIC;
                            idvs_pkt->vertex_count     = 24;
                            idvs_pkt->index_count      = 36;
                            idvs_pkt->instance_count   = cmd.instance_count;
                            idvs_pkt->frame_index      = cmd.first_instance;
                            idvs_pkt->rotation_deg     = angle_deg;
                            idvs_pkt->vertex_bo_gpu_va = reinterpret_cast<uint64_t>(cube_verts);
                            idvs_pkt->index_bo_gpu_va  = reinterpret_cast<uint64_t>(cube_indices);
                            idvs_pkt->mvp_ubo_gpu_va   = reinterpret_cast<uint64_t>(mvp_ubo);
                            idvs_pkt->mfbd_gpu_va      = reinterpret_cast<uint64_t>(&cmd.framebuffer->hw_mfbd);
                            idvs_pkt->vs_spd_gpu_va    = reinterpret_cast<uint64_t>(vs_spd);
                            idvs_pkt->fs_spd_gpu_va    = reinterpret_cast<uint64_t>(fs_spd);

                            uint64_t pkt_va = reinterpret_cast<uint64_t>(idvs_pkt);
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (pkt_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->vs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (16ULL << 48) | (pkt_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_IDVS) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_TILING) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->fs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_FRAGMENT) << 56));
                            pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_FRAGMENT) << 56));
                            kbase::kickCsfQueue(dev->csi_queue);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
    return 0;
}


static constexpr uint64_t R54P3_SURFACE_MAGIC = 0x5552465235345033ULL;

struct R54p3XcbLib {
    void* handle = nullptr;
    typedef uint32_t (*pfn_xcb_generate_id)(void* c);
    typedef struct { unsigned int sequence; } xcb_void_cookie_t;
    typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;
    struct xcb_get_geometry_reply_t {
        uint8_t  response_type;
        uint8_t  depth;
        uint16_t sequence;
        uint32_t length;
        uint32_t root;
        int16_t  x, y;
        uint16_t width, height;
        uint16_t border_width;
        uint8_t  pad0[2];
    };
    typedef xcb_get_geometry_cookie_t (*pfn_xcb_get_geometry)(void* c, uint32_t drawable);
    typedef xcb_get_geometry_reply_t* (*pfn_xcb_get_geometry_reply)(void* c, xcb_get_geometry_cookie_t cookie, void** e);
    typedef xcb_void_cookie_t (*pfn_xcb_create_gc)(void* c, uint32_t cid, uint32_t drawable, uint32_t value_mask, const void* value_list);
    typedef xcb_void_cookie_t (*pfn_xcb_put_image)(void* c, uint8_t format, uint32_t drawable, uint32_t gc,
                                                   uint16_t width, uint16_t height, int16_t dst_x, int16_t dst_y,
                                                   uint8_t left_pad, uint8_t depth, uint32_t data_len, const uint8_t* data);
    typedef int (*pfn_xcb_flush)(void* c);

    pfn_xcb_generate_id        generate_id = nullptr;
    pfn_xcb_get_geometry       get_geometry = nullptr;
    pfn_xcb_get_geometry_reply get_geometry_reply = nullptr;
    pfn_xcb_create_gc          create_gc = nullptr;
    pfn_xcb_put_image          put_image = nullptr;
    pfn_xcb_flush              flush = nullptr;

    bool ensureLoaded() {
        if (handle) return true;
        handle = dlopen("libxcb.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) handle = dlopen("libxcb.so", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return false;
        generate_id        = reinterpret_cast<pfn_xcb_generate_id>(dlsym(handle, "xcb_generate_id"));
        get_geometry       = reinterpret_cast<pfn_xcb_get_geometry>(dlsym(handle, "xcb_get_geometry"));
        get_geometry_reply = reinterpret_cast<pfn_xcb_get_geometry_reply>(dlsym(handle, "xcb_get_geometry_reply"));
        create_gc          = reinterpret_cast<pfn_xcb_create_gc>(dlsym(handle, "xcb_create_gc"));
        put_image          = reinterpret_cast<pfn_xcb_put_image>(dlsym(handle, "xcb_put_image"));
        flush              = reinterpret_cast<pfn_xcb_flush>(dlsym(handle, "xcb_flush"));
        return generate_id && get_geometry && get_geometry_reply && create_gc && put_image && flush;
    }

    bool queryWindowGeometry(void* conn, uint32_t win, uint32_t* out_w, uint32_t* out_h, uint8_t* out_depth) {
        if (!conn || !win || !ensureLoaded()) return false;
        auto cookie = get_geometry(conn, win);
        auto* reply = get_geometry_reply(conn, cookie, nullptr);
        if (!reply) return false;
        if (out_w && reply->width > 0)  *out_w = reply->width;
        if (out_h && reply->height > 0) *out_h = reply->height;
        if (out_depth && reply->depth > 0) *out_depth = reply->depth;
        std::free(reply);
        return true;
    }
};

static R54p3XcbLib g_r54_xcb_lib;

struct R54p3Surface {
    uint64_t magic      = R54P3_SURFACE_MAGIC;
    uint32_t width      = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    uint32_t height     = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    void*    xcb_conn   = nullptr;
    uint32_t xcb_window = 0;
    uint8_t  xcb_depth  = 24;
};

static void resolveR54SurfaceInfo(uint64_t surface, uint32_t* out_w, uint32_t* out_h,
                                  void** out_xcb_conn, uint32_t* out_xcb_win, uint8_t* out_xcb_depth) {
    uint32_t default_dim = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    if (out_w) *out_w = default_dim;
    if (out_h) *out_h = default_dim;
    if (out_xcb_conn)  *out_xcb_conn = nullptr;
    if (out_xcb_win)   *out_xcb_win = 0;
    if (out_xcb_depth) *out_xcb_depth = 24;
    if (!surface) return;

    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(surface);
    if (ptr64[0] == R54P3_SURFACE_MAGIC) {
        const auto* ms = reinterpret_cast<const R54p3Surface*>(surface);
        uint32_t w = ms->width, h = ms->height;
        uint8_t depth = ms->xcb_depth;
        if (ms->xcb_conn && ms->xcb_window) {
            g_r54_xcb_lib.queryWindowGeometry(ms->xcb_conn, ms->xcb_window, &w, &h, &depth);
        }
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_xcb_conn)  *out_xcb_conn = ms->xcb_conn;
        if (out_xcb_win)   *out_xcb_win = ms->xcb_window;
        if (out_xcb_depth) *out_xcb_depth = depth;
        return;
    }
    uint32_t platform = *reinterpret_cast<const uint32_t*>(surface);
    if (platform == 3U) {
        void* conn = reinterpret_cast<void*>(ptr64[1]);
        uint32_t win = *reinterpret_cast<const uint32_t*>(surface + 16);
        uint32_t w = 500U, h = 500U;
        uint8_t depth = 24U;
        g_r54_xcb_lib.queryWindowGeometry(conn, win, &w, &h, &depth);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_xcb_conn)  *out_xcb_conn = conn;
        if (out_xcb_win)   *out_xcb_win = win;
        if (out_xcb_depth) *out_xcb_depth = depth;
    }
}

struct R54p3Swapchain {
    vulkan::Device*             device = nullptr;
    uint32_t                    width  = 64;
    uint32_t                    height = 64;
    uint32_t                    format = 37;
    uint32_t                    next_image_idx = 0;
    void*                       xcb_conn   = nullptr;
    uint32_t                    xcb_window = 0;
    uint32_t                    xcb_gc     = 0;
    uint8_t                     xcb_depth  = 24;
    std::vector<vulkan::Image*> images;
    std::vector<int>            dmabuf_fds;
};


// ============================================================================
// VK_KHR_display & VK_EXT_debug_utils Support for Official Khronos vkcube (--wsi display)
// ============================================================================
struct VkDisplayPropertiesKHRIcd {
    uint64_t    display;
    const char* displayName;
    uint32_t    physicalDimensions[2];
    uint32_t    physicalResolution[2];
    uint32_t    supportedTransforms;
    uint32_t    planeReorderPossible;
    uint32_t    persistentContent;
};

struct VkDisplayPlanePropertiesKHRIcd {
    uint64_t currentDisplay;
    uint32_t currentStackIndex;
};

struct VkDisplayModeParametersKHRIcd {
    uint32_t visibleRegion[2];
    uint32_t refreshRate;
};

struct VkDisplayModePropertiesKHRIcd {
    uint64_t                      displayMode;
    VkDisplayModeParametersKHRIcd parameters;
};

struct VkDisplayPlaneCapabilitiesKHRIcd {
    uint32_t supportedAlpha;
    int32_t  minSrcPosition[2];
    int32_t  maxSrcPosition[2];
    uint32_t minSrcExtent[2];
    uint32_t maxSrcExtent[2];
    int32_t  minDstPosition[2];
    int32_t  maxDstPosition[2];
    uint32_t minDstExtent[2];
    uint32_t maxDstExtent[2];
};

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceDisplayPropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayPropertiesKHRIcd*>(pProperties);
    p->display = 0xD1590001ULL;
    p->displayName = "Mali-G615 Virtual Kbase Display (500x500)";
    p->physicalDimensions[0] = 500U; p->physicalDimensions[1] = 500U;
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->physicalResolution[0] = disp_res;  p->physicalResolution[1] = disp_res;
    p->supportedTransforms = 1U; // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
    p->planeReorderPossible = 0U;
    p->persistentContent = 0U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceDisplayPlanePropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayPlanePropertiesKHRIcd*>(pProperties);
    p->currentDisplay = 0xD1590001ULL;
    p->currentStackIndex = 0U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayPlaneSupportedDisplaysKHR(void* /*physicalDevice*/, uint32_t /*planeIndex*/, uint32_t* pDisplayCount, uint64_t* pDisplays) {
    if (!pDisplayCount) return -3;
    if (!pDisplays) { *pDisplayCount = 1U; return 0; }
    if (*pDisplayCount == 0U) return 5;
    pDisplays[0] = 0xD1590001ULL;
    *pDisplayCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayModePropertiesKHR(void* /*physicalDevice*/, uint64_t /*display*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayModePropertiesKHRIcd*>(pProperties);
    p->displayMode = 0xD15900DEULL;
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->parameters.visibleRegion[0] = disp_res;
    p->parameters.visibleRegion[1] = disp_res;
    p->parameters.refreshRate = 60000U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDisplayModeKHR(void* /*physicalDevice*/, uint64_t /*display*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pMode) {
    if (!pMode) return -3;
    *pMode = 0xD15900DEULL;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayPlaneCapabilitiesKHR(void* /*physicalDevice*/, uint64_t /*mode*/, uint32_t /*planeIndex*/, void* pCapabilities) {
    if (!pCapabilities) return -3;
    auto* c = static_cast<VkDisplayPlaneCapabilitiesKHRIcd*>(pCapabilities);
    std::memset(c, 0, sizeof(*c));
    c->supportedAlpha = 1U; // VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR
    c->minSrcExtent[0] = 16U; c->minSrcExtent[1] = 16U;
    c->maxSrcExtent[0] = 4096U; c->maxSrcExtent[1] = 4096U;
    c->minDstExtent[0] = 16U; c->minDstExtent[1] = 16U;
    c->maxDstExtent[0] = 4096U; c->maxDstExtent[1] = 4096U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDebugUtilsMessengerEXT(void* /*instance*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pMessenger) {
    if (pMessenger) *pMessenger = 0xDEB06001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDebugUtilsMessengerEXT(void* /*instance*/, uint64_t /*messenger*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkSetDebugUtilsObjectNameEXT(void* /*device*/, const void* /*pNameInfo*/) { return 0; }

__attribute__((visibility("default")))
void vkCmdBeginDebugUtilsLabelEXT(void* /*commandBuffer*/, const void* /*pLabelInfo*/) {}

__attribute__((visibility("default")))
void vkCmdEndDebugUtilsLabelEXT(void* /*commandBuffer*/) {}


__attribute__((visibility("default")))
int32_t vkCreatePipelineCache(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pPipelineCache) {
    if (pPipelineCache) *pPipelineCache = 0xCA000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipelineCache(void* /*device*/, uint64_t /*pipelineCache*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkGetPipelineCacheData(void* /*device*/, uint64_t /*pipelineCache*/, size_t* pDataSize, void* /*pData*/) {
    if (pDataSize) *pDataSize = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkMergePipelineCaches(void* /*device*/, uint64_t /*dstCache*/, uint32_t /*srcCacheCount*/, const uint64_t* /*pSrcCaches*/) {
    return 0;
}

__attribute__((visibility("default")))
void vkGetImageSubresourceLayout(void* /*device*/, uint64_t /*image*/, const void* /*pSubresource*/, void* pLayout) {
    if (!pLayout) return;
    struct SubLayout { uint64_t offset, size, rowPitch, arrayPitch, depthPitch; };
    auto* l = static_cast<SubLayout*>(pLayout);
    l->offset = 0;
    l->size = 64 * 64 * 4;
    l->rowPitch = 64 * 4;
    l->arrayPitch = 64 * 64 * 4;
    l->depthPitch = 64 * 64 * 4;
}

__attribute__((visibility("default")))
int32_t vkFlushMappedMemoryRanges(void* /*device*/, uint32_t /*memoryRangeCount*/, const void* /*pMemoryRanges*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkInvalidateMappedMemoryRanges(void* /*device*/, uint32_t /*memoryRangeCount*/, const void* /*pMemoryRanges*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkGetFenceStatus(void* /*device*/, uint64_t /*fence*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkCreateHeadlessSurfaceEXT(void* /*instance*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSurface) {
    if (!pSurface) return -3;
    *pSurface = reinterpret_cast<uint64_t>(new R54p3Surface());
    return 0;
}
__attribute__((visibility("default")))
int32_t vkCreateDisplayPlaneSurfaceKHR(void* i, const void* ci, const void* a, uint64_t* s) { return vkCreateHeadlessSurfaceEXT(i, ci, a, s); }
__attribute__((visibility("default")))
int32_t vkCreateXcbSurfaceKHR(void* /*i*/, const void* ci, const void* /*a*/, uint64_t* s) {
    if (!s) return -3;
    auto* surf = new R54p3Surface();
    if (ci) {
        struct VkXcbSurfaceCreateInfoKHR_Icd { uint32_t sType; const void* pNext; uint32_t flags; void* connection; uint32_t window; };
        const auto* xci = static_cast<const VkXcbSurfaceCreateInfoKHR_Icd*>(ci);
        surf->xcb_conn   = xci->connection;
        surf->xcb_window = xci->window;
        uint32_t w = 500U, h = 500U;
        uint8_t depth = 24U;
        if (g_r54_xcb_lib.queryWindowGeometry(surf->xcb_conn, surf->xcb_window, &w, &h, &depth)) {
            surf->width     = w;
            surf->height    = h;
            surf->xcb_depth = depth;
        }
    }
    *s = reinterpret_cast<uint64_t>(surf);
    return 0;
}
__attribute__((visibility("default")))
int32_t vkCreateXlibSurfaceKHR(void* i, const void* ci, const void* a, uint64_t* s) { return vkCreateHeadlessSurfaceEXT(i, ci, a, s); }
__attribute__((visibility("default")))
int32_t vkCreateWaylandSurfaceKHR(void* i, const void* ci, const void* a, uint64_t* s) { return vkCreateHeadlessSurfaceEXT(i, ci, a, s); }
__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(void*, uint32_t, void*, uint32_t) { return 1U; }
__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceXlibPresentationSupportKHR(void*, uint32_t, void*, uint64_t) { return 1U; }
__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceWaylandPresentationSupportKHR(void*, uint32_t, void*) { return 1U; }

__attribute__((visibility("default")))
void vkDestroySurfaceKHR(void* /*instance*/, uint64_t surface, const void* /*pAllocator*/) {
    if (!surface) return;
    if (*reinterpret_cast<const uint64_t*>(surface) == R54P3_SURFACE_MAGIC) {
        delete reinterpret_cast<R54p3Surface*>(surface);
    }
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceSupportKHR(void* /*pd*/, uint32_t /*qfi*/, uint64_t /*surf*/, uint32_t* pSupported) {
    if (!pSupported) return -3;
    *pSupported = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceCapabilitiesKHR(void* /*pd*/, uint64_t surface, void* pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return -3;
    uint32_t w = 64U, h = 64U;
    resolveR54SurfaceInfo(surface, &w, &h, nullptr, nullptr, nullptr);
    struct SurfCaps {
        uint32_t minImageCount, maxImageCount;
        uint32_t currentExtent[2], minImageExtent[2], maxImageExtent[2];
        uint32_t maxImageArrayLayers, supportedTransforms, currentTransform;
        uint32_t supportedCompositeAlpha, supportedUsageFlags;
    };
    auto* caps = static_cast<SurfCaps*>(pSurfaceCapabilities);
    caps->minImageCount = 2U;
    caps->maxImageCount = 4U;
    caps->currentExtent[0] = w; caps->currentExtent[1] = h;
    caps->minImageExtent[0] = 16U; caps->minImageExtent[1] = 16U;
    caps->maxImageExtent[0] = 4096U; caps->maxImageExtent[1] = 4096U;
    caps->maxImageArrayLayers = 1U;
    caps->supportedTransforms = 1U;
    caps->currentTransform = 1U;
    caps->supportedCompositeAlpha = 1U;
    caps->supportedUsageFlags = 0x9FU;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceFormatsKHR(void* /*pd*/, uint64_t /*surf*/, uint32_t* pCount, void* pFormats) {
    if (!pCount) return -3;
    struct SurfFmt { uint32_t format; uint32_t colorSpace; };
    static const SurfFmt kFmts[] = {{37U, 0U}, {44U, 0U}};
    if (!pFormats) { *pCount = 2U; return 0; }
    uint32_t n = std::min<uint32_t>(*pCount, 2U);
    std::memcpy(pFormats, kFmts, n * sizeof(SurfFmt));
    *pCount = n;
    return (n < 2U) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfacePresentModesKHR(void* /*pd*/, uint64_t /*surf*/, uint32_t* pCount, uint32_t* pModes) {
    if (!pCount) return -3;
    static const uint32_t kModes[] = {2U, 0U};
    if (!pModes) { *pCount = 2U; return 0; }
    uint32_t n = std::min<uint32_t>(*pCount, 2U);
    std::memcpy(pModes, kModes, n * sizeof(uint32_t));
    *pCount = n;
    return (n < 2U) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSwapchain) {
    if (!device || !pCreateInfo || !pSwapchain) return -3;
    auto* dev = static_cast<vulkan::Device*>(device);
    struct SwapchainCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t surface;
        uint32_t minImageCount; uint32_t imageFormat; uint32_t imageColorSpace;
        uint32_t imageExtent[2]; uint32_t imageArrayLayers; uint32_t imageUsage;
    };
    const auto* sci = static_cast<const SwapchainCreateInfo*>(pCreateInfo);
    auto* sc = new R54p3Swapchain();
    sc->device = dev;
    uint32_t surf_w = 64U, surf_h = 64U;
    resolveR54SurfaceInfo(sci->surface, &surf_w, &surf_h, &sc->xcb_conn, &sc->xcb_window, &sc->xcb_depth);
    sc->width  = std::max<uint32_t>(16U, sci->imageExtent[0] ? sci->imageExtent[0] : surf_w);
    sc->height = std::max<uint32_t>(16U, sci->imageExtent[1] ? sci->imageExtent[1] : surf_h);
    sc->format = sci->imageFormat ? sci->imageFormat : 37U;
    uint32_t img_count = std::max<uint32_t>(2U, sci->minImageCount);
    uint64_t byte_sz = std::max<uint64_t>(4096ULL, (uint64_t(sc->width) * sc->height * 4ULL + 4095ULL) & ~4095ULL);

    int heap_fd = static_cast<int>(mali_r54p3::g_r54p3_kbase_emu.handleOpen("/dev/dma_heap/system", 2));
    for (uint32_t i = 0; i < img_count; ++i) {
        DmaHeapAllocationData dreq{byte_sz, 0, 2, 0};
        mali_r54p3::g_r54p3_kbase_emu.handleIoctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, reinterpret_cast<uint64_t>(&dreq));
        int dmabuf_fd = static_cast<int>(dreq.fd);
        sc->dmabuf_fds.push_back(dmabuf_fd);

        uint64_t umm_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
        KbaseIoctlMemImport imp{};
        imp.in.flags = umm_flags;
        imp.in.phandle = reinterpret_cast<uint64_t>(&dmabuf_fd);
        imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;
        mali_r54p3::g_r54p3_kbase_emu.handleIoctl(dev->kctx.fd, 0xc0188016U, reinterpret_cast<uint64_t>(&imp));
        uint64_t w_gpu_va = mali_r54p3::g_r54p3_kbase_emu.handleMmap(0, imp.out.va_pages * 4096ULL, 3, 1, dev->kctx.fd, imp.out.gpu_va);

        auto* img = new vulkan::Image();
        img->width        = sc->width;
        img->height       = sc->height;
        img->format       = sc->format;
        img->byte_size    = byte_sz;
        img->bound_gpu_va = w_gpu_va;
        sc->images.push_back(img);
    }
    *pSwapchain = reinterpret_cast<uint64_t>(sc);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroySwapchainKHR(void* /*device*/, uint64_t swapchain, const void* /*pAllocator*/) {
    auto* sc = reinterpret_cast<R54p3Swapchain*>(swapchain);
    if (!sc) return;
    for (auto* img : sc->images) delete img;
    delete sc;
}

__attribute__((visibility("default")))
int32_t vkGetSwapchainImagesKHR(void* /*device*/, uint64_t swapchain, uint32_t* pCount, uint64_t* pImages) {
    if (!swapchain || !pCount) return -3;
    auto* sc = reinterpret_cast<R54p3Swapchain*>(swapchain);
    uint32_t total = static_cast<uint32_t>(sc->images.size());
    if (!pImages) { *pCount = total; return 0; }
    uint32_t n = std::min(*pCount, total);
    for (uint32_t i = 0; i < n; ++i) pImages[i] = reinterpret_cast<uint64_t>(sc->images[i]);
    *pCount = n;
    return (n < total) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkAcquireNextImageKHR(void* /*device*/, uint64_t swapchain, uint64_t /*timeout*/, uint64_t /*sem*/, uint64_t /*fence*/, uint32_t* pImageIndex) {
    if (!swapchain || !pImageIndex) return -3;
    auto* sc = reinterpret_cast<R54p3Swapchain*>(swapchain);
    *pImageIndex = sc->next_image_idx;
    sc->next_image_idx = (sc->next_image_idx + 1U) % static_cast<uint32_t>(sc->images.size());
    return 0;
}

__attribute__((visibility("default")))
int32_t vkAcquireNextImage2KHR(void* device, const void* pAcquireInfo, uint32_t* pImageIndex) {
    if (!pAcquireInfo || !pImageIndex) return -3;
    struct Acq2 { uint32_t sType; const void* pNext; uint64_t swapchain; uint64_t timeout; uint64_t sem; uint64_t fence; uint32_t mask; };
    const auto* a2 = static_cast<const Acq2*>(pAcquireInfo);
    return vkAcquireNextImageKHR(device, a2->swapchain, a2->timeout, a2->sem, a2->fence, pImageIndex);
}

__attribute__((visibility("default")))
int32_t vkQueuePresentKHR(void* /*queue*/, const void* pPresentInfo) {
    if (!pPresentInfo) return -3;
    struct PresInfo {
        uint32_t sType; const void* pNext;
        uint32_t waitSemCount; const uint64_t* pWaitSems;
        uint32_t swapchainCount; const uint64_t* pSwapchains;
        const uint32_t* pImageIndices; int32_t* pResults;
    };
    const auto* pi = static_cast<const PresInfo*>(pPresentInfo);
    for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
        auto* sc = reinterpret_cast<R54p3Swapchain*>(pi->pSwapchains[i]);
        uint32_t idx = pi->pImageIndices ? pi->pImageIndices[i] : 0U;
        if (sc && idx < sc->images.size() && sc->xcb_conn && sc->xcb_window && g_r54_xcb_lib.ensureLoaded()) {
            const auto* img = sc->images[idx];
            if (img && img->bound_gpu_va) {
                if (sc->xcb_gc == 0) {
                    sc->xcb_gc = g_r54_xcb_lib.generate_id(sc->xcb_conn);
                    g_r54_xcb_lib.create_gc(sc->xcb_conn, sc->xcb_gc, sc->xcb_window, 0, nullptr);
                }
                const uint32_t* src_px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                std::vector<uint32_t> bgra_px(img->width * img->height);
                for (uint32_t p = 0; p < img->width * img->height; ++p) {
                    uint32_t v = src_px[p];
                    bgra_px[p] = (v & 0xFF00FF00U) | ((v & 0xFFU) << 16) | ((v >> 16) & 0xFFU);
                }
                uint32_t rows_per_band = std::max<uint32_t>(1U, 65536U / (img->width * 4U));
                for (uint32_t y = 0; y < img->height; y += rows_per_band) {
                    uint32_t band_h = std::min<uint32_t>(rows_per_band, img->height - y);
                    g_r54_xcb_lib.put_image(sc->xcb_conn, 2, sc->xcb_window, sc->xcb_gc,
                                            static_cast<uint16_t>(img->width), static_cast<uint16_t>(band_h),
                                            0, static_cast<int16_t>(y), 0, sc->xcb_depth ? sc->xcb_depth : 24,
                                            img->width * band_h * 4U,
                                            reinterpret_cast<const uint8_t*>(bgra_px.data() + y * img->width));
                }
                g_r54_xcb_lib.flush(sc->xcb_conn);
            }
        }
        if (pi->pResults) pi->pResults[i] = 0;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkQueueWaitIdle(void* /*queue*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkDeviceWaitIdle(void* /*device*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkCreateFence(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pFence) {
    if (!pFence) return -3;
    *pFence = 0xFE000001ULL;
    return 0;
}
__attribute__((visibility("default")))
void vkDestroyFence(void* /*device*/, uint64_t /*fence*/, const void* /*pAllocator*/) {}
__attribute__((visibility("default")))
int32_t vkWaitForFences(void* /*device*/, uint32_t /*fenceCount*/, const uint64_t* /*pFences*/, uint32_t /*waitAll*/, uint64_t /*timeout*/) { return 0; }
__attribute__((visibility("default")))
int32_t vkResetFences(void* /*device*/, uint32_t /*fenceCount*/, const uint64_t* /*pFences*/) { return 0; }
__attribute__((visibility("default")))
int32_t vkCreateSemaphore(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSemaphore) {
    if (!pSemaphore) return -3;
    *pSemaphore = 0x5E000001ULL;
    return 0;
}
__attribute__((visibility("default")))
void vkDestroySemaphore(void* /*device*/, uint64_t /*semaphore*/, const void* /*pAllocator*/) {}

typedef void (*PFN_vkVoidFunction)(void);

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(void* /*device*/, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceQueue);
    if (std::strcmp(pName, "vkAllocateMemory") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateMemory);
    if (std::strcmp(pName, "vkFreeMemory") == 0)                return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeMemory);
    if (std::strcmp(pName, "vkMapMemory") == 0)                 return reinterpret_cast<PFN_vkVoidFunction>(&vkMapMemory);
    if (std::strcmp(pName, "vkUnmapMemory") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkUnmapMemory);
    if (std::strcmp(pName, "vkCreateBuffer") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateBuffer);
    if (std::strcmp(pName, "vkDestroyBuffer") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyBuffer);
    if (std::strcmp(pName, "vkGetBufferMemoryRequirements") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetBufferMemoryRequirements);
    if (std::strcmp(pName, "vkBindBufferMemory") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkBindBufferMemory);
    if (std::strcmp(pName, "vkGetBufferDeviceAddress") == 0 ||
        std::strcmp(pName, "vkGetBufferDeviceAddressKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetBufferDeviceAddress);
    if (std::strcmp(pName, "vkCreateImage") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImage);
    if (std::strcmp(pName, "vkDestroyImage") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImage);
    if (std::strcmp(pName, "vkGetImageMemoryRequirements") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageMemoryRequirements);
    if (std::strcmp(pName, "vkBindImageMemory") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkBindImageMemory);
    if (std::strcmp(pName, "vkCreateImageView") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
    if (std::strcmp(pName, "vkDestroyImageView") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImageView);
    if (std::strcmp(pName, "vkCreateSampler") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSampler);
    if (std::strcmp(pName, "vkDestroySampler") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySampler);
    if (std::strcmp(pName, "vkCreateShaderModule") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateShaderModule);
    if (std::strcmp(pName, "vkDestroyShaderModule") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyShaderModule);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkDestroyDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorSetLayout);
    if (std::strcmp(pName, "vkCreatePipelineLayout") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineLayout);
    if (std::strcmp(pName, "vkDestroyPipelineLayout") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineLayout);
    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkDestroyDescriptorPool") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorPool);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkFreeDescriptorSets") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCreateRenderPass") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateRenderPass);
    if (std::strcmp(pName, "vkDestroyRenderPass") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyRenderPass);
    if (std::strcmp(pName, "vkCreateFramebuffer") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateFramebuffer);
    if (std::strcmp(pName, "vkDestroyFramebuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyFramebuffer);
    if (std::strcmp(pName, "vkCreateComputePipelines") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
    if (std::strcmp(pName, "vkCreateGraphicsPipelines") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
    if (std::strcmp(pName, "vkDestroyPipeline") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipeline);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateQueryPool);
    if (std::strcmp(pName, "vkDestroyQueryPool") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyQueryPool);
    if (std::strcmp(pName, "vkGetQueryPoolResults") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkGetQueryPoolResults);
    if (std::strcmp(pName, "vkCreateCommandPool") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateCommandPool);
    if (std::strcmp(pName, "vkDestroyCommandPool") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyCommandPool);
    if (std::strcmp(pName, "vkAllocateCommandBuffers") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateCommandBuffers);
    if (std::strcmp(pName, "vkFreeCommandBuffers") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeCommandBuffers);
    if (std::strcmp(pName, "vkResetCommandBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkResetCommandBuffer);
    if (std::strcmp(pName, "vkBeginCommandBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkBeginCommandBuffer);
    if (std::strcmp(pName, "vkEndCommandBuffer") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkEndCommandBuffer);
    if (std::strcmp(pName, "vkCmdBeginRenderPass") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRenderPass);
    if (std::strcmp(pName, "vkCmdEndRenderPass") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRenderPass);
    if (std::strcmp(pName, "vkCmdBindPipeline") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindPipeline);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindVertexBuffers") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers);
    if (std::strcmp(pName, "vkCmdBindIndexBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindIndexBuffer);
    if (std::strcmp(pName, "vkCmdPushConstants") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPushConstants);
    if (std::strcmp(pName, "vkCmdSetViewport") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetScissor") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetScissor);
    if (std::strcmp(pName, "vkCmdSetColorWriteEnableEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetColorWriteEnableEXT);
    if (std::strcmp(pName, "vkCmdBeginConditionalRenderingEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginConditionalRenderingEXT);
    if (std::strcmp(pName, "vkCmdEndConditionalRenderingEXT") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndConditionalRenderingEXT);
    if (std::strcmp(pName, "vkCmdDraw") == 0)                   return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDraw);
    if (std::strcmp(pName, "vkCmdDrawIndexed") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDrawIndexed);
    if (std::strcmp(pName, "vkCmdDispatch") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDispatch);
    if (std::strcmp(pName, "vkCmdCopyBuffer") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBuffer);
    if (std::strcmp(pName, "vkCmdCopyBufferToImage") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage);
    if (std::strcmp(pName, "vkCmdBlitImage") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBlitImage);
    if (std::strcmp(pName, "vkCmdWriteTimestamp") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdWriteTimestamp);
    if (std::strcmp(pName, "vkCmdPipelineBarrier") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPipelineBarrier);
    if (std::strcmp(pName, "vkQueueSubmit") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit);
    if (std::strcmp(pName, "vkQueueWaitIdle") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueWaitIdle);
    if (std::strcmp(pName, "vkDeviceWaitIdle") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkDeviceWaitIdle);
    if (std::strcmp(pName, "vkCreateFence") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateFence);
    if (std::strcmp(pName, "vkDestroyFence") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyFence);
    if (std::strcmp(pName, "vkWaitForFences") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkWaitForFences);
    if (std::strcmp(pName, "vkResetFences") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkResetFences);
    if (std::strcmp(pName, "vkCreateSemaphore") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSemaphore);
    if (std::strcmp(pName, "vkDestroySemaphore") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySemaphore);
    if (std::strcmp(pName, "vkSetDebugUtilsObjectNameEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkSetDebugUtilsObjectNameEXT);
    if (std::strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineCache);
    if (std::strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineCache);
    if (std::strcmp(pName, "vkGetPipelineCacheData") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPipelineCacheData);
    if (std::strcmp(pName, "vkMergePipelineCaches") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkMergePipelineCaches);
    if (std::strcmp(pName, "vkGetImageSubresourceLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageSubresourceLayout);
    if (std::strcmp(pName, "vkFlushMappedMemoryRanges") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkFlushMappedMemoryRanges);
    if (std::strcmp(pName, "vkInvalidateMappedMemoryRanges") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkInvalidateMappedMemoryRanges);
    if (std::strcmp(pName, "vkGetFenceStatus") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetFenceStatus);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkGetSwapchainImagesKHR);
    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImageKHR);
    if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImage2KHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
    return nullptr;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceVersion") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceVersion);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceExtensionProperties);
    if (std::strcmp(pName, "vkCreateInstance") == 0)                       return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)                      return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumeratePhysicalDevices);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 || std::strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 || std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceQueueFamilyProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceMemoryProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFormatProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceImageFormatProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceLayerProperties") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceLayerProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSparseImageFormatProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSparseImageFormatProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkCreateDevice") == 0)                         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDevice);
    if (std::strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateHeadlessSurfaceEXT);
    if (std::strcmp(pName, "vkGetPhysicalDeviceDisplayPropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceDisplayPropertiesKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    if (std::strcmp(pName, "vkGetDisplayPlaneSupportedDisplaysKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayPlaneSupportedDisplaysKHR);
    if (std::strcmp(pName, "vkGetDisplayModePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayModePropertiesKHR);
    if (std::strcmp(pName, "vkCreateDisplayModeKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDisplayModeKHR);
    if (std::strcmp(pName, "vkGetDisplayPlaneCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayPlaneCapabilitiesKHR);
    if (std::strcmp(pName, "vkCreateDebugUtilsMessengerEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDebugUtilsMessengerEXT);
    if (std::strcmp(pName, "vkDestroyDebugUtilsMessengerEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDebugUtilsMessengerEXT);
    if (std::strcmp(pName, "vkSetDebugUtilsObjectNameEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkSetDebugUtilsObjectNameEXT);
    if (std::strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCreateDisplayPlaneSurfaceKHR") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDisplayPlaneSurfaceKHR);
    if (std::strcmp(pName, "vkCreateXcbSurfaceKHR") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateXcbSurfaceKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceXcbPresentationSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceXlibPresentationSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceWaylandPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    if (std::strcmp(pName, "vkCreateXlibSurfaceKHR") == 0)                 return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateXlibSurfaceKHR);
    if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateWaylandSurfaceKHR);
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)                    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySurfaceKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceFormatsKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfacePresentModesKHR);
    return vkGetDeviceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void* instance, const char* pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(void* instance, const char* pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (!pSupportedVersion) return -3;
    if (*pSupportedVersion > 6U) *pSupportedVersion = 6U;
    return 0;
}

} // extern "C"
