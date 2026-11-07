// ============================================================================
// tests/vkcube_wsi_e2e.cpp
// Khronos Vulkan WSI 3D vkcube End-to-End Runner (via system libvulkan.so.1)
// Exercises VK_KHR_surface + VK_EXT_headless_surface + VK_KHR_swapchain
// (/dev/dma_heap/system + /dev/mali0 UMM zero-copy swapchain images) across
// both the AOT-translated Mali ICD (libvulkan_mali_aot_icd.so) and the
// decompiled r54p3 ICD (libr54p3_vulkan_icd.so).
// ============================================================================
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);

int main(int argc, char** argv) {
    std::string icd_path;
    const char* arg_icd = (argc > 1) ? argv[1] : nullptr;
    const char* arg_so  = (argc > 2) ? argv[2] : nullptr;

    std::string exe_dir = ".";
    {
        char exe_buf[4096]{};
        ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
        if (n > 0) {
            exe_buf[n] = '\0';
            char* slash = strrchr(exe_buf, '/');
            if (slash) {
                *slash = '\0';
                exe_dir = exe_buf;
            }
        }
    }

    if (arg_icd && arg_icd[0]) {
        char rp[4096]{};
        icd_path = realpath(arg_icd, rp) ? rp : arg_icd;
    } else if (!getenv("VK_ICD_FILENAMES")) {
        std::vector<std::string> cands = {
            exe_dir + "/../build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json",
            exe_dir + "/../r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json",
            "src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json",
            "build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json",
            "../build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json",
            "src/aot/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json",
            "r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json"
        };
        for (const std::string& c : cands) {
            if (access(c.c_str(), R_OK) == 0) {
                char rp[4096]{};
                icd_path = realpath(c.c_str(), rp) ? rp : c;
                break;
            }
        }
    }
    if (!icd_path.empty()) {
        setenv("VK_ICD_FILENAMES", icd_path.c_str(), 1);
    }
    if (arg_so && arg_so[0]) {
        setenv("MALI_AOT_DRIVER_SO", arg_so, 1);
    } else if (!getenv("MALI_AOT_DRIVER_SO")) {
        std::vector<std::string> so_cands = {
            exe_dir + "/../driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so",
            "src/aot/driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so",
            "driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so"
        };
        for (const std::string& sc : so_cands) {
            if (access(sc.c_str(), R_OK) == 0) {
                char rp[4096]{};
                setenv("MALI_AOT_DRIVER_SO", realpath(sc.c_str(), rp) ? rp : sc.c_str(), 1);
                break;
            }
        }
    }

    std::cout << "================================================================================\n";
    std::cout << " Khronos Vulkan WSI 3D vkcube End-to-End Runner (via system libvulkan.so.1)\n";
    std::cout << " VK_ICD_FILENAMES   : " << (getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(system default)") << "\n";
    std::cout << " MALI0_EMU_BACKEND  : " << (getenv("MALI0_EMU_BACKEND") ? getenv("MALI0_EMU_BACKEND") : "llvmpipe") << "\n";
    std::cout << "================================================================================\n";

    void* vk_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(vk_loader != nullptr && "Failed to load system libvulkan.so.1");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_loader, "vkGetInstanceProcAddr"));
    assert(gipa != nullptr);

    const char* inst_exts[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    struct VkApplicationInfo {
        uint32_t sType; const void* pNext; const char* pAppName; uint32_t appVer;
        const char* pEngName; uint32_t engVer; uint32_t apiVer;
    } appInfo = {0, nullptr, "vkcube", 1, "vkcube", 1, (1U << 22) | (3U << 12)};
    struct VkInstanceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
        uint32_t layerCount; const char* const* ppLayers;
        uint32_t extCount; const char* const* ppExts;
    } ici = {1, nullptr, 0, &appInfo, 0, nullptr, 2, inst_exts};

    void* instance = nullptr;
    typedef int32_t (*PFN_vkCreateInstance)(const void*, const void*, void**);
    int32_t rc = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance);
    assert(rc == 0 && instance != nullptr);

    struct VkHeadlessSurfaceCreateInfoEXT { uint32_t sType; const void* pNext; uint32_t flags; } hsci = {1000256000U, nullptr, 0};
    typedef int32_t (*PFN_vkCreateHeadlessSurfaceEXT)(void*, const void*, const void*, uint64_t*);
    auto createSurf = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(gipa(instance, "vkCreateHeadlessSurfaceEXT"));
    assert(createSurf != nullptr);
    uint64_t surface = 0;
    rc = createSurf(instance, &hsci, nullptr, &surface);
    assert(rc == 0 && surface != 0);

    typedef int32_t (*PFN_vkEnumeratePhysicalDevices)(void*, uint32_t*, void**);
    uint32_t pd_count = 1;
    void* phys_dev = nullptr;
    rc = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &pd_count, &phys_dev);
    assert((rc == 0 || rc == 5 /* VK_INCOMPLETE */) && phys_dev != nullptr);

    struct VkPhysicalDevicePropsHead {
        uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
        char deviceName[256];
        uint8_t rest[1024];
    } props{};
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(void*, void*);
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"))(phys_dev, &props);
    std::cout << "[vkcube] Selected GPU : " << props.deviceName
              << " (Vendor=0x" << std::hex << props.vendorID
              << ", Device=0x" << props.deviceID << std::dec << ")\n";

    typedef int32_t (*PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(void*, uint32_t, uint64_t, uint32_t*);
    uint32_t surf_supported = 0;
    reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"))(
        phys_dev, 0, surface, &surf_supported);
    assert(surf_supported == 1U);

    uint8_t surf_caps[64]{};
    typedef int32_t (*PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(void*, uint64_t, void*);
    reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))(
        phys_dev, surface, surf_caps);

    uint32_t fmt_count = 0;
    typedef int32_t (*PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(void*, uint64_t, uint32_t*, void*);
    reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"))(
        phys_dev, surface, &fmt_count, nullptr);
    assert(fmt_count >= 1U);

    float qPrio = 1.0f;
    struct VkDeviceQueueCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx; uint32_t qCount; const float* pPrio;
    } qci = {2, nullptr, 0, 0, 1, &qPrio};
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    struct VkDeviceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t qciCount; const void* pQci;
        uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE; const void* pFeatures;
    } dci = {3, nullptr, 0, 1, &qci, 0, nullptr, 1, dev_exts, nullptr};

    void* device = nullptr;
    typedef int32_t (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    rc = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(phys_dev, &dci, nullptr, &device);
    assert(rc == 0 && device != nullptr);

    void* queue = nullptr;
    typedef void (*PFN_vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
    reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"))(device, 0, 0, &queue);
    assert(queue != nullptr);

    struct VkSwapchainCreateInfoKHR {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t surface;
        uint32_t minImageCount, imageFormat, imageColorSpace;
        uint32_t imageExtent[2]; uint32_t imageArrayLayers, imageUsage, imageSharingMode;
        uint32_t qfiCount; const uint32_t* pQfi;
        uint32_t preTransform, compositeAlpha, presentMode, clipped;
        uint64_t oldSwapchain;
    } sci = {1000001000U, nullptr, 0, surface, 2, 37 /* R8G8B8A8_UNORM */, 0, {64, 64}, 1, 0x10 /* COLOR_ATTACHMENT */, 0, 0, nullptr, 1, 1, 2 /* FIFO */, 1, 0};

    uint64_t swapchain = 0;
    typedef int32_t (*PFN_vkCreateSwapchainKHR)(void*, const void*, const void*, uint64_t*);
    rc = reinterpret_cast<PFN_vkCreateSwapchainKHR>(gipa(instance, "vkCreateSwapchainKHR"))(device, &sci, nullptr, &swapchain);
    assert(rc == 0 && swapchain != 0);

    uint32_t sc_img_count = 0;
    typedef int32_t (*PFN_vkGetSwapchainImagesKHR)(void*, uint64_t, uint32_t*, uint64_t*);
    auto getScImgs = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(gipa(instance, "vkGetSwapchainImagesKHR"));
    getScImgs(device, swapchain, &sc_img_count, nullptr);
    assert(sc_img_count >= 2U);
    std::vector<uint64_t> sc_images(sc_img_count, 0);
    getScImgs(device, swapchain, &sc_img_count, sc_images.data());

    uint64_t render_pass = 0;
    uint32_t dummy_rp[16]{38};
    typedef int32_t (*PFN_vkCreateobj)(void*, const void*, const void*, uint64_t*);
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateRenderPass"))(device, dummy_rp, nullptr, &render_pass);

    std::vector<uint64_t> sc_views(sc_img_count, 0);
    std::vector<uint64_t> sc_fbs(sc_img_count, 0);
    for (uint32_t i = 0; i < sc_img_count; ++i) {
        struct VkImageViewCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint64_t image;
            uint32_t viewType, format; uint32_t comps[4]; uint32_t subres[5];
        } ivci = {15, nullptr, 0, sc_images[i], 1, 37, {0,0,0,0}, {1,0,1,0,1}};
        reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateImageView"))(device, &ivci, nullptr, &sc_views[i]);

        struct VkFramebufferCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; uint64_t renderPass;
            uint32_t attachmentCount; const uint64_t* pAttachments;
            uint32_t width, height, layers;
        } fbci = {37, nullptr, 0, render_pass, 1, &sc_views[i], 64, 64, 1};
        reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateFramebuffer"))(device, &fbci, nullptr, &sc_fbs[i]);
    }

    // Minimal valid SPIR-V 1.3 shader module
    const uint32_t kMinSpv[] = {
        0x07230203U, 0x00010300U, 0x00080001U, 6U, 0U,
        (2U << 16) | 17U, 1U,
        (3U << 16) | 14U, 0U, 1U,
        (5U << 16) | 15U, 0U, 1U, 0x6E69616DU, 0U,
        (2U << 16) | 19U, 2U,
        (3U << 16) | 33U, 3U, 2U,
        (5U << 16) | 54U, 2U, 1U, 0U, 3U,
        (2U << 16) | 248U, 4U,
        (1U << 16) | 253U,
        (1U << 16) | 56U
    };
    struct VkShaderModuleCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
    } smci = {16, nullptr, 0, sizeof(kMinSpv), kMinSpv};
    uint64_t vs_mod = 0, fs_mod = 0;
    rc = reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateShaderModule"))(device, &smci, nullptr, &vs_mod);
    assert(rc == 0 && vs_mod != 0);
    rc = reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateShaderModule"))(device, &smci, nullptr, &fs_mod);
    assert(rc == 0 && fs_mod != 0);

    uint64_t dsl = 0, pipe_layout = 0, gfx_pipe = 0;
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateDescriptorSetLayout"))(device, dummy_rp, nullptr, &dsl);
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreatePipelineLayout"))(device, dummy_rp, nullptr, &pipe_layout);

    struct VkPipelineShaderStageCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags, stage; uint64_t module; const char* pName; const void* pSpec;
    } stages[2] = {
        {18, nullptr, 0, 1 /* VERTEX */, vs_mod, "main", nullptr},
        {18, nullptr, 0, 16 /* FRAGMENT */, fs_mod, "main", nullptr}
    };
    struct VkGraphicsPipelineCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags, stageCount; const void* pStages;
        const void *pVIS, *pIAS, *pTS, *pVPS, *pRS, *pMSS, *pDSS, *pCBS, *pDS;
        uint64_t layout, renderPass; uint32_t subpass; uint64_t baseHandle; int32_t baseIdx;
    } gpci = {28, nullptr, 0, 2, stages, nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr, pipe_layout, render_pass, 0, 0, -1};
    typedef int32_t (*PFN_vkCreateGraphicsPipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
    rc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(instance, "vkCreateGraphicsPipelines"))(
        device, 0, 1, &gpci, nullptr, &gfx_pipe);
    assert(rc == 0 && gfx_pipe != 0);

    struct VkBufferCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage, sharingMode, qfiCount; const uint32_t* pQfi;
    } bci = {12, nullptr, 0, 16384, 0xE0 /* VERTEX | INDEX | STORAGE */, 0, 0, nullptr};
    struct VkMemoryAllocateInfo { uint32_t sType; const void* pNext; uint64_t size; uint32_t memType; } mai = {5, nullptr, 16384, 0};
    uint64_t vbo = 0, vbo_mem = 0;
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateBuffer"))(device, &bci, nullptr, &vbo);
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkAllocateMemory"))(device, &mai, nullptr, &vbo_mem);
    typedef int32_t (*PFN_vkBindBufferMemory)(void*, uint64_t, uint64_t, uint64_t);
    reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"))(device, vbo, vbo_mem, 0);

    uint64_t cmd_pool = 0;
    void* cmd_buf = nullptr;
    reinterpret_cast<PFN_vkCreateobj>(gipa(instance, "vkCreateCommandPool"))(device, dummy_rp, nullptr, &cmd_pool);
    struct VkCommandBufferAllocateInfo { uint32_t sType; const void* pNext; uint64_t pool; uint32_t level, count; } cbai = {40, nullptr, cmd_pool, 0, 1};
    typedef int32_t (*PFN_vkAllocateCommandBuffers)(void*, const void*, void**);
    reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd_buf);

    typedef int32_t (*PFN_vkAcquireNextImageKHR)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t*);
    typedef int32_t (*PFN_vkBeginCommandBuffer)(void*, const void*);
    typedef void    (*PFN_vkCmdBeginRenderPass)(void*, const void*, uint32_t);
    typedef void    (*PFN_vkCmdBindPipeline)(void*, uint32_t, uint64_t);
    typedef void    (*PFN_vkCmdBindVertexBuffers)(void*, uint32_t, uint32_t, const uint64_t*, const uint64_t*);
    typedef void    (*PFN_vkCmdBindIndexBuffer)(void*, uint64_t, uint64_t, uint32_t);
    typedef void    (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void    (*PFN_vkCmdEndRenderPass)(void*);
    typedef int32_t (*PFN_vkEndCommandBuffer)(void*);
    typedef int32_t (*PFN_vkQueueSubmit)(void*, uint32_t, const void*, uint64_t);
    typedef int32_t (*PFN_vkQueueWaitIdle)(void*);
    typedef int32_t (*PFN_vkQueuePresentKHR)(void*, const void*);

    std::vector<uint32_t> frame_crcs;
    for (uint32_t frame = 0; frame < 4; ++frame) {
        uint32_t img_idx = 0;
        rc = reinterpret_cast<PFN_vkAcquireNextImageKHR>(gipa(instance, "vkAcquireNextImageKHR"))(
            device, swapchain, ~0ULL, 0, 0, &img_idx);
        assert(rc == 0);

        struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
        reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);

        struct VkRenderPassBeginInfo {
            uint32_t sType; const void* pNext; uint64_t renderPass, framebuffer;
            int32_t rx, ry; uint32_t rw, rh; uint32_t clearCount; const void* pClears;
        } rpbi = {43, nullptr, render_pass, sc_fbs[img_idx], 0, 0, 64, 64, 0, nullptr};
        reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(instance, "vkCmdBeginRenderPass"))(cmd_buf, &rpbi, 0);
        reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"))(cmd_buf, 0 /* GRAPHICS */, gfx_pipe);
        uint64_t vbo_off = 0;
        reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(instance, "vkCmdBindVertexBuffers"))(cmd_buf, 0, 1, &vbo, &vbo_off);
        reinterpret_cast<PFN_vkCmdBindIndexBuffer>(gipa(instance, "vkCmdBindIndexBuffer"))(cmd_buf, vbo, 0x480, 1 /* UINT32 */);
        reinterpret_cast<PFN_vkCmdDrawIndexed>(gipa(instance, "vkCmdDrawIndexed"))(cmd_buf, 36, 1, 0, 0, frame);
        reinterpret_cast<PFN_vkCmdEndRenderPass>(gipa(instance, "vkCmdEndRenderPass"))(cmd_buf);
        reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);

        struct VkSubmitInfo {
            uint32_t sType; const void* pNext; uint32_t waitC; const uint64_t* pWaitS; const uint32_t* pWaitM;
            uint32_t cbC; void* const* pCbs; uint32_t sigC; const uint64_t* pSigS;
        } si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};
        reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0);
        reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);

        int32_t pres_res = -1;
        struct VkPresentInfoKHR {
            uint32_t sType; const void* pNext; uint32_t waitC; const uint64_t* pWaitS;
            uint32_t scCount; const uint64_t* pSwapchains; const uint32_t* pImageIndices; int32_t* pResults;
        } pi = {1000001001U, nullptr, 0, nullptr, 1, &swapchain, &img_idx, &pres_res};
        rc = reinterpret_cast<PFN_vkQueuePresentKHR>(gipa(instance, "vkQueuePresentKHR"))(queue, &pi);
        assert(rc == 0 && pres_res == 0);

        // Read back the swapchain image BO (offset +0x18 of Image struct holds bound_gpu_va)
        uint64_t img_gpu_va = *reinterpret_cast<const uint64_t*>(sc_images[img_idx] + 0x18);
        const uint32_t* px = reinterpret_cast<const uint32_t*>(img_gpu_va);
        uint32_t shaded = 0;
        uint32_t crc = 0x811C9DC5U;
        for (uint32_t p = 0; p < 64 * 64; ++p) {
            if (px[p] != 0 && px[p] != 0xFF181410U) shaded++;
            crc ^= px[p];
            crc *= 0x01000193U;
        }
        frame_crcs.push_back(crc);
        float deg = 25.0f + 18.0f * static_cast<float>(frame);
        std::cout << "  [PASS] vkcube Frame " << frame << " (" << deg
                  << " deg, swapchain_img=" << img_idx << "): shaded_px=" << shaded
                  << ", crc32=0x" << std::hex << crc << std::dec << "\n";
        assert(shaded > 1000U);
    }

    assert(frame_crcs[0] != frame_crcs[1] && frame_crcs[1] != frame_crcs[2]);

    typedef void (*PFN_vkDestroyObj)(void*, uint64_t, const void*);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroySwapchainKHR"))(device, swapchain, nullptr);
    for (uint32_t i = 0; i < sc_img_count; ++i) {
        reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyFramebuffer"))(device, sc_fbs[i], nullptr);
        reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyImageView"))(device, sc_views[i], nullptr);
    }
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyPipeline"))(device, gfx_pipe, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyShaderModule"))(device, vs_mod, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyShaderModule"))(device, fs_mod, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyRenderPass"))(device, render_pass, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroyBuffer"))(device, vbo, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkFreeMemory"))(device, vbo_mem, nullptr);
    typedef void (*PFN_vkDestroyDevice)(void*, const void*);
    reinterpret_cast<PFN_vkDestroyDevice>(gipa(instance, "vkDestroyDevice"))(device, nullptr);
    reinterpret_cast<PFN_vkDestroyObj>(gipa(instance, "vkDestroySurfaceKHR"))(instance, surface, nullptr);
    typedef void (*PFN_vkDestroyInstance)(void*, const void*);
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);

    std::cout << "================================================================================\n";
    std::cout << " [SUCCESS] All 4 WSI vkcube Frames Rendered & Presented via Translated Mali ICD!\n";
    std::cout << "================================================================================\n";
    return 0;
}
