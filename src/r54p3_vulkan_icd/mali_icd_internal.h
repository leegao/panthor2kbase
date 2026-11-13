// ============================================================================
// src/aot/src/mali_icd_internal.h
// Shared internal structures, global resource maps (g_icd_mutex, g_buffers,
// g_images), and AOT runtime state for the modular Mali AOT Vulkan ICD.
// ============================================================================
#pragma once

// ============================================================================
// mali_vulkan_icd.cpp
// Full x86-64 Khronos Vulkan ICD (libvulkan_mali_aot_icd.so) that:
//   1. Loads the AOT-translated 64-bit ARM64 Mali driver (e.g. r54p3, r49p1, r26p0)
//   2. Executes AOT-translated Vulkan ICD & CMPBE functions (vk_hal_open,
//      vkGetInstanceProcAddr, vkEnumerateInstance*, vkCreateShaderModule,
//      vkDestroyShaderModule, cmpbe_v2_deserialize_MBS2_to_C)
//   3. Executes AOT-translated /dev/mali0 (kbase CSF & JM) UAPI functions
//      (kbase_version_check_*, kbase_set_flags, kbase_mem_alloc,
//       kbase_cs_queue_group_create, kbase_cs_queue_register,
//       kbase_cs_queue_bind, kbase_cs_queue_kick, kbase_job_submit)
//   4. Emulates the Mali hardware layer (/dev/mali0 + /dev/dma_heap + CSF/JM +
//      Valhall/Bifrost OBJC -> SPIR-V 1.3) via mali0_emu on either Mesa
//      llvmpipe or the host Vulkan driver.
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <dlfcn.h>

#include <cstdarg>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "mali_csf_uapi_types.h"
#include "valhall_spirv_lifter.hpp"
#include "../emulator/mali_kbase_emu.h"
#include "mali_aapcs64_auto_thunks.hpp"
#define AOT_SO_PROFILER_IMPLEMENTATION
#include "aot_so_profiler.hpp"
#include "../emulator/lifted_kbase_ko.h"
#include "generated/mali_bionic_stubs_autogen.h"
#include "generated/mali_vk_dispatch_table_autogen.h"

static bool mali_icd_log_enabled() {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* env = getenv("MALI_AOT_ICD_LOG");
        s_enabled = (env && env[0] && std::atoi(env) != 0) ? 1 : 0;
    }
    return s_enabled != 0;
}

static void mali_icd_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void mali_icd_log(const char* fmt, ...) {
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
static void mali_aot_icd_onload() {
    if (!mali_icd_log_enabled()) return;
    char exe_buf[512]{};
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) exe_buf[len] = '\0';
    mali_icd_log("Loaded libvulkan_mali_aot_icd.so into pid=%d exe='%s' DISPLAY='%s' WAYLAND_DISPLAY='%s' VK_ICD_FILENAMES='%s' VK_DRIVER_FILES='%s'",
                 static_cast<int>(getpid()),
                 exe_buf[0] ? exe_buf : "(unknown)",
                 getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)",
                 getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
                 getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(unset)",
                 getenv("VK_DRIVER_FILES") ? getenv("VK_DRIVER_FILES") : "(unset)");
}

// ============================================================================
// Section 1: AOT ARM64 Runtime State & Imported Symbols from mali_mbs2_aot.o
// ============================================================================
static constexpr uint64_t IMAGE_BASE = 0x400000000ULL;
static constexpr uint64_t HLE_MALLOC_MAGIC   = 0xF000FFF0ULL;
static constexpr uint64_t HLE_TLSDESC_MAGIC  = 0xF000FFF4ULL;
static constexpr uint64_t HLE_VK_FREE_MAGIC  = 0xF000FFF6ULL;
static constexpr uint64_t HLE_FREE_MAGIC     = 0xF000FFF8ULL;
static constexpr uint64_t HLE_VK_ALLOC_MAGIC = 0xF000FFFCULL;
static constexpr uint64_t BIONIC_STACK_GUARD_CANARY = 0xDEADBEEFCAFEBABEULL;
static constexpr uintptr_t ICD_LOADER_MAGIC = 0x01CDC0DEULL;

struct alignas(16) AOTCPUState {
    uint64_t    x[31]{};
    uint64_t    sp = 0;
    uint64_t    pc = 0;
    uint64_t    tpidr_el0 = 0;
    __uint128_t v[32]{};
};

extern "C" {
    __attribute__((weak)) uint64_t aot_get_ctx_malloc_offset() { return 0x78; }
    __attribute__((weak)) uint64_t aot_get_vk_hmi_addr() { return IMAGE_BASE + 0x03070018ULL; }
    __attribute__((weak)) uint64_t aot_get_kbase_page_shift_addr() { return IMAGE_BASE + 0x0313b940ULL; }
    __attribute__((weak)) uint64_t aot_get_vk_dispatch_table_rva() { return 0x02f9bfb0ULL; }
    __attribute__((weak)) uint64_t aot_get_vk_dispatch_table_count() { return 456ULL; }
    __attribute__((weak)) uint64_t aot_get_vk_dispatch_table_stride() { return 56ULL; }
    __attribute__((weak)) uint64_t aot_get_lifted_vk_entrypoint_count() { return 456ULL; }
    __attribute__((weak)) uint64_t aot_get_init_array_count() { return 0ULL; }
    __attribute__((weak)) void aot_run_dt_init_array_entry(AOTCPUState* /*cpu*/, uint64_t /*idx*/) {}
    __attribute__((weak)) void aot_run_dt_init_array(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_deserialize_MBS2_to_C(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_init_options(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_set_option_value(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_free_compilation_result(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_compile_multiple_shaders(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_compile_single_shader(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_blend_shadergen_generate_shader(AOTCPUState* /*cpu*/) {}
    void aot_vk_hal_open(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetInstanceProcAddr(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetDeviceProcAddr(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceVersion(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceLayerProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceExtensionProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumeratePhysicalDevices(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumeratePhysicalDeviceGroups(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceFeatures(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceFeatures2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceMemoryProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceMemoryProperties2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceQueueFamilyProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetPhysicalDeviceQueueFamilyProperties2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateDeviceExtensionProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateDeviceLayerProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetDeviceQueue(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetDeviceQueue2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreateBuffer(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyBuffer(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreateEvent(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyEvent(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetEventStatus(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkSetEvent(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkResetEvent(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreateFence(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetFenceStatus(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkWaitForFences(AOTCPUState* cpu) __attribute__((weak));
    int mali_kbase_emu_wait_csf_idle(uint64_t timeout_ns) __attribute__((weak));
    void aot_vkCreateDescriptorSetLayout(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreatePipelineLayout(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDeviceWaitIdle(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkQueueWaitIdle(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_event_signal(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetBufferMemoryRequirements(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetBufferMemoryRequirements2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetBufferDeviceAddress(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetImageMemoryRequirements(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetImageMemoryRequirements2(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkMapMemory(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkUnmapMemory(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkResetFences(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyFence(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyDescriptorSetLayout(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyPipelineLayout(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreateShaderModule(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyShaderModule(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetBlendConstants(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetDepthBias(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetDepthBounds(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetLineWidth(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetStencilCompareMask(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetStencilReference(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCmdSetStencilWriteMask(AOTCPUState* cpu) __attribute__((weak));
    void aot_mali_gpu_props_get(AOTCPUState* cpu) __attribute__((weak));
    void aot_sub_01c64f90(AOTCPUState* cpu) __attribute__((weak));

    void aot_kbase_version_check_csf(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_version_check_jm(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_version_check_reserved(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_job_submit(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_post_term(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_soft_event_update(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_free(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_query(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_set_flags(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_jit_init(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_exec_init(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_alloc(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_commit(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_alias(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_import(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_flags_change(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_mem_sync(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_get_glb_iface(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_register(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_register_ex(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_terminate(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_bind(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_kick(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_group_create(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_queue_group_terminate(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_tiler_heap_init(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_cs_tiler_heap_term(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_kcpu_queue_create(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_kcpu_queue_delete(AOTCPUState* cpu) __attribute__((weak));
    void aot_kbase_kcpu_queue_enqueue(AOTCPUState* cpu) __attribute__((weak));

    __attribute__((weak)) void aot_indirect_dispatch(AOTCPUState* /*cpu*/, uint64_t /*target_va*/) {}
}

// ============================================================================
// Section 2: Bionic AAPCS64 Bridges & /dev/mali0 (mali0_emu) Hooks
// ============================================================================
struct AAPCS64VaList {
    uint64_t stack;
    uint64_t gr_top;
    uint64_t vr_top;
    int32_t  gr_offs;
    int32_t  vr_offs;

    uint64_t nextGPR() {
        if (gr_offs < 0) {
            uint64_t addr = gr_top + static_cast<int64_t>(gr_offs);
            gr_offs += 8;
            if (gr_offs > 0) gr_offs = 0;
            return *reinterpret_cast<const uint64_t*>(addr);
        }
        uint64_t addr = stack;
        stack += 8;
        return *reinterpret_cast<const uint64_t*>(addr);
    }
    double nextFP() {
        if (vr_offs < 0) {
            uint64_t addr = vr_top + static_cast<int64_t>(vr_offs);
            vr_offs += 16;
            if (vr_offs > 0) vr_offs = 0;
            return *reinterpret_cast<const double*>(addr);
        }
        uint64_t addr = stack;
        stack += 8;
        return *reinterpret_cast<const double*>(addr);
    }
};

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

class HostSpirvExecutor {
public:
    static constexpr size_t kBufferCapacityBytes = 4 * 1024 * 1024;
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
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    bool tryOpenBackendIcdLibrary(const char* so_path) {
        void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) return false;
        auto fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vk_icdGetInstanceProcAddr"));
        if (!fn) fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        if (!fn) { dlclose(handle); return false; }
        struct AppInfo { uint32_t sType; const void* pNext; const char* pAppName; uint32_t appVer; const char* pEngineName; uint32_t engineVer; uint32_t apiVersion; }
            appInfo = {0, nullptr, "mali0_spirv_exec", 1, "mali0", 1, (1U << 22) | (3U << 12)};
        struct InstCI { uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo; uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE; }
            ici = {1, nullptr, 0, &appInfo, 0, nullptr, 0, nullptr};
        auto createInst = reinterpret_cast<PFN_vkCreateInstance>(fn(nullptr, "vkCreateInstance"));
        void* test_inst = nullptr;
        if (!createInst || createInst(&ici, nullptr, &test_inst) != 0 || !test_inst) { dlclose(handle); return false; }
        auto enumPD = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fn(test_inst, "vkEnumeratePhysicalDevices"));
        uint32_t pdCount = 1;
        void* test_pd = nullptr;
        if (!enumPD || enumPD(test_inst, &pdCount, &test_pd) != 0 || pdCount == 0 || !test_pd) { dlclose(handle); return false; }
        vk_lib = handle; gipa = fn; instance = test_inst; phys_dev = test_pd;
        return true;
    }

    bool init() {
        if (device) return true;
        static thread_local bool s_in_init = false;
        if (s_in_init) return false;
        struct Guard { bool& f; Guard(bool& v) : f(v) { f = true; } ~Guard() { f = false; } } guard(s_in_init);
        const char* custom_lib = getenv("MALI0_EMU_BACKEND_LIB");
        if (!custom_lib || !tryOpenBackendIcdLibrary(custom_lib)) {
            const char* candidates[] = {
                "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
                "/usr/lib/libvulkan_lvp.so",
                "/usr/lib64/libvulkan_lvp.so",
                "libvulkan_lvp.so"
            };
            for (const char* cand : candidates) {
                if (tryOpenBackendIcdLibrary(cand)) break;
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
        struct QCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx; uint32_t qCount; const float* pPrio; } qci = {2, nullptr, 0, 0, 1, &qPrio};
        struct DCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qciCount; const void* pQci; uint32_t lCount; const char* const* ppL; uint32_t eCount; const char* const* ppE; const void* pFeatures; }
            dci = {3, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};
        auto createDev = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
        if (!createDev || createDev(phys_dev, &dci, nullptr, &device) != 0) return false;
        reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(instance, "vkGetDeviceQueue"))(device, 0, 0, &queue);
        struct DSLB { uint32_t binding; uint32_t descriptorType; uint32_t descriptorCount; uint32_t stageFlags; const void* pImm; } bindings[2] = {
            {0, 7, 1, 0x20, nullptr}, {1, 7, 1, 0x20, nullptr}
        };
        struct DSLCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t bindingCount; const void* pBindings; } dslci = {32, nullptr, 0, 2, bindings};
        if (reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(gipa(instance, "vkCreateDescriptorSetLayout"))(device, &dslci, nullptr, &desc_set_layout) != 0) return false;
        struct PLCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t setLayoutCount; const uint64_t* pSetLayouts; uint32_t pcCount; const void* pPc; }
            plci = {30, nullptr, 0, 1, &desc_set_layout, 0, nullptr};
        if (reinterpret_cast<PFN_vkCreatePipelineLayout>(gipa(instance, "vkCreatePipelineLayout"))(device, &plci, nullptr, &pipeline_layout) != 0) return false;
        struct BCI { uint32_t sType; const void* pNext; uint32_t flags; uint64_t size; uint32_t usage; uint32_t sharingMode; uint32_t qfiCount; const uint32_t* pQfi; }
            bci = {12, nullptr, 0, kBufferCapacityBytes, 0xE3U, 0, 0, nullptr};
        auto createBuf = reinterpret_cast<PFN_vkCreateBuffer>(gipa(instance, "vkCreateBuffer"));
        createBuf(device, &bci, nullptr, &buf_in);
        createBuf(device, &bci, nullptr, &buf_out);
        struct MAI { uint32_t sType; const void* pNext; uint64_t allocationSize; uint32_t memoryTypeIndex; } mai = {5, nullptr, kBufferCapacityBytes, 0};
        auto allocMem = reinterpret_cast<PFN_vkAllocateMemory>(gipa(instance, "vkAllocateMemory"));
        allocMem(device, &mai, nullptr, &mem_in);
        allocMem(device, &mai, nullptr, &mem_out);
        auto bindBuf = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(instance, "vkBindBufferMemory"));
        bindBuf(device, buf_in, mem_in, 0);
        bindBuf(device, buf_out, mem_out, 0);
        auto mapMem = reinterpret_cast<PFN_vkMapMemory>(gipa(instance, "vkMapMemory"));
        mapMem(device, mem_in, 0, kBufferCapacityBytes, 0, &mapped_in);
        mapMem(device, mem_out, 0, kBufferCapacityBytes, 0, &mapped_out);
        struct DPS { uint32_t type; uint32_t count; } poolSz = {7, 2};
        struct DPCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t maxSets; uint32_t poolSizeCount; const void* pPoolSizes; } dpci = {33, nullptr, 0, 1, 1, &poolSz};
        reinterpret_cast<PFN_vkCreateDescriptorPool>(gipa(instance, "vkCreateDescriptorPool"))(device, &dpci, nullptr, &desc_pool);
        struct DSAI { uint32_t sType; const void* pNext; uint64_t descriptorPool; uint32_t descriptorSetCount; const uint64_t* pSetLayouts; } dsai = {34, nullptr, desc_pool, 1, &desc_set_layout};
        reinterpret_cast<PFN_vkAllocateDescriptorSets>(gipa(instance, "vkAllocateDescriptorSets"))(device, &dsai, &desc_set);
        struct DBI { uint64_t buffer; uint64_t offset; uint64_t range; } dbi[2] = { {buf_in, 0, kBufferCapacityBytes}, {buf_out, 0, kBufferCapacityBytes} };
        struct WDS { uint32_t sType; const void* pNext; uint64_t dstSet; uint32_t dstBinding; uint32_t dstArrayElement; uint32_t descriptorCount; uint32_t descriptorType; const void* pImg; const void* pBuf; const void* pTex; } wds[2] = {
            {35, nullptr, desc_set, 0, 0, 1, 7, nullptr, &dbi[0], nullptr},
            {35, nullptr, desc_set, 1, 0, 1, 7, nullptr, &dbi[1], nullptr}
        };
        reinterpret_cast<PFN_vkUpdateDescriptorSets>(gipa(instance, "vkUpdateDescriptorSets"))(device, 2, wds, 0, nullptr);
        struct CPOCI { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfIdx; } cpoci = {39, nullptr, 0x02, 0};
        reinterpret_cast<PFN_vkCreateCommandPool>(gipa(instance, "vkCreateCommandPool"))(device, &cpoci, nullptr, &cmd_pool);
        struct CBAI { uint32_t sType; const void* pNext; uint64_t commandPool; uint32_t level; uint32_t count; } cbai = {40, nullptr, cmd_pool, 0, 1};
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd_buf);
        return true;
    }

    bool executeLiftedSpirv(const std::vector<uint32_t>& spv_words,
                            const void* kbase_in_bo_ptr, void* kbase_out_bo_ptr,
                            size_t copy_bytes = 64,
                            uint32_t wg_x = 1, uint32_t wg_y = 1, uint32_t wg_z = 1) {
        if (!init()) return false;
        size_t clamped_bytes = std::min<size_t>(copy_bytes, kBufferCapacityBytes);
        if (kbase_in_bo_ptr && mapped_in) std::memcpy(mapped_in, kbase_in_bo_ptr, clamped_bytes);
        if (mapped_out) std::memset(mapped_out, 0, clamped_bytes);
        struct SMCI { uint32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode; }
            smci = {16, nullptr, 0, spv_words.size() * sizeof(uint32_t), spv_words.data()};
        uint64_t shader_mod = 0;
        if (reinterpret_cast<PFN_vkCreateShaderModule>(gipa(instance, "vkCreateShaderModule"))(device, &smci, nullptr, &shader_mod) != 0 || !shader_mod) return false;
        struct CPCI {
            uint32_t sType; const void* pNext; uint32_t flags;
            struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; uint64_t module; const char* pName; const void* pSpec; } stage;
            uint64_t layout; uint64_t basePipelineHandle; int32_t basePipelineIndex;
        } cpci = {29, nullptr, 0, {18, nullptr, 0, 0x20, shader_mod, "main", nullptr}, pipeline_layout, 0, -1};
        uint64_t pipeline = 0;
        if (reinterpret_cast<PFN_vkCreateComputePipelines>(gipa(instance, "vkCreateComputePipelines"))(device, 0, 1, &cpci, nullptr, &pipeline) != 0 || !pipeline) {
            reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
            return false;
        }
        reinterpret_cast<PFN_vkResetCommandBuffer>(gipa(instance, "vkResetCommandBuffer"))(cmd_buf, 0);
        struct CBBI { uint32_t sType; const void* pNext; uint32_t flags; const void* pInh; } cbbi = {42, nullptr, 0, nullptr};
        reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(instance, "vkBeginCommandBuffer"))(cmd_buf, &cbbi);
        reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(instance, "vkCmdBindPipeline"))(cmd_buf, 1, pipeline);
        reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gipa(instance, "vkCmdBindDescriptorSets"))(cmd_buf, 1, pipeline_layout, 0, 1, &desc_set, 0, nullptr);
        uint32_t max_wg_x = static_cast<uint32_t>(kBufferCapacityBytes / 16);
        uint32_t safe_wg_x = (wg_x == 0) ? 1 : ((wg_x > max_wg_x) ? max_wg_x : wg_x);
        reinterpret_cast<PFN_vkCmdDispatch>(gipa(instance, "vkCmdDispatch"))(cmd_buf, safe_wg_x, wg_y ? wg_y : 1, wg_z ? wg_z : 1);
        reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(instance, "vkEndCommandBuffer"))(cmd_buf);
        struct SI { uint32_t sType; const void* pNext; uint32_t waitSemCount; const uint64_t* pWaitSems; const uint32_t* pWaitStages; uint32_t cmdBufCount; void* const* pCmdBufs; uint32_t sigSemCount; const uint64_t* pSigSems; }
            si = {4, nullptr, 0, nullptr, nullptr, 1, &cmd_buf, 0, nullptr};
        int32_t src = reinterpret_cast<PFN_vkQueueSubmit>(gipa(instance, "vkQueueSubmit"))(queue, 1, &si, 0);
        reinterpret_cast<PFN_vkQueueWaitIdle>(gipa(instance, "vkQueueWaitIdle"))(queue);
        if (src == 0 && kbase_out_bo_ptr && mapped_out) {
            std::memcpy(kbase_out_bo_ptr, mapped_out, clamped_bytes);
        }
        reinterpret_cast<PFN_vkDestroyPipeline>(gipa(instance, "vkDestroyPipeline"))(device, pipeline, nullptr);
        reinterpret_cast<PFN_vkDestroyShaderModule>(gipa(instance, "vkDestroyShaderModule"))(device, shader_mod, nullptr);
        return src == 0;
    }
};

static HostSpirvExecutor        g_icd_spirv_executor;
static uint32_t                 g_icd_csf_jobs_executed = 0;
static int                      g_icd_kbase_ioctl_calls = 0;

static void onLiftedCsfJobCallback(uint8_t /*opc*/, uint64_t insn, const uint64_t* cs_regs64, void* /*user_data*/) {
    if (!cs_regs64) return;
    auto getReg64 = [&](unsigned d_idx) -> uint64_t {
        if (d_idx + 1 >= 96) return 0;
        return (cs_regs64[d_idx] & 0xFFFFFFFFULL) | ((cs_regs64[d_idx + 1] & 0xFFFFFFFFULL) << 32);
    };
    uint8_t srt_sel = static_cast<uint8_t>((insn >> 40) & 0x3U);
    uint8_t spd_sel = static_cast<uint8_t>((insn >> 42) & 0x3U);
    uint64_t srt_gpu_va = getReg64(srt_sel * 2);
    uint64_t spd_gpu_va = getReg64(8 + spd_sel * 2);
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
struct HostVkAllocatorTable {
    std::mutex mtx;
    std::unordered_map<uint64_t, void* (*)(void*, size_t, size_t, uint32_t)> alloc_fns;
    std::unordered_map<uint64_t, void* (*)(void*, void*, size_t, size_t, uint32_t)> realloc_fns;
    std::unordered_map<uint64_t, void (*)(void*, void*)> free_fns;

    void registerCallbacks(const void* pAllocator) {
        if (!pAllocator) return;
        const uint64_t* words = static_cast<const uint64_t*>(pAllocator);
        std::lock_guard<std::mutex> lk(mtx);
        if (words[1]) alloc_fns[words[1]] = reinterpret_cast<void* (*)(void*, size_t, size_t, uint32_t)>(words[1]);
        if (words[2]) realloc_fns[words[2]] = reinterpret_cast<void* (*)(void*, void*, size_t, size_t, uint32_t)>(words[2]);
        if (words[3]) free_fns[words[3]] = reinterpret_cast<void (*)(void*, void*)>(words[3]);
    }

    bool tryDispatch(AOTCPUState* cpu, uint64_t target_va) {
        void* (*afn)(void*, size_t, size_t, uint32_t) = nullptr;
        void* (*rfn)(void*, void*, size_t, size_t, uint32_t) = nullptr;
        void (*ffn)(void*, void*) = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx);
            auto it_a = alloc_fns.find(target_va);
            if (it_a != alloc_fns.end()) afn = it_a->second;
            auto it_r = realloc_fns.find(target_va);
            if (it_r != realloc_fns.end()) rfn = it_r->second;
            auto it_f = free_fns.find(target_va);
            if (it_f != free_fns.end()) ffn = it_f->second;
        }
        if (afn) {
            void* res = afn(reinterpret_cast<void*>(cpu->x[0]),
                            static_cast<size_t>(cpu->x[1]),
                            static_cast<size_t>(cpu->x[2]),
                            static_cast<uint32_t>(cpu->x[3]));
            cpu->x[0] = reinterpret_cast<uint64_t>(res);
            return true;
        }
        if (rfn) {
            void* res = rfn(reinterpret_cast<void*>(cpu->x[0]),
                            reinterpret_cast<void*>(cpu->x[1]),
                            static_cast<size_t>(cpu->x[2]),
                            static_cast<size_t>(cpu->x[3]),
                            static_cast<uint32_t>(cpu->x[4]));
            cpu->x[0] = reinterpret_cast<uint64_t>(res);
            return true;
        }
        if (ffn) {
            ffn(reinterpret_cast<void*>(cpu->x[0]),
                reinterpret_cast<void*>(cpu->x[1]));
            cpu->x[0] = 0;
            return true;
        }
        return false;
    }
};

static HostVkAllocatorTable g_host_vk_allocators;

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

// ============================================================================
// Section 3: ELF64 Image Mapper & AOT Driver Context Manager
// ============================================================================
struct MaliIcdImageMapper {
    uint8_t* base = nullptr;
    size_t   image_size = 0x04000000;
    std::vector<uint8_t> elf;
    std::string driver_version = "r54p3-00eac0";
    std::string so_path_loaded;

    static int64_t readSLEB128(const uint8_t* buf, size_t& pos) {
        int64_t res = 0;
        unsigned shift = 0;
        while (true) {
            uint8_t b = buf[pos++];
            res |= static_cast<int64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0) {
                if (shift < 64 && (b & 0x40)) res |= -(1LL << shift);
                return res;
            }
        }
    }

    bool loadAndRelocate(const char* so_path) {
        std::ifstream in(so_path, std::ios::binary);
        if (!in) return false;
        so_path_loaded = so_path;
        elf.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (elf.size() < 0x1000 || elf[0] != 0x7f || elf[4] != 2) return false;

        for (size_t i = 0; i + 16 < elf.size(); ++i) {
            if (elf[i] == 'r' && elf[i + 1] >= '0' && elf[i + 1] <= '9') {
                size_t j = i + 1;
                while (j < i + 20 && ((elf[j] >= '0' && elf[j] <= '9') || (elf[j] >= 'a' && elf[j] <= 'z') || elf[j] == '-')) j++;
                std::string cand(reinterpret_cast<const char*>(&elf[i]), j - i);
                if (cand.find("p") != std::string::npos && cand.find("-0") != std::string::npos && cand.size() >= 10) {
                    driver_version = cand;
                    break;
                }
            }
        }

        void* mapped = mmap(reinterpret_cast<void*>(IMAGE_BASE), image_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped == MAP_FAILED) return false;
        base = static_cast<uint8_t*>(mapped);
        std::memset(base, 0, image_size);

        uint64_t e_phoff = *reinterpret_cast<const uint64_t*>(&elf[0x20]);
        uint16_t e_phentsize = *reinterpret_cast<const uint16_t*>(&elf[0x36]);
        uint16_t e_phnum = *reinterpret_cast<const uint16_t*>(&elf[0x38]);
        for (uint16_t i = 0; i < e_phnum; ++i) {
            const uint8_t* ph = &elf[e_phoff + i * e_phentsize];
            uint32_t p_type = *reinterpret_cast<const uint32_t*>(ph + 0);
            uint64_t p_offset = *reinterpret_cast<const uint64_t*>(ph + 8);
            uint64_t p_vaddr = *reinterpret_cast<const uint64_t*>(ph + 16);
            uint64_t p_filesz = *reinterpret_cast<const uint64_t*>(ph + 32);
            if (p_type == 1 && p_vaddr + p_filesz <= image_size) {
                std::memcpy(base + p_vaddr, &elf[p_offset], p_filesz);
            }
        }

        uint64_t e_shoff = *reinterpret_cast<const uint64_t*>(&elf[0x28]);
        uint16_t e_shentsize = *reinterpret_cast<const uint16_t*>(&elf[0x3a]);
        uint16_t e_shnum = *reinterpret_cast<const uint16_t*>(&elf[0x3c]);
        uint16_t e_shstrndx = *reinterpret_cast<const uint16_t*>(&elf[0x3e]);
        uint64_t shstr_off = *reinterpret_cast<const uint64_t*>(&elf[e_shoff + e_shstrndx * e_shentsize + 24]);

        uint64_t dynsym_off = 0, relr_off = 0, relr_sz = 0, reladyn_off = 0, reladyn_sz = 0;
        for (uint16_t i = 0; i < e_shnum; ++i) {
            const uint8_t* sh = &elf[e_shoff + i * e_shentsize];
            uint32_t sh_name = *reinterpret_cast<const uint32_t*>(sh + 0);
            uint64_t sh_offset = *reinterpret_cast<const uint64_t*>(sh + 24);
            uint64_t sh_size = *reinterpret_cast<const uint64_t*>(sh + 32);
            const char* sname = reinterpret_cast<const char*>(&elf[shstr_off + sh_name]);
            if (std::strcmp(sname, ".dynsym") == 0) dynsym_off = sh_offset;
            else if (std::strcmp(sname, ".relr.dyn") == 0) { relr_off = sh_offset; relr_sz = sh_size; }
            else if (std::strcmp(sname, ".rela.dyn") == 0) { reladyn_off = sh_offset; reladyn_sz = sh_size; }
            else if (std::strcmp(sname, ".note.gnu.build-id") == 0 && sh_size >= 20) {
                char hexbuf[16];
                std::snprintf(hexbuf, sizeof(hexbuf), "-%02x%02x%02x",
                              elf[sh_offset + 16], elf[sh_offset + 17], elf[sh_offset + 18]);
                driver_version += hexbuf;
            }
        }

        if (relr_off && relr_sz) {
            const uint64_t* relr = reinterpret_cast<const uint64_t*>(&elf[relr_off]);
            size_t relr_count = relr_sz / 8;
            uint64_t where = 0;
            for (size_t i = 0; i < relr_count; ++i) {
                uint64_t e = relr[i];
                if ((e & 1ULL) == 0) {
                    where = e;
                    uint64_t raw_v = *reinterpret_cast<uint64_t*>(base + where) & 0xFFFFFFFFFFFFULL;
                    *reinterpret_cast<uint64_t*>(base + where) = reinterpret_cast<uint64_t>(base) + raw_v;
                    where += 8;
                } else {
                    uint64_t bmap = e >> 1;
                    for (unsigned b = 0; b < 63; ++b) {
                        if ((bmap >> b) & 1ULL) {
                            uint64_t raw_v = *reinterpret_cast<uint64_t*>(base + where + b * 8) & 0xFFFFFFFFFFFFULL;
                            *reinterpret_cast<uint64_t*>(base + where + b * 8) = reinterpret_cast<uint64_t>(base) + raw_v;
                        }
                    }
                    where += 63 * 8;
                }
            }
        }

        uint64_t ext_glob_stub_va = reinterpret_cast<uint64_t>(base + image_size - 0x2000);
        if (reladyn_off && reladyn_sz >= 4 && std::memcmp(&elf[reladyn_off], "APS2", 4) == 0) {
            const uint8_t* aps2 = &elf[reladyn_off];
            size_t pos = 4;
            int64_t num_relocs = readSLEB128(aps2, pos);
            int64_t cur_offset = readSLEB128(aps2, pos);
            int64_t cur_addend = 0;
            int64_t done = 0;
            while (done < num_relocs && pos < reladyn_sz) {
                int64_t group_size = readSLEB128(aps2, pos);
                int64_t group_flags = readSLEB128(aps2, pos);
                int64_t group_off_delta = (group_flags & 2) ? readSLEB128(aps2, pos) : 0;
                int64_t group_info = (group_flags & 1) ? readSLEB128(aps2, pos) : 0;
                if ((group_flags & 8) && (group_flags & 4)) cur_addend += readSLEB128(aps2, pos);
                else if (!(group_flags & 8)) cur_addend = 0;
                for (int64_t g = 0; g < group_size; ++g) {
                    cur_offset += (group_flags & 2) ? group_off_delta : readSLEB128(aps2, pos);
                    uint64_t r_info = (group_flags & 1) ? static_cast<uint64_t>(group_info)
                                                        : static_cast<uint64_t>(readSLEB128(aps2, pos));
                    if ((group_flags & 8) && !(group_flags & 4)) cur_addend += readSLEB128(aps2, pos);
                    uint32_t sym_idx = static_cast<uint32_t>(r_info >> 32);
                    uint32_t r_type = static_cast<uint32_t>(r_info & 0xffffffffU);
                    uint64_t* target = reinterpret_cast<uint64_t*>(base + cur_offset);
                    if (r_type == 1027) {
                        *target = reinterpret_cast<uint64_t>(base) + static_cast<uint64_t>(cur_addend);
                    } else if (r_type == 1031) {
                        target[0] = HLE_TLSDESC_MAGIC;
                        target[1] = static_cast<uint64_t>(cur_addend);
                    } else if (dynsym_off != 0) {
                        const uint8_t* s = &elf[dynsym_off + sym_idx * 24];
                        uint16_t shndx = *reinterpret_cast<const uint16_t*>(s + 6);
                        uint64_t sval = *reinterpret_cast<const uint64_t*>(s + 8);
                        if (shndx != 0 && sval != 0) {
                            *target = reinterpret_cast<uint64_t>(base) + sval + cur_addend;
                        } else if (shndx == 0 && (r_type == 1025 || r_type == 257)) {
                            *target = ext_glob_stub_va;
                        }
                    }
                    done++;
                }
            }
        }
        return true;
    }

    std::vector<std::pair<const uint8_t*, uint32_t>> findEmbeddedMBS2Blobs() const {
        std::vector<std::pair<const uint8_t*, uint32_t>> blobs;
        for (size_t off = 0; off + 32 < elf.size(); off += 4) {
            if (elf[off] == 'M' && elf[off + 1] == 'B' && elf[off + 2] == 'S' && elf[off + 3] == '2') {
                uint32_t payload_len = *reinterpret_cast<const uint32_t*>(&elf[off + 4]);
                uint32_t ver = *reinterpret_cast<const uint32_t*>(&elf[off + 8]);
                if (ver >= 1 && ver <= 100 && payload_len >= 32 && payload_len <= 16384 &&
                    off + 8 + payload_len <= elf.size() &&
                    std::memcmp(&elf[off + 12], "VEHW", 4) == 0) {
                    blobs.emplace_back(base + off, payload_len + 8);
                }
            }
        }
        return blobs;
    }
};

struct MaliAotGlobalRuntime {
    std::mutex           mtx;
    bool                 initialized = false;
    MaliIcdImageMapper   mapper;
    AOTCPUState          cpu{};
    uint8_t*             stack_mem = nullptr;
    size_t               stack_size = 8 * 1024 * 1024;
    uint8_t*             tls_mem = nullptr;
    uint64_t             hwvulkan_device_va = 0;
    uint32_t             aot_queried_api_version = (1U << 22) | (3U << 12) | 250U;
    uint8_t              template_mbs2[256]{};
    uint32_t             template_mbs2_len = 0;
    uint32_t             shader_modules_created = 0;
    uint32_t             mbs2_deserializations_run = 0;
    uint32_t             lifted_vk_calls_executed = 0;

    uint64_t lookupDispatchEntryRva(const char* name) const {
        if (!name || !mapper.base) return 0;
        uint64_t tbl_rva = aot_get_vk_dispatch_table_rva();
        uint64_t cnt     = aot_get_vk_dispatch_table_count();
        uint64_t stride  = aot_get_vk_dispatch_table_stride();
        if (!tbl_rva || !cnt || !stride || tbl_rva + cnt * stride > mapper.image_size) return 0;
        size_t lo = 0, hi = static_cast<size_t>(cnt);
        while (lo < hi) {
            size_t mid = lo + ((hi - lo) >> 1);
            uint64_t epos = tbl_rva + mid * stride;
            uint64_t s_va = *reinterpret_cast<const uint64_t*>(mapper.base + epos);
            if (s_va < IMAGE_BASE || s_va >= IMAGE_BASE + mapper.image_size) break;
            const char* ename = reinterpret_cast<const char*>(s_va);
            int cmp = std::strcmp(name, ename);
            if (cmp == 0) {
                uint64_t fn_off = (stride >= 48) ? 40ULL : 32ULL;
                uint64_t fn_va = *reinterpret_cast<const uint64_t*>(mapper.base + epos + fn_off);
                return (fn_va >= IMAGE_BASE) ? (fn_va - IMAGE_BASE) : fn_va;
            }
            if (cmp < 0) hi = mid;
            else lo = mid + 1;
        }
        return 0;
    }

    void resetGuestStackLocked() {
        if (stack_mem) {
            cpu.sp    = reinterpret_cast<uint64_t>(stack_mem + stack_size - 0x10000);
            cpu.x[29] = cpu.sp;
        }
    }

    uint64_t invokeLiftedVkByRva(uint64_t rva,
                                 uint64_t x0 = 0, uint64_t x1 = 0,
                                 uint64_t x2 = 0, uint64_t x3 = 0,
                                 uint64_t x4 = 0, uint64_t x5 = 0,
                                 uint64_t x6 = 0, uint64_t x7 = 0) {
        if (!rva) return static_cast<uint64_t>(-3LL);
        std::lock_guard<std::mutex> lock(mtx);
        resetGuestStackLocked();
        cpu.x[0] = x0; cpu.x[1] = x1; cpu.x[2] = x2; cpu.x[3] = x3;
        cpu.x[4] = x4; cpu.x[5] = x5; cpu.x[6] = x6; cpu.x[7] = x7;
        lifted_vk_calls_executed++;
        aot_indirect_dispatch(&cpu, IMAGE_BASE + rva);
        return cpu.x[0];
    }

    uint64_t invokeLiftedVkDirect(const char* name,
                                  uint64_t x0 = 0, uint64_t x1 = 0,
                                  uint64_t x2 = 0, uint64_t x3 = 0,
                                  uint64_t x4 = 0, uint64_t x5 = 0,
                                  uint64_t x6 = 0, uint64_t x7 = 0) {
        const auto* e = lookupMaliLiftedVkDispatch(name);
        if (e && e->aot_fn) {
            std::lock_guard<std::mutex> lock(mtx);
            resetGuestStackLocked();
            cpu.x[0] = x0; cpu.x[1] = x1; cpu.x[2] = x2; cpu.x[3] = x3;
            cpu.x[4] = x4; cpu.x[5] = x5; cpu.x[6] = x6; cpu.x[7] = x7;
            lifted_vk_calls_executed++;
            e->aot_fn(&cpu);
            return cpu.x[0];
        }
        return invokeLiftedVkByName(name, x0, x1, x2, x3, x4, x5, x6, x7);
    }

    uint64_t invokeLiftedVkByName(const char* name,
                                  uint64_t x0 = 0, uint64_t x1 = 0,
                                  uint64_t x2 = 0, uint64_t x3 = 0,
                                  uint64_t x4 = 0, uint64_t x5 = 0,
                                  uint64_t x6 = 0, uint64_t x7 = 0) {
        uint64_t rva = lookupDispatchEntryRva(name);
        if (!rva) return static_cast<uint64_t>(-3LL);
        return invokeLiftedVkByRva(rva, x0, x1, x2, x3, x4, x5, x6, x7);
    }

    bool ensureInitialized() {
        std::lock_guard<std::mutex> lock(mtx);
        if (initialized) return true;

        const char* env_so = getenv("MALI_AOT_DRIVER_SO");
        std::string icd_dir_so, icd_aot_so, icd_repo_so, icd_repo_up_so, icd_root_r44_so;
        Dl_info dli{};
        if (dladdr(reinterpret_cast<void*>(&mali_aot_icd_onload), &dli) && dli.dli_fname) {
            std::string p(dli.dli_fname);
            size_t sl = p.find_last_of('/');
            if (sl != std::string::npos) {
                std::string d = p.substr(0, sl);
                icd_dir_so      = d + "/ddk/libGLES_mali.so";
                icd_aot_so      = d + "/../ddk/libGLES_mali.so";
                icd_repo_so     = d + "/../../ddk/libGLES_mali.so";
                icd_repo_up_so  = d + "/../../../ddk/libGLES_mali.so";
                icd_root_r44_so = d + "/../../../ddk/libGLES_mali.r44p1.so";
            }
        }
        const char* candidates[] = {
            (env_so && env_so[0]) ? env_so : nullptr,
            "ddk/libGLES_mali.so",
            "libGLES_mali.so",
            "libGLES_mali.r44p1.so",
            "libGLES_mali.r44.so",
            "../libGLES_mali.so",
            "src/aot/libGLES_mali.so",
            "src/aot/driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so",
            icd_dir_so.empty() ? nullptr : icd_dir_so.c_str(),
            icd_aot_so.empty() ? nullptr : icd_aot_so.c_str(),
            icd_repo_so.empty() ? nullptr : icd_repo_so.c_str(),
            icd_repo_up_so.empty() ? nullptr : icd_repo_up_so.c_str(),
            icd_root_r44_so.empty() ? nullptr : icd_root_r44_so.c_str()
        };
        volatile bool mapped_ok = false;
        for (const char* cand : candidates) {
            if (cand && mapper.loadAndRelocate(cand)) {
                mapped_ok = true;
                break;
            }
        }
        if (!mapped_ok) {
            // Standalone fallback when external 45 MB libGLES_mali.so is not present on disk:
            // Map IMAGE_BASE and synthesize a valid MBS2/VEHW header so AOT ICD still runs standalone.
            void* mapped = mmap(reinterpret_cast<void*>(IMAGE_BASE), mapper.image_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (mapped == MAP_FAILED) {
                std::fprintf(stderr, "[MALI_ICD] Failed to map standalone IMAGE_BASE.\n");
                return false;
            }
            mapper.base = static_cast<uint8_t*>(mapped);
            std::memset(mapper.base, 0, mapper.image_size);
            static const uint8_t kSynthMbs2[64] = {
                'M','B','S','2', 56,0,0,0, 2,0,0,0, 'V','E','H','W',
                16,0,0,0, 11,0,8,3, 'C','F','I','N', 8,0,0,0,
                0,0,0,0, 0,0,0,0, 'O','B','J','C', 16,0,0,0,
                0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x01,
                0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
            };
            template_mbs2_len = sizeof(kSynthMbs2);
            std::memcpy(template_mbs2, kSynthMbs2, template_mbs2_len);
        }

        mali_kbase_emu_register_csf_job_hook(&onLiftedCsfJobCallback, nullptr);

        stack_mem = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(0x700000000ULL), stack_size,
                                               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        tls_mem   = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        *reinterpret_cast<uint64_t*>(tls_mem + 0x28) = BIONIC_STACK_GUARD_CANARY;
        cpu.sp        = reinterpret_cast<uint64_t>(stack_mem + stack_size - 0x10000);
        cpu.x[29]     = cpu.sp;
        cpu.tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);

        {
            uint64_t ps_addr = aot_get_kbase_page_shift_addr();
            if (ps_addr >= IMAGE_BASE) {
                *reinterpret_cast<uint32_t*>(ps_addr) = 12U;
            }
            if (mapper.base && mapper.image_size >= 0x03150000ULL) {
                uint8_t* bss_Scratch = mapper.base + mapper.image_size - 0x10000ULL;
                std::memset(bss_Scratch, 0, 0x8000ULL);
                uint64_t ddebug_cfg_va = reinterpret_cast<uint64_t>(bss_Scratch);
                uint64_t gpu_tbl_va    = reinterpret_cast<uint64_t>(bss_Scratch + 0x2000ULL);
                uint64_t gpu_entry_va  = reinterpret_cast<uint64_t>(bss_Scratch + 0x2100ULL);
                *reinterpret_cast<uint32_t*>(gpu_entry_va + 144) = 2U; // Valhall arch version
                for (uint64_t i = 0; i < 8; ++i) {
                    reinterpret_cast<uint64_t*>(gpu_tbl_va)[i] = gpu_entry_va;
                }
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d170ULL) = 8ULL;
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d178ULL) = gpu_tbl_va;
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d190ULL) = gpu_entry_va;
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d1c8ULL) = ddebug_cfg_va;
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d1f0ULL) = ddebug_cfg_va;
                *reinterpret_cast<uint64_t*>(mapper.base + 0x0314d9d0ULL) = gpu_entry_va;
            }
        }

        // Extract an embedded MBS2 template from the driver's .rodata for CMPBE OBJC packaging
        auto blobs = mapper.findEmbeddedMBS2Blobs();
        if (!blobs.empty()) {
            template_mbs2_len = std::min<uint32_t>(sizeof(template_mbs2), blobs[0].second);
            std::memcpy(template_mbs2, blobs[0].first, template_mbs2_len);
        }

        // 0. Execute lifted Bionic DT_INIT_ARRAY (.init_array) constructors
        const char* skip_ia = getenv("MALI_AOT_SKIP_INIT_ARRAY");
        if (mapped_ok && (!skip_ia || skip_ia[0] == '0')) {
            static sigjmp_buf s_ia_jmp;
            static volatile sig_atomic_t s_ia_guard = 0;
            struct sigaction sa_new{}, sa_old{};
            sa_new.sa_handler = [](int) {
                if (s_ia_guard) siglongjmp(s_ia_jmp, 1);
            };
            sigemptyset(&sa_new.sa_mask);
            sigaction(SIGSEGV, &sa_new, &sa_old);

            volatile uint64_t ia_count = aot_get_init_array_count();
            const char* lim_env = getenv("MALI_AOT_INIT_ARRAY_LIMIT");
            if (lim_env && lim_env[0]) {
                uint64_t lim = std::strtoull(lim_env, nullptr, 10);
                if (lim < ia_count) ia_count = lim;
            }
            const bool trace_ia = (getenv("MALI_AOT_TRACE_INIT_ARRAY") != nullptr);
            volatile uint64_t ia_ok = 0, ia_skipped = 0;
            for (volatile uint64_t i = 0; i < ia_count; ++i) {
                resetGuestStackLocked();
                volatile uint32_t saved_so_depth = aot_so_get_stack_depth();
                s_ia_guard = 1;
                if (sigsetjmp(s_ia_jmp, 1) == 0) {
                    aot_run_dt_init_array_entry(&cpu, i);
                    s_ia_guard = 0;
                    ia_ok++;
                } else {
                    s_ia_guard = 0;
                    aot_so_unwind_to_depth(saved_so_depth);
                    ia_skipped++;
                    if (trace_ia) {
                        std::fprintf(stderr, "[MALI_AOT_ICD] DT_INIT_ARRAY[%llu/%llu] skipped (unresolved Android GOT/TLS)\n",
                                     (unsigned long long)i, (unsigned long long)ia_count);
                    }
                }
            }
            sigaction(SIGSEGV, &sa_old, nullptr);
            if (trace_ia || getenv("MALI_AOT_ICD_LOG")) {
                std::fprintf(stderr, "[MALI_AOT_ICD] DT_INIT_ARRAY complete: %llu/%llu succeeded, %llu skipped\n",
                             (unsigned long long)ia_ok, (unsigned long long)ia_count, (unsigned long long)ia_skipped);
            }
        }

        // 1. Open Android Vulkan HAL ("vk0") via AOT-translated vk_hal_open (when ELF image is mapped)
        if (mapped_ok && aot_vk_hal_open) {
            uint64_t hmi_va = aot_get_vk_hmi_addr();
            const char* vk0_str = "vk0";
            uint64_t out_dev_va = 0;
            cpu.x[0] = hmi_va;
            cpu.x[1] = reinterpret_cast<uint64_t>(vk0_str);
            cpu.x[2] = reinterpret_cast<uint64_t>(&out_dev_va);
            aot_vk_hal_open(&cpu);
            hwvulkan_device_va = out_dev_va;
        }

        // 2. Query instance version via AOT-translated vkEnumerateInstanceVersion
        if (mapped_ok && aot_vkEnumerateInstanceVersion) {
            uint32_t ver = 0;
            cpu.x[0] = reinterpret_cast<uint64_t>(&ver);
            aot_vkEnumerateInstanceVersion(&cpu);
            if (cpu.x[0] == 0 && ver != 0) {
                aot_queried_api_version = ver;
            }
        }

        // 3. Query /dev/mali0 GPU properties via AOT-translated mali_gpu_props_get
        if (mapped_ok && aot_mali_gpu_props_get) {
            uint64_t kctx_va = IMAGE_BASE + 0x03fff000ULL;
            *reinterpret_cast<uint32_t*>(kctx_va) = 100U;
            uint8_t out_props[512]{};
            cpu.x[0] = kctx_va;
            cpu.x[1] = reinterpret_cast<uint64_t>(out_props);
            aot_mali_gpu_props_get(&cpu);
        }

        // 3b. Initialize libGLES_mali.so .bss global config pointer at IMAGE_BASE + 0x31462a0
        // (read by vkEnumerateDeviceExtensionProperties @ 0x977140 and vkGetPhysicalDeviceFeatures2 @ 0x97ad30)
        if (mapper.base) {
            uint8_t* cfg_blob = mapper.base + 0x03fd0000ULL;
            // In libGLES_mali.so (r54p3):
            //   - cfg_blob[704] (0x2c0) is read by vkGetPhysicalDeviceProperties @ 0x9778d4 for
            //     framebufferNoAttachmentsSampleCounts AND bit 3 (0x08) is tested by tbz/tbnz
            //     in vkEnumerateDeviceExtensionProperties @ 0x977140.
            //   - cfg_blob[7368] (0x1cc8) is read by vkGetPhysicalDeviceProperties @ 0x9778cc for
            //     framebufferColorSampleCounts, framebufferDepthSampleCounts, framebufferStencilSampleCounts,
            //     and duplicated across v0.4s into sampledImage{Color,Integer,Depth,Stencil}SampleCounts!
            // Setting 0x0FU (VK_SAMPLE_COUNT_1_BIT | 2_BIT | 4_BIT | 8_BIT) keeps bit 3 (0x08) set while
            // advertising 1x, 2x, 4x, and 8x sample counts directly from lifted aot_vkGetPhysicalDeviceProperties!
            cfg_blob[704]  = 0x0FU;
            cfg_blob[7368] = 0x0FU;
            *reinterpret_cast<uint64_t*>(mapper.base + 0x031462a0ULL) = reinterpret_cast<uint64_t>(cfg_blob);
        }

        // Mark initialized = true BEFORE probing Host Vulkan so any re-entrant loader probe
        // into libvulkan_mali_aot_icd.so returns immediately instead of deadlocking g_aot_rt.mtx!
        initialized = true;

        // 4. Initialize Host SPIR-V executor for CSF compute jobs
        g_icd_spirv_executor.init();
        return true;
    }
};

static MaliAotGlobalRuntime g_aot_rt;

// ============================================================================
// Section 4: Vulkan C ABI Types & ICD Dispatchable Objects
// ============================================================================
// Standard Vulkan C ABI structs are provided by <vulkan/vulkan.h>.
static_assert(offsetof(VkPhysicalDeviceProperties, limits) == 296,
              "VkPhysicalDeviceProperties::limits must be at byte offset 296 (matching Vulkan C ABI)");
static_assert(sizeof(VkPhysicalDeviceProperties) == 824,
              "VkPhysicalDeviceProperties must be 824 bytes (matching Vulkan C ABI)");

// Dispatchable ICD Handles (MUST start with uintptr_t loader_magic = 0x01CDC0DE)
// Note: In libGLES_mali.so (r54p3), aot_vkGetPhysicalDeviceProperties (0x977730) and
// aot_vkGetPhysicalDeviceFeatures2 (0x97ad30) add +8 to x0 before reading the internal
// physical-device structure, whereas aot_vkGetPhysicalDeviceMemoryProperties (0x978fe0),
// aot_vkGetPhysicalDeviceQueueFamilyProperties (0x97ab30), and
// aot_vkEnumerateDeviceExtensionProperties (0x977140) read directly from x0.
struct MaliIcdPhysicalDevice {
    uintptr_t loader_magic = ICD_LOADER_MAGIC;
    void*     parent_instance = nullptr;
    uint8_t   aot_internal[2048]{};
    uint8_t   aot_inst_internal[512]{};
    bool      aot_layout_initialized = false;

    void initAotInternalLayout() {
        if (aot_layout_initialized) return;
        std::memset(aot_internal, 0, sizeof(aot_internal));
        std::memset(aot_inst_internal, 0, sizeof(aot_inst_internal));

        uint32_t gpu_product_id = MALI_PROFILE_G615_CSF;
        {
            int mfd = mali_kbase_emu_open_direct("/dev/mali0", 2 /* O_RDWR */);
            if (mfd >= 0) {
                uint8_t tlv[2048]{};
                struct { uint64_t buffer; uint32_t size; uint32_t flags; } gp{reinterpret_cast<uint64_t>(tlv), sizeof(tlv), 0};
                int sz = mali_kbase_emu_ioctl_direct(mfd, 0xc0108003UL /* KBASE_IOCTL_GET_GPUPROPS */, &gp);
                if (sz > 0 && sz <= static_cast<int>(sizeof(tlv))) {
                    size_t off = 0;
                    while (off + 4 <= static_cast<size_t>(sz)) {
                        uint32_t hdr = 0;
                        std::memcpy(&hdr, tlv + off, 4);
                        off += 4;
                        uint32_t prop_id = hdr >> 2;
                        uint32_t vlen = 1U << (hdr & 3U);
                        if (off + vlen > static_cast<size_t>(sz)) break;
                        if (prop_id == 1U /* PRODUCT_ID */ && vlen == 4U) {
                            std::memcpy(&gpu_product_id, tlv + off, 4);
                            break;
                        }
                        off += vlen;
                    }
                }
                mali_kbase_emu_close_direct(mfd);
            }
        }
        uint64_t raw_gpu_id = (static_cast<uint64_t>(gpu_product_id) << 16) | 1ULL;
        const char* gpu_model = "Arm Mali-G615";
        if (gpu_product_id == MALI_PROFILE_G76_JM) gpu_model = "Arm Mali-G76";
        else if (gpu_product_id == MALI_PROFILE_G78_JM) gpu_model = "Arm Mali-G78";
        else if (gpu_product_id == MALI_PROFILE_G925_CSF) gpu_model = "Arm Immortalis-G925";

        // Populate fields relative to aot_internal (x0) so both direct-x0 and (x0+8) AOT functions succeed:
        // Direct-x0 functions (MemoryProperties @ 0x978fe0, QueueFamilyProperties @ 0x97ab30, EnumerateDeviceExtensionProperties @ 0x977140):
        aot_internal[13] = 1U; // sparse binding support -> queueFlags = 0x17
        *reinterpret_cast<uint32_t*>(aot_internal + 48) = 3U; // memoryTypeCount = 3
        *reinterpret_cast<uint32_t*>(aot_internal + 52) = 1U; // memoryHeapCount = 1
        *reinterpret_cast<uint64_t*>(aot_internal + 64) = reinterpret_cast<uint64_t>(aot_inst_internal);
        // 3 Mali memoryTypes at +152 + i*12:
        //   [0]: DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT (0x07)
        //   [1]: DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED   (0x0B)
        //   [2]: DEVICE_LOCAL | LAZILY_ALLOCATED             (0x11)
        *reinterpret_cast<uint32_t*>(aot_internal + 160 + 0 * 12) = 0x07U;
        *reinterpret_cast<uint32_t*>(aot_internal + 160 + 1 * 12) = 0x0FU;
        *reinterpret_cast<uint32_t*>(aot_internal + 160 + 2 * 12) = 0x11U;
        // 1 MemoryHeap at +712: 8 GiB DEVICE_LOCAL
        *reinterpret_cast<uint64_t*>(aot_internal + 712) = (8ULL << 30);
        *reinterpret_cast<uint32_t*>(aot_internal + 720) = 1U; // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
        *reinterpret_cast<uint32_t*>(aot_internal + 0x2e0) = 4096U; // buffer alignment in aot_vkGetBufferMemoryRequirements (0x00939670)
        *reinterpret_cast<uint32_t*>(aot_internal + 752) = 0xFFFFFFFFU; // hw extension bitmask + timestampValidBits = 64

        // (x0 + 8) shifted view for aot_vkGetPhysicalDeviceProperties (0x977730 -> 0x977738)
        // We store a second dedicated block at aot_internal + 1024 where base = aot_internal + 1024 + 8:
        uint8_t* pbase = aot_internal + 1024 + 8;
        *reinterpret_cast<uint32_t*>(pbase + 8)  = static_cast<uint32_t>(raw_gpu_id); // deviceID
        *reinterpret_cast<uint32_t*>(pbase + 12) = 0x13B5U; // vendorID (Arm Ltd.)
        *reinterpret_cast<uint32_t*>(pbase + 20) = 65536U;  // maxUniformBufferRange
        *reinterpret_cast<uint32_t*>(pbase + 48) = (54U << 22) | (3U << 12); // driverVersion r54p3
        const uint8_t uuid[16] = {'M','A','L','I','-','G','6','1','5','-','R','5','4','P','3','0'};
        std::memcpy(pbase + 96, uuid, 16);
        // VkPipelineCacheHeaderVersionOne at pbase + 0x70..+0x8f (read by aot_vkCreatePipelineCache / aot_vkGetPipelineCacheData)
        *reinterpret_cast<uint32_t*>(pbase + 0x70) = 32U; // headerSize
        *reinterpret_cast<uint32_t*>(pbase + 0x74) = 1U;  // VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        *reinterpret_cast<uint32_t*>(pbase + 0x78) = 0x13B5U; // vendorID
        *reinterpret_cast<uint32_t*>(pbase + 0x7c) = static_cast<uint32_t>(raw_gpu_id); // deviceID
        std::memcpy(pbase + 0x80, uuid, 16);
        std::snprintf(reinterpret_cast<char*>(pbase + 192), 256,
                      "%s (AOT %s via mali_kbase_emu)",
                      gpu_model,
                      g_aot_rt.mapper.driver_version.c_str());
        pbase[745] = 0x80U; // timestampComputeAndGraphics = VK_TRUE
        *reinterpret_cast<float*>(pbase + 748)    = 1000.0f; // timestampPeriod = 1.0 ns
        *reinterpret_cast<uint32_t*>(pbase + 752) = 0xFFFFFFFFU;
        aot_layout_initialized = true;
    }
};

struct MaliIcdInstance {
    uintptr_t             loader_magic = ICD_LOADER_MAGIC;
    MaliIcdPhysicalDevice phys_dev{};
};

struct MaliIcdQueue {
    uintptr_t loader_magic = ICD_LOADER_MAGIC;
    void*     parent_device = nullptr;
};

struct MaliIcdDevice {
    uintptr_t            loader_magic = ICD_LOADER_MAGIC;
    MaliIcdPhysicalDevice* phys_dev = nullptr;
    MaliIcdQueue         queue{};
    int32_t              mali_fd = 100;
    std::vector<uint8_t> kctx_mem;
    std::vector<uint8_t> aot_cctx_mem;
    std::vector<uint8_t> aot_vk_device_mem;
    std::vector<uint8_t> csg_group_struct;
    std::vector<uint8_t> csi_queue_struct;
    uint64_t             ring_bo_va     = 0;
    uint64_t             doorbell_va    = 0;
    uint64_t             user_input_va  = 0;
    uint64_t             user_output_va = 0;
    uint64_t             ring_write_off = 0;
    uint64_t             cqs_bo_va      = 0;
    uint64_t             srt_bo_va      = 0;
    uint64_t             spd_bo_va      = 0;
    uint64_t             rsd_bo_va      = 0;
    uint64_t             tsd_bo_va      = 0;
    uint64_t             tiler_heap_va  = 0;
    uint8_t              kcpu_queue_id  = 0;
};

struct MaliIcdDeviceMemory {
    uint64_t gpu_va     = 0;
    uint64_t size_bytes = 0;
    uint64_t pages      = 0;
};

struct MaliIcdBuffer {
    uint64_t hdr_meta     = 9;     // +0x00: libGLES_mali.so object type = 9 (VkBuffer)
    uint64_t dev_plus_8   = 0;     // +0x08: device + 8
    uint64_t usage        = 0;     // +0x10: 64-bit usage (VkBufferCreateInfo::usage or VkBufferUsageFlags2CreateInfoKHR)
    uint32_t create_flags = 0;     // +0x18: VkBufferCreateFlags
    uint32_t ext_handles  = 0;     // +0x1c: VkExternalMemoryHandleTypeFlags
    uint64_t bound_gpu_va = 0;     // +0x20: GPU VA (read by aot_vkGetBufferDeviceAddress @ 0x009398c0)
    uint64_t size_bytes   = 0;     // +0x28: size in bytes (read by aot_vkGetBufferMemoryRequirements @ 0x00939670)
    uint8_t  is_sparse    = 0;     // +0x30: (flags >> 3) & 1 (read by aot_vkGetBufferMemoryRequirements @ 0x00939670)
    uint8_t  pad31[7]{};           // +0x31..0x37
    uint64_t size         = 0;     // +0x38: alias of size_bytes
    uint64_t reserved40   = 0;     // +0x40
};
static_assert(offsetof(MaliIcdBuffer, bound_gpu_va) == 0x20,
              "MaliIcdBuffer::bound_gpu_va must be at byte offset 0x20 (matching libGLES_mali.so)");
static_assert(offsetof(MaliIcdBuffer, size_bytes) == 0x28,
              "MaliIcdBuffer::size_bytes must be at byte offset 0x28 (matching libGLES_mali.so)");
static_assert(sizeof(MaliIcdBuffer) == 0x48,
              "MaliIcdBuffer must be 0x48 bytes (matching libGLES_mali.so vkCreateBuffer @ 0x009392f0)");

struct MaliIcdShaderModule {
    std::vector<uint32_t> spirv_words;
    std::vector<uint64_t> objc_words;
    uint64_t              aot_shader_module_handle = 0;
    uint64_t              exec_bo_gpu_va           = 0;
    uint8_t               sha1_digest[20]{};
    std::string           deserialized_c_source;
    uint32_t              execution_model          = 0; // 0=Vertex, 4=Fragment, 5=GLCompute
    bool                  has_image_sample         = false;
    bool                  has_ubo_load             = false;
    bool                  has_push_constant        = false;
    bool                  has_const_color          = false;
    bool                  mbs2_compiled            = false;
    float                 const_color[4]           = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct MaliVertexBindingDesc {
    uint32_t binding    = 0;
    uint32_t stride     = 0;
    uint32_t input_rate = 0;
};

struct MaliVertexAttrDesc {
    uint32_t location = 0;
    uint32_t binding  = 0;
    uint32_t format   = 0;
    uint32_t offset   = 0;
};

struct MaliIcdPipeline {
    MaliIcdShaderModule*           shader_mod     = nullptr;
    MaliIcdShaderModule*           vs_mod         = nullptr;
    MaliIcdShaderModule*           fs_mod         = nullptr;
    uint64_t                       exec_bo_gpu_va = 0;
    uint64_t                       fs_exec_bo_va  = 0;
    size_t                         objc_bytes     = 0;
    size_t                         fs_objc_bytes  = 0;
    bool                           is_compute     = true;
    uint64_t                       aot_pipeline_handle = 0;
    uint32_t                       vertex_stride  = 0;
    uint32_t                       topology       = 3; // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    uint32_t                       cull_mode      = 0;
    uint32_t                       front_face     = 0;
    bool                           depth_test     = true;
    bool                           depth_write    = true;
    uint32_t                       depth_compare  = 1; // VK_COMPARE_OP_LESS
    bool                           blend_enable   = false;
    std::vector<MaliVertexBindingDesc> vertex_bindings;
    std::vector<MaliVertexAttrDesc>    vertex_attrs;
};

struct MaliIcdDescriptorSetLayoutBinding {
    uint32_t binding         = 0;
    uint32_t descriptor_type = 0;
    uint32_t count           = 1;
    uint32_t stage_flags     = 0;
};

struct MaliIcdDescriptorSetLayout {
    std::vector<uint8_t>                          aot_handle_mem;
    std::vector<MaliIcdDescriptorSetLayoutBinding> bindings;
};

struct MaliIcdPipelineLayout {
    std::vector<uint8_t>  aot_handle_mem;
    std::vector<uint64_t> set_layouts;
    uint32_t              push_constant_size = 128;
};

struct MaliIcdDescriptorPool {
    uint32_t max_sets   = 4096;
    uint64_t aot_handle = 0;
};

struct MaliIcdCommandPool {
    uint32_t queue_family_index = 0;
    uint32_t flags              = 0;
    uint64_t aot_handle         = 0;
};

struct MaliIcdFence {
    std::vector<uint8_t> aot_fence_mem;
    bool                 signaled = false;
};

struct MaliIcdSemaphore {
    bool     is_timeline   = false;
    uint64_t counter_value = 0;
};

struct MaliIcdEvent {
    bool signaled = false;
};

struct MaliIcdPipelineCache {
    std::vector<uint8_t> cache_data;
};

struct MaliIcdImage {
    uint32_t             width = 64, height = 64, depth = 1, mip_levels = 1, array_layers = 1, format = 37;
    uint64_t             bound_gpu_va = 0;
    uint64_t             byte_size = 16384;
    uint64_t             aot_image_handle = 0;
    bool                 has_uploaded_data = false;
    std::vector<uint8_t> stub_texture_backing;
};

struct MaliIcdImageView {
    MaliIcdImage* image = nullptr;
    uint32_t      format = 37;
};

struct MaliIcdSampler {
    float    max_anisotropy = 16.0f;
    float    max_lod        = 15.0f;
    uint64_t aot_handle     = 0;
};

struct MaliIcdRenderPass {
    uint32_t attachment_count = 1;
    uint32_t color_load_op    = 0;
    uint32_t depth_load_op    = 0;
};

struct MaliIcdFramebuffer {
    uint32_t           width = 64, height = 64;
    MaliMfbdDescriptor hw_mfbd{};
    std::vector<float> depth_buffer;
    bool               cleared_in_rp = false;
};

struct MaliIcdQueryPool {
    uint32_t              query_count = 8;
    std::vector<uint64_t> results;
};

struct MaliIcdDescriptorSet {
    uint64_t          bound_buffer_va[64]{};
    uint64_t          bound_buffer_range[64]{};
    uint64_t          bound_buffer_handle[64]{};
    uint64_t          bound_buffer_offset[64]{};
    MaliIcdImageView* bound_image_view[64]{};
    MaliIcdSampler*   bound_sampler[64]{};
};

struct MaliIcdRecordedCmd {
    enum Type {
        CMD_DISPATCH = 0,
        CMD_DRAW = 1,
        CMD_DRAW_INDEXED = 2,
        CMD_COPY_BUFFER = 3,
        CMD_COPY_BUFFER_TO_IMAGE = 4,
        CMD_BLIT_IMAGE = 5,
        CMD_CLEAR_COLOR_IMAGE = 6,
        CMD_CLEAR_ATTACHMENTS = 7,
        CMD_WRITE_TIMESTAMP = 8,
        CMD_PIPELINE_BARRIER = 9
    } type;
    uint32_t              rp_draw_idx = 0;
    MaliIcdPipeline*      pipeline = nullptr;
    MaliIcdDescriptorSet* desc_set = nullptr;
    MaliIcdFramebuffer*   framebuffer = nullptr;
    uint32_t              dynamic_offset = 0;
    uint32_t              wg_x = 1, wg_y = 1, wg_z = 1;
    uint32_t              index_count = 0, instance_count = 1;
    uint32_t              first_vertex = 0;
    uint32_t              first_index = 0;
    int32_t               vertex_offset = 0;
    uint32_t              first_instance = 0;
    uint64_t              vertex_bo_va = 0;
    uint64_t              vertex_bo_size = 0;
    uint64_t              vertex_bo_vas[8]{};
    uint64_t              vertex_bo_sizes[8]{};
    uint64_t              index_bo_va = 0;
    uint64_t              index_bo_size = 0;
    uint32_t              index_type = 1; // 0 = UINT16, 1 = UINT32
    uint8_t               push_constants[256]{};
    uint64_t              src_va = 0, dst_va = 0, copy_bytes = 0;
    MaliIcdImage*         src_image = nullptr;
    MaliIcdImage*         dst_image = nullptr;
    uint32_t              clear_rgba8 = 0xFF000000U;
    MaliIcdQueryPool*     query_pool = nullptr;
    uint32_t              query_index = 0;
};

struct MaliIcdCommandBuffer {
    uintptr_t                        loader_magic = ICD_LOADER_MAGIC;
    MaliIcdDevice*                   device = nullptr;
    std::vector<uint8_t>             aot_cmdbuf_mem;
    MaliIcdPipeline*                 current_pipeline = nullptr;
    MaliIcdDescriptorSet*            current_desc_set = nullptr;
    MaliIcdDescriptorSet*            bound_desc_sets[4]{};
    MaliIcdFramebuffer*              active_fb = nullptr;
    std::unique_ptr<MaliIcdFramebuffer> dynamic_rendering_fb;
    uint64_t                         bound_vertex_bo_va = 0;
    uint64_t                         bound_vertex_bo_size = 0;
    uint64_t                         bound_vertex_bo_vas[8]{};
    uint64_t                         bound_vertex_bo_sizes[8]{};
    uint64_t                         bound_index_bo_va  = 0;
    uint64_t                         bound_index_bo_size = 0;
    uint32_t                         bound_index_type   = 1;
    uint32_t                         dynamic_ubo_offset = 0;
    uint8_t                          push_constants[256]{};
    float                            blend_constants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float                            line_width         = 1.0f;
    float                            depth_bias_const   = 0.0f;
    float                            depth_bias_clamp   = 0.0f;
    float                            depth_bias_slope   = 0.0f;
    float                            min_depth_bounds   = 0.0f;
    float                            max_depth_bounds   = 1.0f;
    uint32_t                         stencil_comp_mask  = 0xFFFFFFFFU;
    uint32_t                         stencil_write_mask = 0xFFFFFFFFU;
    uint32_t                         stencil_ref        = 0;
    bool                             conditional_active = true;
    uint32_t                         rp_draw_counter    = 0;
    std::vector<MaliIcdRecordedCmd>  cmds;
};

// ============================================================================
// Global ICD Resource Tracking (Buffers & Images)
// ============================================================================
struct IcdBufferRecord {
    uint64_t bound_gpu_va = 0;
    uint64_t size         = 0;
};

struct IcdImageRecord {
    uint64_t bound_gpu_va = 0;
    uint32_t width        = 0;
    uint32_t height       = 0;
};

static inline bool is_probable_icd_ptr(uint64_t ptr) {
    return (ptr >= 0x10000ULL && ptr <= 0x00007fffffffffffULL && (ptr & 0x7ULL) == 0);
}

struct IcdBufferMap : public std::unordered_map<uint64_t, IcdBufferRecord> {
    iterator find(uint64_t handle) {
        auto it = std::unordered_map<uint64_t, IcdBufferRecord>::find(handle);
        if (it != end()) return it;
        if (is_probable_icd_ptr(handle)) {
            const auto* buf = reinterpret_cast<const MaliIcdBuffer*>(handle);
            (*this)[handle] = {buf->bound_gpu_va, buf->size_bytes ? buf->size_bytes : buf->size};
            return std::unordered_map<uint64_t, IcdBufferRecord>::find(handle);
        }
        return end();
    }
    const_iterator find(uint64_t handle) const {
        return std::unordered_map<uint64_t, IcdBufferRecord>::find(handle);
    }
};

struct IcdImageMap : public std::unordered_map<uint64_t, IcdImageRecord> {
    iterator find(uint64_t handle) {
        auto it = std::unordered_map<uint64_t, IcdImageRecord>::find(handle);
        if (it != end()) return it;
        if (is_probable_icd_ptr(handle)) {
            const auto* img = reinterpret_cast<const MaliIcdImage*>(handle);
            (*this)[handle] = {img->bound_gpu_va, img->width, img->height};
            return std::unordered_map<uint64_t, IcdImageRecord>::find(handle);
        }
        return end();
    }
    const_iterator find(uint64_t handle) const {
        return std::unordered_map<uint64_t, IcdImageRecord>::find(handle);
    }
};

static std::mutex   g_icd_mutex;
static IcdBufferMap g_buffers;
static IcdImageMap  g_images;

// Helper: Allocate a /dev/mali0 BO via AOT aot_kbase_mem_alloc or ioctl
static uint64_t icdAllocKbaseBo(MaliIcdDevice* dev, uint64_t pages, uint64_t flags) {
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
