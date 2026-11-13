// ============================================================================
// src/aot/include/aot_so_profiler.hpp
// Flag-Toggled Guest Stackframe Recorder & Function Call Profiler/Tracer for
// AOT-Lifted 64-Bit ARM64 Userspace Shared Libraries (`libGLES_mali.so` / `driver.so`).
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#define AOT_SO_DEBUG_RECORD_STACKFRAMES (1U << 0)
#define AOT_SO_DEBUG_TRACE_CALLS        (1U << 1)
#define AOT_SO_DEBUG_PROFILE_CALLS      (1U << 2)
#define AOT_SO_MAX_CALL_FRAMES          128U
#define AOT_SO_MAX_PROFILE_FUNCS        1024U

extern "C" {

struct AotSoStackFrame {
    uint64_t    fn_va;
    const char* fn_name;
    uint64_t    caller_va;
    uint64_t    sp;
    uint64_t    fp;
    uint64_t    lr;
    uint64_t    args[4];
    uint64_t    enter_ns;
    uint64_t    child_ns;
};

struct AotSoFnProfileEntry {
    uint64_t    fn_va;
    const char* fn_name;
    uint64_t    call_count;
    uint64_t    total_inclusive_ns;
    uint64_t    total_exclusive_ns;
    uint64_t    max_ns;
};

void aot_so_set_debug_flags(uint32_t flags);
uint32_t aot_so_get_debug_flags(void);
uint32_t aot_so_get_stack_depth(void);
void aot_so_unwind_to_depth(uint32_t target_depth);
uint32_t aot_so_get_max_stack_depth(void);
const struct AotSoStackFrame* aot_so_get_stack_frame(uint32_t idx);
void aot_so_reset_profile(void);
size_t aot_so_get_profile_entries(struct AotSoFnProfileEntry* out_entries, size_t max_entries);
void aot_so_dump_stacktrace(FILE* out);
void aot_so_dump_profile(FILE* out);
void aot_so_trace_fn_enter(void* cpu_ptr, uint64_t fn_va, const char* fn_name);
void aot_so_trace_fn_exit(void* cpu_ptr, uint64_t fn_va, const char* fn_name);

} // extern "C"

#ifdef AOT_SO_PROFILER_IMPLEMENTATION

#include "../aot/generated/r54p3_debug_symbols.h"

static inline const char* aot_so_resolve_debug_name(uint64_t fn_va, const char* fn_name) {
    if (!fn_name || std::strncmp(fn_name, "sub_", 4) == 0) {
        const char* sym = r54p3_lookup_debug_symbol_by_va(fn_va);
        if (sym && sym[0] != '\0') return sym;
    }
    return fn_name ? fn_name : "<sub_unknown>";
}

namespace aot_so_prof_detail {

struct RawAotCpuPrefix {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
};

static uint32_t g_so_debug_flags = 0;
static bool g_so_env_checked = false;
static bool g_so_trace_std = false;
static size_t g_so_profile_top_n = 0;
static thread_local AotSoStackFrame g_so_call_frames[AOT_SO_MAX_CALL_FRAMES]{};
static thread_local uint32_t g_so_call_depth = 0;
static thread_local uint32_t g_so_max_call_depth = 0;
static AotSoFnProfileEntry g_so_profile_table[AOT_SO_MAX_PROFILE_FUNCS]{};
static size_t g_so_profile_count = 0;

static inline uint64_t monotonic_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

static inline bool env_flag_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
}

static inline void ensure_env_initialized() {
    if (__builtin_expect(g_so_env_checked, 1)) return;
    g_so_env_checked = true;
    uint32_t flags = 0;
    const char* raw_mask = std::getenv("AOT_SO_DEBUG_FLAGS");
    if (!raw_mask || !raw_mask[0]) raw_mask = std::getenv("MALI_SO_DEBUG_FLAGS");
    if (raw_mask && raw_mask[0]) {
        flags |= static_cast<uint32_t>(std::strtoul(raw_mask, nullptr, 0)) & 0x7U;
    }
    if (env_flag_on("AOT_SO_STACK_TRACE") || env_flag_on("AOT_SO_RECORD_STACKFRAMES") ||
        env_flag_on("MALI_SO_RECORD_STACKFRAMES") || env_flag_on("MALI_LIFTED_RECORD_STACKFRAMES")) {
        flags |= AOT_SO_DEBUG_RECORD_STACKFRAMES;
    }
    if (env_flag_on("AOT_SO_TRACE_CALLS") || env_flag_on("MALI_SO_TRACE_CALLS") ||
        env_flag_on("MALI_LIFTED_TRACE_CALLS")) {
        flags |= (AOT_SO_DEBUG_RECORD_STACKFRAMES | AOT_SO_DEBUG_TRACE_CALLS);
    }
    if (env_flag_on("AOT_SO_PROFILE_CALLS") || env_flag_on("MALI_SO_PROFILE_CALLS") ||
        env_flag_on("MALI_LIFTED_PROFILE_CALLS")) {
        flags |= (AOT_SO_DEBUG_RECORD_STACKFRAMES | AOT_SO_DEBUG_PROFILE_CALLS);
    }
    const char* top_n_env = std::getenv("AOT_SO_PROFILE_TOP_N");
    if (!top_n_env || !top_n_env[0]) top_n_env = std::getenv("MALI_SO_PROFILE_TOP_N");
    if (top_n_env && top_n_env[0]) {
        g_so_profile_top_n = static_cast<size_t>(std::strtoull(top_n_env, nullptr, 0));
        if (g_so_profile_top_n > 0) {
            flags |= (AOT_SO_DEBUG_RECORD_STACKFRAMES | AOT_SO_DEBUG_PROFILE_CALLS);
        }
    }
    if (env_flag_on("MALI_SO_TRACE_STD") || env_flag_on("AOT_SO_TRACE_STD")) {
        g_so_trace_std = true;
    }
    if (flags & (AOT_SO_DEBUG_TRACE_CALLS | AOT_SO_DEBUG_PROFILE_CALLS)) {
        flags |= AOT_SO_DEBUG_RECORD_STACKFRAMES;
    }
    g_so_debug_flags |= flags;
}

static inline bool is_std_trace_suppressed(const char* fn_name) {
    if (g_so_trace_std || !fn_name || !fn_name[0]) return false;
    if (std::strncmp(fn_name, "_ZNSt3__1", 9) == 0 ||
        std::strncmp(fn_name, "_ZNKSt3__1", 10) == 0) {
        return true;
    }
    if (std::strncmp(fn_name, "std::__1::", 10) == 0 || std::strncmp(fn_name, "void std::__1::", 15) == 0) {
        // int angle_depth = 0;
        // for (const char* p = fn_name; *p != '\0'; ++p) {
        //     if (*p == '<') {
        //         angle_depth++;
        //     } else if (*p == '>' && angle_depth > 0) {
        //         angle_depth--;
        //     } else if (angle_depth == 0) {
        //         if (*p == '(') return true;
        //         if (*p == ' ') return false;
        //     }
        // }
        return true;
    }
    return false;
}

} // namespace aot_so_prof_detail

extern "C" {

void aot_so_set_debug_flags(uint32_t flags) {
    aot_so_prof_detail::ensure_env_initialized();
    if (flags & (AOT_SO_DEBUG_TRACE_CALLS | AOT_SO_DEBUG_PROFILE_CALLS)) {
        flags |= AOT_SO_DEBUG_RECORD_STACKFRAMES;
    }
    aot_so_prof_detail::g_so_debug_flags = flags;
}

uint32_t aot_so_get_debug_flags(void) {
    aot_so_prof_detail::ensure_env_initialized();
    return aot_so_prof_detail::g_so_debug_flags;
}

uint32_t aot_so_get_stack_depth(void) {
    return aot_so_prof_detail::g_so_call_depth;
}

void aot_so_unwind_to_depth(uint32_t target_depth) {
    uint32_t flags = aot_so_prof_detail::g_so_debug_flags;
    while (aot_so_prof_detail::g_so_call_depth > target_depth) {
        aot_so_prof_detail::g_so_call_depth--;
        uint32_t idx = aot_so_prof_detail::g_so_call_depth;
        const char* fn_name = "<sub_unknown>";
        uint64_t elapsed_ns = 0ULL;
        if (idx < AOT_SO_MAX_CALL_FRAMES) {
            AotSoStackFrame& fr = aot_so_prof_detail::g_so_call_frames[idx];
            if (fr.fn_name) fn_name = fr.fn_name;
            if ((flags & (AOT_SO_DEBUG_TRACE_CALLS | AOT_SO_DEBUG_PROFILE_CALLS)) && fr.enter_ns != 0ULL) {
                uint64_t now_ns = aot_so_prof_detail::monotonic_ns();
                if (now_ns >= fr.enter_ns) {
                    elapsed_ns = now_ns - fr.enter_ns;
                }
            }
            if (idx > 0U) {
                aot_so_prof_detail::g_so_call_frames[idx - 1U].child_ns += elapsed_ns;
            }
        }
        if ((flags & AOT_SO_DEBUG_TRACE_CALLS) && !aot_so_prof_detail::is_std_trace_suppressed(fn_name)) {
            std::fprintf(stderr, "[SO_TRACE] %*s<- %s [unwound %llu ns]\n",
                         static_cast<int>(idx * 2U), "",
                         fn_name,
                         static_cast<unsigned long long>(elapsed_ns));
        }
    }
}

uint32_t aot_so_get_max_stack_depth(void) {
    return aot_so_prof_detail::g_so_max_call_depth;
}

const struct AotSoStackFrame* aot_so_get_stack_frame(uint32_t idx) {
    if (idx >= aot_so_prof_detail::g_so_call_depth || idx >= AOT_SO_MAX_CALL_FRAMES) return nullptr;
    return &aot_so_prof_detail::g_so_call_frames[idx];
}

void aot_so_reset_profile(void) {
    std::memset(aot_so_prof_detail::g_so_profile_table, 0, sizeof(aot_so_prof_detail::g_so_profile_table));
    aot_so_prof_detail::g_so_profile_count = 0;
    aot_so_prof_detail::g_so_call_depth = 0;
    aot_so_prof_detail::g_so_max_call_depth = 0;
}

size_t aot_so_get_profile_entries(struct AotSoFnProfileEntry* out_entries, size_t max_entries) {
    size_t n = (aot_so_prof_detail::g_so_profile_count < max_entries)
                   ? aot_so_prof_detail::g_so_profile_count
                   : max_entries;
    if (out_entries && n > 0) {
        std::memcpy(out_entries, aot_so_prof_detail::g_so_profile_table, n * sizeof(AotSoFnProfileEntry));
    }
    return aot_so_prof_detail::g_so_profile_count;
}

void aot_so_dump_stacktrace(FILE* out) {
    if (!out) out = stderr;
    uint32_t depth = aot_so_prof_detail::g_so_call_depth;
    std::fprintf(out, "[AOT_SO_STACKTRACE] Guest Call Stack (depth=%u, max_depth=%u):\n",
                 depth, aot_so_prof_detail::g_so_max_call_depth);
    uint32_t lim = (depth < AOT_SO_MAX_CALL_FRAMES) ? depth : AOT_SO_MAX_CALL_FRAMES;
    for (uint32_t i = lim; i > 0; --i) {
        const AotSoStackFrame& fr = aot_so_prof_detail::g_so_call_frames[i - 1U];
        std::fprintf(out, "  frame[%u] %s (va=0x%llx caller_va=0x%llx sp=0x%llx fp=0x%llx lr=0x%llx x0=0x%llx x1=0x%llx)\n",
                     i - 1U,
                     fr.fn_name ? fr.fn_name : "<sub_unknown>",
                     static_cast<unsigned long long>(fr.fn_va),
                     static_cast<unsigned long long>(fr.caller_va),
                     static_cast<unsigned long long>(fr.sp),
                     static_cast<unsigned long long>(fr.fp),
                     static_cast<unsigned long long>(fr.lr),
                     static_cast<unsigned long long>(fr.args[0]),
                     static_cast<unsigned long long>(fr.args[1]));
    }
}

void aot_so_dump_profile(FILE* out) {
    if (!out) out = stderr;
    size_t total = aot_so_prof_detail::g_so_profile_count;
    std::vector<AotSoFnProfileEntry> sorted(
        aot_so_prof_detail::g_so_profile_table,
        aot_so_prof_detail::g_so_profile_table + total);
    std::sort(sorted.begin(), sorted.end(), [](const AotSoFnProfileEntry& a, const AotSoFnProfileEntry& b) {
        if (a.total_inclusive_ns != b.total_inclusive_ns)
            return a.total_inclusive_ns > b.total_inclusive_ns;
        return a.call_count > b.call_count;
    });
    size_t show_n = total;
    if (aot_so_prof_detail::g_so_profile_top_n > 0 && aot_so_prof_detail::g_so_profile_top_n < show_n) {
        show_n = aot_so_prof_detail::g_so_profile_top_n;
    }
    std::fprintf(out, "================================================================================\n");
    std::fprintf(out, " [Lifted libGLES_mali.so Profiler] Function Call & Latency Summary (showing %zu/%zu functions)\n",
                 show_n, total);
    std::fprintf(out, " %-14s %-42s %8s %12s %12s %10s\n",
                 "Guest VA", "Function", "Calls", "Incl (ns)", "Excl (ns)", "Max (ns)");
    std::fprintf(out, "--------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < show_n; ++i) {
        const AotSoFnProfileEntry& e = sorted[i];
        std::fprintf(out, " 0x%012llx %-42s %8llu %12llu %12llu %10llu\n",
                     static_cast<unsigned long long>(e.fn_va),
                     e.fn_name ? e.fn_name : "<sub_unknown>",
                     static_cast<unsigned long long>(e.call_count),
                     static_cast<unsigned long long>(e.total_inclusive_ns),
                     static_cast<unsigned long long>(e.total_exclusive_ns),
                     static_cast<unsigned long long>(e.max_ns));
    }
    std::fprintf(out, "================================================================================\n");
}

__attribute__((destructor))
static void aot_so_auto_dump_profile_on_exit(void) {
    aot_so_prof_detail::ensure_env_initialized();
    if ((aot_so_prof_detail::g_so_debug_flags & AOT_SO_DEBUG_PROFILE_CALLS) &&
        aot_so_prof_detail::g_so_profile_count > 0) {
        aot_so_dump_profile(stderr);
    }
}

void aot_so_trace_fn_enter(void* cpu_ptr, uint64_t fn_va, const char* fn_name) {
    aot_so_prof_detail::ensure_env_initialized();
    uint32_t flags = aot_so_prof_detail::g_so_debug_flags;
    if (__builtin_expect(flags == 0U, 1)) return;

    fn_name = aot_so_resolve_debug_name(fn_va, fn_name);
    auto* cpu = static_cast<aot_so_prof_detail::RawAotCpuPrefix*>(cpu_ptr);
    if (cpu) cpu->pc = fn_va;
    uint64_t now_ns = (flags & (AOT_SO_DEBUG_TRACE_CALLS | AOT_SO_DEBUG_PROFILE_CALLS))
                          ? aot_so_prof_detail::monotonic_ns()
                          : 0ULL;
    uint32_t depth = aot_so_prof_detail::g_so_call_depth;
    if (depth < AOT_SO_MAX_CALL_FRAMES) {
        AotSoStackFrame& fr = aot_so_prof_detail::g_so_call_frames[depth];
        fr.fn_va = fn_va;
        fr.fn_name = fn_name;
        fr.caller_va = (depth > 0) ? aot_so_prof_detail::g_so_call_frames[depth - 1U].fn_va : (cpu ? cpu->x[30] : 0ULL);
        fr.sp = cpu ? cpu->sp : 0ULL;
        fr.fp = cpu ? cpu->x[29] : 0ULL;
        fr.lr = cpu ? cpu->x[30] : 0ULL;
        fr.args[0] = cpu ? cpu->x[0] : 0ULL;
        fr.args[1] = cpu ? cpu->x[1] : 0ULL;
        fr.args[2] = cpu ? cpu->x[2] : 0ULL;
        fr.args[3] = cpu ? cpu->x[3] : 0ULL;
        fr.enter_ns = now_ns;
        fr.child_ns = 0ULL;
    }
    aot_so_prof_detail::g_so_call_depth = depth + 1U;
    if (aot_so_prof_detail::g_so_call_depth > aot_so_prof_detail::g_so_max_call_depth) {
        aot_so_prof_detail::g_so_max_call_depth = aot_so_prof_detail::g_so_call_depth;
    }
    if ((flags & AOT_SO_DEBUG_TRACE_CALLS) && !aot_so_prof_detail::is_std_trace_suppressed(fn_name)) {
        std::fprintf(stderr, "[SO_TRACE] %*s-> %s(x0=0x%llx, x1=0x%llx, x2=0x%llx) [va=0x%llx sp=0x%llx lr=0x%llx depth=%u]\n",
                     static_cast<int>(depth * 2U), "",
                     fn_name ? fn_name : "<sub_unknown>",
                     static_cast<unsigned long long>(cpu ? cpu->x[0] : 0ULL),
                     static_cast<unsigned long long>(cpu ? cpu->x[1] : 0ULL),
                     static_cast<unsigned long long>(cpu ? cpu->x[2] : 0ULL),
                     static_cast<unsigned long long>(fn_va),
                     static_cast<unsigned long long>(cpu ? cpu->sp : 0ULL),
                     static_cast<unsigned long long>(cpu ? cpu->x[30] : 0ULL),
                     depth);
    }
}

void aot_so_trace_fn_exit(void* cpu_ptr, uint64_t fn_va, const char* fn_name) {
    uint32_t flags = aot_so_prof_detail::g_so_debug_flags;
    if (__builtin_expect(flags == 0U, 1)) return;

    fn_name = aot_so_resolve_debug_name(fn_va, fn_name);
    auto* cpu = static_cast<aot_so_prof_detail::RawAotCpuPrefix*>(cpu_ptr);
    uint64_t now_ns = (flags & (AOT_SO_DEBUG_TRACE_CALLS | AOT_SO_DEBUG_PROFILE_CALLS))
                          ? aot_so_prof_detail::monotonic_ns()
                          : 0ULL;
    uint64_t elapsed_ns = 0ULL;
    uint64_t excl_ns = 0ULL;

    if (aot_so_prof_detail::g_so_call_depth > 0U) {
        aot_so_prof_detail::g_so_call_depth--;
        uint32_t idx = aot_so_prof_detail::g_so_call_depth;
        if (idx < AOT_SO_MAX_CALL_FRAMES) {
            AotSoStackFrame& fr = aot_so_prof_detail::g_so_call_frames[idx];
            if (now_ns >= fr.enter_ns && fr.enter_ns != 0ULL) {
                elapsed_ns = now_ns - fr.enter_ns;
                excl_ns = (elapsed_ns >= fr.child_ns) ? (elapsed_ns - fr.child_ns) : 0ULL;
            }
            if (idx > 0U) {
                aot_so_prof_detail::g_so_call_frames[idx - 1U].child_ns += elapsed_ns;
                if (cpu) cpu->pc = aot_so_prof_detail::g_so_call_frames[idx - 1U].fn_va;
            }
        }
    }

    if (flags & AOT_SO_DEBUG_PROFILE_CALLS) {
        AotSoFnProfileEntry* target = nullptr;
        for (size_t i = 0; i < aot_so_prof_detail::g_so_profile_count; ++i) {
            if (aot_so_prof_detail::g_so_profile_table[i].fn_va == fn_va) {
                target = &aot_so_prof_detail::g_so_profile_table[i];
                break;
            }
        }
        if (!target && aot_so_prof_detail::g_so_profile_count < AOT_SO_MAX_PROFILE_FUNCS) {
            target = &aot_so_prof_detail::g_so_profile_table[aot_so_prof_detail::g_so_profile_count++];
            target->fn_va = fn_va;
            target->fn_name = fn_name;
        }
        if (target) {
            target->call_count++;
            target->total_inclusive_ns += elapsed_ns;
            target->total_exclusive_ns += excl_ns;
            if (elapsed_ns > target->max_ns) target->max_ns = elapsed_ns;
        }
    }

    if ((flags & AOT_SO_DEBUG_TRACE_CALLS) && !aot_so_prof_detail::is_std_trace_suppressed(fn_name)) {
        uint64_t ret_x0 = cpu ? cpu->x[0] : 0ULL;
        std::fprintf(stderr, "[SO_TRACE] %*s<- %s = 0x%llx [%llu ns]\n",
                     static_cast<int>(aot_so_prof_detail::g_so_call_depth * 2U), "",
                     fn_name ? fn_name : "<sub_unknown>",
                     static_cast<unsigned long long>(ret_x0),
                     static_cast<unsigned long long>(elapsed_ns));
    }
}

} // extern "C"

#endif // AOT_SO_PROFILER_IMPLEMENTATION
