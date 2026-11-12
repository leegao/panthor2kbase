#pragma once
// ============================================================================
// Function-Signature Template-Metaprogrammed SysV x86-64 <-> ARM64 AAPCS64 ABI
// ============================================================================
// Instantiates a specialized compile-time invoker (`Aapcs64FnTraits<FnSig>`)
// per Vulkan C function prototype `Ret(Args...)` or function pointer type
// `Ret(*)(Args...)`:
//   - Zero runtime loop / dead branch overhead: only the exact `N = sizeof...(Args)`
//     arguments are marshaled at compile time (`if constexpr (Idx < 8)` writes
//     directly to `cpu.x[Idx]`; stack spill code `[cpu.sp + 8 * (Idx - 8)]` is
//     only emitted by the compiler for functions with `sizeof...(Args) > 8`).
//   - Void return (`std::is_void_v<Ret>`) emits zero return-value unmarshaling.
//   - RAII `Aapcs64StackFrameGuard` saves/restores `x19..x30` and `sp` and
//     guarantees `cpu.x[29] == cpu.sp` before every lifted call.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

struct AArch64CpuState;

namespace mali_aapcs64 {

template <typename T>
constexpr inline uint64_t marshalArgToXReg(T val) noexcept {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(val));
    } else if constexpr (std::is_enum_v<T>) {
        using UT = std::underlying_type_t<T>;
        return static_cast<uint64_t>(static_cast<std::make_unsigned_t<UT>>(val));
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof(T) < 8) {
            return static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(val));
        } else {
            return static_cast<uint64_t>(val);
        }
    } else {
        static_assert(sizeof(T) <= sizeof(uint64_t), "Composite AAPCS64 argument exceeds 8 bytes");
        uint64_t raw = 0;
        std::memcpy(&raw, &val, sizeof(T));
        return raw;
    }
}

template <typename Ret>
constexpr inline Ret unmarshalRetFromX0(uint64_t x0) noexcept {
    if constexpr (std::is_void_v<Ret>) {
        return;
    } else if constexpr (std::is_pointer_v<Ret>) {
        return reinterpret_cast<Ret>(static_cast<uintptr_t>(x0));
    } else if constexpr (std::is_enum_v<Ret>) {
        return static_cast<Ret>(static_cast<int32_t>(x0 & 0xFFFFFFFFULL));
    } else if constexpr (std::is_integral_v<Ret>) {
        return static_cast<Ret>(x0);
    }
}

template <typename CpuStateT>
struct Aapcs64StackFrameGuard {
    CpuStateT &cpu;
    uint64_t saved_callee_regs[12]; // x19..x30
    uint64_t saved_sp;

    Aapcs64StackFrameGuard(CpuStateT &c, uint8_t *stack_base, size_t stack_size, uintptr_t tls_base) noexcept
        : cpu(c), saved_sp(c.sp) {
        for (int i = 0; i < 12; ++i) {
            saved_callee_regs[i] = cpu.x[19 + i];
        }
        const uint64_t stack_lo = reinterpret_cast<uint64_t>(stack_base);
        const uint64_t stack_hi = stack_lo + stack_size;
        uint64_t new_sp = (saved_sp >= stack_lo + 16384 && saved_sp <= stack_hi)
                              ? ((saved_sp - 8192ULL) & ~0x3FULL)
                              : ((stack_hi - 4096ULL) & ~0x3FULL);
        cpu.sp = new_sp;
        cpu.x[29] = new_sp;
        cpu.x[30] = 0xDEAD0000ULL;
        cpu.tpidr_el0 = static_cast<uint64_t>(tls_base);
    }

    ~Aapcs64StackFrameGuard() noexcept {
        for (int i = 0; i < 12; ++i) {
            cpu.x[19 + i] = saved_callee_regs[i];
        }
        cpu.sp = saved_sp;
    }
};

// Compile-time single-argument marshaler specialized on argument index `Idx`:
// - When `Idx < 8`, emits a single direct register write `cpu.x[Idx] = ...` with
//   ZERO stack-spill code generated.
// - When `Idx >= 8`, emits a single 8-byte store to `[cpu.sp + 8 * (Idx - 8)]`.
template <std::size_t Idx, typename CpuStateT, typename ArgT>
inline void marshalSingleArgAt(CpuStateT &cpu, ArgT arg) noexcept {
    if constexpr (Idx < 8) {
        cpu.x[Idx] = marshalArgToXReg(arg);
    } else {
        *reinterpret_cast<uint64_t *>(
            static_cast<uintptr_t>(cpu.sp + (Idx - 8) * sizeof(uint64_t))) = marshalArgToXReg(arg);
    }
}

template <typename CpuStateT, typename... Args, std::size_t... Is>
inline void marshalArgPack(CpuStateT &cpu, std::index_sequence<Is...>, Args... args) noexcept {
    (marshalSingleArgAt<Is>(cpu, args), ...);
}

// Primary template for function-pointer / function-signature introspection
template <typename FnSig>
struct Aapcs64Invoker;

// Specialization for `Ret(Args...)`
template <typename Ret, typename... Args>
struct Aapcs64Invoker<Ret(Args...)> {
    static constexpr std::size_t kArity = sizeof...(Args);
    static constexpr bool kSpillsToStack = (sizeof...(Args) > 8);
    static constexpr bool kHasReturnValue = !std::is_void_v<Ret>;

    template <typename CpuStateT>
    static inline Ret invoke(void (*aot_fn)(CpuStateT *),
                             CpuStateT &cpu,
                             uint8_t *stack_base,
                             size_t stack_size,
                             uintptr_t tls_base,
                             Args... args) noexcept {
        Aapcs64StackFrameGuard<CpuStateT> frame(cpu, stack_base, stack_size, tls_base);
        if constexpr (kArity > 0) {
            marshalArgPack(cpu, std::index_sequence_for<Args...>{}, args...);
        }
        aot_fn(&cpu);
        if constexpr (kHasReturnValue) {
            return unmarshalRetFromX0<Ret>(cpu.x[0]);
        }
    }
};

// Specialization for function pointer types `Ret(*)(Args...)`
template <typename Ret, typename... Args>
struct Aapcs64Invoker<Ret (*)(Args...)> : Aapcs64Invoker<Ret(Args...)> {};

// Deduced invoker from explicit return type `Ret` and argument pack `Args...`
template <typename Ret, typename CpuStateT, typename... Args>
inline Ret invokeLiftedAapcs64(void (*aot_fn)(CpuStateT *),
                               CpuStateT &cpu,
                               uint8_t *stack_base,
                               size_t stack_size,
                               uintptr_t tls_base,
                               Args... args) noexcept {
    return Aapcs64Invoker<Ret(Args...)>::invoke(aot_fn, cpu, stack_base, stack_size, tls_base, args...);
}

template <typename Ret, typename CpuStateT, typename... Args>
inline Ret invokeLiftedAapcs64(CpuStateT &cpu,
                               uint8_t *stack_base,
                               size_t stack_size,
                               void (*aot_fn)(CpuStateT *),
                               Args... args) noexcept {
    return Aapcs64Invoker<Ret(Args...)>::invoke(
        aot_fn, cpu, stack_base, stack_size, static_cast<uintptr_t>(cpu.tpidr_el0), args...);
}

// Invoker specialized directly on a Vulkan PFN_* function pointer type `FnPtrT`
template <typename FnPtrT, typename CpuStateT, typename... CallArgs>
inline auto invokeLiftedByFnType(CpuStateT &cpu,
                                 uint8_t *stack_base,
                                 size_t stack_size,
                                 void (*aot_fn)(CpuStateT *),
                                 CallArgs... args) noexcept {
    return Aapcs64Invoker<FnPtrT>::invoke(
        aot_fn, cpu, stack_base, stack_size, static_cast<uintptr_t>(cpu.tpidr_el0), args...);
}

} // namespace mali_aapcs64
