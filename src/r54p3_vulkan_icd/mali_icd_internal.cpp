// ============================================================================
// src/r54p3_vulkan_icd/mali_icd_internal.cpp
// Definitions of shared AOT runtime state, Bionic HLE bridges, CSF job
// callback, and HostSpirvExecutor dispatch for r54p3_vulkan_icd.
// ============================================================================
#define AOT_SO_PROFILER_IMPLEMENTATION
#include "mali_icd_internal.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_set>

bool mali_icd_log_enabled() {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* env = getenv("MALI_AOT_ICD_LOG");
        s_enabled = (env && env[0] && std::atoi(env) != 0) ? 1 : 0;
    }
    return s_enabled != 0;
}

void mali_icd_log(const char* fmt, ...) {
    if (!mali_icd_log_enabled()) return;
    std::fprintf(stderr, "[MALI_AOT_ICD] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

__attribute__((constructor))
void mali_aot_icd_onload() {
    if (!mali_icd_log_enabled()) return;
    char exe_buf[512]{};
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) exe_buf[len] = '\0';
    mali_icd_log("Loaded libr54p3_vulkan_icd.so into pid=%d exe='%s' DISPLAY='%s' WAYLAND_DISPLAY='%s' VK_ICD_FILENAMES='%s' VK_DRIVER_FILES='%s'",
                 static_cast<int>(getpid()),
                 exe_buf[0] ? exe_buf : "(unknown)",
                 getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)",
                 getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
                 getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(unset)",
                 getenv("VK_DRIVER_FILES") ? getenv("VK_DRIVER_FILES") : "(unset)");
}

extern "C" uint64_t bionic_aapcs64_vsnprintf_bridge(uint64_t dst_addr, uint64_t maxlen,
                                                    uint64_t fmt_addr, uint64_t va_addr) {
    char* dst = reinterpret_cast<char*>(dst_addr);
    const char* fmt = reinterpret_cast<const char*>(fmt_addr);
    const AAPCS64VaList* vap = reinterpret_cast<const AAPCS64VaList*>(va_addr);
    if (!fmt) return 0;

    AAPCS64VaList va = vap ? *vap : AAPCS64VaList{};
    std::string out;
    out.reserve(256);

    const char* p = fmt;
    while (*p) {
        if (*p != '%') { out.push_back(*p++); continue; }
        if (p[1] == '%') { out.push_back('%'); p += 2; continue; }
        const char* start = p++;
        while (*p && std::strchr("-+ #0", *p)) p++;
        if (*p == '*') { (void)va.nextGPR(); p++; }
        else { while (*p >= '0' && *p <= '9') p++; }
        if (*p == '.') {
            p++;
            if (*p == '*') { (void)va.nextGPR(); p++; }
            else { while (*p >= '0' && *p <= '9') p++; }
        }
        while (*p && std::strchr("hljztL", *p)) p++;
        char spec = *p ? *p++ : '\0';
        std::string subfmt(start, p - start);
        char buf[512];
        switch (spec) {
            case 's': {
                const char* s = reinterpret_cast<const char*>(va.nextGPR());
                std::snprintf(buf, sizeof(buf), subfmt.c_str(), s ? s : "(null)");
                out += buf;
                break;
            }
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': case 'c': {
                uint64_t v = va.nextGPR();
                std::snprintf(buf, sizeof(buf), subfmt.c_str(), v);
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double d = va.nextFP();
                std::snprintf(buf, sizeof(buf), subfmt.c_str(), d);
                out += buf;
                break;
            }
            default:
                out += subfmt;
                break;
        }
    }

    if (dst && maxlen > 0) {
        size_t copy_len = std::min<size_t>(maxlen - 1, out.size());
        std::memcpy(dst, out.data(), copy_len);
        dst[copy_len] = '\0';
    }
    return out.size();
}

HostSpirvExecutor        g_icd_spirv_executor;
uint32_t                 g_icd_csf_jobs_executed = 0;
int                      g_icd_kbase_ioctl_calls = 0;

static uint32_t vkFormatBytesPerPixel(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_S8_UINT:
            return 1U;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_D16_UNORM:
            return 2U;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_B8G8R8_SRGB:
            return 3U;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
            return 8U;
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12U;
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16U;
        default:
            return 4U;
    }
}

bool executeGraphicsDrawOnHostVulkan(HostSpirvExecutor& exec, const MaliCsfGraphicsDrawPacket& pkt) {
    if (!exec.init() || !pkt.mfbd_color_va || pkt.width == 0 || pkt.height == 0) return false;
    auto* gipa = exec.gipa;
    void* inst = exec.instance;
    void* dev  = exec.device;
    uint32_t W = pkt.width, H = pkt.height;
    size_t img_bytes = static_cast<size_t>(W) * H * 4ULL;
    if (img_bytes > HostSpirvExecutor::kBufferCapacityBytes) return false;

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
    typedef void    (*PFN_vkCmdDraw)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef void    (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void    (*PFN_vkCmdPipelineBarrier)(void*, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*, uint32_t, const void*);
    typedef void    (*PFN_vkCmdCopyImageToBuffer)(void*, uint64_t, uint32_t, uint64_t, uint32_t, const void*);

    // Valid SPIR-V 1.0 pass-through vertex shader (vec4 in_pos @ location 0 -> gl_PerVertex.gl_Position)
    static const uint32_t kPassthroughVs[] = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000011U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000EU, 0x00000000U, 0x00000001U,
        0x0007000FU, 0x00000000U, 0x00000001U, 0x6E69616DU, 0x00000000U, 0x00000002U, 0x00000003U,
        0x00030047U, 0x00000004U, 0x00000002U,
        0x00050048U, 0x00000004U, 0x00000000U, 0x0000000BU, 0x00000000U,
        0x00040047U, 0x00000003U, 0x0000001EU, 0x00000000U,
        0x00020013U, 0x00000005U, 0x00030021U, 0x00000006U, 0x00000005U,
        0x00030016U, 0x00000007U, 0x00000020U,
        0x00040017U, 0x00000008U, 0x00000007U, 0x00000004U,
        0x0003001EU, 0x00000004U, 0x00000008U,
        0x00040020U, 0x00000009U, 0x00000003U, 0x00000004U,
        0x0004003BU, 0x00000009U, 0x00000002U, 0x00000003U,
        0x00040015U, 0x0000000AU, 0x00000020U, 0x00000001U,
        0x0004002BU, 0x0000000AU, 0x0000000BU, 0x00000000U,
        0x00040020U, 0x0000000CU, 0x00000001U, 0x00000008U,
        0x0004003BU, 0x0000000CU, 0x00000003U, 0x00000001U,
        0x00040020U, 0x0000000DU, 0x00000003U, 0x00000008U,
        0x00050036U, 0x00000005U, 0x00000001U, 0x00000000U, 0x00000006U,
        0x000200F8U, 0x0000000EU,
        0x0004003DU, 0x00000008U, 0x0000000FU, 0x00000003U,
        0x00050041U, 0x0000000DU, 0x00000010U, 0x00000002U, 0x0000000BU,
        0x0003003EU, 0x00000010U, 0x0000000FU,
        0x000100FDU, 0x00010038U
    };
    std::vector<uint32_t> vs_spv(kPassthroughVs, kPassthroughVs + sizeof(kPassthroughVs) / 4);
    auto hasSpirvOpFunction = [](const uint32_t* words, uint32_t count) -> bool {
        if (!words || count < 5) return false;
        size_t p = 5;
        while (p < count) {
            uint16_t wc = static_cast<uint16_t>(words[p] >> 16);
            uint16_t op = static_cast<uint16_t>(words[p] & 0xFFFFU);
            if (wc == 0 || p + wc > count) break;
            if (op == 54U /* OpFunction */) return true;
            p += wc;
        }
        return false;
    };
    if (pkt.vs_spirv_ptr && hasSpirvOpFunction(pkt.vs_spirv_ptr, pkt.vs_spirv_words)) {
        vs_spv.assign(pkt.vs_spirv_ptr, pkt.vs_spirv_ptr + pkt.vs_spirv_words);
    }
    // Valid SPIR-V 1.0 constant-color fragment shader
    auto makeConstFsSpv = [&]() -> std::vector<uint32_t> {
        uint32_t col = pkt.has_frag_const ? pkt.frag_const_rgba8 : 0xFF59A6D9U;
        auto f2u = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
        uint32_t r_u = f2u((col & 0xFFU) / 255.0f);
        uint32_t g_u = f2u(((col >> 8) & 0xFFU) / 255.0f);
        uint32_t b_u = f2u(((col >> 16) & 0xFFU) / 255.0f);
        uint32_t a_u = f2u(((col >> 24) & 0xFFU) / 255.0f);
        return {
            0x07230203U, 0x00010000U, 0x00080001U, 18U, 0U,
            (2U<<16)|17U, 1U,                              // OpCapability Shader
            (3U<<16)|14U, 0U, 1U,                          // OpMemoryModel Logical GLSL450
            (6U<<16)|15U, 4U, 4U, 0x6E69616DU, 0U, 7U,     // OpEntryPoint Fragment %4 "main" %7
            (3U<<16)|16U, 4U, 7U,                          // OpExecutionMode %4 OriginUpperLeft
            (4U<<16)|71U, 7U, 30U, 0U,                     // OpDecorate %7 Location 0
            (2U<<16)|19U, 1U,                              // OpTypeVoid %1
            (3U<<16)|33U, 2U, 1U,                          // OpTypeFunction %2 %1
            (3U<<16)|22U, 3U, 32U,                         // OpTypeFloat %3 32
            (4U<<16)|23U, 5U, 3U, 4U,                      // OpTypeVector %5 %3 4
            (4U<<16)|32U, 6U, 3U, 5U,                      // OpTypePointer %6 Output %5
            (4U<<16)|59U, 6U, 7U, 3U,                      // OpVariable %6 %7 Output
            (4U<<16)|43U, 3U, 8U, r_u,                     // OpConstant %3 %8 r
            (4U<<16)|43U, 3U, 9U, g_u,                     // OpConstant %3 %9 g
            (4U<<16)|43U, 3U, 10U, b_u,                    // OpConstant %3 %10 b
            (4U<<16)|43U, 3U, 11U, a_u,                    // OpConstant %3 %11 a
            (7U<<16)|44U, 5U, 12U, 8U, 9U, 10U, 11U,       // OpConstantComposite %5 %12 ...
            (5U<<16)|54U, 1U, 4U, 0U, 2U,                  // OpFunction %1 %4 None %2
            (2U<<16)|248U, 13U,                            // OpLabel %13
            (3U<<16)|62U, 7U, 12U,                         // OpStore %7 %12
            (1U<<16)|253U,                                 // OpReturn
            (1U<<16)|56U                                   // OpFunctionEnd
        };
    };
    std::vector<uint32_t> fs_spv = (pkt.fs_spirv_ptr && hasSpirvOpFunction(pkt.fs_spirv_ptr, pkt.fs_spirv_words))
        ? std::vector<uint32_t>(pkt.fs_spirv_ptr, pkt.fs_spirv_ptr + pkt.fs_spirv_words)
        : makeConstFsSpv();

    VkShaderModuleCreateInfo vs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, vs_spv.size() * 4, vs_spv.data()};
    VkShaderModuleCreateInfo fs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, fs_spv.size() * 4, fs_spv.data()};
    uint64_t vs_mod = 0, fs_mod = 0;
    auto createSM = reinterpret_cast<HostSpirvExecutor::PFN_vkCreateShaderModule>(gipa(inst, "vkCreateShaderModule"));
    auto destroySM = reinterpret_cast<HostSpirvExecutor::PFN_vkDestroyShaderModule>(gipa(inst, "vkDestroyShaderModule"));
    if (createSM(dev, &vs_ci, nullptr, &vs_mod) != 0) {
        std::fprintf(stderr, "[HostSpirvExecutor] vkCreateShaderModule(VS) failed\n");
        return false;
    }
    if (createSM(dev, &fs_ci, nullptr, &fs_mod) != 0) {
        std::fprintf(stderr, "[HostSpirvExecutor] vkCreateShaderModule(FS) failed\n");
        destroySM(dev, vs_mod, nullptr);
        return false;
    }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {W, H, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    uint64_t img = 0, img_mem = 0, img_view = 0;
    if (reinterpret_cast<PFN_vkCreateImage>(gipa(inst, "vkCreateImage"))(dev, &ici, nullptr, &img) != 0) {
        destroySM(dev, vs_mod, nullptr); destroySM(dev, fs_mod, nullptr);
        return false;
    }
    VkMemoryRequirements req{};
    reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gipa(inst, "vkGetImageMemoryRequirements"))(dev, img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, std::max<uint64_t>(req.size, img_bytes),
                             exec.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    reinterpret_cast<HostSpirvExecutor::PFN_vkAllocateMemory>(gipa(inst, "vkAllocateMemory"))(dev, &mai, nullptr, &img_mem);
    reinterpret_cast<PFN_vkBindImageMemory>(gipa(inst, "vkBindImageMemory"))(dev, img, img_mem, 0);

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = reinterpret_cast<VkImage>(img);
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    reinterpret_cast<PFN_vkCreateImageView>(gipa(inst, "vkCreateImageView"))(dev, &ivci, nullptr, &img_view);

    bool use_depth = (pkt.depth_test_enable != 0U || pkt.depth_write_enable != 0U);
    uint64_t depth_img = 0, depth_mem = 0, depth_view = 0;
    if (use_depth) {
        VkImageCreateInfo dici{};
        dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = VK_FORMAT_D32_SFLOAT;
        dici.extent = {W, H, 1};
        dici.mipLevels = 1;
        dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (reinterpret_cast<PFN_vkCreateImage>(gipa(inst, "vkCreateImage"))(dev, &dici, nullptr, &depth_img) == 0 && depth_img) {
            VkMemoryRequirements dreq{};
            reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gipa(inst, "vkGetImageMemoryRequirements"))(dev, depth_img, &dreq);
            VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, std::max<uint64_t>(dreq.size, static_cast<uint64_t>(W) * H * 4ULL),
                                      exec.findMemoryType(dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            if (reinterpret_cast<HostSpirvExecutor::PFN_vkAllocateMemory>(gipa(inst, "vkAllocateMemory"))(dev, &dmai, nullptr, &depth_mem) == 0 && depth_mem &&
                reinterpret_cast<PFN_vkBindImageMemory>(gipa(inst, "vkBindImageMemory"))(dev, depth_img, depth_mem, 0) == 0) {
                VkImageViewCreateInfo divci{};
                divci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                divci.image = reinterpret_cast<VkImage>(depth_img);
                divci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                divci.format = VK_FORMAT_D32_SFLOAT;
                divci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                reinterpret_cast<PFN_vkCreateImageView>(gipa(inst, "vkCreateImageView"))(dev, &divci, nullptr, &depth_view);
            }
        }
        if (!depth_view) use_depth = false;
    }

    VkAttachmentDescription atts[2]{};
    atts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = use_depth ? &depth_ref : nullptr;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, use_depth ? 2U : 1U, atts, 1, &sub, 0, nullptr};
    uint64_t rp = 0, fb = 0, pipe = 0;
    int32_t rprc = reinterpret_cast<PFN_vkCreateRenderPass>(gipa(inst, "vkCreateRenderPass"))(dev, &rpci, nullptr, &rp);

    VkImageView fb_atts[2] = {reinterpret_cast<VkImageView>(img_view), reinterpret_cast<VkImageView>(depth_view)};
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, reinterpret_cast<VkRenderPass>(rp), use_depth ? 2U : 1U, fb_atts, W, H, 1};
    int32_t fbrc = reinterpret_cast<PFN_vkCreateFramebuffer>(gipa(inst, "vkCreateFramebuffer"))(dev, &fbci, nullptr, &fb);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, reinterpret_cast<VkShaderModule>(vs_mod), "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, reinterpret_cast<VkShaderModule>(fs_mod), "main", nullptr}
    };
    std::vector<VkVertexInputBindingDescription> vbinds;
    std::vector<VkVertexInputAttributeDescription> vattrs;
    if (pkt.vertex_bindings && pkt.vertex_binding_count > 0) {
        for (uint32_t b = 0; b < pkt.vertex_binding_count; ++b) {
            vbinds.push_back({pkt.vertex_bindings[b].binding, pkt.vertex_bindings[b].stride,
                              static_cast<VkVertexInputRate>(pkt.vertex_bindings[b].input_rate)});
        }
    } else if (pkt.vertex_bo_va != 0) {
        vbinds.push_back({0, pkt.stride_bytes ? pkt.stride_bytes : 16U, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    if (pkt.vertex_attrs && pkt.vertex_attr_count > 0) {
        for (uint32_t a = 0; a < pkt.vertex_attr_count; ++a) {
            vattrs.push_back({pkt.vertex_attrs[a].location, pkt.vertex_attrs[a].binding,
                              static_cast<VkFormat>(pkt.vertex_attrs[a].format), pkt.vertex_attrs[a].offset});
        }
    } else if (pkt.vertex_bo_va != 0) {
        vattrs.push_back({0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        static_cast<uint32_t>(vbinds.size()), vbinds.empty() ? nullptr : vbinds.data(),
        static_cast<uint32_t>(vattrs.size()), vattrs.empty() ? nullptr : vattrs.data()
    };
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        static_cast<VkPrimitiveTopology>(pkt.topology <= 10U ? pkt.topology : 3U), VK_FALSE};
    VkViewport vp{0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = static_cast<VkPolygonMode>(pkt.polygon_mode <= 2U ? pkt.polygon_mode : 0U);
    rs.cullMode = static_cast<VkCullModeFlags>(pkt.cull_mode & 0x3U);
    rs.frontFace = static_cast<VkFrontFace>(pkt.front_face <= 1U ? pkt.front_face : 0U);
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0.0f, nullptr, VK_FALSE, VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo ds_ci{};
    ds_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds_ci.depthTestEnable = (pkt.depth_test_enable != 0U) ? VK_TRUE : VK_FALSE;
    ds_ci.depthWriteEnable = (pkt.depth_write_enable != 0U) ? VK_TRUE : VK_FALSE;
    ds_ci.depthCompareOp = static_cast<VkCompareOp>(pkt.depth_compare_op <= 7U ? pkt.depth_compare_op : 1U);
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xFU;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba, {0,0,0,0}};
    // Build a descriptor set layout matching the SPIR-V bindings used by vs_spv and fs_spv
    std::unordered_map<uint32_t, uint32_t> binding_types;
    auto scanSpirvBindings = [&](const std::vector<uint32_t>& spv) {
        if (spv.size() < 5) return;
        std::unordered_map<uint32_t, uint32_t> var_binding;
        std::unordered_map<uint32_t, uint32_t> ptr_sc;
        std::unordered_map<uint32_t, uint32_t> ptr_pointee;
        std::unordered_set<uint32_t> buffer_blocks;
        size_t p = 5;
        while (p < spv.size()) {
            uint16_t wc = static_cast<uint16_t>(spv[p] >> 16);
            uint16_t op = static_cast<uint16_t>(spv[p] & 0xFFFFU);
            if (wc == 0 || p + wc > spv.size()) break;
            if (op == 71U /* OpDecorate */ && wc >= 3) {
                uint32_t target = spv[p + 1], dec = spv[p + 2];
                if (dec == 33U /* Binding */ && wc >= 4) var_binding[target] = spv[p + 3];
                else if (dec == 3U /* BufferBlock */) buffer_blocks.insert(target);
            } else if (op == 32U /* OpTypePointer */ && wc >= 4) {
                ptr_sc[spv[p + 1]] = spv[p + 2];
                ptr_pointee[spv[p + 1]] = spv[p + 3];
            } else if (op == 59U /* OpVariable */ && wc >= 4) {
                uint32_t pt = spv[p + 1], id = spv[p + 2], sc = spv[p + 3];
                auto bit = var_binding.find(id);
                if (bit != var_binding.end()) {
                    uint32_t dtype = 6U; // UNIFORM_BUFFER
                    if (sc == 12U /* StorageBuffer */) dtype = 7U;
                    else if (sc == 0U /* UniformConstant */) dtype = 1U; // COMBINED_IMAGE_SAMPLER
                    else if (sc == 2U /* Uniform */) {
                        auto pit = ptr_pointee.find(pt);
                        if (pit != ptr_pointee.end() && buffer_blocks.count(pit->second)) dtype = 7U;
                    }
                    binding_types[bit->second] = dtype;
                }
            }
            p += wc;
        }
    };
    scanSpirvBindings(vs_spv);
    scanSpirvBindings(fs_spv);

    struct HostTextureUpload {
        uint64_t image = 0;
        uint64_t mem = 0;
        uint64_t view = 0;
        uint64_t sampler = 0;
        uint64_t staging_buf = 0;
        uint64_t staging_mem = 0;
        uint32_t width = 1;
        uint32_t height = 1;
    };
    std::vector<HostTextureUpload> tex_uploads;

    uint64_t draw_dsl = 0, draw_pl = 0, draw_dp = 0, draw_ds = 0;
    int32_t plrc = 0;
    if (!binding_types.empty()) {
        struct DSLB { uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount; uint32_t stageFlags; const void* pImm; };
        struct DSLCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t bindingCount; const void* pBindings; };
        struct PLCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t setLayoutCount; const uint64_t* pSetLayouts; uint32_t pcCount; const void* pPc; };
        struct DPS { uint32_t type; uint32_t count; };
        struct DPCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t maxSets; uint32_t poolSizeCount; const void* pPoolSizes; };
        struct DSAI { uint32_t sType; const void* pNext; uint64_t descriptorPool; uint32_t descriptorSetCount; const uint64_t* pSetLayouts; };
        struct DBI { uint64_t buffer; uint64_t offset; uint64_t range; };
        struct DII { uint64_t sampler; uint64_t imageView; uint32_t imageLayout; };
        struct WDS { uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBinding; uint32_t dstArrayElement; uint32_t descriptorCount; uint32_t descriptorType; const void* pImg; const void* pBuf; const void* pTex; };
        std::vector<DSLB> dslbs;
        for (const auto& kv : binding_types) {
            dslbs.push_back({kv.first, kv.second, 1U, 0x7FFFFFFFU, nullptr});
        }
        DSLCI dslci{32, nullptr, 0, static_cast<uint32_t>(dslbs.size()), dslbs.data()};
        if (reinterpret_cast<HostSpirvExecutor::PFN_vkCreateDescriptorSetLayout>(gipa(inst, "vkCreateDescriptorSetLayout"))(dev, &dslci, nullptr, &draw_dsl) == 0 && draw_dsl) {
            PLCI plci{30, nullptr, 0, 1, &draw_dsl, 0, nullptr};
            plrc = reinterpret_cast<HostSpirvExecutor::PFN_vkCreatePipelineLayout>(gipa(inst, "vkCreatePipelineLayout"))(dev, &plci, nullptr, &draw_pl);
            DPS dps[3] = {{6U, 8U}, {7U, 8U}, {1U, 8U}};
            DPCI dpci{33, nullptr, 0, 1U, 3U, dps};
            if (reinterpret_cast<HostSpirvExecutor::PFN_vkCreateDescriptorPool>(gipa(inst, "vkCreateDescriptorPool"))(dev, &dpci, nullptr, &draw_dp) == 0 && draw_dp) {
                DSAI dsai{34, nullptr, draw_dp, 1U, &draw_dsl};
                if (reinterpret_cast<HostSpirvExecutor::PFN_vkAllocateDescriptorSets>(gipa(inst, "vkAllocateDescriptorSets"))(dev, &dsai, &draw_ds) == 0 && draw_ds) {
                    DBI ubo_dbi{exec.ubo_buf, 0, HostSpirvExecutor::kUboCapacityBytes};
                    DBI ssbo_dbi{exec.buf_in, 0, HostSpirvExecutor::kBufferCapacityBytes};
                    std::vector<DII> img_diis;
                    img_diis.reserve(binding_types.size());
                    std::vector<WDS> wds;
                    auto findBoundTexture = [&](uint32_t binding) -> const MaliBoundTextureDesc* {
                        for (uint32_t i = 0; i < pkt.bound_texture_count && i < MaliCsfGraphicsDrawPacket::kMaxBoundTextures; ++i) {
                            if (pkt.bound_textures[i].binding == binding) return &pkt.bound_textures[i];
                        }
                        return nullptr;
                    };
                    auto createImgFn  = reinterpret_cast<PFN_vkCreateImage>(gipa(inst, "vkCreateImage"));
                    auto getImgReqFn  = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gipa(inst, "vkGetImageMemoryRequirements"));
                    auto bindImgFn    = reinterpret_cast<PFN_vkBindImageMemory>(gipa(inst, "vkBindImageMemory"));
                    auto createIvFn   = reinterpret_cast<PFN_vkCreateImageView>(gipa(inst, "vkCreateImageView"));
                    typedef int32_t (*PFN_vkCreateSamplerLocal)(void*, const void*, const void*, uint64_t*);
                    auto createSmpFn  = reinterpret_cast<PFN_vkCreateSamplerLocal>(gipa(inst, "vkCreateSampler"));
                    auto createBufFn  = reinterpret_cast<HostSpirvExecutor::PFN_vkCreateBuffer>(gipa(inst, "vkCreateBuffer"));
                    auto getBufReqFn  = reinterpret_cast<void (*)(void*, uint64_t, void*)>(gipa(inst, "vkGetBufferMemoryRequirements"));
                    auto bindBufFn    = reinterpret_cast<HostSpirvExecutor::PFN_vkBindBufferMemory>(gipa(inst, "vkBindBufferMemory"));
                    auto allocMemFn   = reinterpret_cast<HostSpirvExecutor::PFN_vkAllocateMemory>(gipa(inst, "vkAllocateMemory"));
                    auto mapMemFn     = reinterpret_cast<HostSpirvExecutor::PFN_vkMapMemory>(gipa(inst, "vkMapMemory"));
                    auto unmapMemFn   = reinterpret_cast<void (*)(void*, uint64_t)>(gipa(inst, "vkUnmapMemory"));

                    for (const auto& kv : binding_types) {
                        if (kv.second == 6U) {
                            wds.push_back({35, nullptr, draw_ds, kv.first, 0, 1, 6U, nullptr, &ubo_dbi, nullptr});
                        } else if (kv.second == 1U) {
                            DII dii{exec.dummy_sampler, exec.dummy_img_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                            const MaliBoundTextureDesc* btd = findBoundTexture(kv.first);
                            if (btd && btd->pixel_va && btd->width > 0 && btd->height > 0 &&
                                createImgFn && getImgReqFn && bindImgFn && createIvFn && createSmpFn &&
                                createBufFn && bindBufFn && allocMemFn && mapMemFn) {
                                uint32_t tw = btd->width;
                                uint32_t th = btd->height;
                                VkFormat fmt = btd->format ? static_cast<VkFormat>(btd->format) : VK_FORMAT_R8G8B8A8_UNORM;
                                uint32_t bpp = vkFormatBytesPerPixel(fmt);
                                uint64_t tightly_bytes = static_cast<uint64_t>(tw) * th * bpp;
                                uint64_t copy_bytes = btd->pixel_bytes ? btd->pixel_bytes : tightly_bytes;

                                HostTextureUpload up{};
                                up.width = tw;
                                up.height = th;
                                VkImageCreateInfo tici{};
                                tici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                                tici.imageType = VK_IMAGE_TYPE_2D;
                                tici.format = fmt;
                                tici.extent = {tw, th, 1};
                                tici.mipLevels = 1;
                                tici.arrayLayers = 1;
                                tici.samples = VK_SAMPLE_COUNT_1_BIT;
                                tici.tiling = VK_IMAGE_TILING_OPTIMAL;
                                tici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                                if (createImgFn(dev, &tici, nullptr, &up.image) == 0 && up.image) {
                                    VkMemoryRequirements treq{};
                                    getImgReqFn(dev, up.image, &treq);
                                    VkMemoryAllocateInfo tmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, std::max<uint64_t>(treq.size, copy_bytes),
                                                              exec.findMemoryType(treq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
                                    if (allocMemFn(dev, &tmai, nullptr, &up.mem) == 0 && up.mem &&
                                        bindImgFn(dev, up.image, up.mem, 0) == 0) {
                                        VkImageViewCreateInfo tivci{};
                                        tivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                                        tivci.image = reinterpret_cast<VkImage>(up.image);
                                        tivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                                        tivci.format = fmt;
                                        tivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                        createIvFn(dev, &tivci, nullptr, &up.view);

                                        VkSamplerCreateInfo tsci{};
                                        tsci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                                        tsci.magFilter = static_cast<VkFilter>(btd->mag_filter <= 1U ? btd->mag_filter : 1U);
                                        tsci.minFilter = static_cast<VkFilter>(btd->min_filter <= 1U ? btd->min_filter : 1U);
                                        tsci.mipmapMode = static_cast<VkSamplerMipmapMode>(btd->mipmap_mode <= 1U ? btd->mipmap_mode : 1U);
                                        tsci.addressModeU = static_cast<VkSamplerAddressMode>(btd->address_mode_u <= 4U ? btd->address_mode_u : 0U);
                                        tsci.addressModeV = static_cast<VkSamplerAddressMode>(btd->address_mode_v <= 4U ? btd->address_mode_v : 0U);
                                        tsci.addressModeW = static_cast<VkSamplerAddressMode>(btd->address_mode_w <= 4U ? btd->address_mode_w : 0U);
                                        tsci.anisotropyEnable = (btd->max_anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
                                        tsci.maxAnisotropy = btd->max_anisotropy;
                                        tsci.maxLod = btd->max_lod;
                                        createSmpFn(dev, &tsci, nullptr, &up.sampler);

                                        VkBufferCreateInfo sbci{};
                                        sbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                                        sbci.size = std::max<uint64_t>(copy_bytes, 4ULL);
                                        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                                        if (createBufFn(dev, &sbci, nullptr, &up.staging_buf) == 0 && up.staging_buf) {
                                            VkMemoryRequirements breq{};
                                            if (getBufReqFn) getBufReqFn(dev, up.staging_buf, &breq);
                                            uint32_t stg_mem_type = exec.findMemoryType(
                                                breq.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                                            VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, std::max<uint64_t>(breq.size, sbci.size), stg_mem_type};
                                            if (allocMemFn(dev, &smai, nullptr, &up.staging_mem) == 0 && up.staging_mem &&
                                                bindBufFn(dev, up.staging_buf, up.staging_mem, 0) == 0) {
                                                void* mapped_stg = nullptr;
                                                if (mapMemFn(dev, up.staging_mem, 0, sbci.size, 0, &mapped_stg) == 0 && mapped_stg) {
                                                    const uint8_t* src_px = reinterpret_cast<const uint8_t*>(btd->pixel_va);
                                                    uint32_t row_bytes = tw * bpp;
                                                    if (btd->row_pitch > row_bytes && btd->row_pitch * static_cast<uint64_t>(th) <= copy_bytes) {
                                                        for (uint32_t y = 0; y < th; ++y) {
                                                            std::memcpy(static_cast<uint8_t*>(mapped_stg) + static_cast<size_t>(y) * row_bytes,
                                                                        src_px + static_cast<size_t>(y) * btd->row_pitch, row_bytes);
                                                        }
                                                    } else {
                                                        std::memcpy(mapped_stg, src_px, static_cast<size_t>(std::min<uint64_t>(copy_bytes, sbci.size)));
                                                    }
                                                    if (unmapMemFn) unmapMemFn(dev, up.staging_mem);
                                                }
                                            }
                                        }
                                    }
                                }
                                if (up.view && up.sampler && up.staging_buf && up.staging_mem) {
                                    dii.sampler = up.sampler;
                                    dii.imageView = up.view;
                                    tex_uploads.push_back(up);
                                } else {
                                    tex_uploads.push_back(up);
                                }
                            }
                            if (dii.sampler && dii.imageView) {
                                img_diis.push_back(dii);
                                wds.push_back({35, nullptr, draw_ds, kv.first, 0, 1, 1U, &img_diis.back(), nullptr, nullptr});
                            }
                        } else if (kv.second == 7U) {
                            wds.push_back({35, nullptr, draw_ds, kv.first, 0, 1, 7U, nullptr, &ssbo_dbi, nullptr});
                        }
                    }
                    if (!wds.empty()) {
                        reinterpret_cast<HostSpirvExecutor::PFN_vkUpdateDescriptorSets>(gipa(inst, "vkUpdateDescriptorSets"))(dev, static_cast<uint32_t>(wds.size()), wds.data(), 0, nullptr);
                    }
                }
            }
        }
    }
    uint64_t gfx_pl = 0;
    if (!draw_pl && !exec.gfx_pipeline_layout) {
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 0, nullptr};
        plrc = reinterpret_cast<HostSpirvExecutor::PFN_vkCreatePipelineLayout>(gipa(inst, "vkCreatePipelineLayout"))(dev, &plci, nullptr, &gfx_pl);
    }

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2; gpci.pStages = stages;
    gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps; gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = use_depth ? &ds_ci : nullptr;
    gpci.pColorBlendState = &cb;
    uint64_t active_pl = draw_pl ? draw_pl : (exec.gfx_pipeline_layout ? exec.gfx_pipeline_layout : (gfx_pl ? gfx_pl : exec.pipeline_layout));
    gpci.layout = reinterpret_cast<VkPipelineLayout>(active_pl);
    gpci.renderPass = reinterpret_cast<VkRenderPass>(rp);
    gpci.basePipelineIndex = -1;
    int32_t gprc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(inst, "vkCreateGraphicsPipelines"))(dev, 0, 1, &gpci, nullptr, &pipe);
    if (gprc != 0 || pipe == 0) {
        std::fprintf(stderr, "[HostSpirvExecutor] vkCreateGraphicsPipelines failed rc=%d dev='%s' pipe=%llu rp=%llu(rc=%d) fb=%llu(rc=%d) gfx_pl=%llu(rc=%d) vs=%llu(%zu) fs=%llu(%zu) has_fc=%u\n",
                     gprc, exec.device_name.c_str(), (unsigned long long)pipe, (unsigned long long)rp, rprc, (unsigned long long)fb, fbrc,
                     (unsigned long long)active_pl, plrc, (unsigned long long)vs_mod, vs_spv.size(), (unsigned long long)fs_mod, fs_spv.size(), pkt.has_frag_const);
    }

    bool ok = false;
    if (gprc == 0 && pipe != 0) {
        if (pkt.ubo_va && exec.mapped_ubo) {
            size_t ubytes = pkt.ubo_range ? std::min<size_t>(pkt.ubo_range, HostSpirvExecutor::kUboCapacityBytes) : HostSpirvExecutor::kUboCapacityBytes;
            std::memcpy(exec.mapped_ubo, reinterpret_cast<const void*>(pkt.ubo_va), ubytes);
        }
        size_t vbytes = std::min<size_t>(pkt.vertex_bo_size, HostSpirvExecutor::kBufferCapacityBytes / 2);
        if (pkt.vertex_bo_va && vbytes && exec.mapped_in) {
            std::memcpy(exec.mapped_in, reinterpret_cast<const void*>(pkt.vertex_bo_va), vbytes);
        }
        size_t ibytes = std::min<size_t>(pkt.index_bo_size, HostSpirvExecutor::kBufferCapacityBytes / 2);
        size_t ioff = HostSpirvExecutor::kBufferCapacityBytes / 2;
        if (pkt.is_indexed && pkt.index_bo_va && ibytes && exec.mapped_in) {
            std::memcpy(static_cast<uint8_t*>(exec.mapped_in) + ioff, reinterpret_cast<const void*>(pkt.index_bo_va), ibytes);
        }
        reinterpret_cast<HostSpirvExecutor::PFN_vkResetCommandBuffer>(gipa(inst, "vkResetCommandBuffer"))(exec.cmd_buf, 0);
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr};
        reinterpret_cast<HostSpirvExecutor::PFN_vkBeginCommandBuffer>(gipa(inst, "vkBeginCommandBuffer"))(exec.cmd_buf, &cbbi);
        if (!tex_uploads.empty()) {
            auto cmdBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(inst, "vkCmdPipelineBarrier"));
            typedef void (*PFN_vkCmdCopyBufferToImage)(void*, uint64_t, uint64_t, uint32_t, uint32_t, const void*);
            auto cmdCopyB2I = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(gipa(inst, "vkCmdCopyBufferToImage"));
            for (const auto& up : tex_uploads) {
                if (!up.image || !up.staging_buf || !cmdBarrier || !cmdCopyB2I) continue;
                VkImageMemoryBarrier to_dst{};
                to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_dst.srcAccessMask = 0;
                to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_dst.image = reinterpret_cast<VkImage>(up.image);
                to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                cmdBarrier(exec.cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {up.width, up.height, 1};
                cmdCopyB2I(exec.cmd_buf, up.staging_buf, up.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                VkImageMemoryBarrier to_read{};
                to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_read.image = reinterpret_cast<VkImage>(up.image);
                to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                cmdBarrier(exec.cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_read);
            }
        }
        VkClearValue clrs[2]{};
        clrs[0].color.float32[0] = (pkt.clear_rgba8 & 0xFFU) / 255.0f;
        clrs[0].color.float32[1] = ((pkt.clear_rgba8 >> 8) & 0xFFU) / 255.0f;
        clrs[0].color.float32[2] = ((pkt.clear_rgba8 >> 16) & 0xFFU) / 255.0f;
        clrs[0].color.float32[3] = ((pkt.clear_rgba8 >> 24) & 0xFFU) / 255.0f;
        clrs[1].depthStencil.depth = 1.0f;
        clrs[1].depthStencil.stencil = 0U;
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, reinterpret_cast<VkRenderPass>(rp), reinterpret_cast<VkFramebuffer>(fb), {{0,0},{W,H}}, use_depth ? 2U : 1U, clrs};
        reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(inst, "vkCmdBeginRenderPass"))(exec.cmd_buf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        reinterpret_cast<HostSpirvExecutor::PFN_vkCmdBindPipeline>(gipa(inst, "vkCmdBindPipeline"))(exec.cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        uint64_t bind_ds = draw_ds ? draw_ds : exec.gfx_desc_set;
        if (bind_ds && active_pl) {
            reinterpret_cast<HostSpirvExecutor::PFN_vkCmdBindDescriptorSets>(gipa(inst, "vkCmdBindDescriptorSets"))(
                exec.cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pl, 0, 1, &bind_ds, 0, nullptr);
        }
        if (pkt.vertex_bo_va) {
            uint64_t vb_off = 0;
            reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(inst, "vkCmdBindVertexBuffers"))(exec.cmd_buf, 0, 1, &exec.buf_in, &vb_off);
        }
        if (pkt.is_indexed && pkt.index_bo_va) {
            reinterpret_cast<PFN_vkCmdBindIndexBuffer>(gipa(inst, "vkCmdBindIndexBuffer"))(exec.cmd_buf, exec.buf_in, ioff, pkt.index_type);
            reinterpret_cast<PFN_vkCmdDrawIndexed>(gipa(inst, "vkCmdDrawIndexed"))(exec.cmd_buf, pkt.count, pkt.instance_count ? pkt.instance_count : 1U, pkt.first_index, pkt.vertex_offset, 0);
        } else {
            reinterpret_cast<PFN_vkCmdDraw>(gipa(inst, "vkCmdDraw"))(exec.cmd_buf, pkt.count, pkt.instance_count ? pkt.instance_count : 1U, pkt.first_vertex, 0);
        }
        reinterpret_cast<PFN_vkCmdEndRenderPass>(gipa(inst, "vkCmdEndRenderPass"))(exec.cmd_buf);
        VkBufferImageCopy bic{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {W, H, 1}};
        reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(gipa(inst, "vkCmdCopyImageToBuffer"))(exec.cmd_buf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, exec.buf_out, 1, &bic);
        reinterpret_cast<HostSpirvExecutor::PFN_vkEndCommandBuffer>(gipa(inst, "vkEndCommandBuffer"))(exec.cmd_buf);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, reinterpret_cast<VkCommandBuffer*>(&exec.cmd_buf), 0, nullptr};
        if (reinterpret_cast<HostSpirvExecutor::PFN_vkQueueSubmit>(gipa(inst, "vkQueueSubmit"))(exec.queue, 1, &si, 0) == 0) {
            reinterpret_cast<HostSpirvExecutor::PFN_vkQueueWaitIdle>(gipa(inst, "vkQueueWaitIdle"))(exec.queue);
            if (exec.mapped_out) {
                std::memcpy(reinterpret_cast<void*>(pkt.mfbd_color_va), exec.mapped_out, img_bytes);
                ok = true;
            }
        }
    }
    if (pipe) reinterpret_cast<HostSpirvExecutor::PFN_vkDestroyPipeline>(gipa(inst, "vkDestroyPipeline"))(dev, pipe, nullptr);
    if (draw_pl || gfx_pl) {
        typedef void (*PFN_vkDestroyPipelineLayout)(void*, uint64_t, const void*);
        auto destroyPL = reinterpret_cast<PFN_vkDestroyPipelineLayout>(gipa(inst, "vkDestroyPipelineLayout"));
        if (destroyPL) {
            if (draw_pl) destroyPL(dev, draw_pl, nullptr);
            if (gfx_pl) destroyPL(dev, gfx_pl, nullptr);
        }
    }
    if (draw_dp) {
        typedef void (*PFN_vkDestroyDescriptorPool)(void*, uint64_t, const void*);
        auto destroyDP = reinterpret_cast<PFN_vkDestroyDescriptorPool>(gipa(inst, "vkDestroyDescriptorPool"));
        if (destroyDP) destroyDP(dev, draw_dp, nullptr);
    }
    if (draw_dsl) {
        typedef void (*PFN_vkDestroyDescriptorSetLayout)(void*, uint64_t, const void*);
        auto destroyDSL = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(gipa(inst, "vkDestroyDescriptorSetLayout"));
        if (destroyDSL) destroyDSL(dev, draw_dsl, nullptr);
    }
    for (const auto& up : tex_uploads) {
        if (up.sampler) {
            typedef void (*PFN_vkDestroySampler)(void*, uint64_t, const void*);
            auto destroySmp = reinterpret_cast<PFN_vkDestroySampler>(gipa(inst, "vkDestroySampler"));
            if (destroySmp) destroySmp(dev, up.sampler, nullptr);
        }
        if (up.view) reinterpret_cast<PFN_vkDestroyImageView>(gipa(inst, "vkDestroyImageView"))(dev, up.view, nullptr);
        if (up.image) reinterpret_cast<PFN_vkDestroyImage>(gipa(inst, "vkDestroyImage"))(dev, up.image, nullptr);
        if (up.mem) reinterpret_cast<PFN_vkFreeMemory>(gipa(inst, "vkFreeMemory"))(dev, up.mem, nullptr);
        if (up.staging_buf) {
            typedef void (*PFN_vkDestroyBuffer)(void*, uint64_t, const void*);
            auto destroyBuf = reinterpret_cast<PFN_vkDestroyBuffer>(gipa(inst, "vkDestroyBuffer"));
            if (destroyBuf) destroyBuf(dev, up.staging_buf, nullptr);
        }
        if (up.staging_mem) reinterpret_cast<PFN_vkFreeMemory>(gipa(inst, "vkFreeMemory"))(dev, up.staging_mem, nullptr);
    }
    reinterpret_cast<PFN_vkDestroyFramebuffer>(gipa(inst, "vkDestroyFramebuffer"))(dev, fb, nullptr);
    reinterpret_cast<PFN_vkDestroyRenderPass>(gipa(inst, "vkDestroyRenderPass"))(dev, rp, nullptr);
    if (depth_view) reinterpret_cast<PFN_vkDestroyImageView>(gipa(inst, "vkDestroyImageView"))(dev, depth_view, nullptr);
    if (depth_img) reinterpret_cast<PFN_vkDestroyImage>(gipa(inst, "vkDestroyImage"))(dev, depth_img, nullptr);
    if (depth_mem) reinterpret_cast<PFN_vkFreeMemory>(gipa(inst, "vkFreeMemory"))(dev, depth_mem, nullptr);
    reinterpret_cast<PFN_vkDestroyImageView>(gipa(inst, "vkDestroyImageView"))(dev, img_view, nullptr);
    reinterpret_cast<PFN_vkDestroyImage>(gipa(inst, "vkDestroyImage"))(dev, img, nullptr);
    reinterpret_cast<PFN_vkFreeMemory>(gipa(inst, "vkFreeMemory"))(dev, img_mem, nullptr);
    destroySM(dev, fs_mod, nullptr);
    destroySM(dev, vs_mod, nullptr);
    return ok;
}

void onLiftedCsfJobCallback(uint8_t opc, uint64_t insn, const uint64_t* cs_regs64, void* /*user_data*/) {
    if (!cs_regs64) return;
    auto getReg64 = [&](unsigned d_idx) -> uint64_t {
        if (d_idx + 1 >= 96) return 0;
        return (cs_regs64[d_idx] & 0xFFFFFFFFULL) | ((cs_regs64[d_idx + 1] & 0xFFFFFFFFULL) << 32);
    };
    uint8_t srt_sel = static_cast<uint8_t>((insn >> 40) & 0x3U);
    uint8_t spd_sel = static_cast<uint8_t>((insn >> 42) & 0x3U);
    uint64_t srt_gpu_va = getReg64(srt_sel * 2);
    uint64_t spd_gpu_va = getReg64(8 + spd_sel * 2);

    if (opc == MALI_CS_OPCODE_RUN_IDVS) {
        g_icd_csf_jobs_executed++;
        return;
    }
    if (opc == MALI_CS_OPCODE_RUN_FRAGMENT) {
        if (srt_gpu_va >= 0x10000ULL) {
            const uint64_t* srt = reinterpret_cast<const uint64_t*>(srt_gpu_va);
            if (srt[0] >= 0x10000ULL && srt[2] == sizeof(MaliCsfGraphicsDrawPacket)) {
                const auto* pkt = reinterpret_cast<const MaliCsfGraphicsDrawPacket*>(srt[0]);
                if (executeGraphicsDrawOnHostVulkan(g_icd_spirv_executor, *pkt)) {
                    g_icd_csf_jobs_executed++;
                }
            }
        }
        return;
    }

    uint32_t wg_x = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[32]));
    uint32_t wg_y = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[33]));
    uint32_t wg_z = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[34]));

    uint64_t shader_objc_va = 0;
    uint32_t shader_objc_bytes = 0;
    if (spd_gpu_va >= 0x10000ULL) {
        const uint64_t* spd = reinterpret_cast<const uint64_t*>(spd_gpu_va);
        shader_objc_va    = spd[0];
        shader_objc_bytes = static_cast<uint32_t>(spd[1] & 0xFFFFFFFFULL);
    }
    if (shader_objc_va >= 0x10000ULL && shader_objc_bytes >= 8 && shader_objc_bytes <= 65536) {
        const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(shader_objc_va);
        std::vector<ValhallDecodedInstr> disasm;
        auto spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(objc_words, shader_objc_bytes / 8, &disasm);
        const void* in_ptr = nullptr;
        void* out_ptr = nullptr;
        size_t copy_bytes = 64;
        if (srt_gpu_va >= 0x10000ULL) {
            const uint64_t* srt = reinterpret_cast<const uint64_t*>(srt_gpu_va);
            in_ptr  = reinterpret_cast<const void*>(srt[0]);
            out_ptr = reinterpret_cast<void*>(srt[1]);
            if (srt[2] >= 64 && srt[2] <= 65536) copy_bytes = static_cast<size_t>(srt[2]);
        }
        if (g_icd_spirv_executor.executeLiftedSpirv(spirv, in_ptr, out_ptr, copy_bytes, wg_x, wg_y, wg_z)) {
            g_icd_csf_jobs_executed++;
        }
    }
}

extern "C" uint64_t bionic_mali_open_stub(uint64_t path_addr, uint64_t flags) {
    int fd = mali_kbase_emu_open_direct(reinterpret_cast<const char*>(path_addr), static_cast<int>(flags));
    return (fd >= 0) ? static_cast<uint64_t>(fd) : static_cast<uint64_t>(-1LL);
}
extern "C" uint64_t bionic_mali_close_stub(uint64_t fd) {
    return static_cast<uint64_t>(static_cast<int64_t>(mali_kbase_emu_close_direct(static_cast<int>(fd))));
}
extern "C" uint64_t bionic_mali_kbase_ioctl_stub(uint64_t fd, uint64_t cmd, uint64_t arg_addr) {
    g_icd_kbase_ioctl_calls++;
    int rc = mali_kbase_emu_ioctl_direct(static_cast<int>(fd), static_cast<unsigned long>(cmd), reinterpret_cast<void*>(arg_addr));
    return (rc == 0) ? 0ULL : static_cast<uint64_t>(static_cast<int64_t>(rc));
}
extern "C" uint64_t bionic_mali_mmap_stub(uint64_t addr, uint64_t length, uint64_t prot,
                                          uint64_t flags, uint64_t fd, uint64_t offset) {
    void* p = mali_kbase_emu_mmap_direct(reinterpret_cast<void*>(addr), static_cast<size_t>(length),
                                         static_cast<int>(prot), static_cast<int>(flags),
                                         static_cast<int>(fd), static_cast<off_t>(offset));
    return reinterpret_cast<uint64_t>(p);
}
extern "C" uint64_t bionic_mali_munmap_stub(uint64_t addr, uint64_t length) {
    return static_cast<uint64_t>(static_cast<int64_t>(mali_kbase_emu_munmap_direct(reinterpret_cast<void*>(addr), static_cast<size_t>(length))));
}
extern "C" uint64_t bionic_mali_read_stub(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    return static_cast<uint64_t>(static_cast<int64_t>(mali_kbase_emu_read_direct(static_cast<int>(fd), reinterpret_cast<void*>(buf_addr), static_cast<size_t>(count))));
}
extern "C" uint64_t bionic_mali_poll_stub(uint64_t fds_addr, uint64_t nfds, uint64_t timeout) {
    return static_cast<uint64_t>(static_cast<int64_t>(mali_kbase_emu_poll_direct(reinterpret_cast<void*>(fds_addr), static_cast<unsigned long>(nfds), static_cast<int>(timeout))));
}
namespace {
void writeBionicStat64(void* dst_buf, const struct stat& st) {
    if (!dst_buf) return;
    uint8_t* p = static_cast<uint8_t*>(dst_buf);
    std::memset(p, 0, 128); // AArch64 Bionic struct stat layout is 128 bytes
    auto put64 = [&](size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); };
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); };
    put64(0x00, static_cast<uint64_t>(st.st_dev));
    put64(0x08, static_cast<uint64_t>(st.st_ino));
    put32(0x10, static_cast<uint32_t>(st.st_mode));
    put32(0x14, static_cast<uint32_t>(st.st_nlink));
    put32(0x18, static_cast<uint32_t>(st.st_uid));
    put32(0x1c, static_cast<uint32_t>(st.st_gid));
    put64(0x20, static_cast<uint64_t>(st.st_rdev));
    put64(0x30, static_cast<uint64_t>(static_cast<int64_t>(st.st_size)));
    put32(0x38, static_cast<uint32_t>(static_cast<int32_t>(st.st_blksize)));
    put64(0x40, static_cast<uint64_t>(static_cast<int64_t>(st.st_blocks)));
    put64(0x48, static_cast<uint64_t>(static_cast<int64_t>(st.st_atim.tv_sec)));
    put64(0x50, static_cast<uint64_t>(static_cast<int64_t>(st.st_atim.tv_nsec)));
    put64(0x58, static_cast<uint64_t>(static_cast<int64_t>(st.st_mtim.tv_sec)));
    put64(0x60, static_cast<uint64_t>(static_cast<int64_t>(st.st_mtim.tv_nsec)));
    put64(0x68, static_cast<uint64_t>(static_cast<int64_t>(st.st_ctim.tv_sec)));
    put64(0x70, static_cast<uint64_t>(static_cast<int64_t>(st.st_ctim.tv_nsec)));
}
} // namespace
extern "C" uint64_t bionic_mali_fstat_stub(uint64_t fd, uint64_t buf_addr) {
    struct stat st{};
    int rc = mali_kbase_emu_fstat_direct(static_cast<int>(fd), &st);
    if (rc == 0 && buf_addr) {
        writeBionicStat64(reinterpret_cast<void*>(buf_addr), st);
    }
    return (rc == 0) ? 0ULL : static_cast<uint64_t>(-1LL);
}
extern "C" uint64_t bionic_mali_stat_stub(uint64_t path_addr, uint64_t buf_addr) {
    struct stat st{};
    int rc = mali_kbase_emu_stat_direct(reinterpret_cast<const char*>(path_addr), &st);
    if (rc == 0 && buf_addr) {
        writeBionicStat64(reinterpret_cast<void*>(buf_addr), st);
    }
    return (rc == 0) ? 0ULL : static_cast<uint64_t>(-1LL);
}
extern "C" uint64_t bionic_mali_access_stub(uint64_t path_addr, uint64_t mode) {
    int rc = mali_kbase_emu_access_direct(reinterpret_cast<const char*>(path_addr), static_cast<int>(mode));
    return (rc == 0) ? 0ULL : static_cast<uint64_t>(-1LL);
}
extern "C" int access(const char* pathname, int mode) noexcept {
    return mali_kbase_emu_access_direct(pathname, mode);
}
extern "C" void __mali_stack_chk_fail_diag(AOTCPUState* cpu) {
    std::fprintf(stderr, "[MALI_ICD_ERROR] Bionic __stack_chk_fail! SP=0x%016lx LR=0x%016lx\n",
                 cpu->sp, cpu->x[30]);
    std::abort();
}

HostVkAllocatorTable g_host_vk_allocators;

extern "C" void aot_unhandled_plt(AOTCPUState* cpu, uint64_t target_va) {
    if (target_va == IMAGE_BASE + 0x01c650d0ULL) {
        // cctx hash-table malloc callback (0x01c650d0: bti c; mov x0, x1; b malloc@PLT)
        size_t sz = static_cast<size_t>(cpu->x[1] ? cpu->x[1] : cpu->x[0]);
        void* p = std::calloc(1, std::max<size_t>(sz, 64));
        cpu->x[0] = reinterpret_cast<uint64_t>(p);
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c650e0ULL) {
        // cctx hash-table free callback (0x01c650e0: bti c; b free@PLT)
        if (cpu->x[0] >= 0x10000ULL && cpu->x[0] < IMAGE_BASE) {
            std::free(reinterpret_cast<void*>(cpu->x[0]));
        }
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c9ecb0ULL) {
        // Delegate indirect call to lifted cmpbe_v2_compile_multiple_shaders
        aot_cmpbe_v2_compile_multiple_shaders(cpu);
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c9ec50ULL) {
        aot_cmpbe_v2_compile_single_shader(cpu);
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c680b0ULL) {
        // cmemp_hoard_back_hmem_alloc(x0, x1=hunk*, x2=out_size*)
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        auto* out_sz = reinterpret_cast<uint64_t*>(cpu->x[2]);
        uint64_t sz = hunk[2];
        void* p = std::malloc(std::max<size_t>(sz, 16));
        if (p) {
            hunk[1] = reinterpret_cast<uint64_t>(p);
            hunk[3] = reinterpret_cast<uint64_t>(p);
            if (out_sz) *out_sz = sz;
            cpu->x[0] = 0;
        } else {
            hunk[1] = 0;
            hunk[2] = 0;
            hunk[3] = 0;
            if (out_sz) *out_sz = 0;
            cpu->x[0] = 2;
        }
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c68110ULL || target_va == IMAGE_BASE + 0x01c68130ULL) {
        // cmemp_hoard_back_hmem_commit / query
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        auto* out_sz = reinterpret_cast<uint64_t*>(cpu->x[2]);
        if (out_sz) *out_sz = hunk[2];
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c68150ULL) {
        // cmemp_hoard_back_hmem_free
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        std::free(reinterpret_cast<void*>(hunk[3]));
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c68160ULL) {
        // cmemp_hoard_back_hmem_map
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        auto* out_map = reinterpret_cast<uint64_t*>(cpu->x[2]);
        if (out_map) {
            out_map[1] = hunk[1];
            out_map[2] = 0;
        }
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c68180ULL ||
        target_va == IMAGE_BASE + 0x01c68190ULL ||
        target_va == IMAGE_BASE + 0x01c681a0ULL) {
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c67af0ULL && aot_kbase_mem_alloc) {
        // cmemp_hoard_back_base_alloc(x0=kctx, x1=hunk*, x2=out_commit_sz*, x3=extent, x4=flags)
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        auto* out_sz = reinterpret_cast<uint64_t*>(cpu->x[2]);
        uint64_t va_pages = std::max<uint64_t>(1ULL, (hunk[2] + 4095ULL) >> 12);
        uint64_t commit_pages = out_sz ? ((std::max<uint64_t>(1ULL, *out_sz) + 4095ULL) >> 12) : va_pages;
        uint64_t flags = cpu->x[4] & 0xFFFF00003FFFFFFFULL;
        cpu->x[1] = va_pages;
        cpu->x[2] = commit_pages;
        cpu->x[3] = 0;
        cpu->x[4] = flags & ~0x10000ULL;
        cpu->x[5] = 0;
        cpu->x[6] = 0;
        cpu->x[7] = 0;
        aot_kbase_mem_alloc(cpu);
        uint64_t raw_va = cpu->x[0];
        hunk[3] = raw_va;
        if (raw_va == 0 || raw_va == ~0ULL) {
            hunk[1] = 0;
            hunk[2] = 0;
            if (out_sz) *out_sz = 0;
            cpu->x[0] = 3;
        } else {
            hunk[1] = raw_va & ~0xFFFULL;
            hunk[2] = va_pages << 12;
            if (out_sz) *out_sz = commit_pages << 12;
            cpu->x[0] = 0;
        }
        return;
    }
    if (target_va == IMAGE_BASE + 0x01c67c20ULL || target_va == IMAGE_BASE + 0x01c67c90ULL) {
        auto* hunk = reinterpret_cast<uint64_t*>(cpu->x[1]);
        auto* out_sz = reinterpret_cast<uint64_t*>(cpu->x[2]);
        if (out_sz) *out_sz = hunk[2];
        cpu->x[0] = 0;
        return;
    }
    if (target_va == IMAGE_BASE + 0x01ae2980ULL ||
        target_va == IMAGE_BASE + 0x01ae2220ULL ||
        target_va == IMAGE_BASE + 0x01c35540ULL ||
        (target_va >= IMAGE_BASE + 0x01c67ac0ULL && target_va <= IMAGE_BASE + 0x01c67ff0ULL)) {
        // Indirectly-called internal helpers not yet lifted in r54p3_lifted.o
        cpu->x[0] = 0;
        return;
    }
    if (g_host_vk_allocators.tryDispatch(cpu, target_va)) {
        return;
    }
    std::fprintf(stderr, "[MALI_ICD_ERROR] Unhandled indirect/PLT target VA: 0x%016lx LR=0x%016lx\n",
                 target_va, cpu->x[30]);
    std::abort();
}

extern "C" void aot_host_hle_dispatch(AOTCPUState* cpu, uint64_t target_va) {
    if (target_va == HLE_MALLOC_MAGIC) {
        size_t sz = static_cast<size_t>(cpu->x[1]);
        void* p = std::calloc(1, std::max<size_t>(sz, 16));
        cpu->x[0] = reinterpret_cast<uint64_t>(p);
        return;
    }
    if (target_va == HLE_FREE_MAGIC) {
        void* p = reinterpret_cast<void*>(cpu->x[1]);
        std::free(p);
        cpu->x[0] = 0;
        return;
    }
    if (target_va == HLE_VK_ALLOC_MAGIC) {
        size_t sz = static_cast<size_t>(cpu->x[1]);
        size_t align = std::max<size_t>(static_cast<size_t>(cpu->x[2]), sizeof(void*));
        void* p = nullptr;
        if (posix_memalign(&p, align, std::max<size_t>(sz, 16)) == 0 && p) {
            std::memset(p, 0, std::max<size_t>(sz, 16));
        }
        cpu->x[0] = reinterpret_cast<uint64_t>(p);
        return;
    }
    if (target_va == HLE_VK_FREE_MAGIC) {
        void* p = reinterpret_cast<void*>(cpu->x[1]);
        std::free(p);
        cpu->x[0] = 0;
        return;
    }
    if (target_va == HLE_TLSDESC_MAGIC) {
        uint64_t tls_ptr = cpu->x[0];
        cpu->x[0] = *reinterpret_cast<const uint64_t*>(tls_ptr + 8);
        return;
    }
    aot_indirect_dispatch(cpu, target_va);
}

MaliAotGlobalRuntime g_aot_rt;

MaliThreadGuestContext& maliGetThreadGuestContext() {
    thread_local MaliThreadGuestContext tctx;
    tctx.ensureAllocated(reinterpret_cast<uint64_t>(g_aot_rt.tls_mem));
    return tctx;
}

struct MaliBionicPthreadLaunchArg {
    uint64_t guest_fn_va;
    uint64_t guest_arg;
};

static void* mali_bionic_pthread_trampoline(void* raw) {
    auto* launch = static_cast<MaliBionicPthreadLaunchArg*>(raw);
    uint64_t fn_va = launch->guest_fn_va;
    uint64_t arg   = launch->guest_arg;
    delete launch;

    if (!fn_va) return nullptr;

    constexpr size_t kThreadStackSize = 1ULL << 20; // 1 MiB
    uint8_t* stack = static_cast<uint8_t*>(mmap(nullptr, kThreadStackSize,
                                                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (!stack || stack == MAP_FAILED) return nullptr;
    uint8_t* tls   = static_cast<uint8_t*>(mmap(nullptr, 4096,
                                                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    AOTCPUState tcpu{};
    tcpu.sp        = reinterpret_cast<uint64_t>(stack + kThreadStackSize - 0x1000);
    tcpu.x[29]     = tcpu.sp;
    tcpu.tpidr_el0 = (tls && tls != MAP_FAILED) ? reinterpret_cast<uint64_t>(tls) : 0ULL;
    tcpu.x[0]      = arg;
    tcpu.x[30]     = 0xDEAD0000ULL;
    if (tls && tls != MAP_FAILED) {
        *reinterpret_cast<uint64_t*>(tls + 0x28) = BIONIC_STACK_GUARD_CANARY;
    }

    aot_indirect_dispatch(&tcpu, fn_va);
    uint64_t ret = tcpu.x[0];
    munmap(stack, kThreadStackSize);
    if (tls && tls != MAP_FAILED) munmap(tls, 4096);
    return reinterpret_cast<void*>(ret);
}

extern "C" {
__attribute__((visibility("default"), weak, used))
int __mali_bionic_pthread_create(uint64_t thread_out_va, uint64_t /*attr_va*/, uint64_t start_routine_va, uint64_t arg) {
    pthread_t host_tid = 0;
    auto* launch = new MaliBionicPthreadLaunchArg{start_routine_va, arg};
    int rc = pthread_create(&host_tid, nullptr, &mali_bionic_pthread_trampoline, launch);
    if (rc != 0) {
        delete launch;
        return rc;
    }
    if (thread_out_va) {
        *reinterpret_cast<uint64_t*>(thread_out_va) = static_cast<uint64_t>(host_tid);
    }
    return 0;
}

__attribute__((visibility("default"), weak, used))
int __mali_bionic_pthread_join(uint64_t thread_val, uint64_t retval_out_va) {
    void* ret = nullptr;
    int rc = pthread_join(static_cast<pthread_t>(thread_val), retval_out_va ? &ret : nullptr);
    if (rc == 0 && retval_out_va) {
        *reinterpret_cast<uint64_t*>(retval_out_va) = reinterpret_cast<uint64_t>(ret);
    }
    return rc;
}

__attribute__((visibility("default"), weak, used))
int __mali_bionic_pthread_detach(uint64_t thread_val) {
    return pthread_detach(static_cast<pthread_t>(thread_val));
}
} // extern "C"

std::mutex   g_icd_mutex;
IcdBufferMap g_buffers;
IcdImageMap  g_images;

// Helper: Allocate a /dev/mali0 BO via AOT aot_kbase_mem_alloc or ioctl
uint64_t icdAllocKbaseBo(MaliIcdDevice* dev, uint64_t pages, uint64_t flags) {
    int32_t fd = (dev && dev->mali_fd >= 0) ? dev->mali_fd : 100;
    if (aot_kbase_version_check_csf && aot_kbase_mem_alloc && dev && !dev->kctx_mem.empty()) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        auto& cpu = tctx.cpu;
        uint64_t* sp_args = reinterpret_cast<uint64_t*>(cpu.sp);
        sp_args[0] = 0;
        sp_args[1] = 0;
        cpu.x[0] = reinterpret_cast<uint64_t>(dev->kctx_mem.data());
        cpu.x[1] = pages;
        cpu.x[2] = pages;
        cpu.x[3] = 0;
        cpu.x[4] = flags;
        cpu.x[5] = 0; /* fixed_address = 0 for r54p3 kbase_mem_alloc (0x01c69c00) */
        cpu.x[6] = 0;
        cpu.x[7] = 0;
        aot_kbase_mem_alloc(&cpu);
        if (cpu.x[0] > 0x1000ULL && cpu.x[0] != ~0ULL) {
            uint64_t va = cpu.x[0] & ~0xFFFULL;
            if ((flags & BASE_MEM_SAME_VA) != 0 && (flags & BASE_MEM_PROT_GPU_EX) == 0 && va < 0x10000000ULL) {
                return bionic_mali_mmap_stub(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<uint64_t>(fd), va);
            }
            return va;
        }
    }
    uint64_t req[8]{};
    req[0] = pages;
    req[1] = pages;
    req[3] = flags;
    bionic_mali_kbase_ioctl_stub(static_cast<uint64_t>(fd), 0xc040803bU, reinterpret_cast<uint64_t>(req));
    uint64_t cookie_or_va = req[1];
    if ((flags & BASE_MEM_SAME_VA) != 0 && (flags & BASE_MEM_PROT_GPU_EX) == 0) {
        return bionic_mali_mmap_stub(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<uint64_t>(fd), cookie_or_va);
    }
    return cookie_or_va;
}

