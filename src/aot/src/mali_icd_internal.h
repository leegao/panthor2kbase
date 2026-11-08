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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <dlfcn.h>

#include <cstdarg>
#include <unistd.h>
#include "../include/mali0_emu.hpp"
#include "../generated/mali_bionic_stubs_autogen.h"

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
    __attribute__((weak)) void aot_cmpbe_v2_deserialize_MBS2_to_C(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_init_options(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_set_option_value(AOTCPUState* /*cpu*/) {}
    __attribute__((weak)) void aot_cmpbe_v2_free_compilation_result(AOTCPUState* /*cpu*/) {}
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

static MaliKbaseCsfKernelEmulator g_icd_kbase_emu;
static int g_icd_kbase_ioctl_calls = 0;

extern "C" uint64_t bionic_mali_open_stub(uint64_t path_addr, uint64_t flags) {
    return g_icd_kbase_emu.handleOpen(reinterpret_cast<const char*>(path_addr), flags);
}
extern "C" uint64_t bionic_mali_close_stub(uint64_t fd) {
    return g_icd_kbase_emu.handleClose(fd);
}
extern "C" uint64_t bionic_mali_kbase_ioctl_stub(uint64_t fd, uint64_t cmd, uint64_t arg_addr) {
    g_icd_kbase_ioctl_calls++;
    return g_icd_kbase_emu.handleIoctl(fd, cmd, arg_addr);
}
extern "C" uint64_t bionic_mali_mmap_stub(uint64_t addr, uint64_t length, uint64_t prot,
                                          uint64_t flags, uint64_t fd, uint64_t offset) {
    return g_icd_kbase_emu.handleMmap(addr, length, prot, flags, fd, offset);
}
extern "C" uint64_t bionic_mali_munmap_stub(uint64_t addr, uint64_t length) {
    return g_icd_kbase_emu.handleMunmap(addr, length);
}
extern "C" uint64_t bionic_mali_read_stub(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    return g_icd_kbase_emu.handleRead(fd, buf_addr, count);
}
extern "C" uint64_t bionic_mali_poll_stub(uint64_t fds_addr, uint64_t nfds, uint64_t timeout) {
    return g_icd_kbase_emu.handlePoll(fds_addr, nfds, timeout);
}
extern "C" void __mali_stack_chk_fail_diag(AOTCPUState* cpu) {
    std::fprintf(stderr, "[MALI_ICD_ERROR] Bionic __stack_chk_fail! SP=0x%016lx LR=0x%016lx\n",
                 cpu->sp, cpu->x[30]);
    std::abort();
}
extern "C" void aot_unhandled_plt(AOTCPUState* cpu, uint64_t target_va) {
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

    bool ensureInitialized() {
        std::lock_guard<std::mutex> lock(mtx);
        if (initialized) return true;

        const char* env_so = getenv("MALI_AOT_DRIVER_SO");
        std::string icd_dir_so, icd_root_so, icd_root_r44_so;
        Dl_info dli{};
        if (dladdr(reinterpret_cast<void*>(&mali_aot_icd_onload), &dli) && dli.dli_fname) {
            std::string p(dli.dli_fname);
            size_t sl = p.find_last_of('/');
            if (sl != std::string::npos) {
                std::string d = p.substr(0, sl);
                icd_dir_so      = d + "/libGLES_mali.so";
                icd_root_so     = d + "/../../libGLES_mali.so";
                icd_root_r44_so = d + "/../../../libGLES_mali.r44p1.so";
            }
        }
        const char* candidates[] = {
            (env_so && env_so[0]) ? env_so : nullptr,
            "libGLES_mali.so",
            "libGLES_mali.r44p1.so",
            "libGLES_mali.r44.so",
            "../libGLES_mali.so",
            "src/aot/libGLES_mali.so",
            icd_dir_so.empty() ? nullptr : icd_dir_so.c_str(),
            icd_root_so.empty() ? nullptr : icd_root_so.c_str(),
            icd_root_r44_so.empty() ? nullptr : icd_root_r44_so.c_str(),
            "/working_dir/c_3106ac7496559798/user_uploaded_files/ttl=63d/user_uploaded/libGLES_mali.r44p1.so",
            "/working_dir/c_3106ac7496559798/work/archive/libGLES_mali/libGLES_mali.so",
            "/working_dir/c_24f54338d69d9705/user_uploaded_files/ttl=63d/user_uploaded/libGLES_mali.so",
            "/working_dir/c_a3c1acfa7b15211d/work/libGLES_mali.so"
        };
        bool mapped_ok = false;
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

        // Configure GPU profile in mali0_emu from driver version
        if (mapper.driver_version.find("r26p0") != std::string::npos) {
            g_icd_kbase_emu.setGpuProfile(MALI_PROFILE_G76_JM);
        } else if (mapper.driver_version.find("49d848") != std::string::npos) {
            g_icd_kbase_emu.setGpuProfile(MALI_PROFILE_G78_JM);
        } else if (mapper.driver_version.find("c48934") != std::string::npos) {
            g_icd_kbase_emu.setGpuProfile(MALI_PROFILE_G925_CSF);
        } else {
            g_icd_kbase_emu.setGpuProfile(MALI_PROFILE_G615_CSF);
        }

        stack_mem = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(0x700000000ULL), stack_size,
                                               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        tls_mem   = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        *reinterpret_cast<uint64_t*>(tls_mem + 0x28) = BIONIC_STACK_GUARD_CANARY;
        cpu.sp        = reinterpret_cast<uint64_t>(stack_mem + stack_size - 0x10000);
        cpu.tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);

        if (aot_get_kbase_page_shift_addr) {
            uint64_t ps_addr = aot_get_kbase_page_shift_addr();
            if (ps_addr >= IMAGE_BASE) {
                *reinterpret_cast<uint32_t*>(ps_addr) = 12U;
            }
        }

        // Extract an embedded MBS2 template from the driver's .rodata for CMPBE OBJC packaging
        auto blobs = mapper.findEmbeddedMBS2Blobs();
        if (!blobs.empty()) {
            template_mbs2_len = std::min<uint32_t>(sizeof(template_mbs2), blobs[0].second);
            std::memcpy(template_mbs2, blobs[0].first, template_mbs2_len);
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
            uint8_t* cfg_blob = mapper.base + 0x03ffe000ULL;
            cfg_blob[704]  = 8U; // bit 3: enable full hardware extension & feature table
            cfg_blob[7368] = 8U;
            *reinterpret_cast<uint64_t*>(mapper.base + 0x031462a0ULL) = reinterpret_cast<uint64_t>(cfg_blob);
        }

        // 4. Initialize Host Vulkan (llvmpipe or host GPU) in mali0_emu
        g_icd_kbase_emu.lvp_executor.init();

        initialized = true;
        return true;
    }
};

static MaliAotGlobalRuntime g_aot_rt;

// ============================================================================
// Section 4: Vulkan C ABI Types & ICD Dispatchable Objects
// ============================================================================
struct VkExtensionPropertiesIcd {
    char     extensionName[256];
    uint32_t specVersion;
};

struct VkLayerPropertiesIcd {
    char     layerName[256];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char     description[256];
};

struct VkPhysicalDeviceLimitsIcd {
    uint8_t raw[504]; // Exact size of VkPhysicalDeviceLimits in Vulkan 1.3 (504 bytes, aligned to 8)
};

struct VkPhysicalDeviceSparsePropertiesIcd {
    uint32_t residencyStandard2DBlockShape;
    uint32_t residencyStandard2DMSBlockShape;
    uint32_t residencyStandard3DBlockShape;
    uint32_t residencyAlignedMipSize;
    uint32_t residencyNonResidentStrict;
};

struct VkPhysicalDevicePropertiesIcd {
    uint32_t                            apiVersion;
    uint32_t                            driverVersion;
    uint32_t                            vendorID;
    uint32_t                            deviceID;
    uint32_t                            deviceType;
    char                                deviceName[256];
    uint8_t                             pipelineCacheUUID[16];
    VkPhysicalDeviceLimitsIcd           limits;
    VkPhysicalDeviceSparsePropertiesIcd sparseProperties;
};

struct VkQueueFamilyPropertiesIcd {
    uint32_t queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    uint32_t minImageTransferGranularity[3];
};

struct VkMemoryTypeIcd {
    uint32_t propertyFlags;
    uint32_t heapIndex;
};

struct VkMemoryHeapIcd {
    uint64_t size;
    uint32_t flags;
    uint32_t padding;
};

struct VkPhysicalDeviceMemoryPropertiesIcd {
    uint32_t        memoryTypeCount;
    VkMemoryTypeIcd memoryTypes[32];
    uint32_t        memoryHeapCount;
    VkMemoryHeapIcd memoryHeaps[16];
};

struct VkShaderModuleCreateInfoIcd {
    uint32_t        sType;
    const void*     pNext;
    uint32_t        flags;
    size_t          codeSize;
    const uint32_t* pCode;
};

struct VkBufferCreateInfoIcd {
    uint32_t        sType;
    const void*     pNext;
    uint32_t        flags;
    uint64_t        size;
    uint32_t        usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};

struct VkMemoryRequirementsIcd {
    uint64_t size;
    uint64_t alignment;
    uint32_t memoryTypeBits;
    uint32_t padding;
};

struct VkMemoryAllocateInfoIcd {
    uint32_t    sType;
    const void* pNext;
    uint64_t    allocationSize;
    uint32_t    memoryTypeIndex;
};

struct VkPipelineShaderStageCreateInfoIcd {
    uint32_t    sType;
    const void* pNext;
    uint32_t    flags;
    uint32_t    stage;
    uint64_t    module;
    const char* pName;
    const void* pSpecializationInfo;
};

struct VkComputePipelineCreateInfoIcd {
    uint32_t                           sType;
    const void*                        pNext;
    uint32_t                           flags;
    VkPipelineShaderStageCreateInfoIcd stage;
    uint64_t                           layout;
    uint64_t                           basePipelineHandle;
    int32_t                            basePipelineIndex;
};

struct VkDescriptorBufferInfoIcd {
    uint64_t buffer;
    uint64_t offset;
    uint64_t range;
};

struct VkWriteDescriptorSetIcd {
    uint32_t                         sType;
    const void*                      pNext;
    uint64_t                         dstSet;
    uint32_t                         dstBinding;
    uint32_t                         dstArrayElement;
    uint32_t                         descriptorCount;
    uint32_t                         descriptorType;
    const void*                      pImageInfo;
    const VkDescriptorBufferInfoIcd* pBufferInfo;
    const void*                      pTexelBufferView;
};

struct VkCommandBufferAllocateInfoIcd {
    uint32_t    sType;
    const void* pNext;
    uint64_t    commandPool;
    uint32_t    level;
    uint32_t    commandBufferCount;
};

struct VkSubmitInfoIcd {
    uint32_t        sType;
    const void*     pNext;
    uint32_t        waitSemaphoreCount;
    const uint64_t* pWaitSemaphores;
    const uint32_t* pWaitDstStageMask;
    uint32_t        commandBufferCount;
    void* const*    pCommandBuffers;
    uint32_t        signalSemaphoreCount;
    const uint64_t* pSignalSemaphores;
};

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

    void initAotInternalLayout() {
        std::memset(aot_internal, 0, sizeof(aot_internal));
        std::memset(aot_inst_internal, 0, sizeof(aot_inst_internal));

        const char* gpu_model = "Arm Mali-G615";
        if (g_icd_kbase_emu.gpu_product_id == MALI_PROFILE_G76_JM) gpu_model = "Arm Mali-G76";
        else if (g_icd_kbase_emu.gpu_product_id == MALI_PROFILE_G78_JM) gpu_model = "Arm Mali-G78";
        else if (g_icd_kbase_emu.gpu_product_id == MALI_PROFILE_G925_CSF) gpu_model = "Arm Immortalis-G925";

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
        *reinterpret_cast<uint32_t*>(aot_internal + 752) = 0xFFFFFFFFU; // hw extension bitmask + timestampValidBits = 64

        // (x0 + 8) shifted view for aot_vkGetPhysicalDeviceProperties (0x977730 -> 0x977738)
        // We store a second dedicated block at aot_internal + 1024 where base = aot_internal + 1024 + 8:
        uint8_t* pbase = aot_internal + 1024 + 8;
        *reinterpret_cast<uint32_t*>(pbase + 8)  = static_cast<uint32_t>(g_icd_kbase_emu.raw_gpu_id); // deviceID
        *reinterpret_cast<uint32_t*>(pbase + 12) = 0x13B5U; // vendorID (Arm Ltd.)
        *reinterpret_cast<uint32_t*>(pbase + 20) = 65536U;  // maxUniformBufferRange
        *reinterpret_cast<uint32_t*>(pbase + 48) = (54U << 22) | (3U << 12); // driverVersion r54p3
        const uint8_t uuid[16] = {'M','A','L','I','-','G','6','1','5','-','R','5','4','P','3','0'};
        std::memcpy(pbase + 96, uuid, 16);
        std::snprintf(reinterpret_cast<char*>(pbase + 192), 256,
                      "%s (AOT %s via mali0_emu [%s])",
                      gpu_model,
                      g_aot_rt.mapper.driver_version.c_str(),
                      g_icd_kbase_emu.lvp_executor.device_name.empty() ? "llvmpipe" : g_icd_kbase_emu.lvp_executor.device_name.c_str());
        pbase[745] = 0x80U; // timestampComputeAndGraphics = VK_TRUE
        *reinterpret_cast<float*>(pbase + 748)    = 1000.0f; // timestampPeriod = 1.0 ns
        *reinterpret_cast<uint32_t*>(pbase + 752) = 0xFFFFFFFFU;
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
    std::vector<uint8_t> kctx_mem;
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
};

// Non-dispatchable ICD Handles
struct MaliIcdDeviceMemory {
    uint64_t gpu_va     = 0;
    uint64_t size_bytes = 0;
    uint64_t pages      = 0;
};

struct MaliIcdBuffer {
    uint64_t size_bytes   = 0;
    uint64_t size         = 0;
    uint32_t usage        = 0;
    uint64_t bound_gpu_va = 0;
};

struct MaliIcdShaderModule {
    std::vector<uint32_t> spirv_words;
    std::vector<uint64_t> objc_words;
    uint64_t              aot_shader_module_handle = 0;
    uint64_t              exec_bo_gpu_va           = 0;
    uint8_t               sha1_digest[20]{};
    std::string           deserialized_c_source;
};

struct MaliIcdPipeline {
    MaliIcdShaderModule* shader_mod     = nullptr;
    uint64_t             exec_bo_gpu_va = 0;
    size_t               objc_bytes     = 0;
    bool                 is_compute     = true;
};

struct MaliIcdImage {
    uint32_t             width = 64, height = 64, depth = 1, mip_levels = 1, array_layers = 1, format = 37;
    uint64_t             bound_gpu_va = 0;
    uint64_t             byte_size = 16384;
    std::vector<uint8_t> stub_texture_backing;
};

struct MaliIcdImageView {
    MaliIcdImage* image = nullptr;
    uint32_t      format = 37;
};

struct MaliIcdSampler {
    float max_anisotropy = 16.0f;
    float max_lod = 15.0f;
};

struct MaliIcdRenderPass {
    uint32_t attachment_count = 1;
};

struct MaliIcdFramebuffer {
    uint32_t           width = 64, height = 64;
    MaliMfbdDescriptor hw_mfbd{};
};

struct MaliIcdQueryPool {
    uint32_t              query_count = 8;
    std::vector<uint64_t> results;
};

struct MaliIcdDescriptorSet {
    uint64_t          bound_buffer_va[16]{};
    uint64_t          bound_buffer_range[16]{};
    MaliIcdImageView* bound_image_view[16]{};
    MaliIcdSampler*   bound_sampler[16]{};
};

struct MaliIcdRecordedCmd {
    enum Type {
        CMD_DISPATCH,
        CMD_DRAW_INDEXED_VKCUBE,
        CMD_DRAW_GEARS,
        CMD_COPY_BUFFER,
        CMD_COPY_BUFFER_TO_IMAGE,
        CMD_BLIT_IMAGE,
        CMD_WRITE_TIMESTAMP,
        CMD_PIPELINE_BARRIER
    } type;
    uint32_t              rp_draw_idx = 0;
    MaliIcdPipeline*      pipeline = nullptr;
    MaliIcdDescriptorSet* desc_set = nullptr;
    MaliIcdFramebuffer*   framebuffer = nullptr;
    uint32_t              dynamic_offset = 0;
    uint32_t              wg_x = 1, wg_y = 1, wg_z = 1;
    uint32_t              index_count = 0, instance_count = 1;
    float                 vkcube_angle_deg = 25.0f;
    uint32_t              vkcube_frame_idx = 0;
    uint64_t              src_va = 0, dst_va = 0, copy_bytes = 0;
    MaliIcdImage*         dst_image = nullptr;
    MaliIcdQueryPool*     query_pool = nullptr;
    uint32_t              query_index = 0;
};

struct MaliIcdCommandBuffer {
    uintptr_t                        loader_magic = ICD_LOADER_MAGIC;
    MaliIcdDevice*                   device = nullptr;
    MaliIcdPipeline*                 current_pipeline = nullptr;
    MaliIcdDescriptorSet*            current_desc_set = nullptr;
    MaliIcdFramebuffer*              active_fb = nullptr;
    uint64_t                         bound_vertex_bo_va = 0;
    uint64_t                         bound_index_bo_va  = 0;
    uint32_t                         dynamic_ubo_offset = 0;
    uint8_t                          push_constants[128]{};
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
    std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
    auto& cpu = g_aot_rt.cpu;
    if (aot_kbase_version_check_csf && aot_kbase_mem_alloc && dev && !dev->kctx_mem.empty()) {
        uint64_t out_flags = 0;
        uint64_t out_gpu_va = 0;
        uint64_t* sp_args = reinterpret_cast<uint64_t*>(cpu.sp);
        sp_args[0] = 0;
        sp_args[1] = 0;
        cpu.x[0] = reinterpret_cast<uint64_t>(dev->kctx_mem.data());
        cpu.x[1] = pages;
        cpu.x[2] = pages;
        cpu.x[3] = 0;
        cpu.x[4] = reinterpret_cast<uint64_t>(&out_flags);
        cpu.x[5] = reinterpret_cast<uint64_t>(&out_gpu_va);
        cpu.x[6] = flags;
        cpu.x[7] = 0;
        aot_kbase_mem_alloc(&cpu);
        if (cpu.x[0] == 0 && out_gpu_va != 0) return out_gpu_va;
    }
    uint64_t req[8]{};
    req[0] = pages;
    req[1] = pages;
    req[3] = flags;
    bionic_mali_kbase_ioctl_stub(100, 0xc040803bU, reinterpret_cast<uint64_t>(req));
    uint64_t cookie_or_va = req[1];
    if ((flags & BASE_MEM_SAME_VA) != 0 && (flags & BASE_MEM_PROT_GPU_EX) == 0) {
        return bionic_mali_mmap_stub(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 100, cookie_or_va);
    }
    return cookie_or_va;
}

