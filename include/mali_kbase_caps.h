/*
 * SPDX-License-Identifier: MIT
 *
 * Authoritative Mali Kbase Driver Capability Table & Version Negotiation API
 * (`include/mali_kbase_caps.h`)
 *
 * Grounded in upstream Linux kernel source:
 *   drivers/gpu/arm/midgard/mali_kbase_caps.h
 *   drivers/gpu/arm/midgard/mali_kbase_caps.c (`.rodata` `kbase_caps_table`)
 * and verified against both:
 *   - MT6878 r44p1 `mali_kbase.ko` (`.rodata` offset 67800, 8 x uint16_t)
 *   - Pixel 8 Pro Tensor G3 r54p1 `mali_kbase_husky.ko` (`.rodata` offset 89160, 36 x uint16_t)
 */

#ifndef MALI_KBASE_CAPS_H
#define MALI_KBASE_CAPS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pack (major, minor) into the 32-bit `api_version` format stored in
 * `kctx->api_version` (`((uint32_t)major << 16) | (uint32_t)minor`).
 */
static inline uint64_t mali_kbase_make_api_version(uint16_t major, uint16_t minor)
{
   return ((uint64_t)major << 16) | (uint64_t)minor;
}

static inline uint16_t mali_kbase_api_version_major(uint64_t api_version)
{
   return (uint16_t)((api_version >> 16) & 0xffffU);
}

static inline uint16_t mali_kbase_api_version_minor(uint64_t api_version)
{
   return (uint16_t)(api_version & 0xffffU);
}

/**
 * Authoritative enumeration of Mali Kbase capabilities across the `.rodata`
 * `kbase_caps_table` (0..5) and extended CSF UAPI version gates (6..17).
 *
 * Indices 0..5 correspond 1-for-1 to `enum mali_kbase_cap` in
 * `drivers/gpu/arm/midgard/mali_kbase_caps.h`:
 *   0: MALI_KBASE_CAP_SYSTEM_MONITOR         (CSF min: 1.0)
 *   1: MALI_KBASE_CAP_JIT_PRESSURE_LIMIT     (CSF min: 1.0)
 *   2: MALI_KBASE_CAP_MEM_GROW_ON_GPF        (CSF min: 1.0)
 *   3: MALI_KBASE_CAP_MEM_PROTECTED          (CSF min: 1.0)
 *   4: MALI_KBASE_CAP_TIMELINE_TRACKING      (CSF min: 1.32 in r54p1)
 *   5: MALI_KBASE_CAP_CSG_USER_IO            (CSF min: 1.35 in r54p1)
 *
 * Indices 6..17 cover the remaining UAPI ioctl & CSF capability gates in
 * `mali_kbase_core_linux.c` and `mali_kbase_csf.c`:
 *   6: MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_6   (CSF min: 1.6)
 *   7: MALI_KBASE_CAP_MEM_ALLOC_EX                (CSF min: 1.9)
 *   8: MALI_KBASE_CAP_CS_TILER_HEAP_INIT_1_13     (CSF min: 1.13)
 *   9: MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_18  (CSF min: 1.18)
 *  10: MALI_KBASE_CAP_CS_QUEUE_REGISTER_EX        (CSF min: 1.21)
 *  11: MALI_KBASE_CAP_READ_USER_PAGE              (CSF min: 1.21)
 *  12: MALI_KBASE_CAP_KCPU_QUEUE_CQS_WAIT_SET     (CSF min: 1.21)
 *  13: MALI_KBASE_CAP_MEM_COMMIT_GROW_ON_GPF      (CSF min: 1.21)
 *  14: MALI_KBASE_CAP_CS_TILER_HEAP_CHUNK_VA      (CSF min: 1.28)
 *  15: MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_35  (CSF min: 1.35)
 *  16: MALI_KBASE_CAP_SHARED_CSG_SYNC_SCOPE       (CSF min: 1.37)
 *  17: MALI_KBASE_CAP_HUSKY_R54P3_EXTENDED_CSI    (CSF min: 1.38)
 */
enum mali_kbase_cap {
   MALI_KBASE_CAP_SYSTEM_MONITOR = 0,
   MALI_KBASE_CAP_JIT_PRESSURE_LIMIT = 1,
   MALI_KBASE_CAP_MEM_GROW_ON_GPF = 2,
   MALI_KBASE_CAP_MEM_PROTECTED = 3,
   MALI_KBASE_CAP_TIMELINE_TRACKING = 4,
   MALI_KBASE_CAP_CSG_USER_IO = 5,
   MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_6 = 6,
   MALI_KBASE_CAP_MEM_ALLOC_EX = 7,
   MALI_KBASE_CAP_CS_TILER_HEAP_INIT_1_13 = 8,
   MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_18 = 9,
   MALI_KBASE_CAP_CS_QUEUE_REGISTER_EX = 10,
   MALI_KBASE_CAP_READ_USER_PAGE = 11,
   MALI_KBASE_CAP_KCPU_QUEUE_CQS_WAIT_SET = 12,
   MALI_KBASE_CAP_MEM_COMMIT_GROW_ON_GPF = 13,
   MALI_KBASE_CAP_CS_TILER_HEAP_CHUNK_VA = 14,
   MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_35 = 15,
   MALI_KBASE_CAP_SHARED_CSG_SYNC_SCOPE = 16,
   MALI_KBASE_CAP_HUSKY_R54P3_EXTENDED_CSI = 17,
   MALI_KBASE_NUM_CAPS = 18
};

struct mali_kbase_cap_descriptor {
   enum mali_kbase_cap cap;
   const char *name;
   uint16_t min_major_csf;
   uint16_t min_minor_csf;
   uint16_t min_major_jm;
   uint16_t min_minor_jm;
   const char *kernel_source_ref;
};

/**
 * Authoritative capability check matching `mali_kbase_supports_cap(api_version, cap)`
 * in `mali_kbase.ko` / `mali_kbase_husky.ko`.
 */
bool mali_kbase_supports_cap_query(uint64_t api_version, enum mali_kbase_cap cap);

/**
 * Lookup the descriptor metadata (name, minimum CSF/JM version, kernel source reference)
 * for any capability in `0 .. MALI_KBASE_NUM_CAPS - 1`.
 */
const struct mali_kbase_cap_descriptor *mali_kbase_get_cap_descriptor(enum mali_kbase_cap cap);

/**
 * Human-readable name for a capability enum value.
 */
const char *mali_kbase_cap_name(enum mali_kbase_cap cap);

/* Inline convenience helpers for specific capabilities */
static inline bool mali_kbase_supports_system_monitor(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_SYSTEM_MONITOR);
}

static inline bool mali_kbase_supports_jit_pressure_limit(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_JIT_PRESSURE_LIMIT);
}

static inline bool mali_kbase_supports_mem_grow_on_gpf(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_MEM_GROW_ON_GPF);
}

static inline bool mali_kbase_supports_mem_protected(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_MEM_PROTECTED);
}

static inline bool mali_kbase_supports_timeline_tracking(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_TIMELINE_TRACKING);
}

static inline bool mali_kbase_supports_csg_user_io(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_CSG_USER_IO);
}

static inline bool mali_kbase_supports_mem_alloc_ex(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_MEM_ALLOC_EX);
}

static inline bool mali_kbase_supports_shared_csg_sync_scope(uint64_t api_version)
{
   return mali_kbase_supports_cap_query(api_version, MALI_KBASE_CAP_SHARED_CSG_SYNC_SCOPE);
}

#ifdef __cplusplus
}
#endif

#endif /* MALI_KBASE_CAPS_H */
