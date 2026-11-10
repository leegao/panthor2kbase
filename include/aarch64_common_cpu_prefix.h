/*
 * SPDX-License-Identifier: MIT
 *
 * Shared AArch64 CPU State Prefix Interface (`include/aarch64_common_cpu_prefix.h`)
 *
 * Enforces an identical 256-byte leading prefix across:
 *   1. User-space shared library AOT lifting (`struct AOTCPUState` for `libGLES_mali.so`)
 *   2. Kernel-space module lifting (`struct LiftedKbaseCPU` for `mali_kbase.ko` / `mali_kbase_husky.ko`)
 *
 * Byte Offsets:
 *   - `x[0..30]` : bytes   0 .. 247 (31 * 8 bytes, general-purpose registers X0..X30)
 *   - `sp`       : bytes 248 .. 255 (8 bytes, AArch64 stack pointer SP)
 *
 * Because both `AOTCPUState` and `LiftedKbaseCPU` embed this exact 256-byte prefix at
 * offset 0 and enforce it via `AARCH64_STATIC_ASSERT_CPU_PREFIX(Type)`, every unified
 * `horizon2llvm` pass that operates on `x[0..30]` and `sp` works identically across
 * `LoweringMode::UserspaceBionicSo` and `LoweringMode::KernelModuleKo` without offset
 * translation.
 */
#ifndef AARCH64_COMMON_CPU_PREFIX_H
#define AARCH64_COMMON_CPU_PREFIX_H

#include <stddef.h>
#include <stdint.h>

#define AARCH64_CPU_X0_OFFSET  0U
#define AARCH64_CPU_X18_REG_OFFSET (18U * 8U)
#define AARCH64_CPU_X30_OFFSET (30U * 8U)
#define AARCH64_CPU_SP_OFFSET  248U
#define AARCH64_CPU_PREFIX_BYTES 256U

#define AARCH64_COMMON_CPU_PREFIX_FIELDS \
    uint64_t x[31];                      \
    uint64_t sp;

struct AArch64CommonCPUPrefix {
    AARCH64_COMMON_CPU_PREFIX_FIELDS
};

#if defined(__cplusplus)
#define AARCH64_STATIC_ASSERT_CPU_PREFIX(Type)                                                      \
    static_assert(offsetof(Type, x) == AARCH64_CPU_X0_OFFSET,                                       \
                  #Type "::x[0..30] must start at byte offset 0");                                  \
    static_assert(sizeof(((Type *)nullptr)->x) == 31U * sizeof(uint64_t),                           \
                  #Type "::x must be 31 x uint64_t (248 bytes)");                                   \
    static_assert(offsetof(Type, sp) == AARCH64_CPU_SP_OFFSET,                                      \
                  #Type "::sp must be at byte offset 248")
#else
#define AARCH64_STATIC_ASSERT_CPU_PREFIX(Type)                                                      \
    _Static_assert(offsetof(Type, x) == AARCH64_CPU_X0_OFFSET,                                      \
                   #Type "::x[0..30] must start at byte offset 0");                                 \
    _Static_assert(sizeof(((Type *)0)->x) == 31U * sizeof(uint64_t),                                \
                   #Type "::x must be 31 x uint64_t (248 bytes)");                                  \
    _Static_assert(offsetof(Type, sp) == AARCH64_CPU_SP_OFFSET,                                     \
                   #Type "::sp must be at byte offset 248")
#endif

AARCH64_STATIC_ASSERT_CPU_PREFIX(struct AArch64CommonCPUPrefix);

#endif /* AARCH64_COMMON_CPU_PREFIX_H */
