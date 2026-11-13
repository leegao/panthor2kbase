// ============================================================================
// src/r54p3_vulkan_icd/mali_icd_internal.cpp
// Definitions of shared AOT runtime state, Bionic HLE bridges, CSF job
// callback, and HostSpirvExecutor dispatch for r54p3_vulkan_icd.
// ============================================================================
#define AOT_SO_PROFILER_IMPLEMENTATION
#include "mali_icd_internal.h"

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
    std::vector<uint32_t> fs_spv = (!pkt.has_frag_const && pkt.fs_spirv_ptr && hasSpirvOpFunction(pkt.fs_spirv_ptr, pkt.fs_spirv_words))
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
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, std::max<uint64_t>(req.size, img_bytes), 0};
    reinterpret_cast<HostSpirvExecutor::PFN_vkAllocateMemory>(gipa(inst, "vkAllocateMemory"))(dev, &mai, nullptr, &img_mem);
    reinterpret_cast<PFN_vkBindImageMemory>(gipa(inst, "vkBindImageMemory"))(dev, img, img_mem, 0);

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = reinterpret_cast<VkImage>(img);
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    reinterpret_cast<PFN_vkCreateImageView>(gipa(inst, "vkCreateImageView"))(dev, &ivci, nullptr, &img_view);

    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 0, nullptr};
    uint64_t rp = 0, fb = 0, pipe = 0;
    int32_t rprc = reinterpret_cast<PFN_vkCreateRenderPass>(gipa(inst, "vkCreateRenderPass"))(dev, &rpci, nullptr, &rp);

    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, reinterpret_cast<VkRenderPass>(rp), 1, reinterpret_cast<VkImageView*>(&img_view), W, H, 1};
    int32_t fbrc = reinterpret_cast<PFN_vkCreateFramebuffer>(gipa(inst, "vkCreateFramebuffer"))(dev, &fbci, nullptr, &fb);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, reinterpret_cast<VkShaderModule>(vs_mod), "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, reinterpret_cast<VkShaderModule>(fs_mod), "main", nullptr}
    };
    VkVertexInputBindingDescription vbind{0, pkt.stride_bytes ? pkt.stride_bytes : 16U, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &vbind, 1, &vattr};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkViewport vp{0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0.0f, nullptr, VK_FALSE, VK_FALSE};
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xFU;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba, {0,0,0,0}};
    uint64_t gfx_pl = 0;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 0, nullptr};
    int32_t plrc = reinterpret_cast<HostSpirvExecutor::PFN_vkCreatePipelineLayout>(gipa(inst, "vkCreatePipelineLayout"))(dev, &plci, nullptr, &gfx_pl);

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2; gpci.pStages = stages;
    gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps; gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
    gpci.layout = reinterpret_cast<VkPipelineLayout>(gfx_pl ? gfx_pl : exec.pipeline_layout);
    gpci.renderPass = reinterpret_cast<VkRenderPass>(rp);
    gpci.basePipelineIndex = -1;
    int32_t gprc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(inst, "vkCreateGraphicsPipelines"))(dev, 0, 1, &gpci, nullptr, &pipe);
    if (gprc != 0 || pipe == 0) {
        std::fprintf(stderr, "[HostSpirvExecutor] vkCreateGraphicsPipelines failed rc=%d dev='%s' pipe=%llu rp=%llu(rc=%d) fb=%llu(rc=%d) gfx_pl=%llu(rc=%d) vs=%llu(%zu) fs=%llu(%zu) has_fc=%u\n",
                     gprc, exec.device_name.c_str(), (unsigned long long)pipe, (unsigned long long)rp, rprc, (unsigned long long)fb, fbrc,
                     (unsigned long long)gfx_pl, plrc, (unsigned long long)vs_mod, vs_spv.size(), (unsigned long long)fs_mod, fs_spv.size(), pkt.has_frag_const);
    }

    bool ok = false;
    if (gprc == 0 && pipe != 0) {
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
        VkClearValue clr{};
        clr.color.float32[0] = (pkt.clear_rgba8 & 0xFFU) / 255.0f;
        clr.color.float32[1] = ((pkt.clear_rgba8 >> 8) & 0xFFU) / 255.0f;
        clr.color.float32[2] = ((pkt.clear_rgba8 >> 16) & 0xFFU) / 255.0f;
        clr.color.float32[3] = ((pkt.clear_rgba8 >> 24) & 0xFFU) / 255.0f;
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, reinterpret_cast<VkRenderPass>(rp), reinterpret_cast<VkFramebuffer>(fb), {{0,0},{W,H}}, 1, &clr};
        reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(inst, "vkCmdBeginRenderPass"))(exec.cmd_buf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        reinterpret_cast<HostSpirvExecutor::PFN_vkCmdBindPipeline>(gipa(inst, "vkCmdBindPipeline"))(exec.cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        uint64_t vb_off = 0;
        reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(inst, "vkCmdBindVertexBuffers"))(exec.cmd_buf, 0, 1, &exec.buf_in, &vb_off);
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
    if (gfx_pl) {
        typedef void (*PFN_vkDestroyPipelineLayout)(void*, uint64_t, const void*);
        auto destroyPL = reinterpret_cast<PFN_vkDestroyPipelineLayout>(gipa(inst, "vkDestroyPipelineLayout"));
        if (destroyPL) destroyPL(dev, gfx_pl, nullptr);
    }
    reinterpret_cast<PFN_vkDestroyFramebuffer>(gipa(inst, "vkDestroyFramebuffer"))(dev, fb, nullptr);
    reinterpret_cast<PFN_vkDestroyRenderPass>(gipa(inst, "vkDestroyRenderPass"))(dev, rp, nullptr);
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
    std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
    auto& cpu = g_aot_rt.cpu;
    if (aot_kbase_version_check_csf && aot_kbase_mem_alloc && dev && !dev->kctx_mem.empty()) {
        g_aot_rt.resetGuestStackLocked();
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

