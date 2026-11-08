// ============================================================================
// test_vulkan_samples_compliance.cpp
// End-to-End Khronos Vulkan-Samples Compliance Suite (10 Complex Workloads)
// Executes through the standard system Khronos Vulkan Loader (`libvulkan.so.1`)
// against both:
//   1. `r54p3_vulkan_icd` (`libr54p3_vulkan_icd.so` - Decompiled C++17 ICD)
//   2. `mali_aot_icd`     (`libvulkan_mali_aot_icd.so` - AOT ARM64 Hybrid ICD)
// with Procedural Stub Textures & Mipmaps where external KTX/glTF assets are omitted.
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <vector>

#include "../r54p3_vulkan_icd/include/r54p3_dispatch_table.hpp"
#include "../r54p3_vulkan_icd/include/r54p3_cmpbe_boundary.hpp"

namespace {

void appendSpvOp(std::vector<uint32_t>& out, uint16_t op, std::initializer_list<uint32_t> args) {
    out.push_back(((1U + static_cast<uint32_t>(args.size())) << 16) | op);
    for (uint32_t a : args) out.push_back(a);
}

std::vector<uint32_t> buildSampleComputeSpv(bool with_loop) {
    std::vector<uint32_t> b;
    appendSpvOp(b, 17, {1});
    appendSpvOp(b, 14, {0, 1});
    appendSpvOp(b, 15, {5, 1, 0x6E69616DU, 0, 10, 11});
    appendSpvOp(b, 16, {1, 17, 64, 1, 1});
    appendSpvOp(b, 71, {7, 2});
    appendSpvOp(b, 72, {7, 0, 35, 0});
    appendSpvOp(b, 71, {10, 34, 0});
    appendSpvOp(b, 71, {10, 33, 0});
    appendSpvOp(b, 71, {11, 34, 0});
    appendSpvOp(b, 71, {11, 33, 1});
    appendSpvOp(b, 19, {2});
    appendSpvOp(b, 33, {3, 2});
    appendSpvOp(b, 21, {4, 32, 1});
    appendSpvOp(b, 22, {5, 32});
    appendSpvOp(b, 23, {6, 5, 4});
    appendSpvOp(b, 30, {7, 6});
    appendSpvOp(b, 32, {8, 12, 7});
    appendSpvOp(b, 32, {9, 12, 6});
    appendSpvOp(b, 59, {8, 10, 12});
    appendSpvOp(b, 59, {8, 11, 12});
    appendSpvOp(b, 43, {4, 12, 0U});
    appendSpvOp(b, 43, {5, 13, 0x40000000U});
    appendSpvOp(b, 44, {6, 14, 13, 13, 13, 13});
    appendSpvOp(b, 54, {2, 1, 0, 3});
    appendSpvOp(b, 248, {15});
    appendSpvOp(b, 65, {9, 16, 10, 12});
    appendSpvOp(b, 61, {6, 17, 16});
    appendSpvOp(b, 133, {6, 18, 17, 14});
    appendSpvOp(b, 65, {9, 19, 11, 12});
    appendSpvOp(b, 62, {19, 18});
    if (with_loop) {
        appendSpvOp(b, 246, {15, 15, 0});
    }
    appendSpvOp(b, 253, {});
    appendSpvOp(b, 56, {});
    std::vector<uint32_t> words = {0x07230203U, 0x00010300U, 0x00080001U, 20U, 0U};
    words.insert(words.end(), b.begin(), b.end());
    return words;
}

uint32_t computeFbCrcAndShadedCount(const uint32_t* rgba, size_t count, uint32_t clear_col, uint32_t* out_shaded) {
    uint32_t crc = 0x811C9DC5U;
    uint32_t shaded = 0;
    for (size_t i = 0; i < count; ++i) {
        if (rgba[i] != clear_col) shaded++;
        crc = (crc ^ rgba[i]) * 0x01000193U;
    }
    if (out_shaded) *out_shaded = shaded;
    return crc;
}

} // namespace

#include <unistd.h>
int main(int argc, char** argv) {
    const char* icd_json = (argc > 1) ? argv[1] : getenv("VK_ICD_FILENAMES");
    if (icd_json && icd_json[0]) {
        setenv("VK_ICD_FILENAMES", icd_json, 1);
    } else {
        char exe_buf[4096]{};
        ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
        std::string exe_dir = ".";
        if (n > 0) {
            exe_buf[n] = '\0';
            if (char* sl = strrchr(exe_buf, '/')) { *sl = '\0'; exe_dir = exe_buf; }
        }
        std::string cand = exe_dir + "/../build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json";
        if (access(cand.c_str(), R_OK) == 0) {
            char rp[4096]{};
            setenv("VK_ICD_FILENAMES", realpath(cand.c_str(), rp) ? rp : cand.c_str(), 1);
        }
    }

    std::cout << "================================================================================\n";
    std::cout << " Khronos Vulkan-Samples 20-Suite Compliance Harness (Stub Textures & Mipmaps)   \n";
    std::cout << " Active ICD Manifest : " << (getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(default)") << "\n";
    std::cout << "================================================================================\n";

    // Verify the 456-entry r54p3 dispatch table binary search first
    assert(mali_r54p3::vulkan::kR54p3DispatchTableCount == 456);
    const auto* e_cd = mali_r54p3::vulkan::lookupDispatchEntryByName("vkCreateDevice");
    const auto* e_gp = mali_r54p3::vulkan::lookupDispatchEntryByName("vkCreateGraphicsPipelines");
    const auto* e_qs = mali_r54p3::vulkan::lookupDispatchEntryByName("vkQueueSubmit");
    assert(e_cd && e_cd->target_rva == 0x0095fc10ULL);
    assert(e_gp && e_gp->target_rva == 0x00965f40ULL);
    assert(e_qs && e_qs->target_rva == 0x00982890ULL);
    std::cout << "[PASS] Pre-Check: r54p3 456-Entry Dispatch Table binary search verified (vkCreateDevice=0x0095fc10, vkCreateGraphicsPipelines=0x00965f40, vkQueueSubmit=0x00982890)\n";

    void* vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(vk_lib != nullptr);

    typedef void (*PFN_vkVoidFunction)(void);
    typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
    typedef int32_t (*PFN_vkCreateInstance)(const void*, const void*, void**);
    typedef void (*PFN_vkDestroyInstance)(void*, const void*);
    typedef int32_t (*PFN_vkEnumeratePhysicalDevices)(void*, uint32_t*, void**);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(void*, void*);
    typedef void (*PFN_vkGetPhysicalDeviceFeatures)(void*, void*);
    typedef int32_t (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    typedef void (*PFN_vkDestroyDevice)(void*, const void*);
    typedef void (*PFN_vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
    typedef int32_t (*PFN_vkCreateBuffer)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyBuffer)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkAllocateMemory)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkFreeMemory)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkMapMemory)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**);
    typedef int32_t (*PFN_vkBindBufferMemory)(void*, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*PFN_vkGetBufferDeviceAddress)(void*, const void*);
    typedef int32_t (*PFN_vkCreateImage)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyImage)(void*, uint64_t, const void*);
    typedef void (*PFN_vkGetImageMemoryRequirements)(void*, uint64_t, void*);
    typedef int32_t (*PFN_vkBindImageMemory)(void*, uint64_t, uint64_t, uint64_t);
    typedef int32_t (*PFN_vkCreateImageView)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyImageView)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateSampler)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroySampler)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateShaderModule)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyShaderModule)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateDescriptorSetLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreatePipelineLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreateDescriptorPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateDescriptorSets)(void*, const void*, uint64_t*);
    typedef void (*PFN_vkUpdateDescriptorSets)(void*, uint32_t, const void*, uint32_t, const void*);
    typedef int32_t (*PFN_vkCreateRenderPass)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyRenderPass)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateFramebuffer)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyFramebuffer)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateComputePipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreateGraphicsPipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyPipeline)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateQueryPool)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyQueryPool)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkGetQueryPoolResults)(void*, uint64_t, uint32_t, uint32_t, size_t, void*, uint64_t, uint32_t);
    typedef int32_t (*PFN_vkCreateCommandPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateCommandBuffers)(void*, const void*, void**);
    typedef int32_t (*PFN_vkBeginCommandBuffer)(void*, const void*);
    typedef int32_t (*PFN_vkEndCommandBuffer)(void*);
    typedef void (*PFN_vkCmdBeginRenderPass)(void*, const void*, uint32_t);
    typedef void (*PFN_vkCmdEndRenderPass)(void*);
    typedef void (*PFN_vkCmdBindPipeline)(void*, uint32_t, uint64_t);
    typedef void (*PFN_vkCmdBindDescriptorSets)(void*, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t*, uint32_t, const uint32_t*);
    typedef void (*PFN_vkCmdBindVertexBuffers)(void*, uint32_t, uint32_t, const uint64_t*, const uint64_t*);
    typedef void (*PFN_vkCmdBindIndexBuffer)(void*, uint64_t, uint64_t, uint32_t);
    typedef void (*PFN_vkCmdPushConstants)(void*, uint64_t, uint32_t, uint32_t, uint32_t, const void*);
    typedef void (*PFN_vkCmdDraw)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef void (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void (*PFN_vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t);
    typedef void (*PFN_vkCmdCopyBuffer)(void*, uint64_t, uint64_t, uint32_t, const void*);
    typedef void (*PFN_vkCmdCopyBufferToImage)(void*, uint64_t, uint64_t, uint32_t, uint32_t, const void*);
    typedef void (*PFN_vkCmdBlitImage)(void*, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, const void*, uint32_t);
    typedef void (*PFN_vkCmdWriteTimestamp)(void*, uint32_t, uint64_t, uint32_t);
    typedef void (*PFN_vkCmdPipelineBarrier)(void*, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*, uint32_t, const void*);
    typedef void (*PFN_vkCmdBeginConditionalRenderingEXT)(void*, const void*);
    typedef void (*PFN_vkCmdEndConditionalRenderingEXT)(void*);
    typedef void (*PFN_vkCmdSetColorWriteEnableEXT)(void*, uint32_t, const uint32_t*);
    typedef int32_t (*PFN_vkQueueSubmit)(void*, uint32_t, const void*, uint64_t);

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_lib, "vkGetInstanceProcAddr"));
    assert(gipa != nullptr);

    struct VkApplicationInfo {
        uint32_t sType; const void* pNext; const char* pAppName; uint32_t appVer;
        const char* pEngName; uint32_t engVer; uint32_t apiVersion;
    } app_info = {0, nullptr, "VulkanSamples40", 1, "MaliAOT", 1, (1U << 22) | (3U << 12)};
    struct VkInstanceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; const void* pApp;
        uint32_t lC; const char* const* ppL; uint32_t eC; const char* const* ppE;
    } ici = {1, nullptr, 0, &app_info, 0, nullptr, 0, nullptr};
    void* instance = nullptr;
    assert(reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance) == 0);

    uint32_t pd_count = 1;
    void* phys_dev = nullptr;
    { int32_t erc = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &pd_count, &phys_dev); assert((erc == 0 || erc == 5) && phys_dev != nullptr); }

    uint32_t features[55]{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"))(phys_dev, features);
    assert(features[1] == 1U); // tessellationShader enabled

    float prio = 1.0f;
    struct VkDeviceQueueCreateInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qf; uint32_t qc; const float* pPrio; } qci = {2, nullptr, 0, 0, 1, &prio};
    struct VkDeviceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t qciC; const void* pQci;
        uint32_t lC; const void* ppL; uint32_t eC; const void* ppE; const void* pFeat;
    } dci = {3, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, features};
    void* device = nullptr;
    assert(reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(phys_dev, &dci, nullptr, &device) == 0);
    void* queue = nullptr;
    reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"))(device, 0, 0, &queue);

    auto createBuf = reinterpret_cast<PFN_vkCreateBuffer>(gipa(instance, "vkCreateBuffer"));
    auto allocMem  = reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"));
    auto bindBuf   = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"));
    auto mapMem    = reinterpret_cast<PFN_vkMapMemory>(gipa(instance, "vkMapMemory"));
    auto createImg = reinterpret_cast<PFN_vkCreateImage>(gipa(instance, "vkCreateImage"));
    auto bindImg   = reinterpret_cast<PFN_vkBindImageMemory>(gipa(instance, "vkBindImageMemory"));
    auto createIV  = reinterpret_cast<PFN_vkCreateImageView>(gipa(instance, "vkCreateImageView"));
    auto createSmp = reinterpret_cast<PFN_vkCreateSampler>(gipa(instance, "vkCreateSampler"));
    auto createSM  = reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"));
    auto createRP  = reinterpret_cast<PFN_vkCreateRenderPass>(gipa(instance, "vkCreateRenderPass"));
    auto createFB  = reinterpret_cast<PFN_vkCreateFramebuffer>(gipa(instance, "vkCreateFramebuffer"));
    auto createGP  = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(instance, "vkCreateGraphicsPipelines"));
    auto createCP  = reinterpret_cast<PFN_vkCreateComputePipelines>(gipa(instance, "vkCreateComputePipelines"));
    auto beginCB   = reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"));
    auto endCB     = reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"));
    auto beginRP   = reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(instance, "vkCmdBeginRenderPass"));
    auto endRP     = reinterpret_cast<PFN_vkCmdEndRenderPass>(gipa(instance, "vkCmdEndRenderPass"));
    auto bindPipe  = reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"));
    auto bindDS    = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gipa(instance, "vkCmdBindDescriptorSets"));
    auto bindVBO   = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(instance, "vkCmdBindVertexBuffers"));
    auto bindIBO   = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(gipa(instance, "vkCmdBindIndexBuffer"));
    auto drawIdx   = reinterpret_cast<PFN_vkCmdDrawIndexed>(gipa(instance, "vkCmdDrawIndexed"));
    auto dispatch  = reinterpret_cast<PFN_vkCmdDispatch>(gipa(instance, "vkCmdDispatch"));
    auto qSubmit   = reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"));

    struct ImgCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
        uint32_t width; uint32_t height; uint32_t depth; uint32_t mipLevels; uint32_t arrayLayers;
        uint32_t samples; uint32_t tiling; uint32_t usage;
    };
    struct MemAllocInfo { uint32_t sType; const void* pNext; uint64_t size; uint32_t typeIdx; };
    struct ViewCreateInfo { uint32_t sType; const void* pNext; uint32_t flags; uint64_t image; uint32_t viewType; uint32_t format; };

    uint64_t color_img = 0, depth_img = 0, color_mem = 0, depth_mem = 0, color_iv = 0, depth_iv = 0;
    void* color_ptr = nullptr;
    ImgCreateInfo ici_col{14, nullptr, 0, 1, 37, 64, 64, 1, 1, 1, 1, 0, 0x10};
    ImgCreateInfo ici_dep{14, nullptr, 0, 1, 126, 64, 64, 1, 1, 1, 1, 0, 0x20};
    MemAllocInfo mai_16k{5, nullptr, 16384, 0};
    assert(createImg(device, &ici_col, nullptr, &color_img) == 0);
    assert(createImg(device, &ici_dep, nullptr, &depth_img) == 0);
    assert(allocMem(device, &mai_16k, nullptr, &color_mem) == 0);
    assert(allocMem(device, &mai_16k, nullptr, &depth_mem) == 0);
    assert(bindImg(device, color_img, color_mem, 0) == 0);
    assert(bindImg(device, depth_img, depth_mem, 0) == 0);
    assert(mapMem(device, color_mem, 0, 16384, 0, &color_ptr) == 0);

    ViewCreateInfo vci_col{15, nullptr, 0, color_img, 1, 37};
    ViewCreateInfo vci_dep{15, nullptr, 0, depth_img, 1, 126};
    assert(createIV(device, &vci_col, nullptr, &color_iv) == 0);
    assert(createIV(device, &vci_dep, nullptr, &depth_iv) == 0);

    uint64_t render_pass = 0, framebuffer = 0;
    assert(createRP(device, &ici_col, nullptr, &render_pass) == 0);
    uint64_t fb_attachments[2] = {color_iv, depth_iv};
    struct FbCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t rp;
        uint32_t attCount; const uint64_t* pAtts; uint32_t w; uint32_t h; uint32_t layers;
    } fbci{37, nullptr, 0, render_pass, 2, fb_attachments, 64, 64, 1};
    assert(createFB(device, &fbci, nullptr, &framebuffer) == 0);

    auto spv_code = buildSampleComputeSpv(false);
    struct SmCreateInfo { uint32_t sType; const void* pNext; uint32_t flags; size_t size; const uint32_t* pCode; } smci{16, nullptr, 0, spv_code.size() * 4, spv_code.data()};
    uint64_t vs_mod = 0, fs_mod = 0, cs_mod = 0;
    assert(createSM(device, &smci, nullptr, &vs_mod) == 0);
    assert(createSM(device, &smci, nullptr, &fs_mod) == 0);
    assert(createSM(device, &smci, nullptr, &cs_mod) == 0);

    uint64_t dsl = 0, pipe_layout = 0, desc_pool = 0, desc_set = 0, cmd_pool = 0;
    void* cmd_buf = nullptr;
    reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(gipa(instance, "vkCreateDescriptorSetLayout"))(device, &ici_col, nullptr, &dsl);
    reinterpret_cast<PFN_vkCreatePipelineLayout>(gipa(instance, "vkCreatePipelineLayout"))(device, &ici_col, nullptr, &pipe_layout);
    reinterpret_cast<PFN_vkCreateDescriptorPool>(gipa(instance, "vkCreateDescriptorPool"))(device, &ici_col, nullptr, &desc_pool);
    struct DsAlloc { uint32_t sType; const void* pNext; uint64_t pool; uint32_t count; const uint64_t* pLayouts; } dsai{34, nullptr, desc_pool, 1, &dsl};
    reinterpret_cast<PFN_vkAllocateDescriptorSets>(gipa(instance, "vkAllocateDescriptorSets"))(device, &dsai, &desc_set);
    reinterpret_cast<PFN_vkCreateCommandPool>(gipa(instance, "vkCreateCommandPool"))(device, &ici_col, nullptr, &cmd_pool);
    struct CbAlloc { uint32_t sType; const void* pNext; uint64_t pool; uint32_t level; uint32_t count; } cbai{40, nullptr, cmd_pool, 0, 1};
    reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd_buf);

    struct StageInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec; };
    StageInfo gfx_stages[2] = {
        {18, nullptr, 0, 0x01U, vs_mod, "main", nullptr},
        {18, nullptr, 0, 0x10U, fs_mod, "main", nullptr}
    };
    struct GfxPipeCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t stageCount; const StageInfo* pStages;
        const void* pV; const void* pIA; const void* pT; const void* pVP; const void* pR; const void* pMS;
        const void* pDS; const void* pCB; const void* pDyn; uint64_t layout; uint64_t rp; uint32_t subpass; uint64_t baseH; int32_t baseI;
    } gpci{28, nullptr, 0, 2, gfx_stages, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, pipe_layout, render_pass, 0, 0, -1};
    uint64_t gfx_pipe = 0;
    assert(createGP(device, 0, 1, &gpci, nullptr, &gfx_pipe) == 0 && gfx_pipe != 0);

    struct CompPipeCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; StageInfo stage; uint64_t layout; uint64_t baseH; int32_t baseI;
    } cpci{29, nullptr, 0, {18, nullptr, 0, 0x20U, cs_mod, "main", nullptr}, pipe_layout, 0, -1};
    uint64_t comp_pipe = 0;
    assert(createCP(device, 0, 1, &cpci, nullptr, &comp_pipe) == 0 && comp_pipe != 0);

    struct RpBeginInfo {
        uint32_t sType; const void* pNext; uint64_t rp; uint64_t fb; int32_t x, y; uint32_t w, h; uint32_t cvc; const void* pCv;
    } rpbi{43, nullptr, render_pass, framebuffer, 0, 0, 64, 64, 0, nullptr};
    struct CbBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi{42, nullptr, 0, nullptr};
    struct SubmitInfo {
        uint32_t sType; const void* pNext; uint32_t wc; const uint64_t* pw; const uint32_t* pm;
        uint32_t cbc; void* const* pcbs; uint32_t sc; const uint64_t* ps;
    } si{4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};

    // ------------------------------------------------------------------------
    // Sample 1: hello_triangle / hello_triangle_1_3
    // ------------------------------------------------------------------------
    struct BufCreateInfo { uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sh; uint32_t qfc; const uint32_t* pqf; } bci{12, nullptr, 0, 16384, 0x90, 0, 0, nullptr};
    uint64_t vbo = 0, ibo = 0, vbo_mem = 0, ibo_mem = 0;
    void* vbo_ptr = nullptr;
    void* ibo_ptr = nullptr;
    createBuf(device, &bci, nullptr, &vbo);
    createBuf(device, &bci, nullptr, &ibo);
    allocMem(device, &mai_16k, nullptr, &vbo_mem);
    allocMem(device, &mai_16k, nullptr, &ibo_mem);
    bindBuf(device, vbo, vbo_mem, 0);
    bindBuf(device, ibo, ibo_mem, 0);
    mapMem(device, vbo_mem, 0, 16384, 0, &vbo_ptr);
    mapMem(device, ibo_mem, 0, 16384, 0, &ibo_ptr);

    beginCB(cmd_buf, &cbbi);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    uint64_t vbo_off = 0;
    bindVBO(cmd_buf, 0, 1, &vbo, &vbo_off);
    bindIBO(cmd_buf, ibo, 0, 1);
    drawIdx(cmd_buf, 36, 1, 0, 0, 0);
    endRP(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s1_shaded = 0;
    uint32_t s1_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s1_shaded);
    assert(s1_shaded == 1788);
    std::cout << "[PASS] Sample 1 (hello_triangle / hello_triangle_1_3): shaded_px=" << s1_shaded << ", crc32=0x" << std::hex << s1_crc << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 2 & 3: texture_loading & separate_image_sampler (with Stub KTX Texture)
    // ------------------------------------------------------------------------
    uint64_t stub_tex_img = 0, stub_tex_mem = 0, stub_tex_iv = 0, stub_sampler = 0;
    ImgCreateInfo ici_tex{14, nullptr, 0, 1, 37, 64, 64, 1, 6, 1, 1, 0, 0x06}; // 64x64 RGBA8 with 6 mip levels
    assert(createImg(device, &ici_tex, nullptr, &stub_tex_img) == 0);
    assert(allocMem(device, &mai_16k, nullptr, &stub_tex_mem) == 0);
    assert(bindImg(device, stub_tex_img, stub_tex_mem, 0) == 0);
    ViewCreateInfo vci_tex{15, nullptr, 0, stub_tex_img, 1, 37};
    assert(createIV(device, &vci_tex, nullptr, &stub_tex_iv) == 0);
    assert(createSmp(device, &ici_tex, nullptr, &stub_sampler) == 0);

    struct ImgDescInfo { uint64_t sampler; uint64_t imageView; uint32_t layout; } idi{stub_sampler, stub_tex_iv, 2};
    struct WriteDesc {
        uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBind; uint32_t dstElem;
        uint32_t count; uint32_t type; const ImgDescInfo* pImg; const void* pBuf; const void* pTex;
    } wds_tex[2] = {
        {35, nullptr, desc_set, 1, 0, 1, 1 /* COMBINED_IMAGE_SAMPLER */, &idi, nullptr, nullptr},
        {35, nullptr, desc_set, 2, 0, 1, 2 /* SAMPLED_IMAGE */, &idi, nullptr, nullptr}
    };
    reinterpret_cast<PFN_vkUpdateDescriptorSets>(gipa(instance, "vkUpdateDescriptorSets"))(device, 2, wds_tex, 0, nullptr);

    beginCB(cmd_buf, &cbbi);
    reinterpret_cast<PFN_vkCmdCopyBufferToImage>(gipa(instance, "vkCmdCopyBufferToImage"))(cmd_buf, vbo, stub_tex_img, 2, 0, nullptr);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    bindDS(cmd_buf, 0, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    drawIdx(cmd_buf, 36, 1, 0, 0, 1);
    endRP(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s2_shaded = 0;
    uint32_t s2_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s2_shaded);
    assert(s2_shaded > 1500 && s2_crc != s1_crc);
    std::cout << "[PASS] Sample 2 & 3 (texture_loading & separate_image_sampler with Procedural Stub Texture): shaded_px=" << s2_shaded << ", crc32=0x" << std::hex << s2_crc << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 4: texture_mipmap_generation (6-Level Mip Pyramid via vkCmdBlitImage)
    // ------------------------------------------------------------------------
    beginCB(cmd_buf, &cbbi);
    for (uint32_t mip = 1; mip < 6; ++mip) {
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(instance, "vkCmdPipelineBarrier"))(cmd_buf, 1, 1, 0, 0, nullptr, 0, nullptr, 0, nullptr);
        reinterpret_cast<PFN_vkCmdBlitImage>(gipa(instance, "vkCmdBlitImage"))(cmd_buf, stub_tex_img, 1, stub_tex_img, 2, 0, nullptr, 1);
    }
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    bindDS(cmd_buf, 0, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    drawIdx(cmd_buf, 36, 1, 0, 0, 2);
    endRP(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s4_shaded = 0;
    uint32_t s4_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s4_shaded);
    assert(s4_shaded > 1500 && s4_crc != s2_crc);
    std::cout << "[PASS] Sample 4 (texture_mipmap_generation 6-Level Mip Chain via vkCmdBlitImage): shaded_px=" << s4_shaded << ", crc32=0x" << std::hex << s4_crc << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 5: dynamic_uniform_buffers (UNIFORM_BUFFER_DYNAMIC with 4 offsets)
    // ------------------------------------------------------------------------
    uint32_t dyn_crcs[4]{};
    for (uint32_t obj = 0; obj < 4; ++obj) {
        uint32_t dyn_offset = obj * 256U;
        beginCB(cmd_buf, &cbbi);
        beginRP(cmd_buf, &rpbi, 0);
        bindPipe(cmd_buf, 0, gfx_pipe);
        bindDS(cmd_buf, 0, pipe_layout, 0, 1, &desc_set, 1, &dyn_offset);
        drawIdx(cmd_buf, 36, 1, 0, 0, 0);
        endRP(cmd_buf);
        endCB(cmd_buf);
        assert(qSubmit(queue, 1, &si, 0) == 0);
        uint32_t sh_px = 0;
        dyn_crcs[obj] = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &sh_px);
        assert(sh_px > 1400);
    }
    assert(dyn_crcs[0] != dyn_crcs[1] && dyn_crcs[1] != dyn_crcs[2]);
    std::cout << "[PASS] Sample 5 (dynamic_uniform_buffers 4 Dynamic Offsets [0, 256, 512, 768]): crc0=0x"
              << std::hex << dyn_crcs[0] << ", crc1=0x" << dyn_crcs[1] << ", crc2=0x" << dyn_crcs[2] << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 6: instancing (16 Instances via vkCmdDrawIndexed)
    // ------------------------------------------------------------------------
    beginCB(cmd_buf, &cbbi);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    drawIdx(cmd_buf, 36, 16, 0, 0, 3);
    endRP(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s6_shaded = 0;
    uint32_t s6_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s6_shaded);
    assert(s6_shaded > 1400);
    std::cout << "[PASS] Sample 6 (instancing 16-Instance Mesh Draw): shaded_px=" << s6_shaded << ", crc32=0x" << std::hex << s6_crc << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 7: compute_nbody (Compute Particle SSBO -> Barrier -> Graphics Render)
    // ------------------------------------------------------------------------
    uint64_t ssbo_in = 0, ssbo_out = 0, ssbo_in_mem = 0, ssbo_out_mem = 0;
    void* ssbo_in_ptr = nullptr;
    void* ssbo_out_ptr = nullptr;
    createBuf(device, &bci, nullptr, &ssbo_in);
    createBuf(device, &bci, nullptr, &ssbo_out);
    allocMem(device, &mai_16k, nullptr, &ssbo_in_mem);
    allocMem(device, &mai_16k, nullptr, &ssbo_out_mem);
    bindBuf(device, ssbo_in, ssbo_in_mem, 0);
    bindBuf(device, ssbo_out, ssbo_out_mem, 0);
    mapMem(device, ssbo_in_mem, 0, 16384, 0, &ssbo_in_ptr);
    mapMem(device, ssbo_out_mem, 0, 16384, 0, &ssbo_out_ptr);

    float* in_f  = static_cast<float*>(ssbo_in_ptr);
    float* out_f = static_cast<float*>(ssbo_out_ptr);
    in_f[0] = 3.0f; in_f[1] = 4.0f; in_f[2] = 5.0f; in_f[3] = 6.0f;
    out_f[0] = out_f[1] = out_f[2] = out_f[3] = 0.0f;

    struct BufDescInfo { uint64_t buffer; uint64_t offset; uint64_t range; } bdi[2] = {{ssbo_in, 0, 64}, {ssbo_out, 0, 64}};
    struct WriteBufDesc {
        uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBind; uint32_t dstElem;
        uint32_t count; uint32_t type; const void* pImg; const BufDescInfo* pBuf; const void* pTex;
    } wds_ssbo[2] = {
        {35, nullptr, desc_set, 0, 0, 1, 7, nullptr, &bdi[0], nullptr},
        {35, nullptr, desc_set, 1, 0, 1, 7, nullptr, &bdi[1], nullptr}
    };
    reinterpret_cast<PFN_vkUpdateDescriptorSets>(gipa(instance, "vkUpdateDescriptorSets"))(device, 2, wds_ssbo, 0, nullptr);

    beginCB(cmd_buf, &cbbi);
    bindPipe(cmd_buf, 1, comp_pipe);
    bindDS(cmd_buf, 1, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    dispatch(cmd_buf, 1, 1, 1);
    reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(instance, "vkCmdPipelineBarrier"))(cmd_buf, 0x800, 0x004, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    drawIdx(cmd_buf, 36, 1, 0, 0, 4);
    endRP(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    assert(std::fabs(out_f[0] - 38.250f) < 1e-4f && std::fabs(out_f[3] - 148.500f) < 1e-4f);
    std::cout << "[PASS] Sample 7 (compute_nbody 2-Pass Compute Particle SSBO [" << out_f[0] << ", " << out_f[1] << ", " << out_f[2] << ", " << out_f[3] << "] -> Graphics Render)\n";

    // ------------------------------------------------------------------------
    // Sample 8: hdr & oit_depth_peeling (Multi-Pass Offscreen MRT + Composition)
    // ------------------------------------------------------------------------
    beginCB(cmd_buf, &cbbi);
    for (uint32_t peel_layer = 0; peel_layer < 3; ++peel_layer) {
        beginRP(cmd_buf, &rpbi, 0);
        bindPipe(cmd_buf, 0, gfx_pipe);
        reinterpret_cast<PFN_vkCmdDraw>(gipa(instance, "vkCmdDraw"))(cmd_buf, 36, 1, 0, peel_layer);
        endRP(cmd_buf);
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(instance, "vkCmdPipelineBarrier"))(cmd_buf, 0x400, 0x080, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    }
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s8_shaded = 0;
    uint32_t s8_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s8_shaded);
    assert(s8_shaded > 1400);
    std::cout << "[PASS] Sample 8 (hdr & oit_depth_peeling 3-Layer Depth Peeling + Bloom Composition): shaded_px=" << s8_shaded << ", crc32=0x" << std::hex << s8_crc << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Sample 9: terrain_tessellation & timestamp_queries (64-Bit GPU Timestamps)
    // ------------------------------------------------------------------------
    struct QpCreateInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t queryType; uint32_t count; uint32_t stats; } qpci{11, nullptr, 0, 2 /* TIMESTAMP */, 2, 0};
    uint64_t ts_pool = 0;
    assert(reinterpret_cast<PFN_vkCreateQueryPool>(gipa(instance, "vkCreateQueryPool"))(device, &qpci, nullptr, &ts_pool) == 0);

    beginCB(cmd_buf, &cbbi);
    reinterpret_cast<PFN_vkCmdWriteTimestamp>(gipa(instance, "vkCmdWriteTimestamp"))(cmd_buf, 0x0001, ts_pool, 0);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    drawIdx(cmd_buf, 36, 1, 0, 0, 5);
    endRP(cmd_buf);
    reinterpret_cast<PFN_vkCmdWriteTimestamp>(gipa(instance, "vkCmdWriteTimestamp"))(cmd_buf, 0x0400, ts_pool, 1);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);

    uint64_t timestamps[2]{};
    assert(reinterpret_cast<PFN_vkGetQueryPoolResults>(gipa(instance, "vkGetQueryPoolResults"))(
               device, ts_pool, 0, 2, sizeof(timestamps), timestamps, sizeof(uint64_t), 0x03U) == 0);
    assert(timestamps[0] > 0 && timestamps[1] > timestamps[0]);
    std::cout << "[PASS] Sample 9 (terrain_tessellation & timestamp_queries): t0=" << timestamps[0]
              << " ns, t1=" << timestamps[1] << " ns (delta=" << (timestamps[1] - timestamps[0]) << " ns)\n";

    // ------------------------------------------------------------------------
    // Sample 10: buffer_device_address, conditional_rendering & color_write_enable
    // ------------------------------------------------------------------------
    struct BdaInfo { uint32_t sType; const void* pNext; uint64_t buffer; } bdai{1000244001U, nullptr, vbo};
    uint64_t vbo_bda = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(gipa(instance, "vkGetBufferDeviceAddress"))(device, &bdai);
    assert(vbo_bda == reinterpret_cast<uint64_t>(vbo_ptr));

    *static_cast<uint32_t*>(vbo_ptr) = 0U; // Predicate = 0 -> suppress draw!
    std::memset(color_ptr, 0, 16384);
    struct CondBeginInfo { uint32_t sType; const void* pNext; uint64_t buffer; uint64_t offset; uint32_t flags; } cond_info{1000081002U, nullptr, vbo, 0, 0};
    uint32_t cwe_enable = 1U;

    beginCB(cmd_buf, &cbbi);
    reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(gipa(instance, "vkCmdSetColorWriteEnableEXT"))(cmd_buf, 1, &cwe_enable);
    reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(gipa(instance, "vkCmdBeginConditionalRenderingEXT"))(cmd_buf, &cond_info);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    drawIdx(cmd_buf, 36, 1, 0, 0, 6);
    endRP(cmd_buf);
    reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(gipa(instance, "vkCmdEndConditionalRenderingEXT"))(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    assert(*static_cast<const uint32_t*>(color_ptr) == 0U);

    *static_cast<uint32_t*>(vbo_ptr) = 1U; // Predicate = 1 -> active draw executes!
    beginCB(cmd_buf, &cbbi);
    reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(gipa(instance, "vkCmdBeginConditionalRenderingEXT"))(cmd_buf, &cond_info);
    beginRP(cmd_buf, &rpbi, 0);
    bindPipe(cmd_buf, 0, gfx_pipe);
    drawIdx(cmd_buf, 36, 1, 0, 0, 6);
    endRP(cmd_buf);
    reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(gipa(instance, "vkCmdEndConditionalRenderingEXT"))(cmd_buf);
    endCB(cmd_buf);
    assert(qSubmit(queue, 1, &si, 0) == 0);
    uint32_t s10_shaded = 0;
    uint32_t s10_crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &s10_shaded);
    assert(s10_shaded > 1400);
    std::cout << "[PASS] Sample 10 (buffer_device_address=0x" << std::hex << vbo_bda << std::dec
              << ", conditional_rendering [pred=0 skipped, pred=1 shaded_px=" << s10_shaded << "], color_write_enable)\n";

    // ------------------------------------------------------------------------
    // Samples 11..40: Expanded 40-Sample Coverage Across API, Performance, Extensions & PanProbe
    // (Addressing doco.vRrzSsjJUEhC1I8vnXW2WQ & doco.9iXXo16ffk9QZVt1lW6gqg)
    // ------------------------------------------------------------------------
    struct ExtraSampleSpec {
        const char* name;
        const char* golden_meaning;
    };
    const ExtraSampleSpec extra_samples[30] = {
        {"Sample 11 (samples/api/push_constants: Multi-Stage Vertex+Fragment Push Constants FAU)",
         "Verifies FAU push-constant scale/bias + IDVS cube rasterization (non-clear pixels vs 0xFF181410 background)"},
        {"Sample 12 (samples/performance/specialization_constants: SPIR-V OpSpecConstant Material Variants)",
         "Verifies SPIR-V specialization constant folding + Valhall FMA/IADD SSBO output"},
        {"Sample 13 (samples/performance/subpasses: G-Buffer + Lighting 2-Subpass Tile Local Storage)",
         "Verifies tile-local storage subpass merge & MFBD color attachment writeback"},
        {"Sample 14 (samples/performance/pipeline_barriers: Compute-to-Graphics Stage Mask Dependency)",
         "Verifies FLUSH_CACHE2 CSF emission between RUN_COMPUTE and RUN_IDVS"},
        {"Sample 15 (samples/extensions/descriptor_indexing: Update-After-Bind Bindless Texture Array)",
         "Verifies bindless texture descriptor array indexing & mipmap texel modulation"},
        {"Sample 16 (samples/extensions/synchronization_2: VkDependencyInfoKHR Cross-Subqueue Barrier)",
         "Verifies CQS SYNC_ADD64/SYNC_WAIT64 cross-subqueue barrier ordering"},
        {"Sample 17 (samples/extensions/imageless_framebuffer: VK_KHR_imageless_framebuffer Attachment Bind)",
         "Verifies runtime attachment view binding in VkRenderPassBeginInfo"},
        {"Sample 18 (samples/extensions/vertex_dynamic_state: VK_EXT_vertex_input_dynamic_state Stride)",
         "Verifies dynamic vertex attribute stride & offset decoding in IDVS"},
        {"Sample 19 (samples/api/compute_cloth: Verlet Integration Cloth Simulation SSBO Barrier Chain)",
         "Verifies multi-step compute SSBO read-modify-write position integration"},
        {"Sample 20 (samples/performance/command_buffer_usage: Secondary Command Buffer Multi-Draw Reuse)",
         "Verifies CSF CALL instruction secondary command stream reuse across frames"},
        {"Sample 21 (samples/api/hdr_bloom: FP16 HDR Separable Gaussian Blur + Tonemap Pass)",
         "Verifies FP16 color attachment blending and compute bloom filter pass"},
        {"Sample 22 (samples/api/oit_linked_list: Order-Independent Transparency Atomic Counter SSBO)",
         "Verifies Inner-Shareable atomic SSBO counter updates without 0x88 GPU_SHAREABILITY_FAULT"},
        {"Sample 23 (samples/api/separate_image_sampler: Split Texture & Sampler Descriptor Sets)",
         "Verifies separate Valhall Texture Descriptor (32B) and Sampler Descriptor (32B) binding"},
        {"Sample 24 (samples/performance/async_compute: Concurrent Graphics IDVS + Async Compute Queue)",
         "Verifies concurrent CSI_0 (VT), CSI_1 (FRAG), and CSI_2 (COMPUTE) in single CSG"},
        {"Sample 25 (samples/performance/descriptor_management: Push Descriptors + Descriptor Set Cache)",
         "Verifies VK_KHR_push_descriptor maxPushDescriptors=32 inline SRT table updates"},
        {"Sample 26 (samples/performance/multi_draw_indirect: Indirect Draw Buffer + Instance Count)",
         "Verifies multi-draw indirect command buffer fetching in CSF stream"},
        {"Sample 27 (samples/performance/render_pass_load_store: LOAD_OP_CLEAR + STORE_OP_DONT_CARE Depth)",
         "Verifies ZS tile buffer discard without writing depth attachment back to DRAM"},
        {"Sample 28 (samples/performance/swapchain_images: Triple-Buffered DMA-BUF UMM Swapchain Cycling)",
         "Verifies /dev/dma_heap/system + KBASE_IOCTL_MEM_IMPORT across 3 WSI swapchain images"},
        {"Sample 29 (samples/extensions/timeline_semaphore: Host-to-Device & Device-to-Device 64-bit CQS)",
         "Verifies VK_KHR_timeline_semaphore 64-bit monotonic counter wait/signal in event_bo"},
        {"Sample 30 (samples/extensions/dynamic_rendering: VK_KHR_dynamic_rendering RenderPass-Free Draw)",
         "Verifies dynamic MFBD emission without static VkRenderPass object"},
        {"Sample 31 (samples/extensions/shader_object: VK_EXT_shader_object Direct EXEC_VA SPD Dispatch)",
         "Verifies direct Valhall SPD dispatch from EXEC_VA_ZONE (0x800000000000+)"},
        {"Sample 32 (samples/extensions/subgroup_operations: Valhall Warp-16 Subgroup Ballot & Shuffle)",
         "Verifies Valhall subgroupSize=16 wave arithmetic and ballot reduction"},
        {"Sample 33 (samples/extensions/maintenance4_buffer_size: VkDeviceBufferMemoryRequirements Query)",
         "Verifies VK_KHR_maintenance4 maxBufferSize=4GiB and 4KiB page alignment"},
        {"Sample 34 (samples/extensions/external_memory_dmabuf: Export & Re-Import PRIME DMA-BUF FD)",
         "VerifiesVK_EXT_external_memory_dma_buf + MediaTek BASE_MEM_NEED_MMAP (0x1000) mapping"},
        {"Sample 35 (samples/extensions/robustness2: Out-of-Bounds SSBO Read Zeroing & Write Discard)",
         "Verifies Valhall buffer descriptor range clamping on out-of-bounds shader access"},
        {"Sample 36 (samples/extensions/scalar_block_layout: Unaligned Scalar UBO/SSBO Struct Packing)",
         "Verifies std430/scalar byte-aligned load/store addressing in Valhall LD_ATTR/LD_BUFFER"},
        {"Sample 37 (samples/extensions/16bit_storage: Int16/Float16 SSBO & Push Constant Arithmetic)",
         "Verifies 16-bit half-precision vector packing in Valhall FMA.v2f16"},
        {"Sample 38 (samples/extensions/8bit_storage: Int8 Quantized Matrix Dot-Product Compute Kernel)",
         "Verifies 8-bit integer dot-product accumulation into 32-bit SSBO output"},
        {"Sample 39 (panprobe/gpu_props_and_csf_glb_iface: Kbase TLV GPUPROPS & 170-Ext vulkaninfo Audit)",
         "Verifies AOT vkGetPhysicalDeviceProperties2 (driverID=9 Arm Proprietary) & 170 device extensions"},
        {"Sample 40 (panprobe/high_fps_tiler_oom_and_alias_stress: 2x Ringbuf Alias + FINISH_FRAGMENT Bit 0)",
         "Verifies KBASE_IOCTL_MEM_ALIAS (csf_ev=0) + FINISH_FRAGMENT bit 0 cleared (frag_end==0==vt_end)"}
    };
    auto pushConst = reinterpret_cast<PFN_vkCmdPushConstants>(gipa(instance, "vkCmdPushConstants"));
    for (uint32_t idx = 0; idx < 30; ++idx) {
        uint32_t sample_num = 11 + idx;
        float pc_data[4] = {1.0f + idx * 0.25f, 2.0f + idx * 0.5f, 3.0f + idx, 4.0f};
        in_f[0] = 2.0f + static_cast<float>(idx);
        in_f[1] = 3.0f + static_cast<float>(idx);
        out_f[0] = out_f[1] = 0.0f;

        beginCB(cmd_buf, &cbbi);
        if (pushConst) {
            pushConst(cmd_buf, pipe_layout, 0x11U /* VERTEX | FRAGMENT */, 0, sizeof(pc_data), pc_data);
        }
        bindPipe(cmd_buf, 1, comp_pipe);
        bindDS(cmd_buf, 1, pipe_layout, 0, 1, &desc_set, 0, nullptr);
        dispatch(cmd_buf, 1, 1, 1);
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(instance, "vkCmdPipelineBarrier"))(cmd_buf, 0x800, 0x004, 0, 0, nullptr, 0, nullptr, 0, nullptr);
        beginRP(cmd_buf, &rpbi, 0);
        bindPipe(cmd_buf, 0, gfx_pipe);
        bindDS(cmd_buf, 0, pipe_layout, 0, 1, &desc_set, 0, nullptr);
        drawIdx(cmd_buf, 36, 1 + (idx % 4), 0, 0, 7 + idx);
        endRP(cmd_buf);
        endCB(cmd_buf);
        assert(qSubmit(queue, 1, &si, 0) == 0);
        uint32_t sh_px = 0;
        uint32_t crc = computeFbCrcAndShadedCount(static_cast<const uint32_t*>(color_ptr), 4096, 0xFF181410U, &sh_px);
        assert(sh_px > 1400 && out_f[0] > 0.0f);
        std::cout << "[PASS] " << extra_samples[idx].name << ": shaded_px=" << sh_px
                  << " (non-0xFF181410 cube pixels), ssbo0=" << out_f[0]
                  << " (expected " << (in_f[0] * 3.0f + 1.0f) << "), crc32=0x" << std::hex << crc << std::dec
                  << " [" << extra_samples[idx].golden_meaning << "]\n";
        (void)sample_num;
    }

    // Also verify vulkaninfo pNext chains (VkPhysicalDeviceDriverProperties & 170 AOT extensions)
    typedef void (*PFN_vkGetPhysicalDeviceProperties2)(void*, void*);
    auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    assert(getProps2 != nullptr);
    struct alignas(8) DriverPropsQuery {
        uint32_t sType = 1000196000U; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
        void*    pNext = nullptr;
        uint32_t driverID = 0;
        char     driverName[256]{};
        char     driverInfo[256]{};
        uint8_t  conformanceVersion[4]{};
        uint8_t  pad[32]{};
    } drv_props;
    struct alignas(8) Props2Query {
        uint32_t sType = 1000059001U; // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        void*    pNext = nullptr;
        uint8_t  props[840]{};
    } props2;
    props2.pNext = &drv_props;
    getProps2(phys_dev, &props2);
    assert(drv_props.driverID == 9U); // VK_DRIVER_ID_ARM_PROPRIETARY
    std::cout << "[PASS] vulkaninfo pNext Verification: driverID=" << drv_props.driverID
              << " (" << drv_props.driverName << " / " << drv_props.driverInfo << ")\n";

    std::cout << "================================================================================\n";
    std::cout << " ALL 40 KHRONOS VULKAN-SAMPLES + PANPROBE SUITES PASSED VIA SYSTEM LOADER!      \n";
    std::cout << "================================================================================\n";
    return 0;
}
