/*
 * SPDX-License-Identifier: MIT
 *
 * Authoritative Mali Kbase Driver Capability Table & Version Negotiation Implementation
 * (`src/adapter/mali_kbase_caps.cpp`)
 *
 * Grounded in:
 *   - `drivers/gpu/arm/midgard/mali_kbase_caps.h`
 *   - `drivers/gpu/arm/midgard/mali_kbase_caps.c` (`.rodata` `kbase_caps_table`)
 *   - `mali_kbase.ko` (MT6878 r44p1 `.rodata` offset 67800)
 *   - `mali_kbase_husky.ko` (Pixel 8 Pro Tensor G3 r54p1 `.rodata` offset 89160)
 */

#include "mali_kbase_caps.h"
#include <stddef.h>

static const struct mali_kbase_cap_descriptor g_kbase_cap_table[MALI_KBASE_NUM_CAPS] = {
   {
      MALI_KBASE_CAP_SYSTEM_MONITOR,
      "MALI_KBASE_CAP_SYSTEM_MONITOR",
      1, 0,   /* CSF min: 1.0 */
      11, 21, /* JM min: 11.21 */
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[0]"
   },
   {
      MALI_KBASE_CAP_JIT_PRESSURE_LIMIT,
      "MALI_KBASE_CAP_JIT_PRESSURE_LIMIT",
      1, 0,   /* CSF min: 1.0 */
      11, 25, /* JM min: 11.25 */
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[1]"
   },
   {
      MALI_KBASE_CAP_MEM_GROW_ON_GPF,
      "MALI_KBASE_CAP_MEM_GROW_ON_GPF",
      1, 0,   /* CSF min: 1.0 */
      11, 15, /* JM min: 11.15 */
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[2]"
   },
   {
      MALI_KBASE_CAP_MEM_PROTECTED,
      "MALI_KBASE_CAP_MEM_PROTECTED",
      1, 0,   /* CSF min: 1.0 */
      11, 15, /* JM min: 11.15 */
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[3]"
   },
   {
      MALI_KBASE_CAP_TIMELINE_TRACKING,
      "MALI_KBASE_CAP_TIMELINE_TRACKING",
      1, 32,  /* CSF min: 1.32 (husky r54p1 kbase_caps_table[4]) */
      11, 35, /* JM min: 11.35 */
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[4]"
   },
   {
      MALI_KBASE_CAP_CSG_USER_IO,
      "MALI_KBASE_CAP_CSG_USER_IO",
      1, 35,  /* CSF min: 1.35 (husky r54p1 kbase_caps_table[5]) */
      0, 0,
      "drivers/gpu/arm/midgard/mali_kbase_caps.h:kbase_caps_table[5]"
   },
   {
      MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_6,
      "MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_6",
      1, 6,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf.c:kbasep_cs_queue_group_create_1_6"
   },
   {
      MALI_KBASE_CAP_MEM_ALLOC_EX,
      "MALI_KBASE_CAP_MEM_ALLOC_EX",
      1, 9,
      11, 31,
      "drivers/gpu/arm/midgard/mali_kbase_core_linux.c:kbase_api_mem_alloc_ex"
   },
   {
      MALI_KBASE_CAP_CS_TILER_HEAP_INIT_1_13,
      "MALI_KBASE_CAP_CS_TILER_HEAP_INIT_1_13",
      1, 13,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf_tiler_heap.c:kbasep_cs_tiler_heap_init_1_13"
   },
   {
      MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_18,
      "MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_18",
      1, 18,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf.c:kbasep_cs_queue_group_create_1_18"
   },
   {
      MALI_KBASE_CAP_CS_QUEUE_REGISTER_EX,
      "MALI_KBASE_CAP_CS_QUEUE_REGISTER_EX",
      1, 21,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf.c:kbase_csf_queue_register_ex"
   },
   {
      MALI_KBASE_CAP_READ_USER_PAGE,
      "MALI_KBASE_CAP_READ_USER_PAGE",
      1, 21,
      0, 0,
      "drivers/gpu/arm/midgard/mali_kbase_core_linux.c:kbase_ioctl_read_user_page"
   },
   {
      MALI_KBASE_CAP_KCPU_QUEUE_CQS_WAIT_SET,
      "MALI_KBASE_CAP_KCPU_QUEUE_CQS_WAIT_SET",
      1, 21,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf_kcpu.c:kbase_csf_kcpu_queue_enqueue"
   },
   {
      MALI_KBASE_CAP_MEM_COMMIT_GROW_ON_GPF,
      "MALI_KBASE_CAP_MEM_COMMIT_GROW_ON_GPF",
      1, 21,
      11, 21,
      "drivers/gpu/arm/midgard/mali_kbase_mem_linux.c:kbase_mem_commit"
   },
   {
      MALI_KBASE_CAP_CS_TILER_HEAP_CHUNK_VA,
      "MALI_KBASE_CAP_CS_TILER_HEAP_CHUNK_VA",
      1, 28,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf_tiler_heap.c:kbase_csf_tiler_heap_init"
   },
   {
      MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_35,
      "MALI_KBASE_CAP_CS_QUEUE_GROUP_CREATE_1_35",
      1, 35,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf.c:kbasep_cs_queue_group_create_1_35"
   },
   {
      MALI_KBASE_CAP_SHARED_CSG_SYNC_SCOPE,
      "MALI_KBASE_CAP_SHARED_CSG_SYNC_SCOPE",
      1, 37,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf_firmware.c:MALI_CS_SYNC_SCOPE_CSG"
   },
   {
      MALI_KBASE_CAP_HUSKY_R54P3_EXTENDED_CSI,
      "MALI_KBASE_CAP_HUSKY_R54P3_EXTENDED_CSI",
      1, 38,
      0, 0,
      "drivers/gpu/arm/midgard/csf/mali_kbase_csf.c:kbase_csf_queue_bind"
   },
};

extern "C" const struct mali_kbase_cap_descriptor *
mali_kbase_get_cap_descriptor(enum mali_kbase_cap cap)
{
   if ((int)cap < 0 || (int)cap >= (int)MALI_KBASE_NUM_CAPS)
      return NULL;
   return &g_kbase_cap_table[(int)cap];
}

extern "C" const char *mali_kbase_cap_name(enum mali_kbase_cap cap)
{
   const struct mali_kbase_cap_descriptor *desc = mali_kbase_get_cap_descriptor(cap);
   return desc ? desc->name : "MALI_KBASE_CAP_UNKNOWN";
}

extern "C" bool mali_kbase_supports_cap_query(uint64_t api_version, enum mali_kbase_cap cap)
{
   const struct mali_kbase_cap_descriptor *desc = mali_kbase_get_cap_descriptor(cap);
   if (!desc)
      return false;

   uint16_t major = mali_kbase_api_version_major(api_version);
   uint16_t minor = mali_kbase_api_version_minor(api_version);

   /* Job Manager (JM) major versions >= 10 */
   if (major >= 10) {
      if (desc->min_major_jm == 0)
         return false;
      uint32_t req_ver = ((uint32_t)desc->min_major_jm << 16) | (uint32_t)desc->min_minor_jm;
      uint32_t cur_ver = ((uint32_t)major << 16) | (uint32_t)minor;
      return cur_ver >= req_ver;
   }

   /* CSF major version == 1 (or > 1) */
   if (desc->min_major_csf == 0)
      return false;
   uint32_t req_ver = ((uint32_t)desc->min_major_csf << 16) | (uint32_t)desc->min_minor_csf;
   uint32_t cur_ver = ((uint32_t)major << 16) | (uint32_t)minor;
   return cur_ver >= req_ver;
}
