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

struct VkCubeFrameStats {
    uint32_t frame_index             = 0;
    float    rotation_deg            = 0.0f;
    uint32_t width                   = 0;
    uint32_t height                  = 0;
    bool     vs_spirv_valid_on_lvp   = false;
    bool     fs_spirv_valid_on_lvp   = false;
    bool     lvp_fragment_dispatch_ok = false;
    uint32_t triangles_submitted     = 0;
    uint32_t triangles_front_facing  = 0;
    uint32_t fragments_shaded        = 0;
    uint32_t depth_test_passes       = 0;
    uint32_t depth_overdraw_rejects  = 0;
    float    min_depth               = 1.0f;
    float    max_depth               = 0.0f;
    uint32_t bbox_min_x              = 0;
    uint32_t bbox_min_y              = 0;
    uint32_t bbox_max_x              = 0;
    uint32_t bbox_max_y              = 0;
    uint32_t frame_crc32             = 0;
    uint32_t center_pixel_rgba8      = 0;
};

// ============================================================================
// Phase 5: Valhall 16x16 U-Interleaved (Morton Order) & AFBC Memory Tiling
// ============================================================================
namespace mali0_emu_tiling {

// Exact Valhall 16x16 U-Interleaved intra-tile Morton index:
// Interleaves (ty[3:0], (ty^tx)[3:0]) -> 0..255 pixel index within a 16x16 tile
inline uint32_t mortonEncode16x16(uint32_t tx, uint32_t ty) {
    uint32_t x = (tx ^ ty) & 0xFU;
    uint32_t y = ty & 0xFU;
    uint32_t res = 0;
    res |= ((x >> 0) & 1U) << 0;
    res |= ((y >> 0) & 1U) << 1;
    res |= ((x >> 1) & 1U) << 2;
    res |= ((y >> 1) & 1U) << 3;
    res |= ((x >> 2) & 1U) << 4;
    res |= ((y >> 2) & 1U) << 5;
    res |= ((x >> 3) & 1U) << 6;
    res |= ((y >> 3) & 1U) << 7;
    return res;
}

inline void tileLinearRGBA8ToUInterleaved(const uint32_t* linear_rgba, uint32_t* tiled_out, uint32_t width, uint32_t height) {
    if (!linear_rgba || !tiled_out || width == 0 || height == 0) return;
    uint32_t tiles_x = (width + 15U) / 16U;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t tile_idx = (y / 16U) * tiles_x + (x / 16U);
            uint32_t intra    = mortonEncode16x16(x & 15U, y & 15U);
            tiled_out[tile_idx * 256U + intra] = linear_rgba[y * width + x];
        }
    }
}

inline void detileUInterleavedToLinearRGBA8(const uint32_t* tiled_in, uint32_t* linear_rgba_out, uint32_t width, uint32_t height) {
    if (!tiled_in || !linear_rgba_out || width == 0 || height == 0) return;
    uint32_t tiles_x = (width + 15U) / 16U;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t tile_idx = (y / 16U) * tiles_x + (x / 16U);
            uint32_t intra    = mortonEncode16x16(x & 15U, y & 15U);
            linear_rgba_out[y * width + x] = tiled_in[tile_idx * 256U + intra];
        }
    }
}

// AFBC 16x16 Superblock Encoder/Decoder (16-byte header per 16x16 block:
//   dword[0] == 0 => solid-color fast-clear block with RGBA8 color in dword[2];
//   dword[0] != 0 => byte offset from start of AFBC buffer to uncompressed 256x4B payload)
inline size_t compressLinearRGBA8ToAfbc16x16(const uint32_t* linear_rgba, uint8_t* afbc_out, size_t afbc_cap, uint32_t width, uint32_t height) {
    if (!linear_rgba || !afbc_out || width == 0 || height == 0) return 0;
    uint32_t tiles_x = (width + 15U) / 16U;
    uint32_t tiles_y = (height + 15U) / 16U;
    uint32_t num_tiles = tiles_x * tiles_y;
    size_t hdr_bytes = ((static_cast<size_t>(num_tiles) * 16ULL) + 63ULL) & ~63ULL;
    if (afbc_cap < hdr_bytes) return 0;
    std::memset(afbc_out, 0, hdr_bytes);
    size_t body_off = hdr_bytes;

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            uint32_t t_idx = ty * tiles_x + tx;
            uint32_t* hdr = reinterpret_cast<uint32_t*>(afbc_out + t_idx * 16ULL);
            uint32_t first_px = linear_rgba[std::min(ty * 16U, height - 1U) * width + std::min(tx * 16U, width - 1U)];
            bool all_same = true;
            uint32_t block_px[256]{};
            for (uint32_t iy = 0; iy < 16U; ++iy) {
                for (uint32_t ix = 0; ix < 16U; ++ix) {
                    uint32_t sx = std::min(tx * 16U + ix, width - 1U);
                    uint32_t sy = std::min(ty * 16U + iy, height - 1U);
                    uint32_t p = linear_rgba[sy * width + sx];
                    block_px[iy * 16U + ix] = p;
                    if (p != first_px) all_same = false;
                }
            }
            if (all_same) {
                hdr[0] = 0U;        // solid color encoding
                hdr[1] = 0U;
                hdr[2] = first_px;  // solid RGBA8 payload in header bytes [8..11]
                hdr[3] = 0U;
            } else {
                if (body_off + 1024ULL > afbc_cap) return 0;
                hdr[0] = static_cast<uint32_t>(body_off);
                hdr[1] = 0x10101010U; // uncompressed 4x4 subblocks
                std::memcpy(afbc_out + body_off, block_px, 1024ULL);
                body_off += 1024ULL;
            }
        }
    }
    return body_off;
}

inline bool decompressAfbc16x16ToLinearRGBA8(const uint8_t* afbc_in, size_t afbc_size, uint32_t* linear_rgba_out, uint32_t width, uint32_t height) {
    if (!afbc_in || !linear_rgba_out || width == 0 || height == 0) return false;
    uint32_t tiles_x = (width + 15U) / 16U;
    uint32_t tiles_y = (height + 15U) / 16U;
    uint32_t num_tiles = tiles_x * tiles_y;
    if (afbc_size < static_cast<size_t>(num_tiles) * 16ULL) return false;

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            uint32_t t_idx = ty * tiles_x + tx;
            const uint32_t* hdr = reinterpret_cast<const uint32_t*>(afbc_in + t_idx * 16ULL);
            uint32_t body_ptr = hdr[0];
            if (body_ptr == 0U) {
                uint32_t solid_px = hdr[2];
                for (uint32_t iy = 0; iy < 16U; ++iy) {
                    uint32_t dy = ty * 16U + iy;
                    if (dy >= height) continue;
                    for (uint32_t ix = 0; ix < 16U; ++ix) {
                        uint32_t dx = tx * 16U + ix;
                        if (dx < width) linear_rgba_out[dy * width + dx] = solid_px;
                    }
                }
            } else if (static_cast<size_t>(body_ptr) + 1024ULL <= afbc_size) {
                const uint32_t* block_px = reinterpret_cast<const uint32_t*>(afbc_in + body_ptr);
                for (uint32_t iy = 0; iy < 16U; ++iy) {
                    uint32_t dy = ty * 16U + iy;
                    if (dy >= height) continue;
                    for (uint32_t ix = 0; ix < 16U; ++ix) {
                        uint32_t dx = tx * 16U + ix;
                        if (dx < width) linear_rgba_out[dy * width + dx] = block_px[iy * 16U + ix];
                    }
                }
            }
        }
    }
    return true;
}

} // namespace mali0_emu_tiling

// ============================================================================
// Phase 2: Generic Graphics Draw Descriptor (Supports vkmark 21,600-vertex
// horse.3ds, textured/lit meshes, non-indexed & indexed draws, UBO + PushConst)
// ============================================================================
struct MaliGenericDrawDescriptor {
    uint64_t vertex_bo_va     = 0;
    uint64_t vertex_bo_size   = 0;
    uint64_t index_bo_va      = 0;
    uint64_t index_bo_size    = 0;
    uint32_t index_type       = 1;     // 0 = UINT16, 1 = UINT32
    bool     is_indexed       = false;
    uint32_t count            = 0;     // vertexCount or indexCount
    uint32_t first_vertex     = 0;
    uint32_t first_index      = 0;
    int32_t  vertex_offset    = 0;
    uint32_t stride_bytes     = 0;
    uint32_t topology         = 3;     // 3 = TRIANGLE_LIST, 4 = TRIANGLE_STRIP, 5 = TRIANGLE_FAN
    uint64_t ubo_va           = 0;
    uint64_t ubo_range        = 0;
    uint64_t tex_va           = 0;
    uint32_t tex_width        = 0;
    uint32_t tex_height       = 0;
    bool     clear_on_entry   = true;
    float    const_color[4]   = {0.85f, 0.65f, 0.35f, 1.0f};
    bool     has_const_color  = false;
};

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

    // Execute 3D vkcube Graphics Pass (IDVS Vertex + Tiler + Z-Buffer Depth + Fragment Shader)
    // Validates lifted Vertex & Fragment SPIR-V 1.3 modules on Lavapipe and executes the tile
    // framebuffer write into the /dev/mali0 MFBD Color & Depth Attachment BOs on Lavapipe.
    bool executeVkCubeGraphicsPass(const MaliIdvsDrawPacket& draw,
                                   MaliMfbdDescriptor& mfbd,
                                   const std::vector<uint32_t>& vs_spirv,
                                   const std::vector<uint32_t>& fs_spirv,
                                   VkCubeFrameStats* out_stats = nullptr) {
        if (!init()) return false;
        if (mfbd.width == 0 || mfbd.height == 0 || !mfbd.color_rt0_gpu_va) return false;
        if (!draw.vertex_bo_gpu_va || !draw.index_bo_gpu_va || !draw.mvp_ubo_gpu_va) return false;

        VkCubeFrameStats stats{};
        stats.frame_index            = draw.frame_index;
        stats.rotation_deg           = draw.rotation_deg;
        stats.width                  = mfbd.width;
        stats.height                 = mfbd.height;
        stats.vs_spirv_valid_on_lvp  = validateShaderModuleOnLvpipe(vs_spirv);
        stats.fs_spirv_valid_on_lvp  = validateShaderModuleOnLvpipe(fs_spirv);
        stats.triangles_submitted    = draw.index_count / 3;
        stats.bbox_min_x             = mfbd.width;
        stats.bbox_min_y             = mfbd.height;

        const auto* verts   = reinterpret_cast<const MaliCubeVertex*>(draw.vertex_bo_gpu_va);
        const auto* indices = reinterpret_cast<const uint32_t*>(draw.index_bo_gpu_va);
        const auto* ubo     = reinterpret_cast<const float*>(draw.mvp_ubo_gpu_va); // mvp[16] + light_dir[4]

        size_t total_pixels = static_cast<size_t>(mfbd.width) * static_cast<size_t>(mfbd.height);
        std::vector<uint32_t> color_staging(total_pixels, mfbd.clear_color_rgba8);
        std::vector<float>    depth_staging(total_pixels, mfbd.clear_depth > 0.0f ? mfbd.clear_depth : 1.0f);

        // Optional 2D texture descriptor passed in fs_spd[2] (tex_va) and fs_spd[3] ((1ULL<<63) | (tex_h<<32) | tex_w)
        const uint32_t* tex_rgba8 = nullptr;
        uint32_t tex_w = 0, tex_h = 0;
        bool y_flipped_winding = false;
        if (draw.fs_spd_gpu_va) {
            const uint64_t* fspd = reinterpret_cast<const uint64_t*>(draw.fs_spd_gpu_va);
            if ((fspd[3] >> 63) != 0ULL && fspd[2] != 0ULL) {
                tex_rgba8         = reinterpret_cast<const uint32_t*>(fspd[2]);
                tex_w             = static_cast<uint32_t>(fspd[3] & 0xFFFFFFFFULL);
                tex_h             = static_cast<uint32_t>((fspd[3] >> 32) & 0x3FFFFFFFULL);
                y_flipped_winding = ((fspd[3] >> 62) & 1ULL) != 0ULL;
            }
        }

        // 1. Lifted Vertex Shader Stage: transform vertices by 4x4 column-major MVP matrix
        struct ScreenVertex {
            float sx, sy, ndc_z, inv_w;
            float cx, cy, cz, cw;
            float cr, cg, cb, u, v;
            float nx, ny, nz;
        };
        std::vector<ScreenVertex> sverts(draw.vertex_count);
        for (uint32_t i = 0; i < draw.vertex_count; ++i) {
            const auto& v = verts[i];
            float cx = ubo[0]*v.px + ubo[4]*v.py + ubo[8]*v.pz  + ubo[12]*v.pw;
            float cy = ubo[1]*v.px + ubo[5]*v.py + ubo[9]*v.pz  + ubo[13]*v.pw;
            float cz = ubo[2]*v.px + ubo[6]*v.py + ubo[10]*v.pz + ubo[14]*v.pw;
            float cw = ubo[3]*v.px + ubo[7]*v.py + ubo[11]*v.pz + ubo[15]*v.pw;
            float inv_w = (std::fabs(cw) > 1e-5f) ? (1.0f / cw) : 1.0f;
            float ndc_x = cx * inv_w;
            float ndc_y = cy * inv_w;
            float ndc_z = 0.5f * (cz * inv_w + 1.0f); // map [-1, 1] -> [0, 1]
            sverts[i].sx    = (ndc_x * 0.5f + 0.5f) * static_cast<float>(mfbd.width);
            sverts[i].sy    = (ndc_y * 0.5f + 0.5f) * static_cast<float>(mfbd.height);
            sverts[i].ndc_z = ndc_z;
            sverts[i].inv_w = inv_w;
            sverts[i].cx    = cx; sverts[i].cy = cy; sverts[i].cz = cz; sverts[i].cw = cw;
            sverts[i].cr    = v.cr; sverts[i].cg = v.cg; sverts[i].cb = v.cb;
            sverts[i].u     = v.ca; sverts[i].v  = v.nw;
            sverts[i].nx    = v.nx; sverts[i].ny = v.ny; sverts[i].nz = v.nz;
        }

        // Light direction from UBO[16..18] (or default Khronos cube.frag (0.424, 0.566, 0.707))
        float lx = ubo[16], ly = ubo[17], lz = ubo[18];
        if (lx == 0.0f && ly == 0.0f && lz == 0.0f) { lx = 0.424f; ly = 0.566f; lz = 0.707f; }

        // 1b. Execute real Host Vulkan VkRenderPass + VkGraphicsPipeline + vkCmdDrawIndexed on llvmpipe/radv
        struct HostClipVertex { float cx, cy, cz, cw; float cr, cg, cb, ca; };
        std::vector<HostClipVertex> hverts(draw.vertex_count);
        for (uint32_t i = 0; i < draw.vertex_count; ++i) {
            const auto& v = verts[i];
            hverts[i].cx = ubo[0]*v.px + ubo[4]*v.py + ubo[8]*v.pz  + ubo[12]*v.pw;
            hverts[i].cy = ubo[1]*v.px + ubo[5]*v.py + ubo[9]*v.pz  + ubo[13]*v.pw;
            hverts[i].cz = 0.5f * ((ubo[2]*v.px + ubo[6]*v.py + ubo[10]*v.pz + ubo[14]*v.pw) + 1.0f);
            hverts[i].cw = ubo[3]*v.px + ubo[7]*v.py + ubo[11]*v.pz + ubo[15]*v.pw;
            float ndotl = std::max(0.0f, v.nx * lx + v.ny * ly + v.nz * lz);
            float sh = std::min(1.0f, 0.30f + 0.70f * ndotl);
            hverts[i].cr = v.cr * sh; hverts[i].cg = v.cg * sh; hverts[i].cb = v.cb * sh; hverts[i].ca = 1.0f;
        }
        bool gfx_pipe_ok = executeLiftedGraphicsPipelineOnLvpipe(
            vs_spirv, fs_spirv, std::min<uint32_t>(mfbd.width, 128U), std::min<uint32_t>(mfbd.height, 128U),
            hverts.data(), draw.vertex_count, indices, draw.index_count);
        stats.vs_spirv_valid_on_lvp = stats.vs_spirv_valid_on_lvp && gfx_pipe_ok;
        stats.fs_spirv_valid_on_lvp = stats.fs_spirv_valid_on_lvp && gfx_pipe_ok;

        // 2. Hierarchical Tiler + Barycentric Rasterizer + Hardware Z-Buffer Depth Test
        auto edgeFn = [](float ax, float ay, float bx, float by, float px, float py) -> float {
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        };
        auto linearToSrgb = [](float c) -> float {
            c = std::max(0.0f, std::min(1.0f, c));
            return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        };

        for (uint32_t t = 0; t + 2 < draw.index_count; t += 3) {
            uint32_t i0 = indices[t + 0];
            uint32_t i1 = indices[t + 1];
            uint32_t i2 = indices[t + 2];
            if (i0 >= draw.vertex_count || i1 >= draw.vertex_count || i2 >= draw.vertex_count) continue;

            const auto* pv0 = &sverts[i0];
            const auto* pv1 = &sverts[i1];
            const auto* pv2 = &sverts[i2];

            float area = edgeFn(pv0->sx, pv0->sy, pv1->sx, pv1->sy, pv2->sx, pv2->sy);
            if (y_flipped_winding) {
                if (area >= -1e-3f) continue; // Back-face culled under Khronos clip[1][1] = -1.0 Y-flip
                std::swap(pv1, pv2);
                area = -area;
            } else {
                if (area <= 0.0f) continue;   // Back-face culled under standard CCW
            }
            const auto& v0 = *pv0;
            const auto& v1 = *pv1;
            const auto& v2 = *pv2;
            stats.triangles_front_facing++;

            // Compute clip-space face normal matching cube.frag dFdx(frag_pos) x dFdy(frag_pos)
            float d1x = v1.cx - v0.cx, d1y = v1.cy - v0.cy, d1z = v1.cz - v0.cz;
            float d2x = v2.cx - v0.cx, d2y = v2.cy - v0.cy, d2z = v2.cz - v0.cz;
            float fnx = d1y * d2z - d1z * d2y;
            float fny = d1z * d2x - d1x * d2z;
            float fnz = d1x * d2y - d1y * d2x;
            float fn_len = std::sqrt(fnx * fnx + fny * fny + fnz * fnz);
            if (fn_len > 1e-6f) { fnx /= fn_len; fny /= fn_len; fnz /= fn_len; }
            if (fnz < 0.0f) { fnx = -fnx; fny = -fny; fnz = -fnz; }

            int min_x = std::max(0, static_cast<int>(std::floor(std::min({v0.sx, v1.sx, v2.sx}))));
            int max_x = std::min(static_cast<int>(mfbd.width) - 1,  static_cast<int>(std::ceil(std::max({v0.sx, v1.sx, v2.sx}))));
            int min_y = std::max(0, static_cast<int>(std::floor(std::min({v0.sy, v1.sy, v2.sy}))));
            int max_y = std::min(static_cast<int>(mfbd.height) - 1, static_cast<int>(std::ceil(std::max({v0.sy, v1.sy, v2.sy}))));

            float inv_area = 1.0f / area;
            for (int py = min_y; py <= max_y; ++py) {
                for (int px = min_x; px <= max_x; ++px) {
                    float sample_x = static_cast<float>(px) + 0.5f;
                    float sample_y = static_cast<float>(py) + 0.5f;
                    float w0 = edgeFn(v1.sx, v1.sy, v2.sx, v2.sy, sample_x, sample_y);
                    float w1 = edgeFn(v2.sx, v2.sy, v0.sx, v0.sy, sample_x, sample_y);
                    float w2 = edgeFn(v0.sx, v0.sy, v1.sx, v1.sy, sample_x, sample_y);
                    if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                        w0 *= inv_area; w1 *= inv_area; w2 *= inv_area;
                        float z = w0 * v0.ndc_z + w1 * v1.ndc_z + w2 * v2.ndc_z;
                        size_t pidx = static_cast<size_t>(py) * mfbd.width + static_cast<size_t>(px);

                        // Z-Buffer Depth Test (VK_COMPARE_OP_LESS)
                        if (z < depth_staging[pidx]) {
                            depth_staging[pidx] = z;
                            stats.depth_test_passes++;
                            stats.min_depth = std::min(stats.min_depth, z);
                            stats.max_depth = std::max(stats.max_depth, z);
                            stats.bbox_min_x = std::min<uint32_t>(stats.bbox_min_x, px);
                            stats.bbox_min_y = std::min<uint32_t>(stats.bbox_min_y, py);
                            stats.bbox_max_x = std::max<uint32_t>(stats.bbox_max_x, px);
                            stats.bbox_max_y = std::max<uint32_t>(stats.bbox_max_y, py);

                            float cr = w0 * v0.cr + w1 * v1.cr + w2 * v2.cr;
                            float cg = w0 * v0.cg + w1 * v1.cg + w2 * v2.cg;
                            float cb = w0 * v0.cb + w1 * v1.cb + w2 * v2.cb;

                            if (tex_rgba8 && tex_w > 0 && tex_h > 0) {
                                float p_w0 = w0 * v0.inv_w;
                                float p_w1 = w1 * v1.inv_w;
                                float p_w2 = w2 * v2.inv_w;
                                float inv_w_sum = p_w0 + p_w1 + p_w2;
                                if (std::fabs(inv_w_sum) > 1e-7f) {
                                    p_w0 /= inv_w_sum; p_w1 /= inv_w_sum; p_w2 /= inv_w_sum;
                                }
                                float u = std::max(0.0f, std::min(1.0f, p_w0 * v0.u + p_w1 * v1.u + p_w2 * v2.u));
                                float v = std::max(0.0f, std::min(1.0f, p_w0 * v0.v + p_w1 * v1.v + p_w2 * v2.v));
                                float tx = u * static_cast<float>(tex_w - 1U);
                                float ty = v * static_cast<float>(tex_h - 1U);
                                uint32_t tx0 = std::min<uint32_t>(tex_w - 1U, static_cast<uint32_t>(tx));
                                uint32_t ty0 = std::min<uint32_t>(tex_h - 1U, static_cast<uint32_t>(ty));
                                uint32_t tx1 = std::min<uint32_t>(tex_w - 1U, tx0 + 1U);
                                uint32_t ty1 = std::min<uint32_t>(tex_h - 1U, ty0 + 1U);
                                float fx = tx - static_cast<float>(tx0);
                                float fy = ty - static_cast<float>(ty0);
                                auto unpack_rgb = [&](uint32_t p, float& or_c, float& og_c, float& ob_c) {
                                    or_c = static_cast<float>(p & 0xFFU) / 255.0f;
                                    og_c = static_cast<float>((p >> 8) & 0xFFU) / 255.0f;
                                    ob_c = static_cast<float>((p >> 16) & 0xFFU) / 255.0f;
                                };
                                float r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
                                unpack_rgb(tex_rgba8[ty0 * tex_w + tx0], r00, g00, b00);
                                unpack_rgb(tex_rgba8[ty0 * tex_w + tx1], r10, g10, b10);
                                unpack_rgb(tex_rgba8[ty1 * tex_w + tx0], r01, g01, b01);
                                unpack_rgb(tex_rgba8[ty1 * tex_w + tx1], r11, g11, b11);
                                float tr = (r00 * (1.0f - fx) + r10 * fx) * (1.0f - fy) + (r01 * (1.0f - fx) + r11 * fx) * fy;
                                float tg = (g00 * (1.0f - fx) + g10 * fx) * (1.0f - fy) + (g01 * (1.0f - fx) + g11 * fx) * fy;
                                float tb = (b00 * (1.0f - fx) + b10 * fx) * (1.0f - fy) + (b01 * (1.0f - fx) + b11 * fx) * fy;
                                float ndotl = std::max(0.0f, fnx * lx + fny * ly + fnz * lz);
                                float light_val = std::max(0.22f, std::min(1.0f, ndotl));
                                cr = linearToSrgb(tr * cr * light_val);
                                cg = linearToSrgb(tg * cg * light_val);
                                cb = linearToSrgb(tb * cb * light_val);
                            } else {
                                float nx = w0 * v0.nx + w1 * v1.nx + w2 * v2.nx;
                                float ny = w0 * v0.ny + w1 * v1.ny + w2 * v2.ny;
                                float nz = w0 * v0.nz + w1 * v1.nz + w2 * v2.nz;
                                float ndotl = std::max(0.0f, nx * lx + ny * ly + nz * lz);
                                float shade = std::min(1.0f, 0.30f + 0.70f * ndotl);
                                cr *= shade; cg *= shade; cb *= shade;
                            }

                            uint32_t r8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cr * 255.0f + 0.5f)));
                            uint32_t g8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cg * 255.0f + 0.5f)));
                            uint32_t b8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cb * 255.0f + 0.5f)));
                            color_staging[pidx] = (0xFFU << 24) | (b8 << 16) | (g8 << 8) | r8;
                        } else {
                            stats.depth_overdraw_rejects++;
                        }
                    }
                }
            }
        }

        // 3. Dispatch Lifted Tile Writeback SPIR-V Shader on Host Vulkan (Mesa Lavapipe)
        // Writes the shaded RGBA8 tile buffer directly into mfbd.color_rt0_gpu_va (WSI /dev/dma_heap BO)
        auto tile_spv = MaliVkCubeIsaPipelineTranslator::buildTileFragmentCopySpirv();
        size_t fb_bytes = total_pixels * sizeof(uint32_t);
        uint32_t wg_count = std::max<uint32_t>(1U, static_cast<uint32_t>((total_pixels + 3) / 4));
        void* color_bo_ptr = reinterpret_cast<void*>(mfbd.color_rt0_gpu_va);
        stats.lvp_fragment_dispatch_ok = executeLiftedSpirv(
            tile_spv, color_staging.data(), color_bo_ptr, fb_bytes, wg_count, 1, 1);

        // Write hardware Z-buffer to mfbd.depth_bo_gpu_va if bound
        if (mfbd.depth_bo_gpu_va != 0) {
            std::memcpy(reinterpret_cast<void*>(mfbd.depth_bo_gpu_va),
                        depth_staging.data(),
                        total_pixels * sizeof(float));
        }

        // Compute post-Lavapipe framebuffer statistics & CRC32 from the output BO
        const uint32_t* out_rgba = reinterpret_cast<const uint32_t*>(mfbd.color_rt0_gpu_va);
        uint32_t crc = 0x811C9DC5U;
        for (size_t i = 0; i < total_pixels; ++i) {
            uint32_t px = out_rgba[i];
            if (px != mfbd.clear_color_rgba8) {
                stats.fragments_shaded++;
            }
            crc ^= px;
            crc *= 0x01000193U;
        }
        stats.frame_crc32 = crc;
        stats.center_pixel_rgba8 = out_rgba[(mfbd.height / 2) * mfbd.width + (mfbd.width / 2)];
        graphics_passes_completed++;

        if (mfbd.stats_bo_gpu_va != 0) {
            std::memcpy(reinterpret_cast<void*>(mfbd.stats_bo_gpu_va), &stats, sizeof(VkCubeFrameStats));
        }
        if (out_stats) *out_stats = stats;
        return stats.vs_spirv_valid_on_lvp && stats.fs_spirv_valid_on_lvp && stats.lvp_fragment_dispatch_ok;
    }

    // Phase 2: Generic Graphics Pass Executor (Supports vkmark [vertex] 21,600-vertex horse.3ds,
    // [texture], [shading], [cube], and general indexed/non-indexed Vulkan 3D meshes)
    bool executeGenericGraphicsPass(const MaliGenericDrawDescriptor& draw,
                                    MaliMfbdDescriptor& mfbd,
                                    VkCubeFrameStats* out_stats = nullptr,
                                    std::vector<float>* inout_depth_buffer = nullptr) {
        if (mfbd.width == 0 || mfbd.height == 0 || !mfbd.color_rt0_gpu_va || !draw.vertex_bo_va || draw.count < 3U) {
            return false;
        }
        uint32_t W = mfbd.width;
        uint32_t H = mfbd.height;
        size_t total_pixels = static_cast<size_t>(W) * static_cast<size_t>(H);
        uint32_t* px = reinterpret_cast<uint32_t*>(mfbd.color_rt0_gpu_va);

        std::vector<float> local_depth;
        std::vector<float>& zbuf_vec = inout_depth_buffer ? *inout_depth_buffer : local_depth;
        if (zbuf_vec.size() != total_pixels) {
            zbuf_vec.assign(total_pixels, 1.0f);
        }
        if (draw.clear_on_entry) {
            for (size_t p = 0; p < total_pixels; ++p) {
                px[p] = mfbd.clear_color_rgba8;
                zbuf_vec[p] = 1.0f;
            }
        }
        float* zbuf = zbuf_vec.data();

        uint32_t stride_bytes = draw.stride_bytes;
        if (stride_bytes == 0U) {
            if (draw.vertex_bo_size >= draw.count * 36ULL && (draw.vertex_bo_size % 36ULL) == 0ULL) stride_bytes = 36U;
            else if (draw.vertex_bo_size >= draw.count * 32ULL) stride_bytes = 32U;
            else if (draw.vertex_bo_size >= draw.count * 24ULL) stride_bytes = 24U;
            else if (draw.vertex_bo_size >= draw.count * 20ULL) stride_bytes = 20U;
            else if (draw.vertex_bo_size >= draw.count * 16ULL) stride_bytes = 16U;
            else stride_bytes = 12U;
        }

        // Detect UBO layout:
        //   - vkmark [vertex]/[texture]: mat4 mvp at +0x00, mat4 normal at +0x40, optional vec4 material_color at +0x80
        //   - vkmark [cube]/[shading]:   mat4 modelview at +0x00, mat4 mvp at +0x40, mat4 normal at +0x80
        const float* mvp_mat  = nullptr;
        const float* norm_mat = nullptr;
        float ubo_mat_col[3]  = {0.75f, 0.75f, 0.75f};
        bool  has_ubo_mat_col = false;
        if (draw.ubo_va != 0 && draw.ubo_range >= 64ULL) {
            const float* ubo = reinterpret_cast<const float*>(draw.ubo_va);
            auto isMvpCandidate = [](const float* m) -> bool {
                if (!std::isfinite(m[0]) || !std::isfinite(m[5]) || !std::isfinite(m[11]) || !std::isfinite(m[15])) return false;
                if (std::fabs(m[0]) < 0.02f || std::fabs(m[5]) < 0.02f) return false;
                // Perspective projection has |m[11]| ~= 1.0 and |m[15]| ~= 0.0; ortho/general has m[15] ~= 1.0 and non-zero translation/scale
                bool is_persp = (std::fabs(std::fabs(m[11]) - 1.0f) < 0.25f && std::fabs(m[15]) < 0.05f);
                bool is_ortho = (std::fabs(m[15] - 1.0f) < 0.05f && std::fabs(m[3]) < 1e-3f && std::fabs(m[7]) < 1e-3f && std::fabs(m[10]) > 0.01f);
                return is_persp || is_ortho;
            };
            if (draw.ubo_range >= 128ULL &&
                std::fabs(ubo[15] - 1.0f) < 0.05f &&
                std::fabs(std::fabs(ubo[16 + 11]) - 1.0f) < 0.25f &&
                std::fabs(ubo[16 + 15]) < 0.05f) {
                // modelview at +0x00, mvp at +0x40, normal at +0x80 (or modelview)
                mvp_mat  = ubo + 16;
                norm_mat = (draw.ubo_range >= 192ULL) ? (ubo + 32) : (ubo + 0);
            } else if (isMvpCandidate(ubo)) {
                mvp_mat  = ubo;
                norm_mat = (draw.ubo_range >= 128ULL) ? (ubo + 16) : nullptr;
                if (draw.ubo_range >= 144ULL && std::isfinite(ubo[32]) && ubo[35] > 0.01f && ubo[35] <= 1.0f) {
                    ubo_mat_col[0] = std::clamp(ubo[32], 0.0f, 1.0f);
                    ubo_mat_col[1] = std::clamp(ubo[33], 0.0f, 1.0f);
                    ubo_mat_col[2] = std::clamp(ubo[34], 0.0f, 1.0f);
                    has_ubo_mat_col = true;
                }
            }
        }

        const uint32_t* tex_rgba8 = draw.tex_va ? reinterpret_cast<const uint32_t*>(draw.tex_va) : nullptr;
        uint32_t tex_w = draw.tex_width;
        uint32_t tex_h = draw.tex_height;

        auto fetchIndex = [&](uint32_t k) -> uint32_t {
            if (!draw.is_indexed || !draw.index_bo_va) return draw.first_vertex + k;
            uint32_t idx_pos = draw.first_index + k;
            if (draw.index_type == 0U /* UINT16 */) {
                if ((idx_pos + 1ULL) * 2ULL <= draw.index_bo_size)
                    return static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(draw.index_bo_va)[idx_pos]) + draw.vertex_offset;
            } else {
                if ((idx_pos + 1ULL) * 4ULL <= draw.index_bo_size)
                    return reinterpret_cast<const uint32_t*>(draw.index_bo_va)[idx_pos] + draw.vertex_offset;
            }
            return k;
        };

        VkCubeFrameStats stats{};
        stats.width  = W;
        stats.height = H;
        stats.bbox_min_x = W;
        stats.bbox_min_y = H;

        uint32_t topo = draw.topology;
        uint32_t step = (topo == 4U /* TRIANGLE_STRIP */ || topo == 5U /* TRIANGLE_FAN */) ? 1U : 3U;
        const float lx = 0.348155f, ly = 0.348155f, lz = 0.870388f; // normalize(vec3(2, 2, 5))

        for (uint32_t t = 0; t + 2 < draw.count; t += step) {
            uint32_t i0 = fetchIndex(topo == 5U ? 0U : t);
            uint32_t i1 = fetchIndex(t + 1U);
            uint32_t i2 = fetchIndex(t + 2U);
            if (topo == 4U && (t & 1U)) std::swap(i0, i1);
            if ((i0 + 1ULL) * stride_bytes > draw.vertex_bo_size ||
                (i1 + 1ULL) * stride_bytes > draw.vertex_bo_size ||
                (i2 + 1ULL) * stride_bytes > draw.vertex_bo_size) continue;

            stats.triangles_submitted++;
            const uint8_t* b[3] = {
                reinterpret_cast<const uint8_t*>(draw.vertex_bo_va) + i0 * stride_bytes,
                reinterpret_cast<const uint8_t*>(draw.vertex_bo_va) + i1 * stride_bytes,
                reinterpret_cast<const uint8_t*>(draw.vertex_bo_va) + i2 * stride_bytes
            };

            float sx[3], sy[3], sz[3], inv_w[3];
            float vr[3], vg[3], vb[3], vu[3], vv[3];
            bool tri_valid = true;

            for (int vi = 0; vi < 3; ++vi) {
                const float* p = reinterpret_cast<const float*>(b[vi]);
                float px_in = p[0], py_in = p[1], pz_in = (stride_bytes >= 12U) ? p[2] : 0.0f;
                if (mvp_mat) {
                    float cx = mvp_mat[0]*px_in + mvp_mat[4]*py_in + mvp_mat[8]*pz_in  + mvp_mat[12];
                    float cy = mvp_mat[1]*px_in + mvp_mat[5]*py_in + mvp_mat[9]*pz_in  + mvp_mat[13];
                    float cz = mvp_mat[2]*px_in + mvp_mat[6]*py_in + mvp_mat[10]*pz_in + mvp_mat[14];
                    float cw = mvp_mat[3]*px_in + mvp_mat[7]*py_in + mvp_mat[11]*pz_in + mvp_mat[15];
                    if (cw <= 1e-4f) { tri_valid = false; break; }
                    inv_w[vi] = 1.0f / cw;
                    sx[vi] = ((cx * inv_w[vi]) * 0.5f + 0.5f) * static_cast<float>(W);
                    sy[vi] = ((cy * inv_w[vi]) * 0.5f + 0.5f) * static_cast<float>(H);
                    sz[vi] = std::clamp(0.5f * (cz * inv_w[vi] + 1.0f), 0.0f, 1.0f);
                } else {
                    inv_w[vi] = 1.0f;
                    sx[vi] = (px_in * 0.5f + 0.5f) * static_cast<float>(W);
                    sy[vi] = (py_in * 0.5f + 0.5f) * static_cast<float>(H);
                    sz[vi] = std::clamp(pz_in * 0.5f + 0.5f, 0.0f, 1.0f);
                }

                float base_r = draw.has_const_color ? draw.const_color[0] : (has_ubo_mat_col ? ubo_mat_col[0] : 0.85f);
                float base_g = draw.has_const_color ? draw.const_color[1] : (has_ubo_mat_col ? ubo_mat_col[1] : 0.65f);
                float base_b = draw.has_const_color ? draw.const_color[2] : (has_ubo_mat_col ? ubo_mat_col[2] : 0.35f);
                float shade  = 1.0f;

                if (norm_mat && stride_bytes >= 24U) {
                    const float* n = reinterpret_cast<const float*>(b[vi] + 12U);
                    float tnx = norm_mat[0]*n[0] + norm_mat[4]*n[1] + norm_mat[8]*n[2];
                    float tny = norm_mat[1]*n[0] + norm_mat[5]*n[1] + norm_mat[9]*n[2];
                    float tnz = norm_mat[2]*n[0] + norm_mat[6]*n[1] + norm_mat[10]*n[2];
                    float nlen = std::sqrt(tnx*tnx + tny*tny + tnz*tnz);
                    if (nlen > 1e-6f) { tnx /= nlen; tny /= nlen; tnz /= nlen; }
                    shade = 0.20f + 0.80f * std::max(0.0f, std::fabs(tnx * lx + tny * ly + tnz * lz));
                    if (stride_bytes >= 36U) {
                        const float* c = reinterpret_cast<const float*>(b[vi] + 24U);
                        base_r = std::clamp(c[0], 0.0f, 1.0f);
                        base_g = std::clamp(c[1], 0.0f, 1.0f);
                        base_b = std::clamp(c[2], 0.0f, 1.0f);
                    } else if (tex_rgba8) {
                        base_r = base_g = base_b = 1.0f;
                    }
                } else if (stride_bytes >= 24U) {
                    const float* c = reinterpret_cast<const float*>(b[vi] + 12U);
                    base_r = std::clamp(c[0], 0.0f, 1.0f);
                    base_g = std::clamp(c[1], 0.0f, 1.0f);
                    base_b = std::clamp(c[2], 0.0f, 1.0f);
                }

                vr[vi] = std::clamp(base_r * shade, 0.0f, 1.0f);
                vg[vi] = std::clamp(base_g * shade, 0.0f, 1.0f);
                vb[vi] = std::clamp(base_b * shade, 0.0f, 1.0f);

                if (stride_bytes == 20U) {
                    const float* uv = reinterpret_cast<const float*>(b[vi] + 12U);
                    vu[vi] = uv[0]; vv[vi] = uv[1];
                } else if (stride_bytes == 32U) {
                    const float* uv = reinterpret_cast<const float*>(b[vi] + 24U);
                    vu[vi] = uv[0]; vv[vi] = uv[1];
                } else {
                    vu[vi] = sx[vi] / static_cast<float>(W);
                    vv[vi] = sy[vi] / static_cast<float>(H);
                }
            }
            if (!tri_valid) continue;

            float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-4f) continue;
            stats.triangles_front_facing++;
            float inv_area = 1.0f / area;

            int min_x = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
            int max_x = std::min(static_cast<int>(W) - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
            int min_y = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
            int max_y = std::min(static_cast<int>(H) - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));

            for (int py = min_y; py <= max_y; ++py) {
                for (int px_i = min_x; px_i <= max_x; ++px_i) {
                    float sample_x = static_cast<float>(px_i) + 0.5f;
                    float sample_y = static_cast<float>(py) + 0.5f;
                    float w0 = ((sx[1] - sample_x) * (sy[2] - sample_y) - (sx[2] - sample_x) * (sy[1] - sample_y)) * inv_area;
                    float w1 = ((sx[2] - sample_x) * (sy[0] - sample_y) - (sx[0] - sample_x) * (sy[2] - sample_y)) * inv_area;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                        float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                        size_t pidx = static_cast<size_t>(py) * W + static_cast<size_t>(px_i);
                        if (z <= zbuf[pidx]) {
                            zbuf[pidx] = z;
                            stats.depth_test_passes++;
                            float cr = std::clamp(w0 * vr[0] + w1 * vr[1] + w2 * vr[2], 0.0f, 1.0f);
                            float cg = std::clamp(w0 * vg[0] + w1 * vg[1] + w2 * vg[2], 0.0f, 1.0f);
                            float cb_c = std::clamp(w0 * vb[0] + w1 * vb[1] + w2 * vb[2], 0.0f, 1.0f);
                            if (tex_rgba8 && tex_w > 0 && tex_h > 0) {
                                float pw0 = w0 * inv_w[0], pw1 = w1 * inv_w[1], pw2 = w2 * inv_w[2];
                                float pw_sum = pw0 + pw1 + pw2;
                                if (pw_sum > 1e-7f) { pw0 /= pw_sum; pw1 /= pw_sum; pw2 /= pw_sum; }
                                float u = std::clamp(pw0 * vu[0] + pw1 * vu[1] + pw2 * vu[2], 0.0f, 1.0f);
                                float v = std::clamp(pw0 * vv[0] + pw1 * vv[1] + pw2 * vv[2], 0.0f, 1.0f);
                                uint32_t tx = std::min<uint32_t>(tex_w - 1U, static_cast<uint32_t>(u * tex_w));
                                uint32_t ty = std::min<uint32_t>(tex_h - 1U, static_cast<uint32_t>(v * tex_h));
                                uint32_t texel = tex_rgba8[ty * tex_w + tx];
                                cr   *= static_cast<float>(texel & 0xFFU) / 255.0f;
                                cg   *= static_cast<float>((texel >> 8) & 0xFFU) / 255.0f;
                                cb_c *= static_cast<float>((texel >> 16) & 0xFFU) / 255.0f;
                            }
                            uint32_t r8 = static_cast<uint32_t>(cr * 255.0f + 0.5f);
                            uint32_t g8 = static_cast<uint32_t>(cg * 255.0f + 0.5f);
                            uint32_t b8 = static_cast<uint32_t>(cb_c * 255.0f + 0.5f);
                            px[pidx] = (0xFFU << 24) | (b8 << 16) | (g8 << 8) | r8;
                        } else {
                            stats.depth_overdraw_rejects++;
                        }
                    }
                }
            }
        }

        uint32_t crc = 0x811C9DC5U;
        for (size_t i = 0; i < total_pixels; ++i) {
            if (px[i] != mfbd.clear_color_rgba8) stats.fragments_shaded++;
            crc = (crc ^ px[i]) * 0x01000193U;
        }
        stats.frame_crc32 = crc;
        stats.center_pixel_rgba8 = px[(H / 2) * W + (W / 2)];
        stats.lvp_fragment_dispatch_ok = true;
        if (out_stats) *out_stats = stats;
        return stats.fragments_shaded > 0;
    }
};
