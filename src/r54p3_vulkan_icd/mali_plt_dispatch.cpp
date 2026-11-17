#include "mali_icd_internal.h"
#include "generated/mali_plt_thunks_autogen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <semaphore.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <pwd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Pull in generated PLT & GLOB_DAT manifests from build-x86-64/
#include "r54p3_plt_imports.inc"
#include "r54p3_libcpp_plt_imports.inc"

// Forward-declare all lifted libc++_shared.so function entrypoints
#define LIBCPP_FUNC_EXPORT(sym, rva, fn) extern "C" void fn(AOTCPUState* cpu);
#include "r54p3_libcpp_exports.inc"
#undef LIBCPP_FUNC_EXPORT

// Forward-declare AOT/Bionic bridges defined in mali_icd_internal.cpp
extern "C" {
int32_t __mali_hook_vkCreateXcbSurfaceKHR(void*, void*, void*, void*);
int32_t __mali_hook_vkCreateXlibSurfaceKHR(void*, void*, void*, void*);
int32_t __mali_hook_vkCreateWaylandSurfaceKHR(void*, void*, void*, void*);
int32_t __mali_hook_vkQueuePresentKHR(void*, void*);

int __mali_bionic_pthread_mutex_init(uint64_t, uint64_t);
int __mali_bionic_pthread_mutex_destroy(uint64_t);
int __mali_bionic_pthread_mutex_lock(uint64_t);
int __mali_bionic_pthread_mutex_unlock(uint64_t);
int __mali_bionic_pthread_mutex_trylock(uint64_t);
int __mali_bionic_pthread_cond_init(uint64_t, uint64_t);
int __mali_bionic_pthread_cond_destroy(uint64_t);
int __mali_bionic_pthread_cond_wait(uint64_t, uint64_t);
int __mali_bionic_pthread_cond_timedwait(uint64_t, uint64_t, uint64_t);
int __mali_bionic_pthread_cond_signal(uint64_t);
int __mali_bionic_pthread_cond_broadcast(uint64_t);
int __mali_bionic_pthread_rwlock_init(uint64_t, uint64_t);
int __mali_bionic_pthread_rwlock_destroy(uint64_t);
int __mali_bionic_pthread_rwlock_rdlock(uint64_t);
int __mali_bionic_pthread_rwlock_wrlock(uint64_t);
int __mali_bionic_pthread_rwlock_unlock(uint64_t);
int __mali_bionic_pthread_create(uint64_t, uint64_t, uint64_t, uint64_t);
int __mali_bionic_pthread_join(uint64_t, uint64_t);
int __mali_bionic_pthread_detach(uint64_t);

int64_t __mali_ahb_stub(int64_t op, int64_t a0, int64_t a1);
void    __mali_stack_chk_fail_diag(AOTCPUState* cpu);
}

namespace {

const char* kMaliPltNames[MALI_PLT_TABLE_COUNT > 0 ? MALI_PLT_TABLE_COUNT : 1] = {
#define MALI_PLT_IMPORT(idx, name) name,
#include "r54p3_plt_imports.inc"
#undef MALI_PLT_IMPORT
};

const char* kLibcppPltNames[LIBCPP_PLT_TABLE_COUNT > 0 ? LIBCPP_PLT_TABLE_COUNT : 1] = {
#define LIBCPP_PLT_IMPORT(idx, name) name,
#include "r54p3_libcpp_plt_imports.inc"
#undef LIBCPP_PLT_IMPORT
};

void __mali_report_unhandled_plt(AOTCPUState* cpu, const char* tbl, int idx, const char* sym) {
    static std::mutex s_log_mtx;
    static std::unordered_set<uint64_t> s_logged;
    uint64_t key = (static_cast<uint64_t>(tbl[0]) << 32) | static_cast<uint32_t>(idx);
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(s_log_mtx);
        first = s_logged.insert(key).second;
    }
    const char* trap_env = std::getenv("MALI_AOT_TRAP_UNHANDLED_PLT");
    bool do_trap = (trap_env && trap_env[0] == '1');
    if (first || do_trap) {
        std::fprintf(stderr,
                     "[UNHANDLED_PLT] table=%s idx=%d sym='%s' lr=0x%016llx x0=0x%llx x1=0x%llx x2=0x%llx\n",
                     tbl, idx, sym ? sym : "<null>",
                     static_cast<unsigned long long>(cpu->x[30]),
                     static_cast<unsigned long long>(cpu->x[0]),
                     static_cast<unsigned long long>(cpu->x[1]),
                     static_cast<unsigned long long>(cpu->x[2]));
    }
    if (do_trap) {
        std::abort();
    }
    cpu->x[0] = 0;
}

template <int Idx>
void __mali_unhandled_aot_trampoline(AOTCPUState* cpu) {
    __mali_report_unhandled_plt(cpu, "aot", Idx, kMaliPltNames[Idx]);
}

template <int Idx>
void __mali_unhandled_libcpp_trampoline(AOTCPUState* cpu) {
    __mali_report_unhandled_plt(cpu, "libcpp", Idx, kLibcppPltNames[Idx]);
}

constexpr int kAotPltCount = MALI_PLT_TABLE_COUNT > 0 ? MALI_PLT_TABLE_COUNT : 1;
constexpr int kLibcppPltCount = LIBCPP_PLT_TABLE_COUNT > 0 ? LIBCPP_PLT_TABLE_COUNT : 1;
MaliPltFn g_aot_plt_real[kAotPltCount] = {};
MaliPltFn g_libcpp_plt_real[kLibcppPltCount] = {};
std::atomic<uint64_t> g_aot_plt_hits[kAotPltCount] = {};
std::atomic<uint64_t> g_libcpp_plt_hits[kLibcppPltCount] = {};

template <int Idx>
void __mali_prof_aot_trampoline(AOTCPUState* cpu) {
    g_aot_plt_hits[Idx].fetch_add(1, std::memory_order_relaxed);
    g_aot_plt_real[Idx](cpu);
}

template <int Idx>
void __mali_prof_libcpp_trampoline(AOTCPUState* cpu) {
    g_libcpp_plt_hits[Idx].fetch_add(1, std::memory_order_relaxed);
    g_libcpp_plt_real[Idx](cpu);
}

template <int... Is>
void install_aot_plt_profilers(std::integer_sequence<int, Is...>, MaliPltFn* tbl) {
    ((tbl[Is] = &__mali_prof_aot_trampoline<Is>), ...);
}

template <int... Is>
void install_libcpp_plt_profilers(std::integer_sequence<int, Is...>, MaliPltFn* tbl) {
    ((tbl[Is] = &__mali_prof_libcpp_trampoline<Is>), ...);
}

void mali_dump_plt_profile() {
    struct Row { const char* tbl; const char* sym; uint64_t hits; };
    std::vector<Row> rows;
    for (int i = 0; i < MALI_PLT_TABLE_COUNT; ++i) {
        uint64_t h = g_aot_plt_hits[i].load(std::memory_order_relaxed);
        if (h > 0) rows.push_back({"aot", kMaliPltNames[i], h});
    }
    for (int i = 0; i < LIBCPP_PLT_TABLE_COUNT; ++i) {
        uint64_t h = g_libcpp_plt_hits[i].load(std::memory_order_relaxed);
        if (h > 0) rows.push_back({"libcpp", kLibcppPltNames[i], h});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.hits > b.hits; });
    std::fprintf(stderr, "\n=== [MALI_PLT_PROFILE] Top Hot PLT Calls (candidates for isHotInlinePltSym) ===\n");
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 40); ++i) {
        std::fprintf(stderr, "  #%02zu [%s] %10llu  %s\n",
                     i + 1, rows[i].tbl, static_cast<unsigned long long>(rows[i].hits), rows[i].sym);
    }
}

// ----------------------------------------------------------------------------
// Custom Bionic/POSIX/libm/C++ thunks
// ----------------------------------------------------------------------------

// Bionic open/close/ioctl/mmap/read/write/poll/stat/access
static void thunk_open(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_open_stub(cpu->x[0], cpu->x[1]));
}
static void thunk_openat(AOTCPUState* cpu) {
    int dirfd = static_cast<int>(static_cast<int32_t>(cpu->x[0]));
    const char* path = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[1]));
    int flags = static_cast<int>(static_cast<int32_t>(cpu->x[2]));
    mode_t mode = static_cast<mode_t>(cpu->x[3]);
    if (dirfd == AT_FDCWD && path) {
        cpu->x[0] = static_cast<uint64_t>(bionic_mali_open_stub(cpu->x[1], cpu->x[2]));
        return;
    }
    int r = ::openat(dirfd, path, flags, mode);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_close(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_close_stub(cpu->x[0]));
}
static void thunk_ioctl(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_kbase_ioctl_stub(cpu->x[0], cpu->x[1], cpu->x[2]));
}
static void thunk_mmap(AOTCPUState* cpu) {
    cpu->x[0] = bionic_mali_mmap_stub(cpu->x[0], cpu->x[1], cpu->x[2], cpu->x[3], cpu->x[4], cpu->x[5]);
}
static void thunk_munmap(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_munmap_stub(cpu->x[0], cpu->x[1]));
}
static void thunk_read(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_read_stub(cpu->x[0], cpu->x[1], cpu->x[2]));
}
static void thunk_write(AOTCPUState* cpu) {
    int fd = static_cast<int>(static_cast<int32_t>(cpu->x[0]));
    const void* buf = reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu->x[1]));
    size_t cnt = static_cast<size_t>(cpu->x[2]);
    ssize_t r = ::write(fd, buf, cnt);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_poll(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_poll_stub(cpu->x[0], cpu->x[1], cpu->x[2]));
}
static void thunk_ppoll(AOTCPUState* cpu) {
    const auto* ts = reinterpret_cast<const struct timespec*>(static_cast<uintptr_t>(cpu->x[2]));
    int64_t timeout_ms = ts ? (static_cast<int64_t>(ts->tv_sec) * 1000LL + ts->tv_nsec / 1000000LL) : -1;
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_poll_stub(cpu->x[0], cpu->x[1], static_cast<uint64_t>(timeout_ms)));
}
static void thunk_stat(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_stat_stub(cpu->x[0], cpu->x[1]));
}
static void thunk_fstat(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_fstat_stub(cpu->x[0], cpu->x[1]));
}
static void thunk_access(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_mali_access_stub(cpu->x[0], cpu->x[1]));
}
static void thunk_fcntl(AOTCPUState* cpu) {
    int fd = static_cast<int>(static_cast<int32_t>(cpu->x[0]));
    int cmd = static_cast<int>(static_cast<int32_t>(cpu->x[1]));
    long arg = static_cast<long>(cpu->x[2]);
    int r = ::fcntl(fd, cmd, arg);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}

// __*_chk fortified wrappers
static void thunk_fwrite(AOTCPUState* cpu) {
    size_t r = ::fwrite(reinterpret_cast<const void*>(cpu->x[0]), cpu->x[1], cpu->x[2],
                        reinterpret_cast<FILE*>(cpu->x[3]));
    cpu->x[0] = static_cast<uint64_t>(r);
}
static void thunk_memchr(AOTCPUState* cpu) {
    const void* r = std::memchr(reinterpret_cast<const void*>(cpu->x[0]),
                                static_cast<int>(cpu->x[1]), static_cast<size_t>(cpu->x[2]));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}
static void thunk_strchr(AOTCPUState* cpu) {
    char* r = const_cast<char*>(std::strchr(reinterpret_cast<const char*>(cpu->x[0]),
                                            static_cast<int>(cpu->x[1])));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}
static void thunk_strcpy(AOTCPUState* cpu) {
    char* r = std::strcpy(reinterpret_cast<char*>(cpu->x[0]),
                          reinterpret_cast<const char*>(cpu->x[1]));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}
static void thunk_strncpy(AOTCPUState* cpu) {
    char* r = std::strncpy(reinterpret_cast<char*>(cpu->x[0]),
                           reinterpret_cast<const char*>(cpu->x[1]),
                           static_cast<size_t>(cpu->x[2]));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}
static void thunk_strncat(AOTCPUState* cpu) {
    char* r = std::strncat(reinterpret_cast<char*>(cpu->x[0]),
                           reinterpret_cast<const char*>(cpu->x[1]),
                           static_cast<size_t>(cpu->x[2]));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}

// Variadic / formatted I/O thunks (AAPCS64 registers x0..x7 -> host formatting)
static int format_aapcs64_va(char* dst, size_t n, const char* fmt, const uint64_t* regs, int reg_start) {
    if (!fmt) return 0;
    // Simple safe formatter for common %s/%d/%u/%x/%p/%ld/%zu/%f cases using reg_start..7
    std::string out;
    int ri = reg_start;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            out.push_back(*p);
            continue;
        }
        const char* spec_start = p;
        ++p;
        if (*p == '%') {
            out.push_back('%');
            continue;
        }
        while (*p && std::strchr("-+ #0", *p)) ++p;
        int dyn_w = -1, dyn_p = -1;
        if (*p == '*') {
            dyn_w = (ri < 8) ? static_cast<int>(regs[ri++]) : 0;
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') ++p;
        }
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                dyn_p = (ri < 8) ? static_cast<int>(regs[ri++]) : 0;
                ++p;
            } else {
                while (*p >= '0' && *p <= '9') ++p;
            }
        }
        bool is_long = false, is_ll = false;
        if (*p == 'l') { is_long = true; ++p; if (*p == 'l') { is_ll = true; ++p; } }
        else if (*p == 'z' || *p == 't' || *p == 'j') { is_long = true; ++p; }
        else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        if (!*p) break;
        std::string spec(spec_start, static_cast<size_t>(p - spec_start + 1));
        char conv = *p;
        char buf[512];
        uint64_t arg = (ri < 8) ? regs[ri++] : 0;
        if (conv == 's') {
            const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(arg));
            if (!s) s = "(null)";
            if (dyn_w >= 0 && dyn_p >= 0) std::snprintf(buf, sizeof(buf), spec.c_str(), dyn_w, dyn_p, s);
            else if (dyn_w >= 0) std::snprintf(buf, sizeof(buf), spec.c_str(), dyn_w, s);
            else if (dyn_p >= 0) std::snprintf(buf, sizeof(buf), spec.c_str(), dyn_p, s);
            else std::snprintf(buf, sizeof(buf), spec.c_str(), s);
            out += buf;
        } else if (conv == 'p') {
            std::snprintf(buf, sizeof(buf), spec.c_str(), reinterpret_cast<void*>(static_cast<uintptr_t>(arg)));
            out += buf;
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            double dv = 0.0;
            std::snprintf(buf, sizeof(buf), spec.c_str(), dv);
            out += buf;
        } else if (is_ll || is_long) {
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<long long>(arg));
            out += buf;
        } else {
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(arg));
            out += buf;
        }
    }
    if (dst && n > 0) {
        size_t copy_n = std::min(n - 1, out.size());
        if (copy_n > 0) std::memcpy(dst, out.data(), copy_n);
        dst[copy_n] = '\0';
    }
    return static_cast<int>(out.size());
}

static void thunk_snprintf(AOTCPUState* cpu) {
    char* dst = reinterpret_cast<char*>(static_cast<uintptr_t>(cpu->x[0]));
    size_t n = static_cast<size_t>(cpu->x[1]);
    const char* fmt = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[2]));
    int r = format_aapcs64_va(dst, n, fmt, cpu->x, 3);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_sprintf(AOTCPUState* cpu) {
    char* dst = reinterpret_cast<char*>(static_cast<uintptr_t>(cpu->x[0]));
    const char* fmt = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[1]));
    int r = format_aapcs64_va(dst, 65536, fmt, cpu->x, 2);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_vsnprintf(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(bionic_aapcs64_vsnprintf_bridge(cpu->x[0], cpu->x[1], cpu->x[2], cpu->x[3]));
}
static void thunk_vsnprintf_chk(AOTCPUState* cpu) {
    // __vsnprintf_chk(s, maxlen, flag, slen, format, ap) -> x0, x1, x4, x5
    cpu->x[0] = static_cast<uint64_t>(bionic_aapcs64_vsnprintf_bridge(cpu->x[0], cpu->x[1], cpu->x[4], cpu->x[5]));
}
static void thunk_fprintf(AOTCPUState* cpu) {
    FILE* fp = reinterpret_cast<FILE*>(static_cast<uintptr_t>(cpu->x[0]));
    const char* fmt = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[1]));
    char buf[4096];
    int r = format_aapcs64_va(buf, sizeof(buf), fmt, cpu->x, 2);
    if (fp) std::fwrite(buf, 1, std::min<size_t>(static_cast<size_t>(r), sizeof(buf) - 1), fp);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_printf(AOTCPUState* cpu) {
    const char* fmt = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0]));
    char buf[4096];
    int r = format_aapcs64_va(buf, sizeof(buf), fmt, cpu->x, 1);
    std::fwrite(buf, 1, std::min<size_t>(static_cast<size_t>(r), sizeof(buf) - 1), stdout);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_vfprintf(AOTCPUState* cpu) {
    FILE* fp = reinterpret_cast<FILE*>(static_cast<uintptr_t>(cpu->x[0]));
    char buf[4096];
    int64_t r = bionic_aapcs64_vsnprintf_bridge(reinterpret_cast<uint64_t>(buf), sizeof(buf), cpu->x[1], cpu->x[2]);
    if (fp && r > 0) std::fwrite(buf, 1, std::min<size_t>(static_cast<size_t>(r), sizeof(buf) - 1), fp);
    cpu->x[0] = static_cast<uint64_t>(r);
}
static void thunk_sscanf(AOTCPUState* cpu) {
    const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0]));
    const char* fmt = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[1]));
    int r = std::sscanf(s ? s : "", fmt ? fmt : "",
                        reinterpret_cast<void*>(cpu->x[2]),
                        reinterpret_cast<void*>(cpu->x[3]),
                        reinterpret_cast<void*>(cpu->x[4]),
                        reinterpret_cast<void*>(cpu->x[5]),
                        reinterpret_cast<void*>(cpu->x[6]),
                        reinterpret_cast<void*>(cpu->x[7]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}

// Bionic pthread_* shims
static void thunk_pthread_mutex_init(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_mutex_init(cpu->x[0], cpu->x[1]));
}
static void thunk_pthread_mutex_destroy(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_mutex_destroy(cpu->x[0]));
}
static void thunk_pthread_mutex_lock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_mutex_lock(cpu->x[0]));
}
static void thunk_pthread_mutex_trylock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_mutex_trylock(cpu->x[0]));
}
static void thunk_pthread_mutex_unlock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_mutex_unlock(cpu->x[0]));
}
static void thunk_pthread_cond_init(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_init(cpu->x[0], cpu->x[1]));
}
static void thunk_pthread_cond_destroy(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_destroy(cpu->x[0]));
}
static void thunk_pthread_cond_wait(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_wait(cpu->x[0], cpu->x[1]));
}
static void thunk_pthread_cond_timedwait(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_timedwait(cpu->x[0], cpu->x[1], cpu->x[2]));
}
static void thunk_pthread_cond_signal(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_signal(cpu->x[0]));
}
static void thunk_pthread_cond_broadcast(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_cond_broadcast(cpu->x[0]));
}
static void thunk_pthread_rwlock_init(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_rwlock_init(cpu->x[0], cpu->x[1]));
}
static void thunk_pthread_rwlock_destroy(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_rwlock_destroy(cpu->x[0]));
}
static void thunk_pthread_rwlock_rdlock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_rwlock_rdlock(cpu->x[0]));
}
static void thunk_pthread_rwlock_wrlock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_rwlock_wrlock(cpu->x[0]));
}
static void thunk_pthread_rwlock_unlock(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_rwlock_unlock(cpu->x[0]));
}
static void thunk_pthread_create(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_create(cpu->x[0], cpu->x[1], cpu->x[2], cpu->x[3]));
}
static void thunk_pthread_join(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_join(cpu->x[0], cpu->x[1]));
}
static void thunk_pthread_detach(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_bionic_pthread_detach(cpu->x[0]));
}
static void thunk_pthread_once(AOTCPUState* cpu) {
    volatile int32_t* ctrl = reinterpret_cast<volatile int32_t*>(static_cast<uintptr_t>(cpu->x[0]));
    uint64_t init_fn_va = cpu->x[1];
    if (!ctrl || !init_fn_va) {
        cpu->x[0] = 0;
        return;
    }
    if (__atomic_load_n(ctrl, __ATOMIC_ACQUIRE) == 2) {
        cpu->x[0] = 0;
        return;
    }
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(ctrl, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        AOTCPUState sub = *cpu;
        aot_indirect_dispatch(&sub, init_fn_va);
        __atomic_store_n(ctrl, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(ctrl, __ATOMIC_ACQUIRE) != 2) {
            sched_yield();
        }
    }
    cpu->x[0] = 0;
}
static void thunk_pthread_key_create(AOTCPUState* cpu) {
    pthread_key_t* key = reinterpret_cast<pthread_key_t*>(static_cast<uintptr_t>(cpu->x[0]));
    int r = key ? ::pthread_key_create(key, nullptr) : EINVAL;
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_pthread_noop0(AOTCPUState* cpu) {
    cpu->x[0] = 0;
}

// Bionic sem_t (4 bytes) -> host sem_t* map
static std::mutex s_sem_mtx;
static std::unordered_map<uint64_t, sem_t*> s_sem_map;
static sem_t* resolve_bionic_sem(uint64_t va, bool create_if_missing, unsigned int init_val = 0) {
    if (!va) return nullptr;
    std::lock_guard<std::mutex> lk(s_sem_mtx);
    auto it = s_sem_map.find(va);
    if (it != s_sem_map.end()) return it->second;
    if (!create_if_missing) return nullptr;
    sem_t* s = static_cast<sem_t*>(std::calloc(1, sizeof(sem_t)));
    ::sem_init(s, 0, init_val);
    s_sem_map[va] = s;
    return s;
}
static void thunk_pthread_mutexattr_init(AOTCPUState* cpu) {
    auto* attr = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(cpu->x[0]));
    if (attr) *attr = 0x4D410000U;
    cpu->x[0] = 0;
}
static void thunk_pthread_mutexattr_destroy(AOTCPUState* cpu) {
    auto* attr = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(cpu->x[0]));
    if (attr) *attr = 0;
    cpu->x[0] = 0;
}
static void thunk_pthread_mutexattr_settype(AOTCPUState* cpu) {
    auto* attr = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(cpu->x[0]));
    uint32_t type = static_cast<uint32_t>(cpu->x[1]);
    if (attr) *attr = 0x4D410000U | (type & 0x3U);
    cpu->x[0] = 0;
}
static void thunk_sem_init(AOTCPUState* cpu) {
    uint64_t va = cpu->x[0];
    unsigned int val = static_cast<unsigned int>(cpu->x[2]);
    std::lock_guard<std::mutex> lk(s_sem_mtx);
    auto it = s_sem_map.find(va);
    if (it != s_sem_map.end()) {
        ::sem_destroy(it->second);
        std::free(it->second);
    }
    sem_t* s = static_cast<sem_t*>(std::calloc(1, sizeof(sem_t)));
    int r = ::sem_init(s, 0, val);
    s_sem_map[va] = s;
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_sem_destroy(AOTCPUState* cpu) {
    std::lock_guard<std::mutex> lk(s_sem_mtx);
    auto it = s_sem_map.find(cpu->x[0]);
    if (it != s_sem_map.end()) {
        ::sem_destroy(it->second);
        std::free(it->second);
        s_sem_map.erase(it);
    }
    cpu->x[0] = 0;
}
static void thunk_sem_wait(AOTCPUState* cpu) {
    sem_t* s = resolve_bionic_sem(cpu->x[0], true, 0);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(s ? ::sem_wait(s) : -1));
}
static void thunk_sem_trywait(AOTCPUState* cpu) {
    sem_t* s = resolve_bionic_sem(cpu->x[0], true, 0);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(s ? ::sem_trywait(s) : -1));
}
static void thunk_sem_timedwait(AOTCPUState* cpu) {
    sem_t* s = resolve_bionic_sem(cpu->x[0], true, 0);
    const struct timespec* ts = reinterpret_cast<const struct timespec*>(static_cast<uintptr_t>(cpu->x[1]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>((s && ts) ? ::sem_timedwait(s, ts) : -1));
}
static void thunk_sem_post(AOTCPUState* cpu) {
    sem_t* s = resolve_bionic_sem(cpu->x[0], true, 0);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(s ? ::sem_post(s) : -1));
}
static void thunk_sem_getvalue(AOTCPUState* cpu) {
    sem_t* s = resolve_bionic_sem(cpu->x[0], true, 0);
    int* sval = reinterpret_cast<int*>(static_cast<uintptr_t>(cpu->x[1]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>((s && sval) ? ::sem_getvalue(s, sval) : -1));
}

// Guest-callback qsort
struct GuestQsortCtx {
    AOTCPUState* cpu;
    uint64_t compar_va;
};
static int guest_qsort_cmp(const void* a, const void* b, void* arg) {
    auto* ctx = static_cast<GuestQsortCtx*>(arg);
    AOTCPUState sub = *ctx->cpu;
    sub.x[0] = reinterpret_cast<uint64_t>(a);
    sub.x[1] = reinterpret_cast<uint64_t>(b);
    aot_indirect_dispatch(&sub, ctx->compar_va);
    return static_cast<int>(static_cast<int32_t>(sub.x[0]));
}
static void thunk_qsort(AOTCPUState* cpu) {
    void* base = reinterpret_cast<void*>(static_cast<uintptr_t>(cpu->x[0]));
    size_t nmemb = static_cast<size_t>(cpu->x[1]);
    size_t size = static_cast<size_t>(cpu->x[2]);
    GuestQsortCtx ctx{cpu, cpu->x[3]};
    if (base && nmemb > 1 && ctx.compar_va) {
        ::qsort_r(base, nmemb, size, &guest_qsort_cmp, &ctx);
    }
}

static void thunk_div(AOTCPUState* cpu) {
    div_t r = ::div(static_cast<int>(static_cast<int32_t>(cpu->x[0])),
                    static_cast<int>(static_cast<int32_t>(cpu->x[1])));
    uint64_t packed = static_cast<uint32_t>(r.quot) | (static_cast<uint64_t>(static_cast<uint32_t>(r.rem)) << 32);
    cpu->x[0] = packed;
}
static void thunk_sincosf(AOTCPUState* cpu) {
    float x = 0.0f;
    std::memcpy(&x, &cpu->v[0], 4);
    float* s = reinterpret_cast<float*>(static_cast<uintptr_t>(cpu->x[0]));
    float* c = reinterpret_cast<float*>(static_cast<uintptr_t>(cpu->x[1]));
    if (s && c) ::sincosf(x, s, c);
}
static void thunk_errno(AOTCPUState* cpu) {
    cpu->x[0] = reinterpret_cast<uint64_t>(&errno);
}
static void thunk_gnu_strerror_r(AOTCPUState* cpu) {
    char* r = ::strerror_r(static_cast<int>(cpu->x[0]),
                           reinterpret_cast<char*>(static_cast<uintptr_t>(cpu->x[1])),
                           static_cast<size_t>(cpu->x[2]));
    cpu->x[0] = reinterpret_cast<uint64_t>(r);
}
static void thunk_sysconf(AOTCPUState* cpu) {
    long r = mali_stubs_sysconf_hook(static_cast<int>(cpu->x[0]));
    cpu->x[0] = static_cast<uint64_t>(r);
}
static void thunk_system_property_get(AOTCPUState* cpu) {
    int r = mali_stubs_system_property_get_hook(
        reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0])),
        reinterpret_cast<char*>(static_cast<uintptr_t>(cpu->x[1])));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_property_get(AOTCPUState* cpu) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0]));
    char* val = reinterpret_cast<char*>(static_cast<uintptr_t>(cpu->x[1]));
    const char* def = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[2]));
    int r = mali_stubs_system_property_get_hook(name, val);
    if (r == 0 && def && val) {
        std::strncpy(val, def, 91);
        val[91] = '\0';
        r = static_cast<int>(std::strlen(val));
    }
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_property_get_bool(AOTCPUState* cpu) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0]));
    int def = static_cast<int>(cpu->x[1]);
    char val[92]{};
    int r = mali_stubs_system_property_get_hook(name, val);
    if (r > 0) {
        if (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0) def = 1;
        else if (std::strcmp(val, "0") == 0 || std::strcmp(val, "false") == 0) def = 0;
    }
    cpu->x[0] = static_cast<uint64_t>(def ? 1 : 0);
}
static void thunk_vkCreateXcbSurfaceKHR(AOTCPUState* cpu) {
    int32_t r = __mali_hook_vkCreateXcbSurfaceKHR(
        reinterpret_cast<void*>(cpu->x[0]), reinterpret_cast<void*>(cpu->x[1]),
        reinterpret_cast<void*>(cpu->x[2]), reinterpret_cast<void*>(cpu->x[3]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_vkCreateXlibSurfaceKHR(AOTCPUState* cpu) {
    int32_t r = __mali_hook_vkCreateXlibSurfaceKHR(
        reinterpret_cast<void*>(cpu->x[0]), reinterpret_cast<void*>(cpu->x[1]),
        reinterpret_cast<void*>(cpu->x[2]), reinterpret_cast<void*>(cpu->x[3]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_vkCreateWaylandSurfaceKHR(AOTCPUState* cpu) {
    int32_t r = __mali_hook_vkCreateWaylandSurfaceKHR(
        reinterpret_cast<void*>(cpu->x[0]), reinterpret_cast<void*>(cpu->x[1]),
        reinterpret_cast<void*>(cpu->x[2]), reinterpret_cast<void*>(cpu->x[3]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_vkQueuePresentKHR(AOTCPUState* cpu) {
    int32_t r = __mali_hook_vkQueuePresentKHR(
        reinterpret_cast<void*>(cpu->x[0]), reinterpret_cast<void*>(cpu->x[1]));
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_ahb_alloc(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_ahb_stub(1, static_cast<int64_t>(cpu->x[0]), static_cast<int64_t>(cpu->x[1])));
}
static void thunk_ahb_desc(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_ahb_stub(2, static_cast<int64_t>(cpu->x[0]), static_cast<int64_t>(cpu->x[1])));
}
static void thunk_ahb_nh(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_ahb_stub(3, static_cast<int64_t>(cpu->x[0]), static_cast<int64_t>(cpu->x[1])));
}
static void thunk_ahb_id(AOTCPUState* cpu) {
    cpu->x[0] = static_cast<uint64_t>(__mali_ahb_stub(4, static_cast<int64_t>(cpu->x[0]), static_cast<int64_t>(cpu->x[1])));
}
static void thunk_stack_chk_fail(AOTCPUState* cpu) {
    __mali_stack_chk_fail_diag(cpu);
}
static void thunk_hash_memory(AOTCPUState* cpu) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(cpu->x[0]));
    size_t len = static_cast<size_t>(cpu->x[1]);
    // 64-bit MurmurHash2 / FNV mix
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    uint64_t h = 0x123456789abcdef0ULL ^ (len * m);
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t k = 0;
        std::memcpy(&k, data + i, 8);
        k *= m; k ^= k >> 47; k *= m;
        h ^= k; h *= m;
    }
    uint64_t tail = 0;
    for (size_t j = 0; i + j < len; ++j) tail |= static_cast<uint64_t>(data[i + j]) << (j * 8);
    if (len & 7) { h ^= tail; h *= m; }
    h ^= h >> 47; h *= m; h ^= h >> 47;
    cpu->x[0] = h;
}
static void thunk_noop0(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_ret1(AOTCPUState* cpu) { cpu->x[0] = 1; }
static void thunk_dlsym(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_dladdr(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_dl_iterate_phdr(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_sigaction(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_android_log(AOTCPUState* cpu) { cpu->x[0] = 0; }
static void thunk_localtime(AOTCPUState* cpu) {
    const time_t* tp = reinterpret_cast<const time_t*>(static_cast<uintptr_t>(cpu->x[0]));
    cpu->x[0] = reinterpret_cast<uint64_t>(tp ? std::localtime(tp) : nullptr);
}
static void thunk_localtime_r(AOTCPUState* cpu) {
    const time_t* tp = reinterpret_cast<const time_t*>(static_cast<uintptr_t>(cpu->x[0]));
    struct tm* res = reinterpret_cast<struct tm*>(static_cast<uintptr_t>(cpu->x[1]));
    cpu->x[0] = reinterpret_cast<uint64_t>((tp && res) ? ::localtime_r(tp, res) : nullptr);
}
static void thunk_strftime(AOTCPUState* cpu) {
    size_t r = std::strftime(reinterpret_cast<char*>(cpu->x[0]), static_cast<size_t>(cpu->x[1]),
                             reinterpret_cast<const char*>(cpu->x[2]),
                             reinterpret_cast<const struct tm*>(cpu->x[3]));
    cpu->x[0] = static_cast<uint64_t>(r);
}
static void thunk_readdir(AOTCPUState* cpu) {
    DIR* d = reinterpret_cast<DIR*>(static_cast<uintptr_t>(cpu->x[0]));
    cpu->x[0] = reinterpret_cast<uint64_t>(d ? ::readdir(d) : nullptr);
}
static void thunk_getpwuid(AOTCPUState* cpu) {
    cpu->x[0] = reinterpret_cast<uint64_t>(::getpwuid(static_cast<uid_t>(cpu->x[0])));
}
static void thunk_getpwnam(AOTCPUState* cpu) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[0]));
    cpu->x[0] = reinterpret_cast<uint64_t>(name ? ::getpwnam(name) : nullptr);
}
static void thunk_prctl(AOTCPUState* cpu) {
    int op = static_cast<int>(cpu->x[0]);
    if (op == 0x53564d41) { cpu->x[0] = 0; return; } // PR_SET_VMA (Android-only)
    int r = ::prctl(op, cpu->x[1], cpu->x[2], cpu->x[3], cpu->x[4]);
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_syscall(AOTCPUState* cpu) {
    long nr = static_cast<long>(cpu->x[0]);
    long r = -1;
    switch (nr) {
        case 178: r = ::syscall(SYS_gettid); break; // arm64 __NR_gettid
        case 172: r = ::syscall(SYS_getpid); break; // arm64 __NR_getpid
        case 98:  r = ::syscall(SYS_futex, cpu->x[1], cpu->x[2], cpu->x[3], cpu->x[4], cpu->x[5], cpu->x[6]); break; // arm64 __NR_futex
        case 278: r = ::syscall(SYS_getrandom, cpu->x[1], cpu->x[2], cpu->x[3]); break; // arm64 __NR_getrandom
        default: errno = ENOSYS; r = -1; break;
    }
    cpu->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
static void thunk_enosys(AOTCPUState* cpu) { errno = ENOSYS; cpu->x[0] = static_cast<uint64_t>(-1LL); }
struct HidlString {
    const char* mBuffer;
    uint32_t mSize;
    bool mOwnsBuffer;
};
static void thunk_hidl_string_ctor(AOTCPUState* cpu) {
    auto* self = reinterpret_cast<HidlString*>(static_cast<uintptr_t>(cpu->x[0]));
    const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(cpu->x[1]));
    if (self) {
        self->mBuffer = s ? s : "";
        self->mSize = s ? static_cast<uint32_t>(std::strlen(s)) : 0;
        self->mOwnsBuffer = false;
    }
}
static void thunk_hidl_string_dtor(AOTCPUState* cpu) {
    auto* self = reinterpret_cast<HidlString*>(static_cast<uintptr_t>(cpu->x[0]));
    if (self && self->mOwnsBuffer && self->mBuffer) {
        std::free(const_cast<char*>(self->mBuffer));
        self->mBuffer = nullptr;
        self->mSize = 0;
        self->mOwnsBuffer = false;
    }
}

std::unordered_map<std::string, uint64_t> g_libcpp_func_rvas;
std::unordered_map<std::string, uint64_t> g_libcpp_data_rvas;
std::unordered_map<std::string, uint64_t> g_aot_plt_index_by_name;
std::once_flag g_plt_init_once;

} // namespace

extern "C" {

MaliPltFn g_aot_plt_table[MALI_PLT_TABLE_COUNT > 0 ? MALI_PLT_TABLE_COUNT : 1] = {
#define MALI_PLT_IMPORT(idx, name) &__mali_unhandled_aot_trampoline<idx>,
#include "r54p3_plt_imports.inc"
#undef MALI_PLT_IMPORT
};

MaliPltFn g_libcpp_plt_table[LIBCPP_PLT_TABLE_COUNT > 0 ? LIBCPP_PLT_TABLE_COUNT : 1] = {
#define LIBCPP_PLT_IMPORT(idx, name) &__mali_unhandled_libcpp_trampoline<idx>,
#include "r54p3_libcpp_plt_imports.inc"
#undef LIBCPP_PLT_IMPORT
};

struct LibcppFuncExportDesc { const char* sym; uint64_t rva; MaliPltFn fn; };
static constexpr LibcppFuncExportDesc kLibcppFuncExports[] = {
#define LIBCPP_FUNC_EXPORT(sym, rva, fn) { sym, rva, &fn },
#include "r54p3_libcpp_exports.inc"
#undef LIBCPP_FUNC_EXPORT
};

struct LibcppDataExportDesc { const char* sym; uint64_t rva; };
static constexpr LibcppDataExportDesc kLibcppDataExports[] = {
#define LIBCPP_DATA_EXPORT(sym, rva) { sym, rva },
#include "r54p3_libcpp_exports.inc"
#undef LIBCPP_DATA_EXPORT
};

struct CustomPltDesc { const char* name; MaliPltFn fn; };
static constexpr CustomPltDesc kCustomThunks[] = {
    {"openat", &thunk_openat},
    {"write", &thunk_write},
    {"__write_chk", &thunk_write},
    {"ppoll", &thunk_ppoll},
    {"__ppoll_chk", &thunk_ppoll},
    {"fcntl", &thunk_fcntl},
    {"fwrite", &thunk_fwrite},
    {"__fwrite_chk", &thunk_fwrite},
    {"memchr", &thunk_memchr},
    {"__memchr_chk", &thunk_memchr},
    {"strchr", &thunk_strchr},
    {"__strchr_chk", &thunk_strchr},
    {"strcpy", &thunk_strcpy},
    {"__strcpy_chk", &thunk_strcpy},
    {"strncpy", &thunk_strncpy},
    {"__strncpy_chk", &thunk_strncpy},
    {"__strncpy_chk2", &thunk_strncpy},
    {"strncat", &thunk_strncat},
    {"__strncat_chk", &thunk_strncat},
    {"snprintf", &thunk_snprintf},
    {"sprintf", &thunk_sprintf},
    {"vsnprintf", &thunk_vsnprintf},
    {"__vsnprintf_chk", &thunk_vsnprintf_chk},
    {"fprintf", &thunk_fprintf},
    {"printf", &thunk_printf},
    {"vfprintf", &thunk_vfprintf},
    {"sscanf", &thunk_sscanf},
    {"pthread_mutex_init", &thunk_pthread_mutex_init},
    {"pthread_mutex_destroy", &thunk_pthread_mutex_destroy},
    {"pthread_mutex_lock", &thunk_pthread_mutex_lock},
    {"pthread_mutex_trylock", &thunk_pthread_mutex_trylock},
    {"pthread_mutex_unlock", &thunk_pthread_mutex_unlock},
    {"pthread_cond_init", &thunk_pthread_cond_init},
    {"pthread_cond_destroy", &thunk_pthread_cond_destroy},
    {"pthread_cond_wait", &thunk_pthread_cond_wait},
    {"pthread_cond_timedwait", &thunk_pthread_cond_timedwait},
    {"pthread_cond_signal", &thunk_pthread_cond_signal},
    {"pthread_cond_broadcast", &thunk_pthread_cond_broadcast},
    {"pthread_rwlock_init", &thunk_pthread_rwlock_init},
    {"pthread_rwlock_destroy", &thunk_pthread_rwlock_destroy},
    {"pthread_rwlock_rdlock", &thunk_pthread_rwlock_rdlock},
    {"pthread_rwlock_wrlock", &thunk_pthread_rwlock_wrlock},
    {"pthread_rwlock_unlock", &thunk_pthread_rwlock_unlock},
    {"pthread_create", &thunk_pthread_create},
    {"pthread_join", &thunk_pthread_join},
    {"pthread_detach", &thunk_pthread_detach},
    {"pthread_once", &thunk_pthread_once},
    {"pthread_key_create", &thunk_pthread_key_create},
    {"pthread_mutexattr_init", &thunk_pthread_mutexattr_init},
    {"pthread_mutexattr_destroy", &thunk_pthread_mutexattr_destroy},
    {"pthread_mutexattr_settype", &thunk_pthread_mutexattr_settype},
    {"sem_init", &thunk_sem_init},
    {"sem_destroy", &thunk_sem_destroy},
    {"sem_wait", &thunk_sem_wait},
    {"sem_trywait", &thunk_sem_trywait},
    {"sem_timedwait", &thunk_sem_timedwait},
    {"sem_post", &thunk_sem_post},
    {"sem_getvalue", &thunk_sem_getvalue},
    {"qsort", &thunk_qsort},
    {"div", &thunk_div},
    {"sincosf", &thunk_sincosf},
    {"__errno", &thunk_errno},
    {"__gnu_strerror_r", &thunk_gnu_strerror_r},
    {"__stack_chk_fail", &thunk_stack_chk_fail},
    {"abort", &thunk_stack_chk_fail},
    {"__assert2", &thunk_stack_chk_fail},
    {"sysconf", &thunk_sysconf},
    {"__system_property_get", &thunk_system_property_get},
    {"property_get", &thunk_property_get},
    {"property_get_bool", &thunk_property_get_bool},
    {"vkCreateXcbSurfaceKHR", &thunk_vkCreateXcbSurfaceKHR},
    {"vkCreateXlibSurfaceKHR", &thunk_vkCreateXlibSurfaceKHR},
    {"vkCreateWaylandSurfaceKHR", &thunk_vkCreateWaylandSurfaceKHR},
    {"vkQueuePresentKHR", &thunk_vkQueuePresentKHR},
    {"AHardwareBuffer_allocate", &thunk_ahb_alloc},
    {"AHardwareBuffer_describe", &thunk_ahb_desc},
    {"AHardwareBuffer_getNativeHandle", &thunk_ahb_nh},
    {"AHardwareBuffer_getId", &thunk_ahb_id},
    {"_ZNSt3__113__hash_memoryEPKvm", &thunk_hash_memory},
    {"__android_log_print", &thunk_android_log},
    {"__android_log_write", &thunk_android_log},
    {"dlsym", &thunk_dlsym},
    {"dladdr", &thunk_dladdr},
    {"dl_iterate_phdr", &thunk_dl_iterate_phdr},
    {"sigaction", &thunk_sigaction},
    {"localtime", &thunk_localtime},
    {"localtime_r", &thunk_localtime_r},
    {"strftime", &thunk_strftime},
    {"readdir", &thunk_readdir},
    {"getpwuid", &thunk_getpwuid},
    {"getpwnam", &thunk_getpwnam},
    {"prctl", &thunk_prctl},
    {"syscall", &thunk_syscall},
    {"_ZN7android8hardware11hidl_stringC1EPKc", &thunk_hidl_string_ctor},
    {"_ZN7android8hardware11hidl_stringD1Ev", &thunk_hidl_string_dtor},
};

static inline void register_libcpp_symbol(
    const char* sym, uint64_t rva, MaliPltFn fn,
    std::unordered_map<std::string, MaliPltFn>* reg,
    std::unordered_map<std::string, uint64_t>& rva_map) {
    std::string s(sym);
    if (reg) (*reg)[s] = fn;
    rva_map[s] = rva;
    size_t p = s.find("St6__ndk1");
    if (p != std::string::npos) {
        std::string a = s;
        a.replace(p, 9, "St3__1");
        if (reg) (*reg)[a] = fn;
        rva_map[a] = rva;
    }
}

void mali_init_plt_tables() {
    std::call_once(g_plt_init_once, []() {
        std::unordered_map<std::string, MaliPltFn> reg;

        // 1. Auto-generated C/POSIX/libm + redirect/noop/alias thunks
        mali_register_autogen_plt_thunks(reg);

        // 2. Lifted libc++_shared.so exported functions & data (St6__ndk1 + St3__1)
        for (const auto& e : kLibcppFuncExports) {
            register_libcpp_symbol(e.sym, e.rva, e.fn, &reg, g_libcpp_func_rvas);
        }
        for (const auto& e : kLibcppDataExports) {
            register_libcpp_symbol(e.sym, e.rva, nullptr, nullptr, g_libcpp_data_rvas);
        }

        // 3. Custom Mali/Bionic/POSIX/variadic/pthread/sem/WSI overrides
        for (const auto& e : kCustomThunks) {
            reg[e.name] = e.fn;
        }
        (void)thunk_open; (void)thunk_close; (void)thunk_ioctl; (void)thunk_mmap;
        (void)thunk_munmap; (void)thunk_read; (void)thunk_poll; (void)thunk_stat;
        (void)thunk_fstat; (void)thunk_access; (void)thunk_pthread_noop0;
        (void)thunk_noop0; (void)thunk_enosys;

        // 4. Populate g_aot_plt_table & g_libcpp_plt_table
        for (int i = 0; i < MALI_PLT_TABLE_COUNT; ++i) {
            g_aot_plt_index_by_name[kMaliPltNames[i]] = static_cast<uint64_t>(i);
            auto it = reg.find(kMaliPltNames[i]);
            if (it != reg.end()) {
                g_aot_plt_table[i] = it->second;
            }
        }
        for (int i = 0; i < LIBCPP_PLT_TABLE_COUNT; ++i) {
            auto it = reg.find(kLibcppPltNames[i]);
            if (it != reg.end()) {
                g_libcpp_plt_table[i] = it->second;
            }
        }
        const char* prof_env = std::getenv("MALI_PLT_PROFILE");
        if (prof_env && prof_env[0] == '1') {
            for (int i = 0; i < kAotPltCount; ++i) g_aot_plt_real[i] = g_aot_plt_table[i];
            for (int i = 0; i < kLibcppPltCount; ++i) g_libcpp_plt_real[i] = g_libcpp_plt_table[i];
            install_aot_plt_profilers(std::make_integer_sequence<int, kAotPltCount>{}, g_aot_plt_table);
            install_libcpp_plt_profilers(std::make_integer_sequence<int, kLibcppPltCount>{}, g_libcpp_plt_table);
            std::atexit(mali_dump_plt_profile);
        }
        (void)thunk_ret1;
    });
}

} // extern "C"

static constexpr uint64_t SYNTH_GLOB_VA_BASE = 0x600010000ULL;

static void thunk_glob_free(AOTCPUState* cpu) { std::free(reinterpret_cast<void*>(static_cast<uintptr_t>(cpu->x[0]))); }
static void thunk_glob_malloc(AOTCPUState* cpu) { cpu->x[0] = reinterpret_cast<uint64_t>(std::malloc(static_cast<size_t>(cpu->x[0]))); }
static void thunk_glob_calloc(AOTCPUState* cpu) { cpu->x[0] = reinterpret_cast<uint64_t>(std::calloc(static_cast<size_t>(cpu->x[0]), static_cast<size_t>(cpu->x[1]))); }
static void thunk_glob_realloc(AOTCPUState* cpu) { cpu->x[0] = reinterpret_cast<uint64_t>(std::realloc(reinterpret_cast<void*>(static_cast<uintptr_t>(cpu->x[0])), static_cast<size_t>(cpu->x[1]))); }

template <double (*Fn)(double)>
static void thunk_libm_d1(AOTCPUState* cpu) {
    double a0 = 0.0; std::memcpy(&a0, &cpu->v[0], 8);
    double r = Fn(a0); std::memcpy(&cpu->v[0], &r, 8);
}
template <double (*Fn)(double, double)>
static void thunk_libm_d2(AOTCPUState* cpu) {
    double a0 = 0.0, a1 = 0.0;
    std::memcpy(&a0, &cpu->v[0], 8); std::memcpy(&a1, &cpu->v[1], 8);
    double r = Fn(a0, a1); std::memcpy(&cpu->v[0], &r, 8);
}

struct SynthGlobEntry { const char* name; MaliPltFn fn; };
static const SynthGlobEntry kSynthGlobEntries[] = {
    {"free", &thunk_glob_free},
    {"malloc", &thunk_glob_malloc},
    {"calloc", &thunk_glob_calloc},
    {"realloc", &thunk_glob_realloc},
    {"acos", &thunk_libm_d1<std::acos>},
    {"asin", &thunk_libm_d1<std::asin>},
    {"atan", &thunk_libm_d1<std::atan>},
    {"atan2", &thunk_libm_d2<std::atan2>},
    {"cosh", &thunk_libm_d1<std::cosh>},
    {"exp", &thunk_libm_d1<std::exp>},
    {"log", &thunk_libm_d1<std::log>},
    {"log10", &thunk_libm_d1<std::log10>},
    {"sinh", &thunk_libm_d1<std::sinh>},
    {"sqrt", &thunk_libm_d1<std::sqrt>},
    {"tan", &thunk_libm_d1<std::tan>},
    {"tanh", &thunk_libm_d1<std::tanh>},
};

extern "C" {

uint64_t mali_resolve_glob_dat_symbol(const char* sym_name, int64_t addend) {
    if (!sym_name || !sym_name[0]) return 0;
    if (std::strcmp(sym_name, "stderr") == 0) return reinterpret_cast<uint64_t>(&stderr);
    if (std::strcmp(sym_name, "stdout") == 0) return reinterpret_cast<uint64_t>(&stdout);
    if (std::strcmp(sym_name, "stdin") == 0) return reinterpret_cast<uint64_t>(&stdin);
    if (std::strcmp(sym_name, "_ZN4aidl7android8hardware8graphics9allocator10IAllocator10descriptorE") == 0) {
        static const char* s_aidl_desc = "android.hardware.graphics.allocator.IAllocator";
        return reinterpret_cast<uint64_t>(&s_aidl_desc);
    }

    auto dit = g_libcpp_data_rvas.find(sym_name);
    if (dit != g_libcpp_data_rvas.end()) {
        return LIBCPP_IMAGE_BASE + dit->second + static_cast<uint64_t>(addend);
    }
    auto fit = g_libcpp_func_rvas.find(sym_name);
    if (fit != g_libcpp_func_rvas.end()) {
        return LIBCPP_IMAGE_BASE + fit->second;
    }
    auto pit = g_aot_plt_index_by_name.find(sym_name);
    if (pit != g_aot_plt_index_by_name.end()) {
        return SYNTH_PLT_VA_BASE + pit->second * 4ULL;
    }
    for (size_t i = 0; i < sizeof(kSynthGlobEntries) / sizeof(kSynthGlobEntries[0]); ++i) {
        if (std::strcmp(sym_name, kSynthGlobEntries[i].name) == 0) {
            return SYNTH_GLOB_VA_BASE + static_cast<uint64_t>(i) * 4ULL;
        }
    }
    return 0;
}

bool mali_dispatch_synth_plt_va(AOTCPUState* cpu, uint64_t target_va) {
    if (target_va >= SYNTH_PLT_VA_BASE &&
        target_va < SYNTH_PLT_VA_BASE + static_cast<uint64_t>(MALI_PLT_TABLE_COUNT) * 4ULL &&
        ((target_va - SYNTH_PLT_VA_BASE) & 3ULL) == 0) {
        uint64_t idx = (target_va - SYNTH_PLT_VA_BASE) / 4ULL;
        g_aot_plt_table[idx](cpu);
        return true;
    }
    constexpr size_t kGlobCount = sizeof(kSynthGlobEntries) / sizeof(kSynthGlobEntries[0]);
    if (target_va >= SYNTH_GLOB_VA_BASE &&
        target_va < SYNTH_GLOB_VA_BASE + static_cast<uint64_t>(kGlobCount) * 4ULL &&
        ((target_va - SYNTH_GLOB_VA_BASE) & 3ULL) == 0) {
        uint64_t idx = (target_va - SYNTH_GLOB_VA_BASE) / 4ULL;
        kSynthGlobEntries[idx].fn(cpu);
        return true;
    }
    return false;
}

} // extern "C"
