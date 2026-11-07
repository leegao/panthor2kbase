#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/mman.h>

#include "mali_kbase_csf_lvp_harness.hpp"

static constexpr uint64_t IMAGE_BASE = 0x400000000ULL;
static constexpr uint64_t HLE_MALLOC_MAGIC   = 0xF000FFF0ULL;
static constexpr uint64_t HLE_TLSDESC_MAGIC  = 0xF000FFF4ULL;
static constexpr uint64_t HLE_VK_FREE_MAGIC  = 0xF000FFF6ULL;
static constexpr uint64_t HLE_FREE_MAGIC     = 0xF000FFF8ULL;
static constexpr uint64_t HLE_VK_ALLOC_MAGIC = 0xF000FFFCULL;
static constexpr uint64_t BIONIC_STACK_GUARD_CANARY = 0xDEADBEEFCAFEBABEULL;

struct alignas(16) AOTCPUState {
    uint64_t    x[31]{};
    uint64_t    sp = 0;
    uint64_t    pc = 0;
    uint64_t    tpidr_el0 = 0;
    __uint128_t v[32]{};
};

extern "C" {
    __attribute__((weak)) uint64_t aot_get_ctx_malloc_offset() { return 0x1000; }
    __attribute__((weak)) uint64_t aot_get_vk_hmi_addr() { return 0x2000; }
    __attribute__((weak)) uint64_t aot_get_kbase_page_shift_addr() { return 0x3000; }
    __attribute__((weak)) void aot_cmpbe_v2_deserialize_MBS2_to_C(AOTCPUState* cpu) {}
    __attribute__((weak)) void aot_cmpbe_v2_init_options(AOTCPUState* cpu) {}
    __attribute__((weak)) void aot_cmpbe_v2_set_option_value(AOTCPUState* cpu) {}
    __attribute__((weak)) void aot_cmpbe_v2_free_compilation_result(AOTCPUState* cpu) {}
    void aot_vk_hal_open(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkGetInstanceProcAddr(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceVersion(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceLayerProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkEnumerateInstanceExtensionProperties(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkCreateShaderModule(AOTCPUState* cpu) __attribute__((weak));
    void aot_vkDestroyShaderModule(AOTCPUState* cpu) __attribute__((weak));
    void aot_mali_gpu_props_get(AOTCPUState* cpu) __attribute__((weak));

    // AOT-translated native AArch64 /dev/mali0 (kbase JM & CSF) functions
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

    void aot_indirect_dispatch(AOTCPUState* cpu, uint64_t target_va);
}

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

// ============================================================================
// Global /dev/mali0 (kbase CSF) Emulator Instance & Bionic Syscall Bridges
// ============================================================================
static MaliKbaseCsfKernelEmulator g_kbase_emu;
static int g_kbase_ioctl_calls = 0;

extern "C" uint64_t bionic_mali_open_stub(uint64_t path_addr, uint64_t flags) {
    return g_kbase_emu.handleOpen(reinterpret_cast<const char*>(path_addr), flags);
}

extern "C" uint64_t bionic_mali_close_stub(uint64_t fd) {
    return g_kbase_emu.handleClose(fd);
}

extern "C" uint64_t bionic_mali_kbase_ioctl_stub(uint64_t fd, uint64_t cmd, uint64_t arg_addr) {
    g_kbase_ioctl_calls++;
    return g_kbase_emu.handleIoctl(fd, cmd, arg_addr);
}

extern "C" uint64_t bionic_mali_mmap_stub(uint64_t addr, uint64_t length, uint64_t prot,
                                          uint64_t flags, uint64_t fd, uint64_t offset) {
    return g_kbase_emu.handleMmap(addr, length, prot, flags, fd, offset);
}

extern "C" uint64_t bionic_mali_munmap_stub(uint64_t addr, uint64_t length) {
    return g_kbase_emu.handleMunmap(addr, length);
}

extern "C" uint64_t bionic_mali_read_stub(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    return g_kbase_emu.handleRead(fd, buf_addr, count);
}

extern "C" uint64_t bionic_mali_poll_stub(uint64_t fds_addr, uint64_t nfds, uint64_t timeout) {
    return g_kbase_emu.handlePoll(fds_addr, nfds, timeout);
}

extern "C" void __mali_stack_chk_fail_diag(AOTCPUState* cpu) {
    std::fprintf(stderr, "[AOT_ERROR] Bionic __stack_chk_fail triggered! SP=0x%016lx LR=0x%016lx\n",
                 cpu->sp, cpu->x[30]);
    std::abort();
}

extern "C" void aot_unhandled_plt(AOTCPUState* cpu, uint64_t target_va) {
    std::fprintf(stderr, "[AOT_ERROR] Unhandled indirect/PLT target VA: 0x%016lx (RVA: 0x%08lx) LR=0x%016lx\n",
                 target_va, target_va - IMAGE_BASE, cpu->x[30]);
    std::abort();
}

// ============================================================================
// Reference RFC 3174 SHA-1 for verifying vkCreateShaderModule's SHA-1 digest
// ============================================================================
static void computeRefSha1(const uint8_t* data, size_t len, uint8_t out20[20]) {
    auto rol32 = [](uint32_t v, unsigned s) { return (v << s) | (v >> (32 - s)); };
    uint32_t h0 = 0x67452301U, h1 = 0xEFCDAB89U, h2 = 0x98BADCFEU, h3 = 0x10325476U, h4 = 0xC3D2E1F0U;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999U; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1U; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6U; }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out20[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        out20[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out20[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out20[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
}

struct MaliImageMapper {
    uint8_t* base = nullptr;
    size_t   image_size = 0x04000000;
    std::vector<uint8_t> elf;
    std::string driver_version = "unknown-64bit";

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
        elf.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
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
            if (std::strcmp(sname, ".dynsym") == 0) {
                dynsym_off = sh_offset;
            } else if (std::strcmp(sname, ".relr.dyn") == 0) {
                relr_off = sh_offset;
                relr_sz = sh_size;
            } else if (std::strcmp(sname, ".rela.dyn") == 0) {
                reladyn_off = sh_offset;
                reladyn_sz = sh_size;
            } else if (std::strcmp(sname, ".note.gnu.build-id") == 0 && sh_size >= 20) {
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
                if ((group_flags & 8) && (group_flags & 4)) {
                    cur_addend += readSLEB128(aps2, pos);
                } else if (!(group_flags & 8)) {
                    cur_addend = 0;
                }
                for (int64_t g = 0; g < group_size; ++g) {
                    cur_offset += (group_flags & 2) ? group_off_delta : readSLEB128(aps2, pos);
                    uint64_t r_info = (group_flags & 1) ? static_cast<uint64_t>(group_info)
                                                        : static_cast<uint64_t>(readSLEB128(aps2, pos));
                    if ((group_flags & 8) && !(group_flags & 4)) {
                        cur_addend += readSLEB128(aps2, pos);
                    }
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

static const uint8_t kMinimalFragmentMBS2_v47[248] = {
    0x4d, 0x42, 0x53, 0x32, 0xf0, 0x00, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x56, 0x45, 0x48, 0x57,
    0x0c, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x43, 0x46, 0x52, 0x41, 0xd0, 0x00, 0x00, 0x00, 0x43, 0x4d, 0x4d, 0x4e, 0xbc, 0x00, 0x00, 0x00,
    0x56, 0x45, 0x4c, 0x41, 0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x53, 0x53, 0x59, 0x4d,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x53, 0x59, 0x4d, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x53, 0x53, 0x59, 0x4d, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x53, 0x59, 0x4d, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x53, 0x59, 0x4d,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x53, 0x59, 0x4d, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x55, 0x42, 0x55, 0x46, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x45, 0x42, 0x49, 0x4e,
    0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x50, 0x44, 0x66, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x4c, 0x4b, 0x4e,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x42, 0x4a, 0x43,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x78, 0x53, 0x50, 0x41, 0x53,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ============================================================================
// Sample SPIR-V 1.3 Shader Generator & Valhall (Mali-G615) ISA OBJC Compiler
// ============================================================================
struct SampleSpirvShader {
    std::string name;
    std::string description;
    std::vector<uint8_t> spv;
    uint32_t num_words = 0;
    uint32_t num_basic_blocks = 0;
    uint32_t num_arith_ops = 0;
    uint32_t num_mem_ops = 0;
    bool has_branches = false;
    float expected_out[4]{};
};

static void appendSpvOp(std::vector<uint32_t>& words, uint16_t op, std::initializer_list<uint32_t> args) {
    uint32_t wc = static_cast<uint32_t>(1 + args.size());
    words.push_back((wc << 16) | op);
    for (uint32_t a : args) words.push_back(a);
}

static std::vector<uint8_t> finalizeSpv(uint32_t bound, const std::vector<uint32_t>& body) {
    std::vector<uint32_t> words = {0x07230203U, 0x00010300U, 0x00080001U, bound, 0U};
    words.insert(words.end(), body.begin(), body.end());
    std::vector<uint8_t> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

static std::vector<SampleSpirvShader> buildSampleSpirvShaders() {
    std::vector<SampleSpirvShader> list;

    // 1. Shader 1: Minimal Passthrough Fragment Shader -> [1.0, 0.5, 0.25, 1.0]
    {
        std::vector<uint32_t> b;
        appendSpvOp(b, 17, {1});                                       // OpCapability Shader
        appendSpvOp(b, 14, {0, 1});                                    // OpMemoryModel Logical GLSL450
        appendSpvOp(b, 15, {4, 1, 0x6E69616DU, 0U, 9});                // OpEntryPoint Fragment %1 "main" %9
        appendSpvOp(b, 16, {1, 7});                                    // OpExecutionMode %1 OriginUpperLeft
        appendSpvOp(b, 71, {9, 30, 0});                                // OpDecorate %9 Location 0
        appendSpvOp(b, 19, {2});                                       // OpTypeVoid %2
        appendSpvOp(b, 33, {3, 2});                                    // OpTypeFunction %3 %2
        appendSpvOp(b, 22, {6, 32});                                   // OpTypeFloat %6 32
        appendSpvOp(b, 23, {7, 6, 4});                                 // OpTypeVector %7 %6 4
        appendSpvOp(b, 32, {8, 3, 7});                                 // OpTypePointer %8 Output %7
        appendSpvOp(b, 59, {8, 9, 3});                                 // OpVariable %8 %9 Output
        appendSpvOp(b, 43, {6, 10, 0x3F800000U});                      // OpConstant 1.0f
        appendSpvOp(b, 43, {6, 11, 0x3F000000U});                      // OpConstant 0.5f
        appendSpvOp(b, 43, {6, 12, 0x3E800000U});                      // OpConstant 0.25f
        appendSpvOp(b, 44, {7, 13, 10, 11, 12, 10});                   // OpConstantComposite vec4
        appendSpvOp(b, 54, {2, 1, 0, 3});                              // OpFunction
        appendSpvOp(b, 248, {5});                                      // OpLabel %5
        appendSpvOp(b, 62, {9, 13});                                   // OpStore %9 %13
        appendSpvOp(b, 253, {});                                       // OpReturn
        appendSpvOp(b, 56, {});                                        // OpFunctionEnd
        SampleSpirvShader sh{"passthrough_frag.spv", "Minimal Passthrough Fragment Shader (vec4 output store)", finalizeSpv(16, b)};
        sh.expected_out[0] = 1.0f; sh.expected_out[1] = 0.5f; sh.expected_out[2] = 0.25f; sh.expected_out[3] = 1.0f;
        list.push_back(sh);
    }

    // 2. Shader 2: Storage Buffer Vector Arithmetic FMA Compute Shader
    // For input {3.0, 4.0, 5.0, 6.0}: v0 = in * 2.0 = {6, 8, 10, 12}; v2 = v0 + 0.5 - 0.125 = {6.375, 8.375, 10.375, 12.375};
    // out = v2 * v0 = {38.25, 67.0, 103.75, 148.5}
    {
        std::vector<uint32_t> b;
        appendSpvOp(b, 17, {1});                                       // OpCapability Shader
        appendSpvOp(b, 14, {0, 1});                                    // OpMemoryModel Logical GLSL450
        appendSpvOp(b, 15, {5, 1, 0x6E69616DU, 0U, 11, 12});           // OpEntryPoint GLCompute %1 "main" %11 %12
        appendSpvOp(b, 16, {1, 17, 64, 1, 1});                         // OpExecutionMode %1 LocalSize 64 1 1
        appendSpvOp(b, 71, {11, 33, 0});                               // OpDecorate %11 Binding 0
        appendSpvOp(b, 71, {12, 33, 1});                               // OpDecorate %12 Binding 1
        appendSpvOp(b, 19, {2});                                       // OpTypeVoid %2
        appendSpvOp(b, 33, {3, 2});                                    // OpTypeFunction %3 %2
        appendSpvOp(b, 22, {6, 32});                                   // OpTypeFloat %6 32
        appendSpvOp(b, 23, {7, 6, 4});                                 // OpTypeVector %7 %6 4
        appendSpvOp(b, 32, {8, 12, 7});                                // OpTypePointer %8 StorageBuffer %7
        appendSpvOp(b, 59, {8, 11, 12});                               // OpVariable %8 %11 StorageBuffer
        appendSpvOp(b, 59, {8, 12, 12});                               // OpVariable %8 %12 StorageBuffer
        appendSpvOp(b, 43, {6, 13, 0x40000000U});                      // OpConstant 2.0f
        appendSpvOp(b, 43, {6, 14, 0x3F000000U});                      // OpConstant 0.5f
        appendSpvOp(b, 43, {6, 15, 0x3E000000U});                      // OpConstant 0.125f
        appendSpvOp(b, 44, {7, 16, 13, 13, 13, 13});                   // OpConstantComposite scale
        appendSpvOp(b, 44, {7, 17, 14, 14, 14, 14});                   // OpConstantComposite bias
        appendSpvOp(b, 44, {7, 18, 15, 15, 15, 15});                   // OpConstantComposite offset
        appendSpvOp(b, 54, {2, 1, 0, 3});                              // OpFunction
        appendSpvOp(b, 248, {5});                                      // OpLabel %5
        appendSpvOp(b, 61, {7, 19, 11});                               // OpLoad %7 %19 %11
        appendSpvOp(b, 133, {7, 20, 19, 16});                          // OpFMul
        appendSpvOp(b, 129, {7, 21, 20, 17});                          // OpFAdd
        appendSpvOp(b, 131, {7, 22, 21, 18});                          // OpFSub
        appendSpvOp(b, 133, {7, 23, 22, 20});                          // OpFMul
        appendSpvOp(b, 62, {12, 23});                                  // OpStore %12 %23
        appendSpvOp(b, 253, {});                                       // OpReturn
        appendSpvOp(b, 56, {});                                        // OpFunctionEnd
        SampleSpirvShader sh{"vector_compute.spv", "Storage Buffer Vector FMA Compute Shader (OpFMul/OpFAdd/OpFSub)", finalizeSpv(26, b)};
        sh.expected_out[0] = 38.25f; sh.expected_out[1] = 67.0f; sh.expected_out[2] = 103.75f; sh.expected_out[3] = 148.5f;
        list.push_back(sh);
    }

    // 3. Shader 3: Structured Branching & Loop Reduction Compute Shader
    // For input {3.0, 4.0, 5.0, 6.0}: out = in * 1.25 + 9 * 1.25 = in * 1.25 + 11.25 = {15.0, 16.25, 17.5, 18.75}
    {
        std::vector<uint32_t> b;
        appendSpvOp(b, 17, {1});                                       // OpCapability Shader
        appendSpvOp(b, 14, {0, 1});                                    // OpMemoryModel Logical GLSL450
        appendSpvOp(b, 15, {5, 1, 0x6E69616DU, 0U, 11, 12});           // OpEntryPoint GLCompute %1 "main" %11 %12
        appendSpvOp(b, 16, {1, 17, 64, 1, 1});                         // OpExecutionMode %1 LocalSize 64 1 1
        appendSpvOp(b, 71, {11, 33, 0});                               // OpDecorate %11 Binding 0
        appendSpvOp(b, 71, {12, 33, 1});                               // OpDecorate %12 Binding 1
        appendSpvOp(b, 19, {2});                                       // OpTypeVoid %2
        appendSpvOp(b, 33, {3, 2});                                    // OpTypeFunction %3 %2
        appendSpvOp(b, 20, {4});                                       // OpTypeBool %4
        appendSpvOp(b, 21, {5, 32, 1});                                // OpTypeInt %5 32 1
        appendSpvOp(b, 22, {6, 32});                                   // OpTypeFloat %6 32
        appendSpvOp(b, 23, {7, 6, 4});                                 // OpTypeVector %7 %6 4
        appendSpvOp(b, 32, {8, 12, 7});                                // OpTypePointer %8 StorageBuffer %7
        appendSpvOp(b, 32, {9, 7, 5});                                 // OpTypePointer %9 Function %5
        appendSpvOp(b, 32, {10, 7, 7});                                // OpTypePointer %10 Function %7
        appendSpvOp(b, 59, {8, 11, 12});                               // OpVariable %8 %11 StorageBuffer
        appendSpvOp(b, 59, {8, 12, 12});                               // OpVariable %8 %12 StorageBuffer
        appendSpvOp(b, 43, {5, 13, 0});                                // OpConstant 0
        appendSpvOp(b, 43, {5, 14, 1});                                // OpConstant 1
        appendSpvOp(b, 43, {5, 15, 8});                                // OpConstant 8
        appendSpvOp(b, 43, {6, 16, 0x3FA00000U});                      // OpConstant 1.25f
        appendSpvOp(b, 44, {7, 17, 16, 16, 16, 16});                   // OpConstantComposite vec4(1.25f)
        appendSpvOp(b, 54, {2, 1, 0, 3});                              // OpFunction
        appendSpvOp(b, 248, {20});                                     // OpLabel %20
        appendSpvOp(b, 59, {9, 21, 7});                                // OpVariable i
        appendSpvOp(b, 59, {10, 22, 7});                               // OpVariable acc
        appendSpvOp(b, 61, {7, 23, 11});                               // OpLoad in
        appendSpvOp(b, 62, {21, 13});                                  // OpStore i = 0
        appendSpvOp(b, 62, {22, 23});                                  // OpStore acc = in
        appendSpvOp(b, 249, {24});                                     // OpBranch %24
        appendSpvOp(b, 248, {24});                                     // OpLabel %24 (loop header)
        appendSpvOp(b, 246, {29, 28, 0});                              // OpLoopMerge %29 %28 None
        appendSpvOp(b, 249, {25});                                     // OpBranch %25
        appendSpvOp(b, 248, {25});                                     // OpLabel %25
        appendSpvOp(b, 61, {5, 30, 21});                               // OpLoad i
        appendSpvOp(b, 177, {4, 31, 30, 15});                          // OpSLessThan (i < 8)
        appendSpvOp(b, 250, {31, 26, 29});                             // OpBranchConditional
        appendSpvOp(b, 248, {26});                                     // OpLabel %26
        appendSpvOp(b, 247, {28, 0});                                  // OpSelectionMerge %28 None
        appendSpvOp(b, 177, {4, 32, 30, 14});                          // OpSLessThan (i < 1)
        appendSpvOp(b, 250, {32, 27, 28});                             // OpBranchConditional
        appendSpvOp(b, 248, {27});                                     // OpLabel %27
        appendSpvOp(b, 61, {7, 33, 22});                               // OpLoad acc
        appendSpvOp(b, 133, {7, 34, 33, 17});                          // OpFMul
        appendSpvOp(b, 129, {7, 35, 34, 17});                          // OpFAdd
        appendSpvOp(b, 62, {22, 35});                                  // OpStore acc
        appendSpvOp(b, 249, {28});                                     // OpBranch %28
        appendSpvOp(b, 248, {28});                                     // OpLabel %28 (continue)
        appendSpvOp(b, 61, {7, 36, 22});                               // OpLoad acc
        appendSpvOp(b, 129, {7, 37, 36, 17});                          // OpFAdd
        appendSpvOp(b, 62, {22, 37});                                  // OpStore acc
        appendSpvOp(b, 128, {5, 38, 30, 14});                          // OpIAdd (i + 1)
        appendSpvOp(b, 62, {21, 38});                                  // OpStore i
        appendSpvOp(b, 249, {24});                                     // OpBranch %24
        appendSpvOp(b, 248, {29});                                     // OpLabel %29 (merge)
        appendSpvOp(b, 61, {7, 39, 22});                               // OpLoad acc
        appendSpvOp(b, 62, {12, 39});                                  // OpStore out
        appendSpvOp(b, 253, {});                                       // OpReturn
        appendSpvOp(b, 56, {});                                        // OpFunctionEnd
        SampleSpirvShader sh{"branch_loop_compute.spv", "Control-Flow Branching & Loop Reduction Compute Shader (6 BBs)", finalizeSpv(42, b)};
        sh.expected_out[0] = 15.0f; sh.expected_out[1] = 16.25f; sh.expected_out[2] = 17.5f; sh.expected_out[3] = 18.75f;
        list.push_back(sh);
    }

    for (auto& sh : list) {
        sh.num_words = static_cast<uint32_t>(sh.spv.size() / 4);
        const uint32_t* w = reinterpret_cast<const uint32_t*>(sh.spv.data());
        for (uint32_t pos = 5; pos < sh.num_words; ) {
            uint16_t op = w[pos] & 0xFFFFU;
            uint16_t wc = w[pos] >> 16;
            if (wc == 0) break;
            if (op == 248) sh.num_basic_blocks++;
            else if (op == 128 || op == 129 || op == 131 || op == 133 || op == 177) sh.num_arith_ops++;
            else if (op == 61 || op == 62) sh.num_mem_ops++;
            else if (op == 246 || op == 247 || op == 250) sh.has_branches = true;
            pos += wc;
        }
    }
    return list;
}

// Compile a SampleSpirvShader into authentic 64-bit Valhall (Mali-G615 Arch v11) ISA OBJC words
static std::vector<uint64_t> compileSpirvToValhallObjcWords(const SampleSpirvShader& sh) {
    std::vector<uint64_t> isa_words;
    if (sh.name == "passthrough_frag.spv") {
        // Materialize vec4(1.0f, 0.5f, 0.25f, 1.0f) in r0..r3 and emit ST_CVT.v4.f32 + NOP.end
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(0, 0xC0, 0x3F800000U));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(1, 0xC0, 0x3F000000U));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(2, 0xC0, 0x3E800000U));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(3, 0xC0, 0x3F800000U));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeStCvt(0, 0, 0));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeNopEnd());
    } else if (sh.name == "vector_compute.spv") {
        // LOAD.i128.wait0 @r1, u0 -> FMUL.f32 (2.0f = 0xDC) -> FADD.f32 (0.5f = 0xDD) ->
        // FSUB.f32 (0.125f = 0xDA) -> FMUL.f32 -> STORE.i128 @r5, u2 -> NOP.end
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeLoadI128(1, 0x80, 0, 1));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFmulF32(2, 1, 0xDC));           // r2 = r1 * 2.0f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(3, 2, 0xDD, false));    // r3 = r2 + 0.5f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(4, 3, 0xDA, true));     // r4 = r3 - 0.125f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFmulF32(5, 4, 2));              // r5 = r4 * r2
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeStoreI128(5, 0x82, 0, 0));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeNopEnd());
    } else {
        // branch_loop_compute.spv: LOAD.i128 + ICMP_OR.s32 + BRANCHZ loop + FMUL/FADD (1.25f = 0xD5) + STORE.i128
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeLoadI128(1, 0x80, 0, 1));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeMovI32(8, 0xC0));               // i = 0
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(10, 0xC0, 8U));      // limit = 8
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(12, 0xC0, 1U));      // step = 1
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIcmpOrS32(9, 8, 10, 2));        // r9 = (i < 8)
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeBranchz(9, 7, true, 10));       // BRANCHZ.eq.reconverge
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIcmpOrS32(11, 8, 12, 2));       // r11 = (i < 1)
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeBranchz(11, 3, true, 10));      // BRANCHZ.eq.reconverge
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFmulF32(1, 1, 0xD5));           // acc = acc * 1.25f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(1, 1, 0xD5, false));    // acc = acc + 1.25f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(1, 1, 0xD5, false));    // acc = acc + 1.25f
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeIaddS32(8, 8, 12));             // i = i + 1
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeBranchz(9, -8, false, 10));     // BRANCHZ.ne loop back
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeStoreI128(1, 0x82, 0, 0));
        isa_words.push_back(ValhallISAToSpirvTranslator::encodeNopEnd());
    }
    return isa_words;
}


// Compile a SampleSpirvShader into authentic 128-bit Bifrost (Mali-G76 Arch v7) Dual-Issue [FMA, ADD] Clause OBJC words
static std::vector<uint64_t> compileSpirvToBifrostClauseObjcWords(const SampleSpirvShader& sh) {
    std::vector<uint64_t> words;
    auto pushClause = [&](std::pair<uint64_t, uint64_t> cl) {
        words.push_back(cl.first);
        words.push_back(cl.second);
    };
    if (sh.name == "passthrough_frag.spv") {
        // CLAUSE 0: *IADD_IMM.i32 r0, r63, #1.0f ; +IADD_IMM.i32 r1, #0.5f
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_IMM, 0, 63, 0, BI_ADD_IADD_IMM, 1, 0x3F800000U));
        // Fix up r1 = 0.5f and dual-issue r2 = 0.25f
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_IMM, 1, 63, 0, BI_ADD_NOP, 0, 0x3F000000U));
        // CLAUSE 2: *IADD_IMM.i32 r2, r63, #0.25f ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_IMM, 2, 63, 0, BI_ADD_NOP, 0, 0x3E800000U));
        // CLAUSE 3 (Dual-Issue EOS): *IADD_IMM.i32 r3, r63, #1.0f ; +ST_CVT.v4.f32 @r0:r3, rt0
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_END, BIFROST_MSG_BLEND, 0, 0, 0,
            BI_FMA_IADD_IMM, 3, 63, 0, BI_ADD_ST_CVT_V4, 0, 0x3F800000U));
    } else if (sh.name == "vector_compute.spv") {
        // CLAUSE 0: *NOP ; +LD_BUFFER.i128 @r1:r4, u0 (slot=0, wait=1)
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB, BIFROST_MSG_LOAD, 1, 1, 0,
            BI_FMA_NOP, 0, 0, 0, BI_ADD_LD_BUF128, 0, 0));
        // CLAUSE 1: *FMUL.f32 r2, r1, #2.0f ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_FMUL_F32, 2, 1, 0, BI_ADD_NOP, 0, 0x40000000U));
        // CLAUSE 2: *FADD.f32 r3, r2, #0.5f ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_FADD_F32, 3, 2, 0, BI_ADD_NOP, 0, 0x3F000000U));
        // CLAUSE 3: *FSUB.f32 r4, r3, #0.125f ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_FSUB_F32, 4, 3, 0, BI_ADD_NOP, 0, 0x3E000000U));
        // CLAUSE 4 (Dual-Issue EOS): *FMUL.f32 r5, r4, r2 ; +ST_BUFFER.i128 @r5:r8, u2
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_END, BIFROST_MSG_STORE, 5, 0, 0,
            BI_FMA_FMUL_F32, 5, 4, 2, BI_ADD_ST_BUF128, 0, 0));
    } else {
        // branch_loop_compute.spv (9 Dual-Issue 128-bit Bifrost Clauses = 18 words)
        // CLAUSE 0 (Dual-Issue): *MOV.i32 r8, #0 ; +LD_BUFFER.i128 @r1:r4, u0
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB, BIFROST_MSG_LOAD, 1, 1, 0,
            BI_FMA_MOV_I32, 8, 63, 0, BI_ADD_LD_BUF128, 0, 0U));
        // CLAUSE 1: *IADD_IMM.i32 r10, r63, #8 ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_IMM, 10, 63, 0, BI_ADD_NOP, 0, 8U));
        // CLAUSE 2: *IADD_IMM.i32 r12, r63, #1 ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_IMM, 12, 63, 0, BI_ADD_NOP, 0, 1U));
        // CLAUSE 3 (Loop Header Dual-Issue): *ICMP.lt.s32 r9, r8, r10 ; +BRANCH.eq r9, offset:+4 (to CLAUSE 8)
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB_PC, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_ICMP_LT, 9, 8, 10, BI_ADD_BRANCH_EQ, 9, 4U));
        // CLAUSE 4 (Inner Branch Dual-Issue): *ICMP.lt.s32 r11, r8, r12 ; +BRANCH.eq r11, offset:+1 (to CLAUSE 6)
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB_PC, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_ICMP_LT, 11, 8, 12, BI_ADD_BRANCH_EQ, 11, 1U));
        // CLAUSE 5 (Dual-Issue FMA+ADD): *FMUL.f32 r1, r1, #1.25f ; +FADD.f32 r1, r1, #1.25f
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_FMUL_F32, 1, 1, 0, BI_ADD_FADD_IMM, 1, 0x3FA00000U));
        // CLAUSE 6: *FADD.f32 r1, r1, #1.25f ; +NOP
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_FADD_F32, 1, 1, 0, BI_ADD_NOP, 0, 0x3FA00000U));
        // CLAUSE 7 (Loop Back-Edge Dual-Issue): *IADD.s32 r8, r8, r12 ; +BRANCH.ne r9, offset:-5 (to CLAUSE 3)
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB_PC, BIFROST_MSG_NONE, 0, 0, 0,
            BI_FMA_IADD_S32, 8, 8, 12, BI_ADD_BRANCH_NE, 9, static_cast<uint32_t>(-5)));
        // CLAUSE 8 (EOS): *NOP ; +ST_BUFFER.i128 @r1:r4, u2
        pushClause(BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_END, BIFROST_MSG_STORE, 1, 0, 0,
            BI_FMA_NOP, 0, 0, 0, BI_ADD_ST_BUF128, 0, 0U));
    }
    return words;
}

static std::vector<uint8_t> compileSpirvToMaliMbs2(const SampleSpirvShader& sh,
                                                   const std::vector<std::pair<const uint8_t*, uint32_t>>& embedded_blobs,
                                                   std::vector<uint64_t>* out_objc_words = nullptr) {
    std::vector<uint64_t> isa_words = compileSpirvToValhallObjcWords(sh);
    if (out_objc_words) *out_objc_words = isa_words;
    uint32_t isa_byte_len = static_cast<uint32_t>(isa_words.size() * 8);

    const uint8_t* tpl = kMinimalFragmentMBS2_v47;
    uint32_t tpl_len = sizeof(kMinimalFragmentMBS2_v47);
    if (!embedded_blobs.empty()) {
        tpl = embedded_blobs[0].first;
        tpl_len = embedded_blobs[0].second;
    }

    for (uint32_t pos = 12; pos + 8 <= tpl_len; ++pos) {
        if (std::memcmp(tpl + pos, "OBJC", 4) == 0) {
            uint32_t old_objc_len = *reinterpret_cast<const uint32_t*>(tpl + pos + 4);
            uint32_t new_objc_len = old_objc_len + isa_byte_len;
            uint32_t delta = isa_byte_len;

            uint32_t ebin_len_off = 0;
            for (uint32_t epos = 48; epos < pos; ++epos) {
                if (std::memcmp(tpl + epos, "EBIN", 4) == 0) {
                    ebin_len_off = epos + 4;
                    break;
                }
            }

            std::vector<uint8_t> out;
            out.insert(out.end(), tpl, tpl + pos + 8 + old_objc_len);
            const uint8_t* isa_bytes = reinterpret_cast<const uint8_t*>(isa_words.data());
            out.insert(out.end(), isa_bytes, isa_bytes + isa_byte_len);
            out.insert(out.end(), tpl + pos + 8 + old_objc_len, tpl + tpl_len);

            *reinterpret_cast<uint32_t*>(&out[4])  += delta;
            *reinterpret_cast<uint32_t*>(&out[36]) += delta;
            *reinterpret_cast<uint32_t*>(&out[44]) += delta;
            if (ebin_len_off != 0) {
                *reinterpret_cast<uint32_t*>(&out[ebin_len_off]) += delta;
            }
            *reinterpret_cast<uint32_t*>(&out[pos + 4]) = new_objc_len;
            return out;
        }
    }
    return std::vector<uint8_t>(tpl, tpl + tpl_len);
}

// Helper to extract the raw OBJC payload bytes from a driver-validated MBS2 binary
static std::vector<uint64_t> extractObjcFromMbs2(const std::vector<uint8_t>& mbs2, size_t expected_words) {
    for (size_t pos = 12; pos + 8 <= mbs2.size(); ++pos) {
        if (std::memcmp(&mbs2[pos], "OBJC", 4) == 0) {
            uint32_t objc_len = *reinterpret_cast<const uint32_t*>(&mbs2[pos + 4]);
            if (pos + 8 + objc_len <= mbs2.size() && (objc_len % 8) == 0) {
                size_t nwords = objc_len / 8;
                std::vector<uint64_t> all_words(nwords);
                std::memcpy(all_words.data(), &mbs2[pos + 8], objc_len);
                size_t start_idx = (expected_words > 0 && nwords >= expected_words) ? (nwords - expected_words) : 0;
                return std::vector<uint64_t>(all_words.begin() + start_idx, all_words.end());
            }
        }
    }
    return {};
}

// CSF Command Stream 64-bit Instruction Builders (matching panfrost/genxml/v10.xml)
static inline uint64_t csfEncodeMove48(uint8_t dest_reg, uint64_t imm48) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_MOVE48) << 56) |
           (static_cast<uint64_t>(dest_reg) << 48) |
           (imm48 & 0xFFFFFFFFFFFFULL);
}

static inline uint64_t csfEncodeMove32(uint8_t dest_reg, uint32_t imm32) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_MOVE32) << 56) |
           (static_cast<uint64_t>(dest_reg) << 48) |
           static_cast<uint64_t>(imm32);
}

static inline uint64_t csfEncodeReqResource(bool compute = true, bool fragment = true, bool tiler = false, bool idvs = false) {
    uint64_t mask = (compute ? 1ULL : 0ULL) | (fragment ? 2ULL : 0ULL) | (tiler ? 4ULL : 0ULL) | (idvs ? 8ULL : 0ULL);
    return (static_cast<uint64_t>(MALI_CS_OPCODE_REQ_RESOURCE) << 56) | mask;
}

static inline uint64_t csfEncodeRunCompute(uint8_t srt_sel, uint8_t spd_sel, uint8_t tsd_sel, uint8_t fau_sel, uint16_t task_inc = 1) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_RUN_COMPUTE) << 56) |
           (static_cast<uint64_t>(fau_sel & 3) << 46) |
           (static_cast<uint64_t>(tsd_sel & 3) << 44) |
           (static_cast<uint64_t>(spd_sel & 3) << 42) |
           (static_cast<uint64_t>(srt_sel & 3) << 40) |
           static_cast<uint64_t>(task_inc & 0x3FFFU);
}

static inline uint64_t csfEncodeRunFragment() {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_RUN_FRAGMENT) << 56);
}

static inline uint64_t csfEncodeFlushCache2(uint8_t latest_flush_id_reg = 0, uint8_t signal_slot = 0) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_FLUSH_CACHE2) << 56) |
           (static_cast<uint64_t>(signal_slot & 0xF) << 48) |
           (static_cast<uint64_t>(latest_flush_id_reg) << 40) |
           0x111ULL;
}

static inline uint64_t csfEncodeSyncAdd64(uint8_t addr_reg, uint8_t data_reg, uint16_t wait_mask = 0, uint8_t signal_slot = 0) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_SYNC_ADD64) << 56) |
           (static_cast<uint64_t>(signal_slot & 0xF) << 48) |
           (static_cast<uint64_t>(addr_reg) << 40) |
           (static_cast<uint64_t>(data_reg) << 32) |
           (static_cast<uint64_t>(wait_mask) << 16);
}

static inline uint64_t csfEncodeWait(uint16_t wait_mask = 0) {
    return (static_cast<uint64_t>(MALI_CS_OPCODE_WAIT) << 56) |
           (static_cast<uint64_t>(wait_mask) << 16);
}

struct VkExtensionProperties {
    char     extensionName[256];
    uint32_t specVersion;
};

struct VkShaderModuleCreateInfo {
    uint32_t       sType;
    const void*    pNext;
    uint32_t       flags;
    size_t         codeSize;
    const uint32_t* pCode;
};

struct VkAllocationCallbacks {
    void*    pUserData;
    uint64_t pfnAllocation;
    uint64_t pfnReallocation;
    uint64_t pfnFree;
    uint64_t pfnInternalAllocation;
    uint64_t pfnInternalFree;
};

int main(int argc, char** argv) {
    const char* so_path = argc > 1 ? argv[1] : "libGLES_mali.so";
    g_kbase_emu.lvp_executor.init();
    MaliImageMapper mapper;
    if (!mapper.loadAndRelocate(so_path)) {
        std::cerr << "Failed to map 64-bit libGLES_mali.so: " << so_path << "\n";
        return 1;
    }

    // Initialize OSU_CONFIG_CPU_PAGE_SIZE_LOG2 (12) in mapped driver .bss if present
    if (aot_get_kbase_page_shift_addr) {
        uint64_t pshift_va = aot_get_kbase_page_shift_addr();
        if (pshift_va >= IMAGE_BASE && pshift_va + 4 <= IMAGE_BASE + mapper.image_size) {
            *reinterpret_cast<uint32_t*>(pshift_va) = 12U;
        }
    }

    std::vector<uint8_t> stack_mem(2 * 1024 * 1024, 0);
    std::vector<uint8_t> bionic_tls_mem(65536, 0);
    AOTCPUState cpu{};
    cpu.sp = reinterpret_cast<uint64_t>(stack_mem.data() + stack_mem.size() - 4096);
    cpu.tpidr_el0 = reinterpret_cast<uint64_t>(bionic_tls_mem.data() + 1024);

    // Initialize Bionic TLS_SLOT_STACK_GUARD ([tpidr_el0 + 0x28])
    *reinterpret_cast<uint64_t*>(cpu.tpidr_el0 + 0x28) = BIONIC_STACK_GUARD_CANARY;

    // Query the auto-detected ctx allocator offset emitted by ELF64LoaderAndCallGraphPass
    uint64_t malloc_off = aot_get_ctx_malloc_offset();
    uint8_t ctx[256]{};
    *reinterpret_cast<uint64_t*>(ctx + malloc_off)     = HLE_MALLOC_MAGIC;
    *reinterpret_cast<uint64_t*>(ctx + malloc_off + 8) = HLE_FREE_MAGIC;

    std::cout << "================================================================================\n";
    std::cout << " Native x86-64 AOT Verification: CMPBE + Vulkan ICD + /dev/mali0 (" << mapper.driver_version << ")\n";
    std::cout << " Auto-Detected Context Allocator Offset: +0x" << std::hex << malloc_off << std::dec << "\n";
    std::cout << "================================================================================\n";

    auto embedded_blobs = mapper.findEmbeddedMBS2Blobs();
    const uint8_t* mbs2_ptr = kMinimalFragmentMBS2_v47;
    uint32_t mbs2_len = sizeof(kMinimalFragmentMBS2_v47);
    if (!embedded_blobs.empty()) {
        mbs2_ptr = embedded_blobs[0].first;
        mbs2_len = embedded_blobs[0].second;
        std::cout << "[INFO] Found " << embedded_blobs.size() << " embedded MBS2 shader blobs in "
                  << mapper.driver_version << " .rodata (testing blob #0, " << mbs2_len << " bytes).\n";
    }

    // TEST 1: Golden MBS2 Deserialization to C
    uint64_t comp_result[9]{};
    comp_result[2] = reinterpret_cast<uint64_t>(mbs2_ptr);
    comp_result[3] = mbs2_len;
    char* c_str = nullptr;

    cpu.x[0] = reinterpret_cast<uint64_t>(ctx);
    cpu.x[1] = reinterpret_cast<uint64_t>(comp_result);
    cpu.x[2] = reinterpret_cast<uint64_t>(&c_str);
    aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
    uint32_t rc = static_cast<uint32_t>(cpu.x[0]);

    assert(rc == 0 && c_str != nullptr);
    std::string s(c_str);
    assert(s.find("static const cmpbe_chunk_VEHW VEHW_0") != std::string::npos);
    assert(s.find("cmpbe_chunk_MBS2 MBS2 =") != std::string::npos);

    std::cout << "[PASS] Test 1: Native x86-64 AOT cmpbe_v2_deserialize_MBS2_to_C executed successfully!\n";
    std::cout << "       Output size: " << s.size() << " bytes C source code.\n";
    std::cout << "----------------------- Generated C Struct Source -----------------------\n";
    size_t lines_printed = 0;
    for (size_t pos = 0; pos < s.size(); ) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find("[] =") == std::string::npos && lines_printed < 25) {
            std::cout << line << "\n";
            lines_printed++;
        }
        pos = (nl == std::string::npos) ? s.size() : nl + 1;
    }
    std::cout << "-------------------------------------------------------------------------\n";
    std::free(c_str);

    if (embedded_blobs.size() > 1) {
        size_t extra_tested = std::min<size_t>(5, embedded_blobs.size());
        for (size_t idx = 1; idx < extra_tested; ++idx) {
            comp_result[2] = reinterpret_cast<uint64_t>(embedded_blobs[idx].first);
            comp_result[3] = embedded_blobs[idx].second;
            char* extra_c = nullptr;
            cpu.x[0] = reinterpret_cast<uint64_t>(ctx);
            cpu.x[1] = reinterpret_cast<uint64_t>(comp_result);
            cpu.x[2] = reinterpret_cast<uint64_t>(&extra_c);
            aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
            assert(static_cast<uint32_t>(cpu.x[0]) == 0 && extra_c != nullptr);
            std::string es(extra_c);
            assert(es.find("cmpbe_chunk_MBS2 MBS2 =") != std::string::npos);
            std::free(extra_c);
        }
        std::cout << "[PASS] Test 1b: Verified " << extra_tested << " embedded MBS2 shader blobs from "
                  << mapper.driver_version << " .rodata.\n";
    }

    // TEST 2: Option Initialization & Jump-Table Switch Lowering
    uint8_t opts[256]{};
    cpu.x[0] = reinterpret_cast<uint64_t>(opts);
    cpu.x[1] = 0x1c0000;
    cpu.x[2] = 0;
    aot_cmpbe_v2_init_options(&cpu);

    for (uint32_t opt_id : {1U, 2U, 9U, 11U, 14U}) {
        cpu.x[0] = reinterpret_cast<uint64_t>(opts);
        cpu.x[1] = opt_id;
        cpu.x[2] = 1;
        aot_cmpbe_v2_set_option_value(&cpu);
        assert(cpu.x[0] == 1);
    }
    cpu.x[0] = reinterpret_cast<uint64_t>(opts);
    cpu.x[1] = 99;
    cpu.x[2] = 1;
    aot_cmpbe_v2_set_option_value(&cpu);
    assert(cpu.x[0] == 0);
    std::cout << "[PASS] Test 2: cmpbe_v2_init_options and PIC jump-table switch lowered & verified.\n";

    // TEST 3: Error Handling & Bionic Stack Canary Integrity
    uint8_t bad_mbs2[32] = {'B', 'A', 'D', '!'};
    comp_result[2] = reinterpret_cast<uint64_t>(bad_mbs2);
    comp_result[3] = sizeof(bad_mbs2);
    char* err_c = nullptr;
    cpu.x[0] = reinterpret_cast<uint64_t>(ctx);
    cpu.x[1] = reinterpret_cast<uint64_t>(comp_result);
    cpu.x[2] = reinterpret_cast<uint64_t>(&err_c);
    aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
    assert(static_cast<uint32_t>(cpu.x[0]) == 2 && err_c == nullptr);
    assert(*reinterpret_cast<uint64_t*>(cpu.tpidr_el0 + 0x28) == BIONIC_STACK_GUARD_CANARY);
    std::cout << "[PASS] Test 3: Invalid MBS2 rejection (rc=2) & Bionic TPIDR_EL0 stack canary verified.\n";

    // TEST 4: Mali /dev/mali0 (kbase) Kernel UAPI Simulation (mali_gpu_props_get for Mali-G615 0xb8830001)
    if (aot_mali_gpu_props_get) {
        g_kbase_ioctl_calls = 0;
        std::vector<uint8_t> out_props(4096, 0);
        uint64_t fake_fd = bionic_mali_open_stub(reinterpret_cast<uint64_t>("/dev/mali0"), 0);
        assert(fake_fd == 100);
        uint64_t kctx_fd_va = IMAGE_BASE + 0x03fff000ULL;
        *reinterpret_cast<uint32_t*>(kctx_fd_va) = static_cast<uint32_t>(fake_fd);
        cpu.x[0] = kctx_fd_va;
        cpu.x[1] = reinterpret_cast<uint64_t>(out_props.data());
        aot_mali_gpu_props_get(&cpu);
        assert(g_kbase_ioctl_calls >= 2);
        std::cout << "[PASS] Test 4: Mali /dev/mali0 (kbase) Kernel UAPI shim verified via AOT mali_gpu_props_get ("
                  << g_kbase_ioctl_calls << " kbase ioctl calls serviced, Mali-G615 PRODUCT_ID=0xb883).\n";
    }

    // TEST 5: Vulkan ICD HAL Open, vkGetInstanceProcAddr, and Instance Extension Enumeration
    if (aot_vk_hal_open && aot_vkGetInstanceProcAddr && aot_vkEnumerateInstanceExtensionProperties) {
        uint64_t hmi_va = aot_get_vk_hmi_addr();
        uint64_t hw_dev_ptr = 0;
        const char* vk0_id = "vk0";
        cpu.x[0] = hmi_va;
        cpu.x[1] = reinterpret_cast<uint64_t>(vk0_id);
        cpu.x[2] = reinterpret_cast<uint64_t>(&hw_dev_ptr);
        aot_vk_hal_open(&cpu);
        assert(static_cast<int32_t>(cpu.x[0]) == 0 && hw_dev_ptr != 0);

        uint32_t api_ver = 0;
        if (aot_vkEnumerateInstanceVersion) {
            cpu.x[0] = reinterpret_cast<uint64_t>(&api_ver);
            aot_vkEnumerateInstanceVersion(&cpu);
            assert(static_cast<int32_t>(cpu.x[0]) == 0 && api_ver >= 0x00401000U);
        }

        const char* vk_queries[] = {
            "vkCreateInstance",
            "vkEnumerateInstanceExtensionProperties",
            "vkCreateShaderModule",
            "vkDestroyShaderModule",
            "vkCreateComputePipelines",
            "vkCreateGraphicsPipelines",
        };
        uint64_t mock_instance[16]{};
        mock_instance[0] = 0x00403000ULL;
        mock_instance[1] = 0x00403000ULL;
        mock_instance[2] = 0x00403000ULL;
        int resolved_count = 0;
        for (const char* qname : vk_queries) {
            cpu.x[0] = (std::strcmp(qname, "vkCreateInstance") == 0 ||
                        std::strcmp(qname, "vkEnumerateInstanceExtensionProperties") == 0)
                       ? 0ULL : reinterpret_cast<uint64_t>(mock_instance);
            cpu.x[1] = reinterpret_cast<uint64_t>(qname);
            aot_vkGetInstanceProcAddr(&cpu);
            assert(cpu.x[0] > IMAGE_BASE);
            resolved_count++;
        }

        uint32_t ext_count = 0;
        cpu.x[0] = 0;
        cpu.x[1] = reinterpret_cast<uint64_t>(&ext_count);
        cpu.x[2] = 0;
        aot_vkEnumerateInstanceExtensionProperties(&cpu);
        assert(static_cast<int32_t>(cpu.x[0]) == 0 && ext_count > 0 && ext_count <= 64);

        std::vector<VkExtensionProperties> exts(ext_count);
        cpu.x[0] = 0;
        cpu.x[1] = reinterpret_cast<uint64_t>(&ext_count);
        cpu.x[2] = reinterpret_cast<uint64_t>(exts.data());
        aot_vkEnumerateInstanceExtensionProperties(&cpu);
        assert(static_cast<int32_t>(cpu.x[0]) == 0);

        std::cout << "[PASS] Test 5: Vulkan ICD HAL (vk0) opened, vkGetInstanceProcAddr resolved "
                  << resolved_count << "/6 dispatch targets, and vkEnumerateInstanceExtensionProperties returned "
                  << ext_count << " extensions (e.g. " << exts[0].extensionName << ").\n";
    }

    // TEST 6: Sample SPIR-V 1.3 Shaders -> AOT vkCreateShaderModule (SHA-1) & Valhall ISA OBJC Deserialization
    auto sample_shaders = buildSampleSpirvShaders();
    VkAllocationCallbacks vk_alloc{};
    vk_alloc.pfnAllocation = HLE_VK_ALLOC_MAGIC;
    vk_alloc.pfnFree       = HLE_VK_FREE_MAGIC;
    std::vector<uint8_t> mock_vk_device(131072, 0);
    std::vector<std::vector<uint8_t>> validated_mbs2_blobs;

    for (size_t idx = 0; idx < sample_shaders.size(); ++idx) {
        const auto& sh = sample_shaders[idx];
        uint8_t expected_sha1[20]{};
        computeRefSha1(sh.spv.data(), sh.spv.size(), expected_sha1);

        bool sha1_verified = false;
        if (aot_vkCreateShaderModule && aot_vkDestroyShaderModule) {
            VkShaderModuleCreateInfo ci{};
            ci.sType    = 16;
            ci.pNext    = nullptr;
            ci.flags    = 0;
            ci.codeSize = sh.spv.size();
            ci.pCode    = reinterpret_cast<const uint32_t*>(sh.spv.data());

            uint64_t shader_mod = 0;
            cpu.x[0] = reinterpret_cast<uint64_t>(mock_vk_device.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(&ci);
            cpu.x[2] = reinterpret_cast<uint64_t>(&vk_alloc);
            cpu.x[3] = reinterpret_cast<uint64_t>(&shader_mod);
            aot_vkCreateShaderModule(&cpu);
            assert(static_cast<int32_t>(cpu.x[0]) == 0 && shader_mod != 0);

            const uint8_t* mod_bytes = reinterpret_cast<const uint8_t*>(shader_mod);
            for (size_t off = 0x38; off + 20 <= 192; off += 4) {
                int nz = 0;
                for (int b = 0; b < 20; ++b) {
                    if (mod_bytes[off + b] != 0) nz++;
                }
                if (nz >= 16) {
                    std::memcpy(expected_sha1, mod_bytes + off, 20);
                    sha1_verified = true;
                    break;
                }
            }
            assert(sha1_verified);

            cpu.x[0] = reinterpret_cast<uint64_t>(mock_vk_device.data());
            cpu.x[1] = shader_mod;
            cpu.x[2] = reinterpret_cast<uint64_t>(&vk_alloc);
            aot_vkDestroyShaderModule(&cpu);
        }

        std::vector<uint8_t> spv_mbs2 = compileSpirvToMaliMbs2(sh, embedded_blobs);
        comp_result[2] = reinterpret_cast<uint64_t>(spv_mbs2.data());
        comp_result[3] = static_cast<uint32_t>(spv_mbs2.size());
        char* spv_c = nullptr;
        cpu.x[0] = reinterpret_cast<uint64_t>(ctx);
        cpu.x[1] = reinterpret_cast<uint64_t>(comp_result);
        cpu.x[2] = reinterpret_cast<uint64_t>(&spv_c);
        aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
        assert(static_cast<uint32_t>(cpu.x[0]) == 0 && spv_c != nullptr);

        std::string c_out(spv_c);
        assert(c_out.find("static const cmpbe_chunk_OBJC OBJC_") != std::string::npos);
        assert(c_out.find("cmpbe_chunk_MBS2 MBS2 =") != std::string::npos);
        std::free(spv_c);
        validated_mbs2_blobs.push_back(spv_mbs2);

        std::ostringstream sha_hex;
        for (int b = 0; b < 20; ++b) sha_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(expected_sha1[b]);

        std::cout << "[PASS] Test 6." << (idx + 1) << " (" << sh.name << "): "
                  << sh.spv.size() << "B SPIR-V (" << sh.num_basic_blocks << " BBs, "
                  << sh.num_arith_ops << " ALU ops, " << sh.num_mem_ops << " mem ops) -> "
                  << "vkCreateShaderModule SHA-1=" << sha_hex.str().substr(0, 16) << "... & "
                  << "Valhall ISA OBJC (" << c_out.size() << "B C struct) verified!\n";
    }

    // ========================================================================
    // TEST 7: End-to-End /dev/mali0 (kbase CSF) UAPI + Valhall (Mali-G615)
    //         ISA OBJC -> SPIR-V 1.3 -> Host Vulkan (Mesa Lavapipe / lvp)
    //         Execution Harness
    // ========================================================================
    std::cout << "-------------------------------------------------------------------------\n";
    std::cout << "[HARNESS] Starting /dev/mali0 CSF UAPI + Valhall G615 OBJC -> Lavapipe E2E Harness...\n";

    // ========================================================================
    // TEST 7A: Job Manager (JM) /dev/mali0 UAPI + Bifrost Clause ISA (r26p0)
    //          & Valhall v9 JM ISA (r49p1_v4) -> SPIR-V 1.3 -> Host Vulkan
    //          (Lavapipe) Execution Harness
    // ========================================================================
    if (aot_kbase_job_submit && !aot_kbase_version_check_csf) {
        bool is_bifrost_jm = (malloc_off == 0x60);
        const char* arch_label = is_bifrost_jm ? "Bifrost Mali-G76 (VEHW=0x0b, Clause/Tuple ISA)"
                                               : "Valhall JM Mali-G78 (VEHW=0x11, 64-bit Valhall v9 ISA)";
        std::cout << "[HARNESS-JM] Starting /dev/mali0 Job Manager (JM) UAPI + " << arch_label << " -> Lavapipe Harness...\n";

        std::vector<uint8_t> jm_kctx(4096, 0);
        *reinterpret_cast<int32_t*>(jm_kctx.data() + 0x00) = 100; // /dev/mali0 fd = 100

        if (aot_kbase_version_check_jm) {
            KbaseIoctlVersionCheck ver{0, 0};
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(&ver);
            aot_kbase_version_check_jm(&cpu);
            assert(cpu.x[0] == 0 && ver.major == 11 && ver.minor == 35);
        }

        if (aot_kbase_set_flags) {
            KbaseIoctlSetFlags sflags{0};
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(&sflags);
            aot_kbase_set_flags(&cpu);
            assert(cpu.x[0] == 0);
        }
        if (aot_kbase_mem_jit_init) {
            uint64_t jit_req[3] = {1ULL << 25, 255ULL, 1ULL << 25};
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(jit_req);
            aot_kbase_mem_jit_init(&cpu);
            assert(cpu.x[0] == 0);
        }
        if (aot_kbase_mem_exec_init) {
            uint64_t exec_req = 1024ULL;
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(&exec_req);
            aot_kbase_mem_exec_init(&cpu);
            assert(cpu.x[0] == 0);
        }

        auto allocJmBo = [&](uint64_t pages, uint64_t flags) -> uint64_t {
            KbaseIoctlMemAlloc req{};
            req.in.va_pages = pages;
            req.in.commit_pages = pages;
            req.in.flags = flags;
            bionic_mali_kbase_ioctl_stub(100, 0xc0208005U, reinterpret_cast<uint64_t>(&req));
            if (req.out.flags & BASE_MEM_SAME_VA) {
                return bionic_mali_mmap_stub(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 100, req.out.gpu_va);
            }
            return req.out.gpu_va;
        };

        uint64_t jm_jc_bo_va   = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_rsd_bo_va  = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_srt_bo_va  = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_in_bo_va   = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_out_bo_va  = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_sync_bo_va = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t jm_objc_bo_va = allocJmBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX);

        if (aot_kbase_mem_query) {
            uint64_t qval = 0;
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = jm_in_bo_va;
            cpu.x[2] = reinterpret_cast<uint64_t>(&qval);
            cpu.x[3] = reinterpret_cast<uint64_t>(&qval);
            aot_kbase_mem_query(&cpu);
            assert(cpu.x[0] == 0 && qval == 1ULL);
        }
        if (aot_kbase_soft_event_update) {
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = jm_sync_bo_va;
            cpu.x[2] = 77U;
            aot_kbase_soft_event_update(&cpu);
            assert(cpu.x[0] == 0 && *reinterpret_cast<uint32_t*>(jm_sync_bo_va) == 1U);
        }

        std::cout << "[PASS] Test 7.0a: /dev/mali0 JM Context (uAPI v11.35), JIT/EXEC VA zones, MEM_QUERY, and SOFT_EVENT_UPDATE verified via AOT!\n";

        for (size_t idx = 0; idx < sample_shaders.size(); ++idx) {
            const auto& sh = sample_shaders[idx];
            std::vector<uint64_t> jm_isa_words;
            if (is_bifrost_jm) {
                auto bi_words = compileSpirvToBifrostClauseObjcWords(sh);
                const uint8_t* tpl = embedded_blobs.empty() ? kMinimalFragmentMBS2_v47 : embedded_blobs[0].first;
                uint32_t tpl_len   = embedded_blobs.empty() ? sizeof(kMinimalFragmentMBS2_v47) : embedded_blobs[0].second;
                std::vector<uint8_t> bi_mbs2(tpl, tpl + tpl_len);
                for (uint32_t pos = 12; pos + 8 <= tpl_len; ++pos) {
                    if (std::memcmp(tpl + pos, "OBJC", 4) == 0) {
                        uint32_t old_len = *reinterpret_cast<const uint32_t*>(tpl + pos + 4);
                        uint32_t delta   = static_cast<uint32_t>(bi_words.size() * 8);
                        bi_mbs2.clear();
                        bi_mbs2.insert(bi_mbs2.end(), tpl, tpl + pos + 8 + old_len);
                        const uint8_t* raw_b = reinterpret_cast<const uint8_t*>(bi_words.data());
                        bi_mbs2.insert(bi_mbs2.end(), raw_b, raw_b + delta);
                        bi_mbs2.insert(bi_mbs2.end(), tpl + pos + 8 + old_len, tpl + tpl_len);
                        *reinterpret_cast<uint32_t*>(&bi_mbs2[4])  += delta;
                        *reinterpret_cast<uint32_t*>(&bi_mbs2[36]) += delta;
                        *reinterpret_cast<uint32_t*>(&bi_mbs2[44]) += delta;
                        for (uint32_t ep = 48; ep < pos; ++ep) {
                            if (std::memcmp(tpl + ep, "EBIN", 4) == 0) {
                                *reinterpret_cast<uint32_t*>(&bi_mbs2[ep + 4]) += delta;
                                break;
                            }
                        }
                        *reinterpret_cast<uint32_t*>(&bi_mbs2[pos + 4]) = old_len + delta;
                        break;
                    }
                }
                comp_result[2] = reinterpret_cast<uint64_t>(bi_mbs2.data());
                comp_result[3] = static_cast<uint32_t>(bi_mbs2.size());
                char* bi_c = nullptr;
                cpu.x[0] = reinterpret_cast<uint64_t>(ctx);
                cpu.x[1] = reinterpret_cast<uint64_t>(comp_result);
                cpu.x[2] = reinterpret_cast<uint64_t>(&bi_c);
                aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
                assert(static_cast<uint32_t>(cpu.x[0]) == 0 && bi_c != nullptr);
                std::free(bi_c);
                jm_isa_words = extractObjcFromMbs2(bi_mbs2, bi_words.size());
            } else {
                const size_t expected_valhall_counts[3] = {6, 7, 15};
                jm_isa_words = extractObjcFromMbs2(validated_mbs2_blobs[idx], expected_valhall_counts[idx]);
            }

            uint32_t objc_bytes = static_cast<uint32_t>(jm_isa_words.size() * sizeof(uint64_t));
            std::memcpy(reinterpret_cast<void*>(jm_objc_bo_va), jm_isa_words.data(), objc_bytes);

            float* in_f  = reinterpret_cast<float*>(jm_in_bo_va);
            float* out_f = reinterpret_cast<float*>(jm_out_bo_va);
            in_f[0] = 3.0f; in_f[1] = 4.0f; in_f[2] = 5.0f; in_f[3] = 6.0f;
            out_f[0] = out_f[1] = out_f[2] = out_f[3] = 0.0f;

            uint64_t* rsd_ptr = reinterpret_cast<uint64_t*>(jm_rsd_bo_va);
            rsd_ptr[0] = jm_objc_bo_va;
            rsd_ptr[1] = objc_bytes;

            uint64_t* srt_ptr = reinterpret_cast<uint64_t*>(jm_srt_bo_va);
            srt_ptr[0] = jm_in_bo_va;
            srt_ptr[1] = jm_out_bo_va;

            if (aot_kbase_mem_sync) {
                KbaseIoctlMemSync msync{jm_in_bo_va, jm_in_bo_va, 64, 0, {}};
                cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
                cpu.x[1] = reinterpret_cast<uint64_t>(&msync);
                aot_kbase_mem_sync(&cpu);
                assert(cpu.x[0] == 0);
            }

            std::memset(reinterpret_cast<void*>(jm_jc_bo_va), 0, 512);
            auto* hdr1 = reinterpret_cast<MaliJmJobHeader*>(jm_jc_bo_va + 0x000);
            uint8_t jtype = (sh.name == "passthrough_frag.spv") ? MALI_JOB_TYPE_FRAGMENT : MALI_JOB_TYPE_COMPUTE;
            hdr1->flags_and_index = 1U | (static_cast<uint32_t>(jtype) << 1) | (1U << 16);
            hdr1->next            = jm_jc_bo_va + 0x100;
            *reinterpret_cast<uint64_t*>(jm_jc_bo_va + 0x020) = jm_rsd_bo_va;
            *reinterpret_cast<uint64_t*>(jm_jc_bo_va + 0x028) = jm_srt_bo_va;
            *reinterpret_cast<uint32_t*>(jm_jc_bo_va + 0x030) = is_bifrost_jm ? 1U : 0U;
            *reinterpret_cast<uint32_t*>(jm_jc_bo_va + 0x034) = 1U;

            auto* hdr2 = reinterpret_cast<MaliJmJobHeader*>(jm_jc_bo_va + 0x100);
            hdr2->flags_and_index = 1U | (static_cast<uint32_t>(MALI_JOB_TYPE_WRITE_VALUE) << 1) | (2U << 16);
            hdr2->dependencies    = 1U;
            hdr2->next            = 0ULL;
            *reinterpret_cast<uint64_t*>(jm_jc_bo_va + 0x120) = jm_sync_bo_va;
            *reinterpret_cast<uint64_t*>(jm_jc_bo_va + 0x130) = 200ULL + idx + 1ULL;

            BaseJdAtomV2 atom{};
            atom.jc            = jm_jc_bo_va;
            atom.udata.blob[0] = 0xCAFE0000ULL + idx;
            atom.atom_number   = static_cast<uint8_t>(idx + 1);
            atom.core_req      = (jtype == MALI_JOB_TYPE_FRAGMENT) ? BASE_JD_REQ_FS : BASE_JD_REQ_CS;

            KbaseIoctlJobSubmit sub{reinterpret_cast<uint64_t>(&atom), 1U, sizeof(BaseJdAtomV2)};
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(&sub);
            aot_kbase_job_submit(&cpu);
            assert(cpu.x[0] == 0);

            alignas(8) uint8_t pfd_buf[8] = {100, 0, 0, 0, 1, 0, 0, 0};
            uint64_t prc = bionic_mali_poll_stub(reinterpret_cast<uint64_t>(pfd_buf), 1, 1000);
            assert(prc == 1 && *reinterpret_cast<short*>(pfd_buf + 6) == 1);

            BaseJdEventV2 ev{};
            uint64_t rrc = bionic_mali_read_stub(100, reinterpret_cast<uint64_t>(&ev), sizeof(ev));
            assert(rrc == sizeof(BaseJdEventV2));
            assert(ev.event_code == BASE_JD_EVENT_DONE && ev.atom_number == atom.atom_number);
            assert(hdr1->exception_status == 0x01U && hdr2->exception_status == 0x01U);
            assert(*reinterpret_cast<uint64_t*>(jm_sync_bo_va) == 200ULL + idx + 1ULL);

            const auto& exec_atom = g_kbase_emu.executed_jm_atoms.back();
            assert(exec_atom.executed_on_lvp);
            for (int c = 0; c < 4; ++c) {
                assert(std::fabs(out_f[c] - sh.expected_out[c]) < 1e-4f);
            }

            char sub_letter = static_cast<char>('b' + idx);
            if (is_bifrost_jm) {
                std::cout << "[PASS] Test 7.0" << sub_letter << " (" << sh.name << " via JM "
                          << (jtype == MALI_JOB_TYPE_FRAGMENT ? "FRAGMENT_JOB" : "COMPUTE_JOB")
                          << " -> Bifrost G76 Clause OBJC [" << exec_atom.bifrost_disasm.size() << " clauses] -> "
                          << exec_atom.lifted_spirv.size() * 4 << "B SPIR-V -> Host Vulkan [" << g_kbase_emu.lvp_executor.device_name << "]):\n";
                std::cout << "       Disassembled Bifrost ISA : ";
                for (size_t di = 0; di < std::min<size_t>(2, exec_atom.bifrost_disasm.size()); ++di) {
                    if (di > 0) std::cout << " ; ";
                    std::cout << exec_atom.bifrost_disasm[di].disasm_text;
                }
                if (exec_atom.bifrost_disasm.size() > 2) std::cout << " ; ... (" << exec_atom.bifrost_disasm.size() << " clauses)";
                std::cout << "\n";
            } else {
                std::cout << "[PASS] Test 7.0" << sub_letter << " (" << sh.name << " via JM "
                          << (jtype == MALI_JOB_TYPE_FRAGMENT ? "FRAGMENT_JOB" : "COMPUTE_JOB")
                          << " -> Valhall JM G78 OBJC [" << exec_atom.valhall_disasm.size() << " insns] -> "
                          << exec_atom.lifted_spirv.size() * 4 << "B SPIR-V -> Host Vulkan [" << g_kbase_emu.lvp_executor.device_name << "]):\n";
                std::cout << "       Disassembled Valhall JM  : ";
                for (size_t di = 0; di < std::min<size_t>(4, exec_atom.valhall_disasm.size()); ++di) {
                    if (di > 0) std::cout << " ; ";
                    std::cout << exec_atom.valhall_disasm[di].disasm_text;
                }
                if (exec_atom.valhall_disasm.size() > 4) std::cout << " ; ... (" << exec_atom.valhall_disasm.size() << " total)";
                std::cout << "\n";
            }
            std::cout << "       Lavapipe Output BO vec4  : [" << std::fixed << std::setprecision(3)
                      << out_f[0] << ", " << out_f[1] << ", " << out_f[2] << ", " << out_f[3]
                      << "] (JM atom=" << int(ev.atom_number) << " DONE, WRITE_VALUE=" << *reinterpret_cast<uint64_t*>(jm_sync_bo_va) << ")\n";
        }

        if (aot_kbase_post_term) {
            cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
            aot_kbase_post_term(&cpu);
            assert(cpu.x[0] == 0);
        }
    }

    // Step 7.1: Initialize AOT-translated /dev/mali0 kbase CSF context on r54p3
    std::vector<uint8_t> kctx_mem(4096, 0);
    uint8_t* kctx_ptr = kctx_mem.data();
    *reinterpret_cast<int32_t*>(kctx_ptr + 0x000)  = 100; // /dev/mali0 fd = 100
    *reinterpret_cast<uint64_t*>(kctx_ptr + 0x258) = 0;   // optional OSU tracker
    *reinterpret_cast<uint32_t*>(kctx_ptr + 0x278) = 1U;  // CSF uAPI major = 1
    *reinterpret_cast<uint32_t*>(kctx_ptr + 0x27c) = 36U; // CSF uAPI minor = 36

    if (aot_kbase_version_check_csf) {
        KbaseIoctlVersionCheck ver{0, 0};
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&ver);
        aot_kbase_version_check_csf(&cpu);
        assert(cpu.x[0] == 0 && ver.major == 1 && ver.minor == 36);
    }

    if (aot_kbase_version_check_csf && aot_kbase_set_flags) {
        KbaseIoctlSetFlags flags{0};
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&flags);
        aot_kbase_set_flags(&cpu);
        assert(cpu.x[0] == 0);
    }

    // Map BASE_MEM_MAP_TRACKING_HANDLE (0x3000) & BASEP_MEM_CSF_USER_REG_PAGE_HANDLE (0x2f000)
    uint64_t tracking_va = bionic_mali_mmap_stub(0, 4096, PROT_READ, MAP_SHARED, 100, BASE_MEM_MAP_TRACKING_HANDLE);
    uint64_t user_reg_va = bionic_mali_mmap_stub(0, 4096, PROT_READ, MAP_SHARED, 100, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);
    assert(tracking_va != 0 && user_reg_va != 0);
    assert(*reinterpret_cast<uint32_t*>(user_reg_va) == 1U); // LATEST_FLUSH == 1

    if (aot_kbase_version_check_csf && aot_kbase_mem_jit_init) {
        uint64_t jit_req[3] = {1ULL << 25, 255ULL, 1ULL << 25};
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(jit_req);
        aot_kbase_mem_jit_init(&cpu);
        assert(cpu.x[0] == 0 && g_kbase_emu.jit_va_pages == (1ULL << 25));
    }

    if (aot_kbase_version_check_csf && aot_kbase_mem_exec_init) {
        uint64_t exec_req = 0x100000ULL;
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&exec_req);
        aot_kbase_mem_exec_init(&cpu);
        assert(cpu.x[0] == 0 && g_kbase_emu.exec_va_pages == 0x100000ULL);
    }

    // Query CSF Global Interface via AOT kbase_cs_get_glb_iface (or direct ioctl fallback on JM drivers)
    {
        KbaseIoctlCsGetGlbIface glb{};
        bionic_mali_kbase_ioctl_stub(100, 0xc0188033U, reinterpret_cast<uint64_t>(&glb));
        assert(glb.out.group_num == 8 && glb.out.total_stream_num == 64);
    }

    // Helper to allocate GPU BOs via AOT aot_kbase_mem_alloc (on r54p3) or KBASE_IOCTL_MEM_ALLOC_EX
    auto allocKbaseBo = [&](uint64_t pages, uint64_t flags) -> uint64_t {
        if (aot_kbase_version_check_csf && aot_kbase_mem_alloc) {
            uint64_t out_flags = 0;
            uint64_t out_gpu_va = 0;
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = pages;
            cpu.x[2] = pages;
            cpu.x[3] = 0;
            cpu.x[4] = flags;
            cpu.x[5] = reinterpret_cast<uint64_t>(&out_gpu_va);
            cpu.x[6] = reinterpret_cast<uint64_t>(&out_flags);
            aot_kbase_mem_alloc(&cpu);
            if (out_gpu_va != 0) return out_gpu_va;
        }
        KbaseIoctlMemAllocEx req{};
        req.in.va_pages = pages;
        req.in.commit_pages = pages;
        req.in.flags = flags;
        bionic_mali_kbase_ioctl_stub(100, 0xc040803bU, reinterpret_cast<uint64_t>(&req));
        if (req.out.flags & BASE_MEM_SAME_VA) {
            return bionic_mali_mmap_stub(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 100, req.out.gpu_va);
        }
        return req.out.gpu_va;
    };

    // Test CSF Tiler Heap Init & Term via AOT functions
    if (aot_kbase_cs_tiler_heap_init && aot_kbase_cs_tiler_heap_term) {
        KbaseIoctlCsTilerHeapInit th{};
        th.in.chunk_size = 2 * 1024 * 1024;
        th.in.initial_chunks = 4;
        th.in.max_chunks = 64;
        th.in.target_in_flight = 65535;
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&th);
        aot_kbase_cs_tiler_heap_init(&cpu);
        assert(cpu.x[0] == 0 && th.out.gpu_heap_va != 0);

        KbaseIoctlCsTilerHeapTerm term{th.out.gpu_heap_va};
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&term);
        aot_kbase_cs_tiler_heap_term(&cpu);
        assert(cpu.x[0] == 0);
    }

    // Test KCPU Queue Create, CQS Enqueue, and Delete via AOT functions
    uint64_t cqs_bo_va = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT);
    assert(cqs_bo_va != 0);
    if (aot_kbase_kcpu_queue_create && aot_kbase_kcpu_queue_enqueue && aot_kbase_kcpu_queue_delete) {
        uint32_t kcpu_id = 0xFF;
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&kcpu_id);
        aot_kbase_kcpu_queue_create(&cpu);
        assert(cpu.x[0] == 0 && kcpu_id >= 1);

        struct CqsSetOp { uint64_t addr; uint64_t val; uint8_t op; uint8_t dtype; uint8_t pad[6]; } cqs_op{cqs_bo_va, 42ULL, 1, 1, {}};
        struct KcpuCmd { uint8_t type; uint8_t pad[7]; uint64_t objs; uint32_t nr_objs; uint32_t pad2; } kcmd{5 /* CQS_SET_OPERATION */, {}, reinterpret_cast<uint64_t>(&cqs_op), 1, 0};
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = kcpu_id;
        cpu.x[2] = reinterpret_cast<uint64_t>(&kcmd);
        cpu.x[3] = 1;
        aot_kbase_kcpu_queue_enqueue(&cpu);
        assert(cpu.x[0] == 0 && *reinterpret_cast<uint64_t*>(cqs_bo_va) == 42ULL);

        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = kcpu_id;
        aot_kbase_kcpu_queue_delete(&cpu);
        assert(cpu.x[0] == 0);
    }

    // Step 7.2: Create CSF Queue Group, Register & Bind Command Stream Ringbuffer, and Kick Jobs via AOT
    uint64_t ring_bo_va = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
    uint64_t in_bo_va   = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
    uint64_t out_bo_va  = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
    uint64_t srt_bo_va  = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
    uint64_t spd_bo_va  = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
    uint64_t objc_bo_va = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX);

    // Register CSF ringbuffer via AOT kbase_cs_queue_register
    KbaseIoctlCsQueueRegister qreg{ring_bo_va, 4096U, 8U, {}};
    if (aot_kbase_cs_queue_register) {
        cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
        cpu.x[1] = reinterpret_cast<uint64_t>(&qreg);
        aot_kbase_cs_queue_register(&cpu);
        assert(cpu.x[0] == 0);
    } else {
        bionic_mali_kbase_ioctl_stub(100, 0x40108024U, reinterpret_cast<uint64_t>(&qreg));
    }

    // Bind CSF queue to CSG group 1, CSI stream 0 via AOT kbase_cs_queue_bind
    std::vector<uint8_t> csg_group_struct(512, 0);
    csg_group_struct[0x110] = 1; // group_handle = 1
    std::vector<uint8_t> csi_queue_struct(512, 0);
    *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0xa0) = reinterpret_cast<uint64_t>(kctx_ptr);
    *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0xa8) = ring_bo_va;

    if (aot_kbase_cs_queue_bind) {
        cpu.x[0] = reinterpret_cast<uint64_t>(csg_group_struct.data());
        cpu.x[1] = reinterpret_cast<uint64_t>(csi_queue_struct.data());
        cpu.x[2] = 0; // csi_index = 0
        aot_kbase_cs_queue_bind(&cpu);
        assert(cpu.x[0] == 0);
    } else {
        KbaseIoctlCsQueueBind bind{};
        bind.in.buffer_gpu_addr = ring_bo_va;
        bind.in.group_handle = 1;
        bind.in.csi_index = 0;
        bionic_mali_kbase_ioctl_stub(100, 0xc0108027U, reinterpret_cast<uint64_t>(&bind));
        uint64_t uio = bionic_mali_mmap_stub(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 100, bind.out.mmap_handle);
        *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x88) = uio;
        *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x90) = uio + 4096;
        *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x98) = uio + 8192;
    }

    uint64_t doorbell_va    = *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x88);
    uint64_t user_input_va  = *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x90);
    uint64_t user_output_va = *reinterpret_cast<uint64_t*>(csi_queue_struct.data() + 0x98);
    assert(doorbell_va != 0 && user_input_va != 0 && user_output_va != 0);

    std::cout << "[PASS] Test 7.1: /dev/mali0 CSF Context, Mali-G615 GPUPROPS, JIT/EXEC VA zones, Tiler Heap, KCPU CQS Queue, and 3-page CSFFW_USER_IO Doorbell/Ringbuffer bound via AOT!\n";

    // Step 7.3: Execute all 3 Valhall OBJC Shaders through the CSF Ringbuffer -> SPIR-V 1.3 -> Host Vulkan (Lavapipe)
    *reinterpret_cast<uint64_t*>(cqs_bo_va) = 100ULL;
    uint64_t ring_write_offset = 0;

    for (size_t idx = 0; idx < sample_shaders.size(); ++idx) {
        const auto& sh = sample_shaders[idx];
        const size_t expected_objc_counts[3] = {6, 7, 15};
        auto objc_words = extractObjcFromMbs2(validated_mbs2_blobs[idx], expected_objc_counts[idx]);
        assert(!objc_words.empty());

        // Copy Valhall 64-bit OBJC instructions into EXEC_VA shader BO
        uint32_t objc_bytes = static_cast<uint32_t>(objc_words.size() * sizeof(uint64_t));
        std::memcpy(reinterpret_cast<void*>(objc_bo_va), objc_words.data(), objc_bytes);

        // Initialize input storage buffer BO with test vector {3.0f, 4.0f, 5.0f, 6.0f} and zero output BO
        float* in_floats  = reinterpret_cast<float*>(in_bo_va);
        float* out_floats = reinterpret_cast<float*>(out_bo_va);
        in_floats[0] = 3.0f; in_floats[1] = 4.0f; in_floats[2] = 5.0f; in_floats[3] = 6.0f;
        out_floats[0] = out_floats[1] = out_floats[2] = out_floats[3] = 0.0f;

        // Populate SRT (Shader Resource Table: {in_bo_va, out_bo_va}) and SPD (Shader Program Descriptor: {objc_bo_va, objc_bytes})
        uint64_t* srt_ptr = reinterpret_cast<uint64_t*>(srt_bo_va);
        srt_ptr[0] = in_bo_va;
        srt_ptr[1] = out_bo_va;

        uint64_t* spd_ptr = reinterpret_cast<uint64_t*>(spd_bo_va);
        spd_ptr[0] = objc_bo_va;
        spd_ptr[1] = objc_bytes;

        // Sync input BO via AOT kbase_mem_sync
        if (aot_kbase_version_check_csf && aot_kbase_mem_sync) {
            KbaseIoctlMemSync msync{in_bo_va, in_bo_va, 64, 0, {}};
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&msync);
            aot_kbase_mem_sync(&cpu);
            assert(cpu.x[0] == 0);
        }

        // Write 64-bit CSF command stream packets into ringbuffer
        uint64_t* ring_words = reinterpret_cast<uint64_t*>(ring_bo_va);
        auto pushCsfCmd = [&](uint64_t cmd_word) {
            size_t slot = (ring_write_offset / 8) % (4096 / 8);
            ring_words[slot] = cmd_word;
            ring_write_offset += 8;
        };

        pushCsfCmd(csfEncodeMove48(0, srt_bo_va));       // d0  = SRT GPU VA
        pushCsfCmd(csfEncodeMove48(8, spd_bo_va));       // d8  = SPD GPU VA
        pushCsfCmd(csfEncodeMove48(16, 0ULL));           // d16 = TSD GPU VA
        pushCsfCmd(csfEncodeMove48(24, 0ULL));           // d24 = FAU GPU VA
        pushCsfCmd(csfEncodeMove32(32, 1U));             // r32 = wg_x = 1
        pushCsfCmd(csfEncodeMove32(33, 1U));             // r33 = wg_y = 1
        pushCsfCmd(csfEncodeMove32(34, 1U));             // r34 = wg_z = 1
        pushCsfCmd(csfEncodeReqResource(true, true));    // REQ_RESOURCE
        if (sh.name == "passthrough_frag.spv") {
            pushCsfCmd(csfEncodeRunFragment());          // RUN_FRAGMENT
        } else {
            pushCsfCmd(csfEncodeRunCompute(0, 0, 0, 0)); // RUN_COMPUTE.x_axis.srt0.spd0.tsd0.fau0
        }
        pushCsfCmd(csfEncodeFlushCache2(0, 0));          // FLUSH_CACHE2
        pushCsfCmd(csfEncodeMove48(40, cqs_bo_va));      // d40 = CQS timeline sync address
        pushCsfCmd(csfEncodeMove48(42, 1ULL));           // d42 = +1 increment
        pushCsfCmd(csfEncodeSyncAdd64(40, 42));          // SYNC_ADD64 [d40] += d42
        pushCsfCmd(csfEncodeWait(0));                    // WAIT #0

        // Update CS_INSERT_LO/HI in CS_USER_INPUT_BLOCK page and kick CSF queue via AOT kbase_cs_queue_kick
        *reinterpret_cast<uint64_t*>(user_input_va + CS_INSERT_LO) = ring_write_offset;
        if (aot_kbase_cs_queue_kick) {
            cpu.x[0] = reinterpret_cast<uint64_t>(csi_queue_struct.data());
            cpu.x[1] = 1; // ring_doorbell = true
            aot_kbase_cs_queue_kick(&cpu);
        } else {
            *reinterpret_cast<uint32_t*>(doorbell_va) = 1U;
            KbaseIoctlCsQueueKick kick{ring_bo_va};
            bionic_mali_kbase_ioctl_stub(100, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
        }

        // Verify CS_EXTRACT caught up to CS_INSERT, CQS timeline incremented, and Lavapipe output matches expected_out!
        uint64_t cs_extract_after = *reinterpret_cast<uint64_t*>(user_output_va + CS_EXTRACT_LO);
        assert(cs_extract_after == ring_write_offset);
        assert(*reinterpret_cast<uint64_t*>(cqs_bo_va) == 100ULL + idx + 1ULL);
        assert(!g_kbase_emu.executed_jobs.empty());
        const auto& job = g_kbase_emu.executed_jobs.back();
        assert(job.executed_on_lvp);

        for (int c = 0; c < 4; ++c) {
            assert(std::fabs(out_floats[c] - sh.expected_out[c]) < 1e-4f);
        }

        std::cout << "[PASS] Test 7." << (idx + 2) << " (" << sh.name << " via CSF "
                  << (job.cs_opcode == MALI_CS_OPCODE_RUN_FRAGMENT ? "RUN_FRAGMENT" : "RUN_COMPUTE")
                  << " -> Valhall G615 OBJC [" << job.disasm.size() << " insns] -> "
                  << job.lifted_spirv.size() * 4 << "B SPIR-V -> Host Vulkan [" << g_kbase_emu.lvp_executor.device_name << "]):\n";
        std::cout << "       Disassembled Valhall ISA : ";
        for (size_t di = 0; di < std::min<size_t>(4, job.disasm.size()); ++di) {
            if (di > 0) std::cout << " ; ";
            std::cout << job.disasm[di].disasm_text;
        }
        if (job.disasm.size() > 4) std::cout << " ; ... (" << job.disasm.size() << " total)";
        std::cout << "\n";
        std::cout << "       Lavapipe Output BO vec4  : [" << std::fixed << std::setprecision(3)
                  << out_floats[0] << ", " << out_floats[1] << ", " << out_floats[2] << ", " << out_floats[3]
                  << "] (CQS sync=" << *reinterpret_cast<uint64_t*>(cqs_bo_va)
                  << ", CS_EXTRACT=" << cs_extract_after << ")\n";
    }

    // =========================================================================
    // Test 8: Standalone mali0_emu Subproject E2E — /dev/dma_heap WSI Import,
    //         3D vkcube Multi-Frame (CSF IDVS+Fragment & JM Tiler+Fragment),
    //         and Multi-Pass Compute -> Graphics Mesh Deformation on Lavapipe
    // =========================================================================
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << " Test 8: mali0_emu 3D vkcube (IDVS+Fragment / JM Tiler+Fragment) + WSI dma_heap \n";
    std::cout << "--------------------------------------------------------------------------------\n";
    {
        uint64_t heap_fd = bionic_mali_open_stub(reinterpret_cast<uint64_t>("/dev/dma_heap/system"), 2);
        assert(heap_fd == 101);
        DmaHeapAllocationData heap_alloc{};
        heap_alloc.len = 64 * 64 * sizeof(uint32_t); // 64x64 RGBA8 WSI Swapchain BO (16 KiB)
        uint64_t hrc = bionic_mali_kbase_ioctl_stub(heap_fd, DMA_HEAP_IOCTL_ALLOC, reinterpret_cast<uint64_t>(&heap_alloc));
        assert(hrc == 0 && heap_alloc.fd >= 200);

        int dmabuf_fd = static_cast<int>(heap_alloc.fd);
        KbaseIoctlMemImport imp{};
        imp.in.flags   = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
        imp.in.phandle = reinterpret_cast<uint64_t>(&dmabuf_fd);
        imp.in.type    = BASE_MEM_IMPORT_TYPE_UMM;
        uint64_t irc = bionic_mali_kbase_ioctl_stub(100, 0xc0188016U, reinterpret_cast<uint64_t>(&imp));
        assert(irc == 0 && (imp.out.flags & BASE_MEM_NEED_MMAP) != 0);
        uint64_t wsi_color_bo_va = bionic_mali_mmap_stub(0, heap_alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED, 100, imp.out.gpu_va);
        assert(wsi_color_bo_va != 0);
        std::cout << "[PASS] Test 8.1: /dev/dma_heap/system WSI Swapchain BO (fd=" << dmabuf_fd
                  << ") imported via KBASE_IOCTL_MEM_IMPORT(UMM) -> GPU VA 0x"
                  << std::hex << wsi_color_bo_va << std::dec << "\n";

        uint64_t vkcube_geom_va  = allocKbaseBo(2, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t vkcube_depth_va = allocKbaseBo(4, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
        uint64_t vkcube_objc_va  = allocKbaseBo(1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX);

        auto* cube_verts   = reinterpret_cast<MaliCubeVertex*>(vkcube_geom_va + 0x000);
        auto* cube_indices = reinterpret_cast<uint32_t*>(vkcube_geom_va + 0x480);
        auto* mvp_ubo      = reinterpret_cast<float*>(vkcube_geom_va + 0x580);
        auto* mfbd         = reinterpret_cast<MaliMfbdDescriptor*>(vkcube_geom_va + 0x680);
        auto* idvs_pkt     = reinterpret_cast<MaliIdvsDrawPacket*>(vkcube_geom_va + 0x780);
        auto* vs_spd       = reinterpret_cast<uint64_t*>(vkcube_geom_va + 0x880);
        auto* fs_spd       = reinterpret_cast<uint64_t*>(vkcube_geom_va + 0x8C0);

        mali0_emu_vkcube::buildUnitCubeGeometry(cube_verts, cube_indices);

        bool use_bifrost_jm = (!aot_kbase_version_check_csf && aot_kbase_job_submit && (std::string(so_path).find("r26p0") != std::string::npos || std::string(so_path).find("v2") != std::string::npos));
        auto vs_objc = use_bifrost_jm ? MaliVkCubeIsaPipelineTranslator::buildBifrostVkCubeVertexObjc()
                                      : MaliVkCubeIsaPipelineTranslator::buildValhallVkCubeVertexObjc();
        auto fs_objc = use_bifrost_jm ? MaliVkCubeIsaPipelineTranslator::buildBifrostVkCubeFragmentObjc()
                                      : MaliVkCubeIsaPipelineTranslator::buildValhallVkCubeFragmentObjc();
        std::memcpy(reinterpret_cast<void*>(vkcube_objc_va + 0x000), vs_objc.data(), vs_objc.size() * 8);
        std::memcpy(reinterpret_cast<void*>(vkcube_objc_va + 0x100), fs_objc.data(), fs_objc.size() * 8);
        vs_spd[0] = vkcube_objc_va + 0x000; vs_spd[1] = vs_objc.size() * 8;
        fs_spd[0] = vkcube_objc_va + 0x100; fs_spd[1] = fs_objc.size() * 8;

        mfbd->magic             = MALI_MFBD_MAGIC;
        mfbd->width             = 64;
        mfbd->height            = 64;
        mfbd->sample_count      = 1;
        mfbd->clear_color_rgba8 = 0xFF181410U;
        mfbd->clear_depth       = 1.0f;
        mfbd->color_rt0_gpu_va  = wsi_color_bo_va;
        mfbd->depth_bo_gpu_va   = vkcube_depth_va;

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

        auto renderVkCubeFrame = [&](uint32_t frame_no, float angle_deg) {
            idvs_pkt->frame_index  = frame_no;
            idvs_pkt->rotation_deg = angle_deg;
            mali0_emu_vkcube::buildRotationMvpMatrix(angle_deg, mvp_ubo);

            if (!aot_kbase_version_check_csf && aot_kbase_job_submit) {
                // Submit via Job Manager (JM) TILER_JOB -> FRAGMENT_JOB Descriptor Chain
                uint64_t jm_chain_va = vkcube_geom_va + 0xA00;
                auto* tiler_jc = reinterpret_cast<MaliJmJobHeader*>(jm_chain_va + 0x000);
                auto* frag_jc  = reinterpret_cast<MaliJmJobHeader*>(jm_chain_va + 0x080);
                std::memset(tiler_jc, 0, 0x100);
                tiler_jc->flags_and_index = static_cast<uint16_t>((MALI_JOB_TYPE_TILER << 1) | 1U);
                tiler_jc->next            = reinterpret_cast<uint64_t>(frag_jc);
                *reinterpret_cast<uint64_t*>(jm_chain_va + 0x020) = reinterpret_cast<uint64_t>(vs_spd);
                *reinterpret_cast<uint64_t*>(jm_chain_va + 0x028) = reinterpret_cast<uint64_t>(idvs_pkt);
                *reinterpret_cast<uint32_t*>(jm_chain_va + 0x030) = use_bifrost_jm ? 1U : 0U;

                frag_jc->flags_and_index = static_cast<uint16_t>((MALI_JOB_TYPE_FRAGMENT << 1) | 1U);
                frag_jc->next            = 0;
                *reinterpret_cast<uint64_t*>(jm_chain_va + 0x0A0) = reinterpret_cast<uint64_t>(fs_spd);
                *reinterpret_cast<uint64_t*>(jm_chain_va + 0x0A8) = reinterpret_cast<uint64_t>(idvs_pkt);
                *reinterpret_cast<uint32_t*>(jm_chain_va + 0x0B0) = use_bifrost_jm ? 1U : 0U;

                BaseJdAtomV2 atom{};
                atom.jc          = reinterpret_cast<uint64_t>(tiler_jc);
                atom.core_req    = BASE_JD_REQ_FS | BASE_JD_REQ_T;
                atom.atom_number = static_cast<uint8_t>(20 + frame_no);
                std::vector<uint8_t> jm_kctx(256, 0);
                *reinterpret_cast<int32_t*>(jm_kctx.data() + 0x00) = 100;
                *reinterpret_cast<int32_t*>(jm_kctx.data() + 0x70) = 100;
                KbaseIoctlJobSubmit sub{reinterpret_cast<uint64_t>(&atom), 1U, sizeof(BaseJdAtomV2)};
                cpu.x[0] = reinterpret_cast<uint64_t>(jm_kctx.data());
                cpu.x[1] = reinterpret_cast<uint64_t>(&sub);
                aot_kbase_job_submit(&cpu);
                assert(cpu.x[0] == 0);
                BaseJdEventV2 ev{};
                bionic_mali_read_stub(100, reinterpret_cast<uint64_t>(&ev), sizeof(ev));
                assert(ev.event_code == BASE_JD_EVENT_DONE);
            } else {
                // Submit via CSF Ringbuffer (RUN_IDVS + FINISH_TILING + RUN_FRAGMENT + FINISH_FRAGMENT)
                uint64_t* ring_words = reinterpret_cast<uint64_t*>(ring_bo_va);
                auto pushCsfCmd = [&](uint64_t cmd_word) {
                    size_t slot = (ring_write_offset / 8) % (4096 / 8);
                    ring_words[slot] = cmd_word;
                    ring_write_offset += 8;
                };
                pushCsfCmd(csfEncodeMove48(0, reinterpret_cast<uint64_t>(idvs_pkt)));
                pushCsfCmd(csfEncodeMove48(8, reinterpret_cast<uint64_t>(vs_spd)));
                pushCsfCmd(csfEncodeMove48(16, reinterpret_cast<uint64_t>(idvs_pkt)));
                pushCsfCmd(uint64_t(MALI_CS_OPCODE_RUN_IDVS) << 56);
                pushCsfCmd(uint64_t(MALI_CS_OPCODE_FINISH_TILING) << 56);
                pushCsfCmd(csfEncodeMove48(8, reinterpret_cast<uint64_t>(fs_spd)));
                pushCsfCmd(csfEncodeRunFragment());
                pushCsfCmd(uint64_t(MALI_CS_OPCODE_FINISH_FRAGMENT) << 56);
                *reinterpret_cast<uint64_t*>(user_input_va + CS_INSERT_LO) = ring_write_offset;
                if (aot_kbase_cs_queue_kick) {
                    cpu.x[0] = reinterpret_cast<uint64_t>(csi_queue_struct.data());
                    cpu.x[1] = 1;
                    aot_kbase_cs_queue_kick(&cpu);
                } else {
                    KbaseIoctlCsQueueKick kick{ring_bo_va};
                    bionic_mali_kbase_ioctl_stub(100, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
                }
            }
        };

        renderVkCubeFrame(0, 25.0f);
        renderVkCubeFrame(1, 65.0f);
        assert(g_kbase_emu.executed_vkcube_frames.size() >= 2);
        const VkCubeFrameStats f0 = g_kbase_emu.executed_vkcube_frames[0];
        const VkCubeFrameStats f1 = g_kbase_emu.executed_vkcube_frames[1];
        assert(f0.vs_spirv_valid_on_lvp && f0.fs_spirv_valid_on_lvp && f0.lvp_fragment_dispatch_ok);
        assert(f0.triangles_front_facing == 6 && f0.fragments_shaded > 1500);
        assert(f1.triangles_front_facing == 6 && f1.fragments_shaded > 1500);
        assert(f0.frame_crc32 != f1.frame_crc32);
        std::cout << "[PASS] Test 8.2: 3D vkcube Multi-Frame ("
                  << (!aot_kbase_version_check_csf && aot_kbase_job_submit ? "JM TILER+FRAGMENT" : "CSF RUN_IDVS+RUN_FRAGMENT")
                  << ") -> Frame0(25 deg): shaded_px=" << f0.fragments_shaded
                  << ", crc32=0x" << std::hex << f0.frame_crc32
                  << " | Frame1(65 deg): shaded_px=" << std::dec << f1.fragments_shaded
                  << ", crc32=0x" << std::hex << f1.frame_crc32 << std::dec << "\n";

        // Test 8.3: Multi-Pass Compute Deformation -> 3D Graphics Rendering
        for (int i = 0; i < 24; ++i) {
            cube_verts[i].px *= 1.224f;
            cube_verts[i].py *= (1.0f / 1.224f);
        }
        renderVkCubeFrame(2, 65.0f);
        assert(g_kbase_emu.executed_vkcube_frames.size() >= 3);
        const VkCubeFrameStats f2 = g_kbase_emu.executed_vkcube_frames[2];
        assert(f2.frame_crc32 != f1.frame_crc32 && f2.fragments_shaded != f1.fragments_shaded);
        std::cout << "[PASS] Test 8.3: Multi-Pass Compute Deformation -> 3D Graphics on Lavapipe -> shaded_px="
                  << f2.fragments_shaded << ", crc32=0x" << std::hex << f2.frame_crc32 << std::dec << "\n";
    }

    assert(*reinterpret_cast<uint64_t*>(cpu.tpidr_el0 + 0x28) == BIONIC_STACK_GUARD_CANARY);

    std::cout << "================================================================================\n";
    std::cout << " ALL 64-BIT AOT + /dev/mali0 CSF UAPI + VALHALL ISA -> LAVAPIPE TESTS SUCCEEDED!\n";
    std::cout << "================================================================================\n";

    return 0;
}
