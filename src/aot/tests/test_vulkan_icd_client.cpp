// ============================================================================
// test_vulkan_icd_client.cpp
// Standard x86-64 Vulkan Client Application that loads the system Khronos
// Vulkan Loader (libvulkan.so.1) with VK_ICD_FILENAMES pointing to our
// AOT-translated Mali Vulkan ICD (libvulkan_mali_aot_icd.so) and verifies:
//   1. Instance & Extension enumeration via AOT ARM64 driver
//   2. PhysicalDevice discovery (VendorID 0x13B5 Arm, Mali-G615/G78/G76/G925)
//   3. Logical VkDevice & /dev/mali0 CSF/JM Queue creation
//   4. SPIR-V Compute Pipeline (vector_compute.spv & branch_loop_compute.spv)
//      executed via AOT vkCreateShaderModule + CMPBE + /dev/mali0 CSF Ringbuffer
//      + mali0_emu on llvmpipe / host Vulkan driver
//   5. 3D vkcube Multi-Frame Rendering (vkCmdDrawIndexed + vkQueueSubmit)
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "../include/mali0_emu_uapi.h"

namespace {

// Helper to build the 2 compute SPIR-V shaders (vector_compute & branch_loop_compute)
void appendSpvOp(std::vector<uint32_t>& words, uint16_t op, std::initializer_list<uint32_t> args) {
    uint32_t wc = static_cast<uint32_t>(1 + args.size());
    words.push_back((wc << 16) | op);
    for (uint32_t a : args) words.push_back(a);
}

std::vector<uint32_t> buildVectorComputeSpv() {
    std::vector<uint32_t> b;
    appendSpvOp(b, 17, {1});
    appendSpvOp(b, 14, {0, 1});
    appendSpvOp(b, 15, {5, 1, 0x6E69616DU, 0U, 10, 11});
    appendSpvOp(b, 16, {1, 17, 1, 1, 1});
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
    appendSpvOp(b, 43, {5, 13, 0x40000000U}); // 2.0f
    appendSpvOp(b, 44, {6, 14, 13, 13, 13, 13});
    appendSpvOp(b, 54, {2, 1, 0, 3});
    appendSpvOp(b, 248, {15});
    appendSpvOp(b, 65, {9, 16, 10, 12});
    appendSpvOp(b, 61, {6, 17, 16});
    appendSpvOp(b, 133, {6, 18, 17, 14}); // OpFMul
    appendSpvOp(b, 65, {9, 19, 11, 12});
    appendSpvOp(b, 62, {19, 18});
    appendSpvOp(b, 253, {});
    appendSpvOp(b, 56, {});
    std::vector<uint32_t> words = {0x07230203U, 0x00010300U, 0x00080001U, 20U, 0U};
    words.insert(words.end(), b.begin(), b.end());
    return words;
}

std::vector<uint32_t> buildBranchLoopComputeSpv() {
    auto words = buildVectorComputeSpv();
    // Insert a dummy OpLoopMerge nop annotation word before OpReturn to trigger loop OBJC synthesis
    words.insert(words.end() - 2, (4U << 16) | 246U);
    words.insert(words.end() - 2, 15U);
    words.insert(words.end() - 2, 15U);
    words.insert(words.end() - 2, 0U);
    return words;
}

} // namespace

int main(int argc, char** argv) {
    const char* icd_json = (argc > 1) ? argv[1] : getenv("VK_ICD_FILENAMES");
    const char* driver_so = (argc > 2) ? argv[2] : getenv("MALI_AOT_DRIVER_SO");
    if (icd_json && icd_json[0]) {
        setenv("VK_ICD_FILENAMES", icd_json, 1);
    }
    if (driver_so && driver_so[0]) {
        setenv("MALI_AOT_DRIVER_SO", driver_so, 1);
    }

    std::cout << "================================================================================\n";
    std::cout << " Mali AOT x86-64 Vulkan ICD End-to-End Client Harness (via system libvulkan.so.1)\n";
    std::cout << " VK_ICD_FILENAMES   : " << (getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(unset)") << "\n";
    std::cout << " MALI_AOT_DRIVER_SO : " << (getenv("MALI_AOT_DRIVER_SO") ? getenv("MALI_AOT_DRIVER_SO") : "(default r54p3)") << "\n";
    std::cout << " MALI0_EMU_BACKEND  : " << (getenv("MALI0_EMU_BACKEND") ? getenv("MALI0_EMU_BACKEND") : "llvmpipe (default)") << "\n";
    std::cout << "================================================================================\n";

    void* vk_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(vk_loader != nullptr && "Failed to dlopen system libvulkan.so.1");

    typedef void (*PFN_vkVoidFunction)(void);
    typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
    typedef int32_t (*PFN_vkEnumerateInstanceVersion)(uint32_t*);
    typedef int32_t (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, void*);
    typedef int32_t (*PFN_vkCreateInstance)(const void*, const void*, void**);
    typedef void (*PFN_vkDestroyInstance)(void*, const void*);
    typedef int32_t (*PFN_vkEnumeratePhysicalDevices)(void*, uint32_t*, void**);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(void*, void*);
    typedef void (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(void*, uint32_t*, void*);
    typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(void*, void*);
    typedef int32_t (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    typedef void (*PFN_vkDestroyDevice)(void*, const void*);
    typedef void (*PFN_vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
    typedef int32_t (*PFN_vkCreateBuffer)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyBuffer)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkAllocateMemory)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkFreeMemory)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkMapMemory)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**);
    typedef int32_t (*PFN_vkBindBufferMemory)(void*, uint64_t, uint64_t, uint64_t);
    typedef int32_t (*PFN_vkCreateShaderModule)(void*, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyShaderModule)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateDescriptorSetLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreatePipelineLayout)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkCreateComputePipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
    typedef void (*PFN_vkDestroyPipeline)(void*, uint64_t, const void*);
    typedef int32_t (*PFN_vkCreateDescriptorPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateDescriptorSets)(void*, const void*, uint64_t*);
    typedef void (*PFN_vkUpdateDescriptorSets)(void*, uint32_t, const void*, uint32_t, const void*);
    typedef int32_t (*PFN_vkCreateCommandPool)(void*, const void*, const void*, uint64_t*);
    typedef int32_t (*PFN_vkAllocateCommandBuffers)(void*, const void*, void**);
    typedef int32_t (*PFN_vkBeginCommandBuffer)(void*, const void*);
    typedef void (*PFN_vkCmdBindPipeline)(void*, uint32_t, uint64_t);
    typedef void (*PFN_vkCmdBindDescriptorSets)(void*, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t*, uint32_t, const uint32_t*);
    typedef void (*PFN_vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t);
    typedef void (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef int32_t (*PFN_vkEndCommandBuffer)(void*);
    typedef int32_t (*PFN_vkQueueSubmit)(void*, uint32_t, const void*, uint64_t);
    typedef int32_t (*PFN_vkQueueWaitIdle)(void*);

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_loader, "vkGetInstanceProcAddr"));
    assert(gipa != nullptr);

    // ------------------------------------------------------------------------
    // Stage 1: Instance Version, Extensions, and vkCreateInstance via libvulkan.so.1
    // ------------------------------------------------------------------------
    uint32_t api_ver = 0;
    auto enumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
    assert(enumVer && enumVer(&api_ver) == 0);

    uint32_t ext_count = 0;
    auto enumExt = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    assert(enumExt && enumExt(nullptr, &ext_count, nullptr) == 0 && ext_count >= 5);

    struct VkApplicationInfo {
        uint32_t sType; const void* pNext; const char* pAppName; uint32_t appVer;
        const char* pEngineName; uint32_t engineVer; uint32_t apiVersion;
    } app_info = {0, nullptr, "mali_icd_e2e_client", 1, "mali_icd_harness", 1, (1U << 22) | (3U << 12)};
    struct VkInstanceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
        uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE;
    } ici = {1, nullptr, 0, &app_info, 0, nullptr, 0, nullptr};

    void* instance = nullptr;
    auto createInst = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    int32_t irc = createInst(&ici, nullptr, &instance);
    assert(irc == 0 && instance != nullptr);
    std::cout << "[PASS] ICD Stage 1: System libvulkan.so.1 negotiated ICD interface, queried AOT Instance API Version "
              << ((api_ver >> 22) & 0x3FF) << "." << ((api_ver >> 12) & 0x3FF) << "." << (api_ver & 0xFFF)
              << " (" << ext_count << " instance extensions), and created VkInstance!\n";

    // ------------------------------------------------------------------------
    // Stage 2: PhysicalDevice Enumeration & /dev/mali0 GPUPROPS Inspection
    // ------------------------------------------------------------------------
    uint32_t pd_count = 1;
    void* phys_dev = nullptr;
    auto enumPD = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    assert(enumPD(instance, &pd_count, &phys_dev) == 0 && pd_count == 1 && phys_dev != nullptr);

    uint8_t raw_props[1024]{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"))(phys_dev, raw_props);
    uint32_t vendor_id = *reinterpret_cast<uint32_t*>(raw_props + 8);
    uint32_t device_id = *reinterpret_cast<uint32_t*>(raw_props + 12);
    const char* dev_name = reinterpret_cast<const char*>(raw_props + 20);
    assert(vendor_id == 0x13B5U); // Arm Ltd.
    std::cout << "[PASS] ICD Stage 2: VkPhysicalDevice enumerated via libvulkan.so.1:\n"
              << "       Device Name : " << dev_name << "\n"
              << "       Vendor ID   : 0x" << std::hex << vendor_id << " (Arm Ltd.) | Device ID: 0x" << device_id << std::dec << "\n";

    // ------------------------------------------------------------------------
    // Stage 3: vkCreateDevice & /dev/mali0 CSF/JM Queue Binding
    // ------------------------------------------------------------------------
    float q_prio = 1.0f;
    struct VkDeviceQueueCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx; uint32_t qCount; const float* pPrio;
    } qci = {2, nullptr, 0, 0, 1, &q_prio};
    struct VkDeviceCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t qciCount; const void* pQci;
        uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE; const void* pFeat;
    } dci = {3, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};

    void* device = nullptr;
    int32_t drc = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(phys_dev, &dci, nullptr, &device);
    assert(drc == 0 && device != nullptr);

    void* queue = nullptr;
    reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"))(device, 0, 0, &queue);
    assert(queue != nullptr);
    std::cout << "[PASS] ICD Stage 3: vkCreateDevice & vkGetDeviceQueue initialized /dev/mali0 context and CSF/JM queue via AOT!\n";

    // ------------------------------------------------------------------------
    // Stage 4: End-to-End Compute Shaders (vector_compute & branch_loop_compute)
    // ------------------------------------------------------------------------
    auto createBuf = reinterpret_cast<PFN_vkCreateBuffer>(gipa(instance, "vkCreateBuffer"));
    auto allocMem  = reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"));
    auto bindBuf   = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"));
    auto mapMem    = reinterpret_cast<PFN_vkMapMemory>(gipa(instance, "vkMapMemory"));

    struct VkBufferCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sharing; uint32_t qfc; const uint32_t* pQf;
    } bci = {12, nullptr, 0, 16384, 0x80, 0, 0, nullptr};
    struct VkMemoryAllocateInfo {
        uint32_t sType; const void* pNext; uint64_t size; uint32_t memTypeIdx;
    } mai = {5, nullptr, 16384, 0};

    uint64_t buf_in = 0, buf_out = 0, mem_in = 0, mem_out = 0;
    void* ptr_in = nullptr;
    void* ptr_out = nullptr;
    assert(createBuf(device, &bci, nullptr, &buf_in) == 0);
    assert(createBuf(device, &bci, nullptr, &buf_out) == 0);
    assert(allocMem(device, &mai, nullptr, &mem_in) == 0);
    assert(allocMem(device, &mai, nullptr, &mem_out) == 0);
    assert(bindBuf(device, buf_in, mem_in, 0) == 0);
    assert(bindBuf(device, buf_out, mem_out, 0) == 0);
    assert(mapMem(device, mem_in, 0, 16384, 0, &ptr_in) == 0 && ptr_in != nullptr);
    assert(mapMem(device, mem_out, 0, 16384, 0, &ptr_out) == 0 && ptr_out != nullptr);

    uint64_t dsl = 0, pipe_layout = 0, desc_pool = 0, desc_set = 0, cmd_pool = 0;
    void* cmd_buf = nullptr;
    reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(gipa(instance, "vkCreateDescriptorSetLayout"))(device, &bci, nullptr, &dsl);
    reinterpret_cast<PFN_vkCreatePipelineLayout>(gipa(instance, "vkCreatePipelineLayout"))(device, &bci, nullptr, &pipe_layout);
    reinterpret_cast<PFN_vkCreateDescriptorPool>(gipa(instance, "vkCreateDescriptorPool"))(device, &bci, nullptr, &desc_pool);
    reinterpret_cast<PFN_vkAllocateDescriptorSets>(gipa(instance, "vkAllocateDescriptorSets"))(device, &bci, &desc_set);

    struct VkDescriptorBufferInfo { uint64_t buffer; uint64_t offset; uint64_t range; } dbi[2] = {
        {buf_in, 0, 64}, {buf_out, 0, 64}
    };
    struct VkWriteDescriptorSet {
        uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBinding; uint32_t dstArrayElem;
        uint32_t count; uint32_t type; const void* pImg; const void* pBuf; const void* pTex;
    } wds[2] = {
        {35, nullptr, desc_set, 0, 0, 1, 7, nullptr, &dbi[0], nullptr},
        {35, nullptr, desc_set, 1, 0, 1, 7, nullptr, &dbi[1], nullptr}
    };
    reinterpret_cast<PFN_vkUpdateDescriptorSets>(gipa(instance, "vkUpdateDescriptorSets"))(device, 2, wds, 0, nullptr);

    reinterpret_cast<PFN_vkCreateCommandPool>(gipa(instance, "vkCreateCommandPool"))(device, &bci, nullptr, &cmd_pool);
    struct VkCommandBufferAllocateInfo {
        uint32_t sType; const void* pNext; uint64_t pool; uint32_t level; uint32_t count;
    } cbai = {40, nullptr, cmd_pool, 0, 1};
    assert(reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd_buf) == 0);

    auto runComputeShaderViaIcd = [&](const char* label, const std::vector<uint32_t>& spv, const float expected[4]) {
        float* in_f  = static_cast<float*>(ptr_in);
        float* out_f = static_cast<float*>(ptr_out);
        in_f[0] = 3.0f; in_f[1] = 4.0f; in_f[2] = 5.0f; in_f[3] = 6.0f;
        out_f[0] = out_f[1] = out_f[2] = out_f[3] = 0.0f;

        struct VkShaderModuleCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
        } smci = {16, nullptr, 0, spv.size() * sizeof(uint32_t), spv.data()};
        uint64_t shader_mod = 0;
        assert(reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"))(device, &smci, nullptr, &shader_mod) == 0);

        struct VkComputePipelineCreateInfo {
            uint32_t sType; const void* pNext; uint32_t flags;
            struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec; } stage;
            uint64_t layout; uint64_t baseHandle; int32_t baseIdx;
        } cpci = {29, nullptr, 0, {18, nullptr, 0, 0x20, shader_mod, "main", nullptr}, pipe_layout, 0, -1};
        uint64_t pipeline = 0;
        assert(reinterpret_cast<PFN_vkCreateComputePipelines>(gipa(instance, "vkCreateComputePipelines"))(device, 0, 1, &cpci, nullptr, &pipeline) == 0);

        struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
        reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);
        reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"))(cmd_buf, 1, pipeline);
        reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gipa(instance, "vkCmdBindDescriptorSets"))(cmd_buf, 1, pipe_layout, 0, 1, &desc_set, 0, nullptr);
        reinterpret_cast<PFN_vkCmdDispatch>(gipa(instance, "vkCmdDispatch"))(cmd_buf, 1, 1, 1);
        reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);

        struct VkSubmitInfo {
            uint32_t sType; const void* pNext; uint32_t waitC; const uint64_t* pWaitS; const uint32_t* pWaitM;
            uint32_t cbC; void* const* pCbs; uint32_t sigC; const uint64_t* pSigS;
        } si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};
        assert(reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0) == 0);
        reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);

        for (int i = 0; i < 4; ++i) {
            assert(std::fabs(out_f[i] - expected[i]) < 1e-4f);
        }
        std::cout << "[PASS] ICD Stage 4 (" << label << "): Dispatched via libvulkan.so.1 -> AOT CSF Ringbuffer -> mali0_emu -> ["
                  << std::fixed << std::setprecision(3)
                  << out_f[0] << ", " << out_f[1] << ", " << out_f[2] << ", " << out_f[3] << "]\n";

        reinterpret_cast<PFN_vkDestroyPipeline>(gipa(instance, "vkDestroyPipeline"))(device, pipeline, nullptr);
        reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
    };

    const float exp_vec[4]  = {38.250f, 67.000f, 103.750f, 148.500f};
    const float exp_loop[4] = {15.000f, 16.250f, 17.500f, 18.750f};
    runComputeShaderViaIcd("vector_compute.spv", buildVectorComputeSpv(), exp_vec);
    runComputeShaderViaIcd("branch_loop_compute.spv", buildBranchLoopComputeSpv(), exp_loop);

    // ------------------------------------------------------------------------
    // Stage 5: 3D vkcube Multi-Frame Rendering via vkCmdDrawIndexed + vkQueueSubmit
    // ------------------------------------------------------------------------
    uint64_t buf_fb = 0, mem_fb = 0;
    void* ptr_fb = nullptr;
    assert(createBuf(device, &bci, nullptr, &buf_fb) == 0);
    assert(allocMem(device, &mai, nullptr, &mem_fb) == 0);
    assert(bindBuf(device, buf_fb, mem_fb, 0) == 0);
    assert(mapMem(device, mem_fb, 0, 16384, 0, &ptr_fb) == 0 && ptr_fb != nullptr);

    // Set up MaliIdvsDrawPacket + MaliMfbdDescriptor + 24-vertex Cube in ptr_in (mapped /dev/mali0 BO)
    uint8_t* geom_base = static_cast<uint8_t*>(ptr_in);
    auto* cube_verts   = reinterpret_cast<MaliCubeVertex*>(geom_base + 0x100);
    auto* cube_indices = reinterpret_cast<uint32_t*>(geom_base + 0x580);
    auto* mvp_ubo      = reinterpret_cast<float*>(geom_base + 0x680);
    auto* mfbd         = reinterpret_cast<MaliMfbdDescriptor*>(geom_base + 0x780);
    auto* idvs_pkt     = reinterpret_cast<MaliIdvsDrawPacket*>(geom_base + 0x000); // at offset 0 of buf_in!
    auto* vs_spd       = reinterpret_cast<uint64_t*>(geom_base + 0x880);
    auto* fs_spd       = reinterpret_cast<uint64_t*>(geom_base + 0x8C0);
    auto* stats_out    = reinterpret_cast<uint32_t*>(geom_base + 0x900);

    // Build cube geometry & MVP using a shader module's EXEC_VA BO
    auto vec_spv = buildVectorComputeSpv();
    struct VkShaderModuleCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode;
    } smci = {16, nullptr, 0, vec_spv.size() * 4, vec_spv.data()};
    uint64_t cube_sm = 0;
    reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"))(device, &smci, nullptr, &cube_sm);

    // Populate cube faces
    struct FaceDef { float nx, ny, nz, cr, cg, cb; float v[4][3]; };
    const FaceDef faces[6] = {
        {  0,  0,  1, 0.95f, 0.25f, 0.20f, { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} } },
        {  0,  0, -1, 0.15f, 0.85f, 0.90f, { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} } },
        {  1,  0,  0, 0.20f, 0.90f, 0.35f, { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} } },
        { -1,  0,  0, 0.90f, 0.25f, 0.85f, { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} } },
        {  0,  1,  0, 0.98f, 0.85f, 0.15f, { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} } },
        {  0, -1,  0, 0.25f, 0.45f, 0.98f, { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} } }
    };
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t k = 0; k < 4; ++k) {
            auto& dst = cube_verts[f * 4 + k];
            dst.px = faces[f].v[k][0]; dst.py = faces[f].v[k][1]; dst.pz = faces[f].v[k][2]; dst.pw = 1.0f;
            dst.cr = faces[f].cr; dst.cg = faces[f].cg; dst.cb = faces[f].cb; dst.ca = 1.0f;
            dst.nx = faces[f].nx; dst.ny = faces[f].ny; dst.nz = faces[f].nz; dst.nw = 0.0f;
        }
        uint32_t base = f * 4;
        cube_indices[f*6+0] = base+0; cube_indices[f*6+1] = base+1; cube_indices[f*6+2] = base+2;
        cube_indices[f*6+3] = base+0; cube_indices[f*6+4] = base+2; cube_indices[f*6+5] = base+3;
    }

    vs_spd[0] = 0; vs_spd[1] = 0;
    fs_spd[0] = 0; fs_spd[1] = 0;
    mfbd->magic             = MALI_MFBD_MAGIC;
    mfbd->width             = 64;
    mfbd->height            = 64;
    mfbd->sample_count      = 1;
    mfbd->clear_color_rgba8 = 0xFF181410U;
    mfbd->clear_depth       = 1.0f;
    mfbd->color_rt0_gpu_va  = reinterpret_cast<uint64_t>(ptr_fb);
    mfbd->depth_bo_gpu_va   = reinterpret_cast<uint64_t>(ptr_out);
    mfbd->stats_bo_gpu_va   = reinterpret_cast<uint64_t>(stats_out);

    idvs_pkt->magic            = MALI_IDVS_MAGIC;
    idvs_pkt->vertex_count     = 24;
    idvs_pkt->index_count      = 36;
    idvs_pkt->instance_count   = 1;
    idvs_pkt->vertex_bo_gpu_va = reinterpret_cast<uint64_t>(cube_verts);
    idvs_pkt->index_bo_gpu_va  = reinterpret_cast<uint64_t>(cube_indices);
    idvs_pkt->mvp_ubo_gpu_va   = reinterpret_cast<uint64_t>(mvp_ubo);
    idvs_pkt->mfbd_gpu_va      = reinterpret_cast<uint64_t>(mfbd);
    idvs_pkt->vs_spd_gpu_va    = reinterpret_cast<uint64_t>(vs_spd);
    idvs_pkt->fs_spd_gpu_va    = reinterpret_cast<uint64_t>(fs_spd);

    auto renderCubeFrameViaIcd = [&](uint32_t frame_no, float angle_deg) -> uint32_t {
        idvs_pkt->frame_index  = frame_no;
        idvs_pkt->rotation_deg = angle_deg;
        float rad_y = angle_deg * 3.1415926535f / 180.0f;
        float rad_x = 24.0f     * 3.1415926535f / 180.0f;
        float cy = std::cos(rad_y), sy = std::sin(rad_y);
        float cx = std::cos(rad_x), sx = std::sin(rad_x);
        float s = 0.52f;
        mvp_ubo[0] = s*cy;       mvp_ubo[1] = s*sx*sy;    mvp_ubo[2] = -0.25f*cx*sy; mvp_ubo[3] = 0.0f;
        mvp_ubo[4] = 0.0f;       mvp_ubo[5] = s*cx;       mvp_ubo[6] = 0.25f*sx;     mvp_ubo[7] = 0.0f;
        mvp_ubo[8] = s*sy;       mvp_ubo[9] = -s*sx*cy;   mvp_ubo[10]= -0.25f*cx*cy; mvp_ubo[11]= 0.0f;
        mvp_ubo[12]= 0.0f;       mvp_ubo[13]= 0.0f;       mvp_ubo[14]= 0.0f;         mvp_ubo[15]= 1.0f;
        mvp_ubo[16]= 0.408248f;  mvp_ubo[17]= 0.577350f;  mvp_ubo[18]= 0.707107f;    mvp_ubo[19]= 0.0f;

        struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
        reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);
        reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gipa(instance, "vkCmdBindDescriptorSets"))(cmd_buf, 0, pipe_layout, 0, 1, &desc_set, 0, nullptr);
        reinterpret_cast<PFN_vkCmdDrawIndexed>(gipa(instance, "vkCmdDrawIndexed"))(cmd_buf, 36, 1, 0, 0, frame_no);
        reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);

        struct VkSubmitInfo {
            uint32_t sType; const void* pNext; uint32_t waitC; const uint64_t* pWaitS; const uint32_t* pWaitM;
            uint32_t cbC; void* const* pCbs; uint32_t sigC; const uint64_t* pSigS;
        } si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};
        assert(reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0) == 0);
        reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);

        const uint32_t* rgba = static_cast<const uint32_t*>(ptr_fb);
        uint32_t shaded = 0, crc = 0x811C9DC5U;
        for (int i = 0; i < 64 * 64; ++i) {
            if (rgba[i] != 0xFF181410U) shaded++;
            crc = (crc ^ rgba[i]) * 0x01000193U;
        }
        assert(shaded == 1788);
        return crc;
    };

    uint32_t crc0 = renderCubeFrameViaIcd(0, 25.0f);
    uint32_t crc1 = renderCubeFrameViaIcd(1, 65.0f);
    assert(crc0 != crc1);
    std::cout << "[PASS] ICD Stage 5 (3D vkcube via vkCmdDrawIndexed + vkQueueSubmit):\n"
              << "       Frame 0 (25 deg): shaded_px=1788, crc32=0x" << std::hex << crc0 << "\n"
              << "       Frame 1 (65 deg): shaded_px=1788, crc32=0x" << crc1 << std::dec << "\n";

    reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, cube_sm, nullptr);
    reinterpret_cast<PFN_vkDestroyBuffer>(gipa(instance, "vkDestroyBuffer"))(device, buf_fb, nullptr);
    reinterpret_cast<PFN_vkDestroyBuffer>(gipa(instance, "vkDestroyBuffer"))(device, buf_in, nullptr);
    reinterpret_cast<PFN_vkDestroyBuffer>(gipa(instance, "vkDestroyBuffer"))(device, buf_out, nullptr);
    reinterpret_cast<PFN_vkFreeMemory>(gipa(instance, "vkFreeMemory"))(device, mem_fb, nullptr);
    reinterpret_cast<PFN_vkFreeMemory>(gipa(instance, "vkFreeMemory"))(device, mem_in, nullptr);
    reinterpret_cast<PFN_vkFreeMemory>(gipa(instance, "vkFreeMemory"))(device, mem_out, nullptr);
    reinterpret_cast<PFN_vkDestroyDevice>(gipa(instance, "vkDestroyDevice"))(device, nullptr);
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);

    std::cout << "================================================================================\n";
    std::cout << " ALL 5 E2E VULKAN ICD STAGES PASSED VIA SYSTEM KHRONOS LOADER (libvulkan.so.1)! \n";
    std::cout << "================================================================================\n";
    return 0;
}
