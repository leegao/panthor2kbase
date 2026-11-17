#pragma once
// ============================================================================
// Function-Signature & Arity Template-Metaprogrammed SysV x86-64 <-> AAPCS64 ABI
// ============================================================================
// Provides both:
//   1. `Aapcs64FnTraits<FnSig>` / `Aapcs64Invoker<FnSig>` / `callAapcs64AutoThunk<FnSig>`:
//      Introspects a Vulkan C function prototype `Ret(Args...)` or function
//      pointer type `Ret(*)(Args...)` (`PFN_vk*`) at compile time to extract
//      `ReturnType`, `kArity = sizeof...(Args)`, and `kSpillsToStack = (kArity > 8)`.
//   2. `callAapcs64AutoThunk<N, Ret>(...)`:
//      Explicitly templated on argument count `N = sizeof...(Args)` with
//      compile-time arity verification (`static_assert(sizeof...(Args) == N)`).
//
// Zero runtime loop / dead branch overhead:
//   - Only the exact `N = sizeof...(Args)` arguments are marshaled via a fold
//     expression over `std::index_sequence_for<Args...>`.
//   - `if constexpr (Idx < 8)` writes directly to `cpu.x[Idx]`.
//   - Stack spill code `[cpu.sp + 8 * (Idx - 8)]` is ONLY emitted by the C++
//     compiler for functions with `N > 8` (such as `cmpbe_v2_compile_multiple_shaders`).
//   - Void return (`std::is_void_v<Ret>`) emits zero return-value unmarshaling.
//   - RAII `Aapcs64StackFrameGuard` saves/restores `x19..x30` and `sp` and
//     guarantees `cpu.x[29] == cpu.sp` before every lifted call.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <tuple>
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

template <std::size_t N, typename CpuStateT, typename... Args, std::size_t... Is>
inline void marshalArgPackN(CpuStateT &cpu, std::index_sequence<Is...>, Args... args) noexcept {
    static_assert(sizeof...(Args) == N, "Arity mismatch in marshalArgPackN<N>");
    (marshalSingleArgAt<Is>(cpu, args), ...);
}

template <typename CpuStateT, typename... Args, std::size_t... Is>
inline void marshalArgPack(CpuStateT &cpu, std::index_sequence<Is...> seq, Args... args) noexcept {
    marshalArgPackN<sizeof...(Args)>(cpu, seq, args...);
}

// Compile-time Function-Type Traits (`Ret(Args...)` and `Ret(*)(Args...)`)
template <typename FnSig>
struct Aapcs64FnTraits;

template <typename Ret, typename... Args>
struct Aapcs64FnTraits<Ret(Args...)> {
    using ReturnType = Ret;
    using ArgsTuple  = std::tuple<Args...>;
    static constexpr std::size_t kArity          = sizeof...(Args);
    static constexpr bool        kSpillsToStack  = (sizeof...(Args) > 8);
    static constexpr bool        kHasReturnValue = !std::is_void_v<Ret>;
};

template <typename Ret, typename... Args>
struct Aapcs64FnTraits<Ret (*)(Args...)> : Aapcs64FnTraits<Ret(Args...)> {};

// Primary template for function-pointer / function-signature introspection
template <typename FnSig>
struct Aapcs64Invoker;

// Specialization for `Ret(Args...)`
template <typename Ret, typename... Args>
struct Aapcs64Invoker<Ret(Args...)> : Aapcs64FnTraits<Ret(Args...)> {
    using Traits = Aapcs64FnTraits<Ret(Args...)>;

    template <typename CpuStateT>
    static inline Ret invoke(void (*aot_fn)(CpuStateT *),
                             CpuStateT &cpu,
                             uint8_t *stack_base,
                             size_t stack_size,
                             uintptr_t tls_base,
                             Args... args) noexcept {
        Aapcs64StackFrameGuard<CpuStateT> frame(cpu, stack_base, stack_size, tls_base);
        if constexpr (Traits::kArity > 0) {
            marshalArgPackN<Traits::kArity>(cpu, std::index_sequence_for<Args...>{}, args...);
        }
        aot_fn(&cpu);
        if constexpr (Traits::kHasReturnValue) {
            return unmarshalRetFromX0<Ret>(cpu.x[0]);
        }
    }
};

// Specialization for function pointer types `Ret(*)(Args...)`
template <typename Ret, typename... Args>
struct Aapcs64Invoker<Ret (*)(Args...)> : Aapcs64Invoker<Ret(Args...)> {};

// 1. Explicit arity-templated `callAapcs64AutoThunk<N, Ret>(...)`
template <std::size_t N, typename Ret = uint64_t, typename CpuStateT, typename... Args>
inline Ret callAapcs64AutoThunk(CpuStateT &cpu,
                                uint8_t *stack_base,
                                size_t stack_size,
                                void (*aot_fn)(CpuStateT *),
                                Args... args) noexcept {
    static_assert(sizeof...(Args) == N, "callAapcs64AutoThunk<N>: argument pack length must equal N");
    return Aapcs64Invoker<Ret(Args...)>::invoke(
        aot_fn, cpu, stack_base, stack_size, static_cast<uintptr_t>(cpu.tpidr_el0), args...);
}

// 2. Function-pointer / signature-type-templated `callAapcs64AutoThunk<FnSig>(...)`
template <typename FnSig, typename CpuStateT, typename... CallArgs>
inline typename Aapcs64FnTraits<FnSig>::ReturnType
callAapcs64AutoThunk(CpuStateT &cpu,
                     uint8_t *stack_base,
                     size_t stack_size,
                     void (*aot_fn)(CpuStateT *),
                     CallArgs... args) noexcept {
    static_assert(sizeof...(CallArgs) == Aapcs64FnTraits<FnSig>::kArity,
                  "callAapcs64AutoThunk<FnSig>: call arguments must match function signature arity");
    return Aapcs64Invoker<FnSig>::invoke(
        aot_fn, cpu, stack_base, stack_size, static_cast<uintptr_t>(cpu.tpidr_el0), args...);
}

// 3. Deduced invoker from explicit return type `Ret` and argument pack `Args...`
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
    return callAapcs64AutoThunk<sizeof...(Args), Ret>(cpu, stack_base, stack_size, aot_fn, args...);
}

// 4. Invoker specialized directly on a Vulkan PFN_* function pointer type `FnPtrT`
template <typename FnPtrT, typename CpuStateT, typename... CallArgs>
inline auto invokeLiftedByFnType(CpuStateT &cpu,
                                 uint8_t *stack_base,
                                 size_t stack_size,
                                 void (*aot_fn)(CpuStateT *),
                                 CallArgs... args) noexcept {
    return callAapcs64AutoThunk<FnPtrT>(cpu, stack_base, stack_size, aot_fn, args...);
}

} // namespace mali_aapcs64
