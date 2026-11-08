// ============================================================================
// src/aot/layers/bcn_layer_host.cpp
// Host x86-64 Vulkan Layer (`VK_LAYER_BCN_BCnLayer` / `libVkLayer_BCN_bcn_layer.so`)
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void* instance, const char* pName);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void* device, const char* pName);

static std::mutex g_bcn_mutex;
static PFN_vkGetInstanceProcAddr g_next_gipa = nullptr;
static PFN_vkGetDeviceProcAddr   g_next_gdpa = nullptr;
static void* g_last_instance = nullptr;
static void* g_last_device   = nullptr;

struct BcnImageInfo {
    uint32_t orig_format;
    uint32_t target_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    bool     is_bcn;
};

struct BcnBufferInfo {
    uint64_t size;
    uint64_t memory;
    uint64_t mem_offset;
};

static std::unordered_map<uint64_t, BcnImageInfo>  g_bcn_images;
static std::unordered_map<uint64_t, BcnBufferInfo> g_bcn_buffers;
static std::unordered_map<uint64_t, void*>         g_bcn_mapped_mem;
static uint64_t g_bcn_transcode_count = 0;

static inline bool is_bcn_format(uint32_t fmt) {
    return (fmt >= 131U && fmt <= 146U);
}

static inline uint32_t map_bcn_to_rgba_format(uint32_t fmt) {
    switch (fmt) {
        case 132U: case 134U: case 136U: case 138U: case 146U:
            return 43U; /* VK_FORMAT_R8G8B8A8_SRGB */
        case 143U: case 144U:
            return 97U; /* VK_FORMAT_R16G16B16A16_SFLOAT */
        default:
            return 37U; /* VK_FORMAT_R8G8B8A8_UNORM */
    }
}

static void decode_bc1_block(const uint8_t* block, uint32_t* out_rgba, uint32_t stride_px, bool has_alpha_bc3, const uint8_t* alpha_block) {
    uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    uint32_t bits = static_cast<uint32_t>(block[4]) |
                    (static_cast<uint32_t>(block[5]) << 8) |
                    (static_cast<uint32_t>(block[6]) << 16) |
                    (static_cast<uint32_t>(block[7]) << 24);
    auto rgb565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
        r = static_cast<uint8_t>(((c >> 11) & 0x1FU) * 255U / 31U);
        g = static_cast<uint8_t>(((c >> 5)  & 0x3FU) * 255U / 63U);
        b = static_cast<uint8_t>((c         & 0x1FU) * 255U / 31U);
    };
    uint8_t r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
    rgb565(c0, r[0], g[0], b[0]);
    rgb565(c1, r[1], g[1], b[1]);
    if (c0 > c1 || has_alpha_bc3) {
        r[2] = static_cast<uint8_t>((2U * r[0] + r[1]) / 3U);
        g[2] = static_cast<uint8_t>((2U * g[0] + g[1]) / 3U);
        b[2] = static_cast<uint8_t>((2U * b[0] + b[1]) / 3U);
        r[3] = static_cast<uint8_t>((r[0] + 2U * r[1]) / 3U);
        g[3] = static_cast<uint8_t>((g[0] + 2U * g[1]) / 3U);
        b[3] = static_cast<uint8_t>((b[0] + 2U * b[1]) / 3U);
    } else {
        r[2] = static_cast<uint8_t>((r[0] + r[1]) / 2U);
        g[2] = static_cast<uint8_t>((g[0] + g[1]) / 2U);
        b[2] = static_cast<uint8_t>((b[0] + b[1]) / 2U);
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
    }
    uint8_t apal[8] = {255,255,255,255,255,255,255,255};
    uint64_t abits = 0;
    if (has_alpha_bc3 && alpha_block) {
        apal[0] = alpha_block[0];
        apal[1] = alpha_block[1];
        if (apal[0] > apal[1]) {
            for (int i = 1; i <= 6; ++i)
                apal[i + 1] = static_cast<uint8_t>(((7 - i) * apal[0] + i * apal[1]) / 7);
        } else {
            for (int i = 1; i <= 4; ++i)
                apal[i + 1] = static_cast<uint8_t>(((5 - i) * apal[0] + i * apal[1]) / 5);
            apal[6] = 0;
            apal[7] = 255;
        }
        for (int i = 0; i < 6; ++i)
            abits |= (static_cast<uint64_t>(alpha_block[2 + i]) << (8 * i));
    }
    for (uint32_t py = 0; py < 4U; ++py) {
        for (uint32_t px = 0; px < 4U; ++px) {
            uint32_t idx = (bits >> (2U * (py * 4U + px))) & 0x3U;
            uint8_t alpha = has_alpha_bc3 ? apal[(abits >> (3U * (py * 4U + px))) & 0x7U] : a[idx];
            out_rgba[py * stride_px + px] =
                static_cast<uint32_t>(r[idx]) |
                (static_cast<uint32_t>(g[idx]) << 8) |
                (static_cast<uint32_t>(b[idx]) << 16) |
                (static_cast<uint32_t>(alpha) << 24);
        }
    }
}

__attribute__((visibility("default")))
uint64_t bcn_layer_get_transcode_count(void) {
    std::lock_guard<std::mutex> lk(g_bcn_mutex);
    return g_bcn_transcode_count;
}

__attribute__((visibility("default")))
int32_t BCnLayer_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1; return 0; }
    if (*pPropertyCount == 0) return 5;
    std::memset(pProperties, 0, 520);
    std::snprintf(static_cast<char*>(pProperties), 256, "VK_LAYER_BCN_BCnLayer");
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperties) + 256) = (1U << 22) | (3U << 12);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperties) + 260) = 1U;
    std::snprintf(static_cast<char*>(pProperties) + 264, 256, "Mali BCn Compute Texture Decompression Layer");
    *pPropertyCount = 1;
    return 0;
}

__attribute__((visibility("default")))
int32_t BCnLayer_EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, void* pProperties) {
    if (pLayerName && std::strcmp(pLayerName, "VK_LAYER_BCN_BCnLayer") == 0) {
        if (pPropertyCount) *pPropertyCount = 0;
        return 0;
    }
    if (g_next_gipa) {
        auto fn = reinterpret_cast<int32_t (*)(const char*, uint32_t*, void*)>(
            g_next_gipa(g_last_instance, "vkEnumerateInstanceExtensionProperties"));
        if (fn) return fn(pLayerName, pPropertyCount, pProperties);
    }
    if (pPropertyCount) *pPropertyCount = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t BCnLayer_CreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    struct LayerLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; void* pfnNextGetPhysicalDeviceProcAddr; };
    struct LayerInfo { int32_t sType; const void* pNext; int32_t function; LayerLink* pLayerInfo; };
    const auto* base = static_cast<const LayerInfo*>(pCreateInfo);
    const LayerInfo* cur = base ? static_cast<const LayerInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 47 && cur->function == 0 && cur->pLayerInfo) {
            g_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            const_cast<LayerInfo*>(cur)->pLayerInfo = static_cast<LayerLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerInfo*>(cur->pNext);
    }
    if (!g_next_gipa) return -3;
    auto fpCreate = reinterpret_cast<int32_t (*)(const void*, const void*, void**)>(
        g_next_gipa(nullptr, "vkCreateInstance"));
    if (!fpCreate) return -3;
    int32_t res = fpCreate(pCreateInfo, pAllocator, pInstance);
    if (res == 0 && pInstance) g_last_instance = *pInstance;
    return res;
}

__attribute__((visibility("default")))
int32_t BCnLayer_CreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice) {
    struct LayerDevLink { void* pNext; PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr; PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr; };
    struct LayerDevInfo { int32_t sType; const void* pNext; int32_t function; LayerDevLink* pLayerInfo; };
    const auto* base = static_cast<const LayerDevInfo*>(pCreateInfo);
    const LayerDevInfo* cur = base ? static_cast<const LayerDevInfo*>(base->pNext) : nullptr;
    while (cur) {
        if (cur->sType == 48 && cur->function == 0 && cur->pLayerInfo) {
            g_next_gipa = cur->pLayerInfo->pfnNextGetInstanceProcAddr;
            g_next_gdpa = cur->pLayerInfo->pfnNextGetDeviceProcAddr;
            const_cast<LayerDevInfo*>(cur)->pLayerInfo = static_cast<LayerDevLink*>(cur->pLayerInfo->pNext);
            break;
        }
        cur = static_cast<const LayerDevInfo*>(cur->pNext);
    }
    auto fpCreate = reinterpret_cast<int32_t (*)(void*, const void*, const void*, void**)>(
        g_next_gipa ? g_next_gipa(g_last_instance, "vkCreateDevice") : nullptr);
    if (!fpCreate) return -3;
    int32_t res = fpCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == 0 && pDevice) g_last_device = *pDevice;
    return res;
}

__attribute__((visibility("default")))
void BCnLayer_GetPhysicalDeviceFormatProperties(void* physicalDevice, uint32_t format, void* pFormatProperties) {
    if (g_next_gipa) {
        auto fn = reinterpret_cast<void (*)(void*, uint32_t, void*)>(
            g_next_gipa(g_last_instance, "vkGetPhysicalDeviceFormatProperties"));
        if (fn) fn(physicalDevice, format, pFormatProperties);
    }
    if (pFormatProperties && is_bcn_format(format)) {
        auto* u32 = static_cast<uint32_t*>(pFormatProperties);
        u32[1] |= (0x00000001U | 0x00000400U | 0x00001000U | 0x00008000U);
    }
}

__attribute__((visibility("default")))
void BCnLayer_GetPhysicalDeviceFormatProperties2(void* physicalDevice, uint32_t format, void* pFormatProperties) {
    if (g_next_gipa) {
        auto fn = reinterpret_cast<void (*)(void*, uint32_t, void*)>(
            g_next_gipa(g_last_instance, "vkGetPhysicalDeviceFormatProperties2"));
        if (fn) fn(physicalDevice, format, pFormatProperties);
    }
    if (pFormatProperties && is_bcn_format(format)) {
        auto* u32 = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pFormatProperties) + 16);
        u32[1] |= (0x00000001U | 0x00000400U | 0x00001000U | 0x00008000U);
        struct VkBaseOut { uint32_t sType; VkBaseOut* pNext; };
        auto* next = *reinterpret_cast<VkBaseOut**>(static_cast<uint8_t*>(pFormatProperties) + 8);
        while (next) {
            if (next->sType == 1000360000U /* VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 */) {
                auto* opt_feat2 = reinterpret_cast<uint64_t*>( reinterpret_cast<uint8_t*>(next) + 24 );
                *opt_feat2 |= (0x00000001ULL | 0x00000400ULL | 0x00001000ULL | 0x00008000ULL);
            }
            next = next->pNext;
        }
    }
}

__attribute__((visibility("default")))
int32_t BCnLayer_CreateImage(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pImage) {
    auto fn = reinterpret_cast<int32_t (*)(void*, const void*, const void*, uint64_t*)>(
        g_next_gdpa ? g_next_gdpa(device, "vkCreateImage") : nullptr);
    if (!fn || !pCreateInfo) return -3;
    struct ImgCreateInfo {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint32_t imageType;
        uint32_t format;
        uint32_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t mipLevels;
        uint32_t arrayLayers;
        uint32_t samples;
        uint32_t tiling;
        uint32_t usage;
        uint32_t sharingMode;
        uint32_t queueFamilyIndexCount;
        const uint32_t* pQueueFamilyIndices;
        uint32_t initialLayout;
    };
    const auto* orig = static_cast<const ImgCreateInfo*>(pCreateInfo);
    ImgCreateInfo mod = *orig;
    bool bcn = is_bcn_format(orig->format);
    if (bcn) {
        mod.format = map_bcn_to_rgba_format(orig->format);
        mod.usage |= 0x00000002U | 0x00000008U;
        mod.flags &= ~0x00000008U;
        mod.pNext = nullptr;
    }
    int32_t res = fn(device, bcn ? &mod : pCreateInfo, pAllocator, pImage);
    if (res == 0 && pImage) {
        std::lock_guard<std::mutex> lk(g_bcn_mutex);
        g_bcn_images[*pImage] = { orig->format, mod.format, orig->width, orig->height, orig->depth ? orig->depth : 1U, bcn };
    }
    return res;
}

__attribute__((visibility("default")))
int32_t BCnLayer_CreateImageView(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pView) {
    auto fn = reinterpret_cast<int32_t (*)(void*, const void*, const void*, uint64_t*)>(
        g_next_gdpa ? g_next_gdpa(device, "vkCreateImageView") : nullptr);
    if (!fn || !pCreateInfo) return -3;
    struct ViewCreateInfo {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint64_t image;
        uint32_t viewType;
        uint32_t format;
        uint32_t components[4];
        uint32_t subresourceRange[5];
    };
    const auto* orig = static_cast<const ViewCreateInfo*>(pCreateInfo);
    ViewCreateInfo mod = *orig;
    if (is_bcn_format(orig->format)) {
        mod.format = map_bcn_to_rgba_format(orig->format);
    }
    return fn(device, &mod, pAllocator, pView);
}

__attribute__((visibility("default")))
int32_t BCnLayer_CreateBuffer(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pBuffer) {
    auto fn = reinterpret_cast<int32_t (*)(void*, const void*, const void*, uint64_t*)>(
        g_next_gdpa ? g_next_gdpa(device, "vkCreateBuffer") : nullptr);
    if (!fn) return -3;
    int32_t res = fn(device, pCreateInfo, pAllocator, pBuffer);
    if (res == 0 && pBuffer && pCreateInfo) {
        uint64_t sz = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(pCreateInfo) + 24);
        std::lock_guard<std::mutex> lk(g_bcn_mutex);
        g_bcn_buffers[*pBuffer] = { sz, 0ULL, 0ULL };
    }
    return res;
}

__attribute__((visibility("default")))
int32_t BCnLayer_BindBufferMemory(void* device, uint64_t buffer, uint64_t memory, uint64_t memoryOffset) {
    auto fn = reinterpret_cast<int32_t (*)(void*, uint64_t, uint64_t, uint64_t)>(
        g_next_gdpa ? g_next_gdpa(device, "vkBindBufferMemory") : nullptr);
    if (!fn) return -3;
    int32_t res = fn(device, buffer, memory, memoryOffset);
    if (res == 0) {
        std::lock_guard<std::mutex> lk(g_bcn_mutex);
        g_bcn_buffers[buffer].memory = memory;
        g_bcn_buffers[buffer].mem_offset = memoryOffset;
    }
    return res;
}

__attribute__((visibility("default")))
int32_t BCnLayer_MapMemory(void* device, uint64_t memory, uint64_t offset, uint64_t size, uint32_t flags, void** ppData) {
    auto fn = reinterpret_cast<int32_t (*)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**)>(
        g_next_gdpa ? g_next_gdpa(device, "vkMapMemory") : nullptr);
    if (!fn) return -3;
    int32_t res = fn(device, memory, offset, size, flags, ppData);
    if (res == 0 && ppData && *ppData) {
        std::lock_guard<std::mutex> lk(g_bcn_mutex);
        g_bcn_mapped_mem[memory] = static_cast<uint8_t*>(*ppData) - offset;
    }
    return res;
}

__attribute__((visibility("default")))
void BCnLayer_CmdCopyBufferToImage(void* commandBuffer, uint64_t srcBuffer, uint64_t dstImage, uint32_t dstImageLayout, uint32_t regionCount, const void* pRegions) {
    auto fnCopy = reinterpret_cast<void (*)(void*, uint64_t, uint64_t, uint32_t, uint32_t, const void*)>(
        g_next_gdpa ? g_next_gdpa(g_last_device, "vkCmdCopyBufferToImage") : nullptr);
    if (!fnCopy || !pRegions || regionCount == 0) return;

    BcnImageInfo img_info{};
    const uint8_t* src_cpu = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_bcn_mutex);
        auto it = g_bcn_images.find(dstImage);
        if (it != g_bcn_images.end()) img_info = it->second;
        auto bit = g_bcn_buffers.find(srcBuffer);
        if (bit != g_bcn_buffers.end()) {
            auto mit = g_bcn_mapped_mem.find(bit->second.memory);
            if (mit != g_bcn_mapped_mem.end() && mit->second)
                src_cpu = static_cast<const uint8_t*>(mit->second) + bit->second.mem_offset;
        }
    }
    if (!img_info.is_bcn || !src_cpu) {
        fnCopy(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
        return;
    }

    auto fnCreateBuf  = reinterpret_cast<int32_t (*)(void*, const void*, const void*, uint64_t*)>(g_next_gdpa(g_last_device, "vkCreateBuffer"));
    auto fnAllocMem   = reinterpret_cast<int32_t (*)(void*, const void*, const void*, uint64_t*)>(g_next_gdpa(g_last_device, "vkAllocateMemory"));
    auto fnBindBuf    = reinterpret_cast<int32_t (*)(void*, uint64_t, uint64_t, uint64_t)>(g_next_gdpa(g_last_device, "vkBindBufferMemory"));
    auto fnMapMem     = reinterpret_cast<int32_t (*)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**)>(g_next_gdpa(g_last_device, "vkMapMemory"));

    struct BufImgCopy {
        uint64_t bufferOffset;
        uint32_t bufferRowLength;
        uint32_t bufferImageHeight;
        uint32_t aspectMask;
        uint32_t mipLevel;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
        int32_t  offsetX, offsetY, offsetZ;
        uint32_t width, height, depth;
    };
    const auto* regs = static_cast<const BufImgCopy*>(pRegions);
    for (uint32_t r = 0; r < regionCount; ++r) {
        uint32_t w = regs[r].width ? regs[r].width : img_info.width;
        uint32_t h = regs[r].height ? regs[r].height : img_info.height;
        uint64_t rgba_bytes = static_cast<uint64_t>(w) * h * 4ULL;
        struct BufCI { uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sharingMode; uint32_t qfCount; const uint32_t* pQf; }
            bci = { 12U, nullptr, 0U, rgba_bytes, 0x00000001U, 0U, 0U, nullptr };
        struct MemAI { uint32_t sType; const void* pNext; uint64_t allocationSize; uint32_t memoryTypeIndex; }
            mai = { 5U, nullptr, rgba_bytes, 0U };
        uint64_t stg_buf = 0, stg_mem = 0;
        void* stg_ptr = nullptr;
        if (fnCreateBuf(g_last_device, &bci, nullptr, &stg_buf) == 0 &&
            fnAllocMem(g_last_device, &mai, nullptr, &stg_mem) == 0 &&
            fnBindBuf(g_last_device, stg_buf, stg_mem, 0) == 0 &&
            fnMapMem(g_last_device, stg_mem, 0, rgba_bytes, 0, &stg_ptr) == 0 && stg_ptr) {
            auto* dst_px = static_cast<uint32_t*>(stg_ptr);
            uint32_t blocks_x = (w + 3U) / 4U;
            uint32_t blocks_y = (h + 3U) / 4U;
            bool is_bc1 = (img_info.orig_format >= 131U && img_info.orig_format <= 134U);
            bool is_bc3 = (img_info.orig_format == 137U || img_info.orig_format == 138U);
            size_t block_sz = is_bc1 ? 8U : 16U;
            const uint8_t* bcn_ptr = src_cpu + regs[r].bufferOffset;
            for (uint32_t by = 0; by < blocks_y; ++by) {
                for (uint32_t bx = 0; bx < blocks_x; ++bx) {
                    const uint8_t* blk = bcn_ptr + (by * blocks_x + bx) * block_sz;
                    uint32_t tile[16];
                    if (is_bc1) {
                        decode_bc1_block(blk, tile, 4U, false, nullptr);
                    } else if (is_bc3) {
                        decode_bc1_block(blk + 8U, tile, 4U, true, blk);
                    } else {
                        decode_bc1_block(blk, tile, 4U, false, nullptr);
                    }
                    for (uint32_t py = 0; py < 4U && (by * 4U + py) < h; ++py) {
                        for (uint32_t px = 0; px < 4U && (bx * 4U + px) < w; ++px) {
                            dst_px[(by * 4U + py) * w + (bx * 4U + px)] = tile[py * 4U + px];
                        }
                    }
                }
            }
            BufImgCopy mod_reg = regs[r];
            mod_reg.bufferOffset = 0;
            mod_reg.bufferRowLength = 0;
            mod_reg.bufferImageHeight = 0;
            fnCopy(commandBuffer, stg_buf, dstImage, dstImageLayout, 1U, &mod_reg);
            {
                std::lock_guard<std::mutex> lk(g_bcn_mutex);
                ++g_bcn_transcode_count;
            }
        }
    }
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(void* device, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateImage") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CreateImage);
    if (std::strcmp(pName, "vkCreateImageView") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CreateImageView);
    if (std::strcmp(pName, "vkCreateBuffer") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CreateBuffer);
    if (std::strcmp(pName, "vkBindBufferMemory") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_BindBufferMemory);
    if (std::strcmp(pName, "vkMapMemory") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_MapMemory);
    if (std::strcmp(pName, "vkCmdCopyBufferToImage") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CmdCopyBufferToImage);
    return g_next_gdpa ? g_next_gdpa(device, pName) : nullptr;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_EnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_EnumerateInstanceExtensionProperties);
    if (std::strcmp(pName, "vkCreateInstance") == 0)                       return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CreateInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0)                         return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_CreateDevice);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_GetPhysicalDeviceFormatProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&BCnLayer_GetPhysicalDeviceFormatProperties2);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)                    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    return g_next_gipa ? g_next_gipa(instance, pName) : nullptr;
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
