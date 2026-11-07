/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Complete ARM Mali Kbase Kernel Driver UAPI Definitions (CSF r38p0 - r54p3)
 * Grounded in:
 *  - MediaTek MT6878 mali-r44p1 drivers/gpu/arm/midgard/include/uapi/gpu/arm/midgard/csf/mali_base_csf_kernel.h
 *  - MediaTek MT6878 mali-r44p1 drivers/gpu/arm/midgard/include/uapi/gpu/arm/midgard/csf/mali_kbase_csf_ioctl.h
 *  - Pixel GKI gpu/common/include/uapi/gpu/arm/midgard/mali_kbase_ioctl.h
 *  - Binary verification against unstripped mali_kbase.ko (CSF v1.21, AArch64)
 */
#ifndef MALI_KBASE_UAPI_H
#define MALI_KBASE_UAPI_H

#include <stdint.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBASE_IOCTL_TYPE 0x80

/* ============================================================================
 * 1. Base Memory Allocation Flags (mali_base_common_kernel.h & mali_kbase.ko)
 *
 * Verified against mali_kbase.ko:
 *  - kbase_api_mem_alloc_ex (0x29eec):
 *      * rejects kernel-only bits (flags & 0xa0) -> -ENOMEM (bit 5 & bit 7)
 *      * when BASE_MEM_CSF_EVENT (1<<19) is set, forces flags |= 0x3400
 *        (BASE_MEM_SAME_VA | BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM)
 *  - kbase_mem_alloc (0x1367c):
 *      * rejects setting both BASE_MEM_COHERENT_SYSTEM (1<<10) and
 *        BASE_MEM_COHERENT_LOCAL (1<<11): (~flags & 0xc00) == 0 -> -ENOMEM
 *      * rejects GPU_EX (1<<4) combined with GPU_WR (1<<3) or GROW_ON_GPF (1<<9)
 *      * rejects FIXED/FIXABLE ((1<<8)|(1<<29)) combined with SAME_VA (1<<13)
 *  - kbase_check_alloc_sizes (0x7044):
 *      * rejects BASE_MEM_GROW_ON_GPF (1<<9) when extension == 0 -> -ENOMEM
 * ============================================================================ */
#define BASE_MEM_PROT_CPU_RD                (1ULL << 0)   /* 0x00000001 */
#define BASE_MEM_PROT_CPU_WR                (1ULL << 1)   /* 0x00000002 */
#define BASE_MEM_PROT_GPU_RD                (1ULL << 2)   /* 0x00000004 */
#define BASE_MEM_PROT_GPU_WR                (1ULL << 3)   /* 0x00000008 */
#define BASE_MEM_PROT_GPU_EX                (1ULL << 4)   /* 0x00000010 */
#define BASEP_MEM_PERMANENT_KERNEL_MAPPING  (1ULL << 5)   /* 0x00000020 (KERNEL ONLY!) */
#define BASE_MEM_GPU_VA_SAME_4GB_PAGE       (1ULL << 6)   /* 0x00000040 */
#define BASEP_MEM_NO_USER_FREE              (1ULL << 7)   /* 0x00000080 (KERNEL ONLY!) */
#define BASE_MEM_FIXED                      (1ULL << 8)   /* 0x00000100 */
#define BASE_MEM_GROW_ON_GPF                (1ULL << 9)   /* 0x00000200 */
#define BASE_MEM_COHERENT_SYSTEM            (1ULL << 10)  /* 0x00000400 */
#define BASE_MEM_COHERENT_LOCAL             (1ULL << 11)  /* 0x00000800 */
#define BASE_MEM_CACHED_CPU                 (1ULL << 12)  /* 0x00001000 */
#define BASE_MEM_SAME_VA                    (1ULL << 13)  /* 0x00002000 */
#define BASE_MEM_NEED_MMAP                  (1ULL << 14)  /* 0x00004000 */
#define BASE_MEM_COHERENT_SYSTEM_REQUIRED   (1ULL << 15)  /* 0x00008000 */
#define BASE_MEM_PROTECTED                  (1ULL << 16)  /* 0x00010000 */
#define BASE_MEM_DONT_NEED                  (1ULL << 17)  /* 0x00020000 */
#define BASE_MEM_IMPORT_SHARED              (1ULL << 18)  /* 0x00040000 */
#define BASE_MEM_CSF_EVENT                  (1ULL << 19)  /* 0x00080000 */
#define BASE_MEM_TILER_ALIGN_TOP            (1ULL << 20)  /* 0x00100000 */
#define BASE_MEM_UNCACHED_GPU               (1ULL << 21)  /* 0x00200000 */

#define BASE_MEM_GROUP_ID_SHIFT             22
#define BASE_MEM_GROUP_ID_MASK              (0xFULL << BASE_MEM_GROUP_ID_SHIFT)

#define BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP   (1ULL << 26)  /* 0x04000000 */
#define BASE_MEM_FLAG_MAP_FIXED             (1ULL << 27)  /* 0x08000000 */
#define BASE_MEM_KERNEL_SYNC                (1ULL << 28)  /* 0x10000000 */
#define BASE_MEM_FIXABLE                    (1ULL << 29)  /* 0x20000000 */

#define BASEP_MEM_FLAGS_KERNEL_ONLY \
   (BASEP_MEM_PERMANENT_KERNEL_MAPPING | BASEP_MEM_NO_USER_FREE)

/* Context creation flags */
#define BASE_CONTEXT_CREATE_FLAG_NONE                 0U
#define BASE_CONTEXT_CCTX_EMBEDDED                    (1U << 0)
#define BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED   (1U << 1)
#define BASE_CONTEXT_CSF_EVENT_THREAD                 (1U << 2)

/* CSF CSI exception handler flags (mali_base_csf_kernel.h:145) */
#define BASE_CSF_TILER_OOM_EXCEPTION_FLAG             (1U << 0)

/* Queue Group Priorities (mali_base_csf_kernel.h:187) */
#define BASE_QUEUE_GROUP_PRIORITY_HIGH                0U
#define BASE_QUEUE_GROUP_PRIORITY_MEDIUM              1U
#define BASE_QUEUE_GROUP_PRIORITY_LOW                 2U
#define BASE_QUEUE_GROUP_PRIORITY_REALTIME            3U

/* Special Mmap Handles (Page-Aligned Cookies in mali_base_common_kernel.h) */
#define BASE_MEM_INVALID_HANDLE                (0ULL  << 12)
#define BASE_MEM_MAP_TRACKING_HANDLE           (3ULL  << 12)
#define BASEP_MEM_WRITE_ALLOC_PAGES_HANDLE     (46ULL << 12)
#define BASEP_MEM_CSF_USER_REG_PAGE_HANDLE     (47ULL << 12)
#define BASEP_MEM_CSF_USER_IO_PAGES_HANDLE     (48ULL << 12)
#define BASE_MEM_COOKIE_BASE                   (64ULL << 12)
#define BASE_MEM_FIRST_FREE_ADDRESS            ((64ULL + 64ULL) << 12)

/* CSF USER_IO 3-Page Offsets (4KB Page Granularity) */
#define CS_USER_IO_DOORBELL_PAGE_OFFSET        0x0000UL
#define CS_USER_IO_INPUT_PAGE_OFFSET           0x1000UL
#define CS_USER_IO_OUTPUT_PAGE_OFFSET          0x2000UL
#define CS_USER_IO_MMAP_SIZE                   (3UL * 4096UL)

/* Offsets within USER_IO Pages (mali_kbase_csf_registers.h) */
#define CS_USER_IO_DOORBELL_OFFSET             0x0000UL /* u32 write triggers doorbell */
#define CS_USER_IO_CS_INSERT_OFFSET            0x1000UL /* u64 byte offset in ring */
#define CS_USER_IO_CS_EXTRACT_INIT_OFFSET      0x1008UL /* u64 initial extract */
#define CS_USER_IO_CS_EXTRACT_OFFSET           0x2000UL /* u64 current FW extract */
#define CS_USER_IO_CS_ACTIVE_OFFSET            0x2008UL /* u32 active status */
#define CS_USER_IO_CS_FAULT_OFFSET             0x2080UL /* u32 recoverable CS fault */
#define CS_USER_IO_CS_FATAL_OFFSET             0x2084UL /* u32 fatal CS fault */
#define CS_USER_IO_CS_FAULT_INFO_OFFSET        0x2088UL /* u64 fault info */
#define CS_USER_IO_CS_FATAL_INFO_OFFSET        0x2090UL /* u64 fatal info */
#define CS_USER_IO_CS_HEAP_VT_START_OFFSET     0x20c0UL /* u32 vt_start */
#define CS_USER_IO_CS_HEAP_VT_END_OFFSET       0x20c4UL /* u32 vt_end */
#define CS_USER_IO_CS_HEAP_FRAG_END_OFFSET     0x20ccUL /* u32 frag_end */

/* ============================================================================
 * 2. GPU Property Keys (KBASE_IOCTL_GET_GPUPROPS TLV Encoding)
 * ============================================================================ */
#define KBASE_GPUPROP_VALUE_SIZE_U8            0U
#define KBASE_GPUPROP_VALUE_SIZE_U16           1U
#define KBASE_GPUPROP_VALUE_SIZE_U32           2U
#define KBASE_GPUPROP_VALUE_SIZE_U64           3U

#define KBASE_GPUPROP_PRODUCT_ID                    1U
#define KBASE_GPUPROP_VERSION_STATUS                2U
#define KBASE_GPUPROP_MINOR_REVISION                3U
#define KBASE_GPUPROP_MAJOR_REVISION                4U
#define KBASE_GPUPROP_GPU_FREQ_KHZ_MAX              6U
#define KBASE_GPUPROP_LOG2_PROGRAM_COUNTER_SIZE     8U
#define KBASE_GPUPROP_TEXTURE_FEATURES_0            9U
#define KBASE_GPUPROP_TEXTURE_FEATURES_1            10U
#define KBASE_GPUPROP_TEXTURE_FEATURES_2            11U
#define KBASE_GPUPROP_GPU_AVAILABLE_MEMORY_SIZE     12U
#define KBASE_GPUPROP_L2_LOG2_LINE_SIZE             13U
#define KBASE_GPUPROP_L2_LOG2_CACHE_SIZE            14U
#define KBASE_GPUPROP_L2_NUM_L2_SLICES              15U
#define KBASE_GPUPROP_TILER_BIN_SIZE_BYTES          16U
#define KBASE_GPUPROP_TILER_MAX_ACTIVE_LEVELS       17U
#define KBASE_GPUPROP_MAX_THREADS                   18U
#define KBASE_GPUPROP_MAX_WORKGROUP_SIZE            19U
#define KBASE_GPUPROP_MAX_BARRIER_SIZE              20U
#define KBASE_GPUPROP_MAX_REGISTERS                 21U
#define KBASE_GPUPROP_MAX_TASK_QUEUE                22U
#define KBASE_GPUPROP_MAX_THREAD_GROUP_SPLIT        23U
#define KBASE_GPUPROP_IMPL_TECH                     24U
#define KBASE_GPUPROP_RAW_SHADER_PRESENT            25U
#define KBASE_GPUPROP_RAW_TILER_PRESENT             26U
#define KBASE_GPUPROP_RAW_L2_PRESENT                27U
#define KBASE_GPUPROP_RAW_STACK_PRESENT             28U
#define KBASE_GPUPROP_RAW_L2_FEATURES               29U
#define KBASE_GPUPROP_RAW_CORE_FEATURES             30U
#define KBASE_GPUPROP_RAW_MEM_FEATURES              31U
#define KBASE_GPUPROP_RAW_MMU_FEATURES              32U
#define KBASE_GPUPROP_RAW_AS_PRESENT                33U
#define KBASE_GPUPROP_RAW_JS_PRESENT                34U
#define KBASE_GPUPROP_RAW_JS_FEATURES_0             35U
#define KBASE_GPUPROP_RAW_TILER_FEATURES            51U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0        52U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1        53U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2        54U
#define KBASE_GPUPROP_RAW_GPU_ID                    55U
#define KBASE_GPUPROP_RAW_THREAD_MAX_THREADS        56U
#define KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE 57U
#define KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE   58U
#define KBASE_GPUPROP_RAW_THREAD_FEATURES           59U
#define KBASE_GPUPROP_RAW_COHERENCY_MODE            60U
#define KBASE_GPUPROP_COHERENCY_NUM_GROUPS          61U
#define KBASE_GPUPROP_COHERENCY_NUM_CORE_GROUPS     62U
#define KBASE_GPUPROP_COHERENCY_COHERENCY           63U
#define KBASE_GPUPROP_COHERENCY_GROUP_0             64U
#define KBASE_GPUPROP_TEXTURE_FEATURES_3            80U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3        81U
#define KBASE_GPUPROP_NUM_EXEC_ENGINES              82U
#define KBASE_GPUPROP_RAW_THREAD_TLS_ALLOC          83U
#define KBASE_GPUPROP_TLS_ALLOC                     84U
#define KBASE_GPUPROP_RAW_GPU_FEATURES              85U

/* ============================================================================
 * 3. Kbase Core & CSF IOCTL Structures
 * ============================================================================ */

struct kbase_ioctl_version_check {
   __u16 major;
   __u16 minor;
};

struct kbase_ioctl_set_flags {
   __u32 create_flags;
};

struct kbase_ioctl_get_gpuprops {
   __u64 buffer;
   __u32 size;
   __u32 flags;
};

union kbase_ioctl_mem_alloc {
   struct {
      __u64 va_pages;
      __u64 commit_pages;
      __u64 extension;
      __u64 flags;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
   } out;
};

union kbase_ioctl_mem_alloc_ex {
   struct {
      __u64 va_pages;
      __u64 commit_pages;
      __u64 extension;
      __u64 flags;
      __u64 fixed_address;
      __u64 extra[3];
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
   } out;
};

union kbase_ioctl_mem_query {
   struct {
      __u64 gpu_addr;
      __u64 query;
   } in;
   struct {
      __u64 value;
   } out;
};

#define KBASE_MEM_QUERY_COMMIT_SIZE  1ULL
#define KBASE_MEM_QUERY_VA_SIZE      2ULL
#define KBASE_MEM_QUERY_FLAGS        3ULL

struct kbase_ioctl_mem_free {
   __u64 gpu_addr;
};

struct kbase_ioctl_get_ddk_version {
   __u64 version_buffer;
   __u32 size;
   __u32 padding;
};

struct kbase_ioctl_mem_jit_init {
   __u64 va_pages;
   __u8 max_allocations;
   __u8 trim_level;
   __u8 group_id;
   __u8 padding[5];
   __u64 phys_pages;
};

#define BASE_SYNCSET_OP_MSYNC (1U << 0)
#define BASE_SYNCSET_OP_CSYNC (1U << 1)

struct kbase_ioctl_mem_sync {
   __u64 handle;
   __u64 user_addr;
   __u64 size;
   __u8 type;
   __u8 padding[7];
};

struct kbase_ioctl_mem_commit {
   __u64 gpu_addr;
   __u64 pages;
};

struct base_mem_aliasing_info {
   __u64 handle;
   __u64 offset;
   __u64 length;
};

union kbase_ioctl_mem_alias {
   struct {
      __u64 flags;
      __u64 stride;
      __u64 nents;
      __u64 aliasing_info;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
      __u64 va_pages;
   } out;
};

#define BASE_MEM_IMPORT_TYPE_INVALID     0U
#define BASE_MEM_IMPORT_TYPE_UMP         1U
#define BASE_MEM_IMPORT_TYPE_UMM         2U /* Standard DMA-BUF fd pointer */
#define BASE_MEM_IMPORT_TYPE_USER_BUFFER 3U

/* Standard 24-byte kbase_ioctl_mem_import (ARM r38p0-r54p3) */
union kbase_ioctl_mem_import {
   struct {
      __u64 flags;
      __u64 phandle;
      __u32 type;
      __u32 padding;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
      __u64 va_pages;
   } out;
};

/* MediaTek 40-byte kbase_ioctl_mem_import variant (with 16-byte SLC tail) */
union kbase_ioctl_mem_import_mtk {
   struct {
      __u64 flags;
      __u64 phandle;
      __u32 type;
      __u32 padding;
      __u64 slc_extra[2];
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
      __u64 va_pages;
      __u64 slc_out[2];
   } out;
};

/*
 * KBASE_IOCTL_STREAM_CREATE (nr 24):
 * Verified in mali_kbase.ko (kbase_api_stream_create at 0x2a304):
 * _IOW(0x80, 24, struct kbase_ioctl_stream_create) takes a 32-byte char name[32]
 * input and returns the newly allocated sync stream file descriptor directly
 * as the positive return value `ret` of ioctl().
 */
struct kbase_ioctl_stream_create {
   char name[32];
};

union kbase_ioctl_stream_create_u {
   struct kbase_ioctl_stream_create in;
   struct {
      __s32 fd;
      __u8 pad[28];
   } out;
};

struct kbase_ioctl_mem_exec_init {
   __u64 va_pages;
};

/* CSF Queue & Group IOCTLs */
struct kbase_ioctl_cs_queue_register {
   __u64 buffer_gpu_addr;
   __u32 buffer_size;
   __u8 priority;
   __u8 padding[3];
};

struct kbase_ioctl_cs_queue_kick {
   __u64 buffer_gpu_addr;
};

union kbase_ioctl_cs_queue_bind {
   struct {
      __u64 buffer_gpu_addr;
      __u8 group_handle;
      __u8 csi_index;
      __u8 padding[6];
   } in;
   struct {
      __u64 mmap_handle;
   } out;
};

struct kbase_ioctl_cs_queue_register_ex {
   __u64 buffer_gpu_addr;
   __u32 buffer_size;
   __u8 priority;
   __u8 padding[3];
   __u64 ex_offset_var_addr;
   __u64 ex_buffer_base;
   __u32 ex_buffer_size;
   __u8 ex_event_size;
   __u8 ex_event_state;
   __u8 ex_padding[2];
};

struct kbase_ioctl_cs_queue_terminate {
   __u64 buffer_gpu_addr;
};

/* 32-byte 1.6 layout (supported across all CSF kbase kernels 1.6 - 1.38+) */
union kbase_ioctl_cs_queue_group_create_1_6 {
   struct {
      __u64 tiler_mask;
      __u64 fragment_mask;
      __u64 compute_mask;
      __u8 cs_min;
      __u8 priority;
      __u8 tiler_max;
      __u8 fragment_max;
      __u8 compute_max;
      __u8 padding[3];
   } in;
   struct {
      __u8 group_handle;
      __u8 padding[3];
      __u32 group_uid;
   } out;
};

/* 40-byte 1.18 layout */
union kbase_ioctl_cs_queue_group_create_1_18 {
   struct {
      __u64 tiler_mask;
      __u64 fragment_mask;
      __u64 compute_mask;
      __u8 cs_min;
      __u8 priority;
      __u8 tiler_max;
      __u8 fragment_max;
      __u8 compute_max;
      __u8 csi_handlers;
      __u8 padding[2];
      __u64 dvs_buf;
   } in;
   struct {
      __u8 group_handle;
      __u8 padding[3];
      __u32 group_uid;
   } out;
};

/* 56-byte Pixel r54p3 layout */
union kbase_ioctl_cs_queue_group_create_56 {
   struct {
      __u64 tiler_mask;
      __u64 fragment_mask;
      __u64 compute_mask;
      __u8 cs_min;
      __u8 priority;
      __u8 tiler_max;
      __u8 fragment_max;
      __u8 compute_max;
      __u8 csi_handlers;
      __u8 padding[2];
      __u64 reserved;
      __u64 dvs_buf;
      __u8 neural_max;
      __u8 reserved2[7];
   } in;
   struct {
      __u8 group_handle;
      __u8 padding[3];
      __u32 group_uid;
   } out;
};

/* 112-byte 1.19-1.25+ layout (verified in MT6878 mali_kbase_csf_ioctl.h:348) */
union kbase_ioctl_cs_queue_group_create_112 {
   struct {
      __u64 tiler_mask;
      __u64 fragment_mask;
      __u64 compute_mask;
      __u8 cs_min;
      __u8 priority;
      __u8 tiler_max;
      __u8 fragment_max;
      __u8 compute_max;
      __u8 csi_handlers;
      __u8 neural_max;
      __u8 cs_fault_report_enable;
      __u64 dvs_buf;
      __u64 neural_mask;
      __u8 comp_pri_threshold;
      __u8 comp_pri_ratio;
      __u8 padding[62];
   } in;
   struct {
      __u8 group_handle;
      __u8 padding[3];
      __u32 group_uid;
   } out;
};

struct kbase_ioctl_cs_queue_group_term {
   __u8 group_handle;
   __u8 padding[7];
};

/* CSF Event / Error Notification read from /dev/mali0 (mali_base_csf_kernel.h:477-596) */
enum base_gpu_queue_group_error_type {
   BASE_GPU_QUEUE_GROUP_ERROR_FATAL = 0,
   BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL = 1,
   BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT = 2,
   BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM = 3,
   BASE_GPU_QUEUE_GROUP_ERROR_FATAL_COUNT = 4
};

enum base_csf_notification_type {
   BASE_CSF_NOTIFICATION_EVENT = 0,
   BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR = 1,
   BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP = 2,
   BASE_CSF_NOTIFICATION_COUNT = 3
};

struct base_gpu_queue_group_error_fatal_payload {
   __u64 sideband;
   __u32 status;
   __u32 padding;
};

struct base_gpu_queue_error_fatal_payload {
   __u64 sideband;
   __u32 status;
   __u8 csi_index;
   __u8 padding[3];
};

struct base_gpu_queue_group_error {
   __u8 error_type;
   __u8 padding[7];
   union {
      struct base_gpu_queue_group_error_fatal_payload fatal_group;
      struct base_gpu_queue_error_fatal_payload fatal_queue;
   } payload;
};

struct base_csf_notification {
   __u8 type;
   __u8 padding[7];
   union {
      struct {
         __u8 handle;
         __u8 padding[7];
         struct base_gpu_queue_group_error error;
      } csg_error;
      struct {
         __u8 cmd;
      } dump;
      __u8 align[56];
   } payload;
};

typedef __u8 base_kcpu_queue_id;

struct kbase_ioctl_kcpu_queue_new {
   base_kcpu_queue_id id;
   __u8 padding[7];
};

struct kbase_ioctl_kcpu_queue_delete {
   base_kcpu_queue_id id;
   __u8 padding[7];
};

struct kbase_ioctl_kcpu_queue_enqueue {
   __u64 addr;
   __u32 nr_commands;
   base_kcpu_queue_id id;
   __u8 padding[3];
};

enum base_kcpu_command_type {
   BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL = 0,
   BASE_KCPU_COMMAND_TYPE_FENCE_WAIT = 1,
   BASE_KCPU_COMMAND_TYPE_CQS_WAIT = 2,
   BASE_KCPU_COMMAND_TYPE_CQS_SET = 3,
   BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION = 4,
   BASE_KCPU_COMMAND_TYPE_CQS_SET_OPERATION = 5,
   BASE_KCPU_COMMAND_TYPE_MAP_IMPORT = 6,
   BASE_KCPU_COMMAND_TYPE_UNMAP_IMPORT = 7,
   BASE_KCPU_COMMAND_TYPE_UNMAP_IMPORT_FORCE = 8,
   BASE_KCPU_COMMAND_TYPE_JIT_ALLOC = 9,
   BASE_KCPU_COMMAND_TYPE_JIT_FREE = 10,
   BASE_KCPU_COMMAND_TYPE_GROUP_SUSPEND = 11,
   BASE_KCPU_COMMAND_TYPE_ERROR_BARRIER = 12,
};

#define BASEP_CQS_WAIT_OPERATION_LE 0U
#define BASEP_CQS_WAIT_OPERATION_GT 1U
#define BASEP_CQS_DATA_TYPE_U32     0U
#define BASEP_CQS_DATA_TYPE_U64     1U

struct base_fence {
   struct {
      __s32 fd;
      __s32 stream_fd;
   } basep;
};

struct base_cqs_wait_operation_info {
   __u64 addr;
   __u64 val;
   __u8 operation;
   __u8 data_type;
   __u8 padding[6];
};

struct base_kcpu_command {
   __u8 type;
   __u8 padding[7];
   union {
      struct {
         __u64 fence;
      } fence;
      struct {
         __u64 objs;
         __u32 nr_objs;
         __u32 inherit_err_flags;
      } cqs_wait_operation;
      __u64 padding[2];
   } info;
};

/* 16-byte 1.13 tiler heap init layout */
union kbase_ioctl_cs_tiler_heap_init_1_13 {
   struct {
      __u32 chunk_size;
      __u32 initial_chunks;
      __u32 max_chunks;
      __u16 target_in_flight;
      __u8 group_id;
      __u8 padding;
   } in;
   struct {
      __u64 gpu_heap_va;
      __u64 first_chunk_va;
   } out;
};

/* 24-byte r54p3 tiler heap init layout */
union kbase_ioctl_cs_tiler_heap_init {
   struct {
      __u32 chunk_size;
      __u32 initial_chunks;
      __u32 max_chunks;
      __u16 target_in_flight;
      __u8 group_id;
      __u8 padding;
      __u64 buf_desc_va;
   } in;
   struct {
      __u64 gpu_heap_va;
      __u64 first_chunk_va;
   } out;
};

struct kbase_ioctl_cs_tiler_heap_term {
   __u64 gpu_heap_va;
};

union kbase_ioctl_get_cpu_gpu_timeinfo {
   struct {
      __u32 request_flags;
      __u32 paddings[7];
   } in;
   struct {
      __u64 sec;
      __u32 nsec;
      __u32 padding;
      __u64 timestamp;
      __u64 cycle_counter;
   } out;
};

struct basep_cs_stream_control {
   __u32 features;
   __u32 padding;
};

struct basep_cs_group_control {
   __u32 features;
   __u32 stream_num;
   __u32 suspend_size;
   __u32 padding;
};

union kbase_ioctl_cs_get_glb_iface {
   struct {
      __u32 max_group_num;
      __u32 max_total_stream_num;
      __u64 groups_ptr;
      __u64 streams_ptr;
   } in;
   struct {
      __u32 glb_version;
      __u32 features;
      __u32 group_num;
      __u32 prfcnt_size;
      __u32 total_stream_num;
      __u32 instr_features;
   } out;
};

struct kbase_ioctl_context_priority_check {
   __u8 priority;
};

/* ============================================================================
 * 4. Kbase IOCTL Command Numbers (Verified against mali_kbase.ko kbase_kfile_ioctl)
 * ============================================================================ */
#define KBASE_IOCTL_VERSION_CHECK_RESERVED \
   _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS \
   _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_GET_GPUPROPS \
   _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)
#define KBASE_IOCTL_MEM_ALLOC \
   _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_QUERY \
   _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_IOCTL_MEM_FREE \
   _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)
#define KBASE_IOCTL_GET_DDK_VERSION \
   _IOW(KBASE_IOCTL_TYPE, 13, struct kbase_ioctl_get_ddk_version)
#define KBASE_IOCTL_MEM_JIT_INIT \
   _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)
#define KBASE_IOCTL_MEM_SYNC \
   _IOW(KBASE_IOCTL_TYPE, 15, struct kbase_ioctl_mem_sync)
#define KBASE_IOCTL_MEM_COMMIT \
   _IOW(KBASE_IOCTL_TYPE, 20, struct kbase_ioctl_mem_commit)
#define KBASE_IOCTL_MEM_ALIAS \
   _IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)
#define KBASE_IOCTL_MEM_IMPORT \
   _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)
#define KBASE_IOCTL_MEM_IMPORT_MTK \
   _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import_mtk)
#define KBASE_IOCTL_STREAM_CREATE \
   _IOW(KBASE_IOCTL_TYPE, 24, struct kbase_ioctl_stream_create)

#define KBASE_IOCTL_CS_QUEUE_REGISTER \
   _IOW(KBASE_IOCTL_TYPE, 36, struct kbase_ioctl_cs_queue_register)
#define KBASE_IOCTL_CS_QUEUE_KICK \
   _IOW(KBASE_IOCTL_TYPE, 37, struct kbase_ioctl_cs_queue_kick)
#define KBASE_IOCTL_MEM_EXEC_INIT \
   _IOW(KBASE_IOCTL_TYPE, 38, struct kbase_ioctl_mem_exec_init)
#define KBASE_IOCTL_CS_QUEUE_BIND \
   _IOWR(KBASE_IOCTL_TYPE, 39, union kbase_ioctl_cs_queue_bind)
#define KBASE_IOCTL_CS_QUEUE_REGISTER_EX \
   _IOW(KBASE_IOCTL_TYPE, 40, struct kbase_ioctl_cs_queue_register_ex)
#define KBASE_IOCTL_CS_QUEUE_TERMINATE \
   _IOW(KBASE_IOCTL_TYPE, 41, struct kbase_ioctl_cs_queue_terminate)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 \
   _IOWR(KBASE_IOCTL_TYPE, 42, union kbase_ioctl_cs_queue_group_create_1_6)
#define KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE \
   _IOW(KBASE_IOCTL_TYPE, 43, struct kbase_ioctl_cs_queue_group_term)
#define KBASE_IOCTL_CS_EVENT_SIGNAL \
   _IO(KBASE_IOCTL_TYPE, 44)
#define KBASE_IOCTL_KCPU_QUEUE_CREATE \
   _IOR(KBASE_IOCTL_TYPE, 45, struct kbase_ioctl_kcpu_queue_new)
#define KBASE_IOCTL_KCPU_QUEUE_DELETE \
   _IOW(KBASE_IOCTL_TYPE, 46, struct kbase_ioctl_kcpu_queue_delete)
#define KBASE_IOCTL_KCPU_QUEUE_ENQUEUE \
   _IOW(KBASE_IOCTL_TYPE, 47, struct kbase_ioctl_kcpu_queue_enqueue)
#define KBASE_IOCTL_CS_TILER_HEAP_INIT \
   _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init)
#define KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13 \
   _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init_1_13)
#define KBASE_IOCTL_CS_TILER_HEAP_TERM \
   _IOW(KBASE_IOCTL_TYPE, 49, struct kbase_ioctl_cs_tiler_heap_term)
#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO \
   _IOWR(KBASE_IOCTL_TYPE, 50, union kbase_ioctl_get_cpu_gpu_timeinfo)
#define KBASE_IOCTL_CS_GET_GLB_IFACE \
   _IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)
#define KBASE_IOCTL_VERSION_CHECK \
   _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_CONTEXT_PRIORITY_CHECK \
   _IOWR(KBASE_IOCTL_TYPE, 54, struct kbase_ioctl_context_priority_check)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18 \
   _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create_1_18)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE \
   _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create_56)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_112 \
   _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create_112)
#define KBASE_IOCTL_MEM_ALLOC_EX \
   _IOWR(KBASE_IOCTL_TYPE, 59, union kbase_ioctl_mem_alloc_ex)

#ifdef __cplusplus
}
#endif

#endif /* MALI_KBASE_UAPI_H */
