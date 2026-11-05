/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT
 *
 * Self-contained ARM Mali Kbase CSF UAPI header for Valhall (v10-v14)
 * Synthesized from Pixel mali_kbase (r54p1/r54p3) and panvk-kbase-android.
 */
#ifndef MALI_KBASE_UAPI_H
#define MALI_KBASE_UAPI_H

#include <stdint.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBASE_IOCTL_TYPE 0x80

/* Memory allocation & region flags (mali_base_kernel.h & mali_base_csf_kernel.h) */
typedef uint64_t base_mem_alloc_flags;

#define BASE_MEM_PROT_CPU_RD        ((base_mem_alloc_flags)1 << 0)
#define BASE_MEM_PROT_CPU_WR        ((base_mem_alloc_flags)1 << 1)
#define BASE_MEM_PROT_GPU_RD        ((base_mem_alloc_flags)1 << 2)
#define BASE_MEM_PROT_GPU_WR        ((base_mem_alloc_flags)1 << 3)
#define BASE_MEM_PROT_GPU_EX        ((base_mem_alloc_flags)1 << 4)
#define BASE_MEM_GROW_ON_GPF        ((base_mem_alloc_flags)1 << 5)
#define BASE_MEM_COHERENT_SYSTEM    ((base_mem_alloc_flags)1 << 6)
#define BASE_MEM_COHERENT_LOCAL     ((base_mem_alloc_flags)1 << 7)
#define BASE_MEM_FIXED              ((base_mem_alloc_flags)1 << 8)
#define BASE_MEM_SAME_VA            ((base_mem_alloc_flags)1 << 9)
#define BASE_MEM_NEED_MMAP          ((base_mem_alloc_flags)1 << 10)
#define BASE_MEM_COHERENT_SYSTEM_REQUIRED ((base_mem_alloc_flags)1 << 11)
#define BASE_MEM_CACHED_CPU         ((base_mem_alloc_flags)1 << 12)
#define BASE_MEM_DONT_NEED          ((base_mem_alloc_flags)1 << 14)
#define BASE_MEM_IMPORT_SHARED      ((base_mem_alloc_flags)1 << 15)
#define BASE_MEM_CSF_EVENT          ((base_mem_alloc_flags)1 << 19)
#define BASE_MEM_UNCACHED_GPU       ((base_mem_alloc_flags)1 << 21)
#define BASE_MEM_FIXABLE            ((base_mem_alloc_flags)1 << 29)

/* Context creation flags */
#define BASE_CONTEXT_CREATE_FLAG_NONE                  0U
#define BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED    (1U << 1)

/* Special mmap page handles (multiplied by PAGE_SIZE) */
#define BASEP_MEM_INVALID_HANDLE               (0UL  << 12)
#define BASE_MEM_MAP_TRACKING_HANDLE           (3UL  << 12)
#define BASEP_MEM_WRITE_ALLOC_PAGES_HANDLE     (4UL  << 12)
#define BASEP_MEM_CSF_USER_REG_PAGE_HANDLE     (47UL << 12)
#define BASEP_MEM_CSF_USER_IO_PAGES_HANDLE     (48UL << 12)
#define BASE_MEM_COOKIE_BASE                   (64UL << 12)
#define BASE_MEM_FIRST_FREE_ADDRESS            (0x1000UL << 12)

/* CSF USER_IO 3-page layout (Doorbell page 0, Input page 1, Output page 2) */
#define BASEP_QUEUE_NR_MMAP_USER_PAGES         3U
#define CS_USER_IO_DOORBELL_OFFSET             0x0000U
#define CS_USER_IO_INPUT_PAGE_OFFSET           0x1000U
#define CS_USER_IO_OUTPUT_PAGE_OFFSET          0x2000U
#define CS_USER_IO_INPUT_CS_INSERT             0x00U
#define CS_USER_IO_INPUT_CS_EXTRACT_INIT       0x08U
#define CS_USER_IO_OUTPUT_CS_EXTRACT           0x00U
#define CS_USER_IO_OUTPUT_CS_ACTIVE            0x08U

/* GPU Property keys & size encodings */
#define KBASE_GPUPROP_VALUE_SIZE_U8            0x0U
#define KBASE_GPUPROP_VALUE_SIZE_U16           0x1U
#define KBASE_GPUPROP_VALUE_SIZE_U32           0x2U
#define KBASE_GPUPROP_VALUE_SIZE_U64           0x3U

#define KBASE_GPUPROP_PRODUCT_ID               1U
#define KBASE_GPUPROP_VERSION_STATUS           2U
#define KBASE_GPUPROP_MINOR_REVISION           3U
#define KBASE_GPUPROP_MAJOR_REVISION           4U
#define KBASE_GPUPROP_GPU_FREQ_KHZ_MAX         6U
#define KBASE_GPUPROP_LOG2_PROGRAM_COUNTER_SIZE 8U
#define KBASE_GPUPROP_TEXTURE_FEATURES_0       9U
#define KBASE_GPUPROP_TEXTURE_FEATURES_1       10U
#define KBASE_GPUPROP_TEXTURE_FEATURES_2       11U
#define KBASE_GPUPROP_GPU_AVAILABLE_MEMORY_SIZE 12U
#define KBASE_GPUPROP_L2_LOG2_LINE_SIZE        13U
#define KBASE_GPUPROP_L2_LOG2_CACHE_SIZE       14U
#define KBASE_GPUPROP_L2_NUM_L2_SLICES         15U
#define KBASE_GPUPROP_TILER_BIN_SIZE_BYTES     16U
#define KBASE_GPUPROP_TILER_MAX_ACTIVE_LEVELS  17U
#define KBASE_GPUPROP_MAX_THREADS              18U
#define KBASE_GPUPROP_MAX_WORKGROUP_SIZE       19U
#define KBASE_GPUPROP_MAX_BARRIER_SIZE         20U
#define KBASE_GPUPROP_MAX_REGISTERS            21U
#define KBASE_GPUPROP_RAW_SHADER_PRESENT       25U
#define KBASE_GPUPROP_RAW_TILER_PRESENT        26U
#define KBASE_GPUPROP_RAW_L2_PRESENT           27U
#define KBASE_GPUPROP_RAW_STACK_PRESENT        28U
#define KBASE_GPUPROP_RAW_L2_FEATURES          29U
#define KBASE_GPUPROP_RAW_CORE_FEATURES        30U
#define KBASE_GPUPROP_RAW_MEM_FEATURES         31U
#define KBASE_GPUPROP_RAW_MMU_FEATURES         32U
#define KBASE_GPUPROP_RAW_AS_PRESENT           33U
#define KBASE_GPUPROP_RAW_TILER_FEATURES       51U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0   52U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1   53U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2   54U
#define KBASE_GPUPROP_RAW_GPU_ID               55U
#define KBASE_GPUPROP_RAW_THREAD_MAX_THREADS   56U
#define KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE 57U
#define KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE 58U
#define KBASE_GPUPROP_RAW_THREAD_FEATURES      59U
#define KBASE_GPUPROP_RAW_COHERENCY_MODE       60U
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3   81U
#define KBASE_GPUPROP_NUM_EXEC_ENGINES         82U
#define KBASE_GPUPROP_RAW_THREAD_TLS_ALLOC     83U
#define KBASE_GPUPROP_TLS_ALLOC                84U
#define KBASE_GPUPROP_RAW_GPU_FEATURES         85U

/* Core & CSF IOCTL structs */
struct kbase_ioctl_version_check {
   uint16_t major;
   uint16_t minor;
};

struct kbase_ioctl_set_flags {
   uint32_t create_flags;
};

struct kbase_ioctl_get_gpuprops {
   uint64_t buffer;
   uint32_t size;
   uint32_t flags;
};

struct kbase_ioctl_get_ddk_version {
   uint64_t version_buffer;
   uint32_t size;
   uint32_t padding;
};

union kbase_ioctl_mem_alloc {
   struct {
      uint64_t va_pages;
      uint64_t commit_pages;
      uint64_t extension;
      uint64_t flags;
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
   } out;
};

union kbase_ioctl_mem_alloc_ex {
   struct {
      uint64_t va_pages;
      uint64_t commit_pages;
      uint64_t extension;
      uint64_t flags;
      uint64_t fixed_address;
      uint64_t extra[3];
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
   } out;
};

enum kbase_mem_query_type {
   KBASE_MEM_QUERY_COMMIT_SIZE = 1,
   KBASE_MEM_QUERY_VA_SIZE     = 2,
   KBASE_MEM_QUERY_FLAGS       = 3,
};

union kbase_ioctl_mem_query {
   struct {
      uint64_t gpu_addr;
      uint64_t query;
   } in;
   struct {
      uint64_t value;
   } out;
};

struct kbase_ioctl_mem_free {
   uint64_t gpu_addr;
};

struct kbase_ioctl_mem_jit_init {
   uint64_t va_pages;
   uint8_t max_allocations;
   uint8_t trim_level;
   uint8_t group_id;
   uint8_t padding[5];
   uint64_t phys_pages;
};

struct kbase_ioctl_mem_exec_init {
   uint64_t va_pages;
};

#define BASE_SYNCSET_OP_MSYNC (1U << 0)
#define BASE_SYNCSET_OP_CSYNC (1U << 1)

struct kbase_ioctl_mem_sync {
   uint64_t handle;
   uint64_t user_addr;
   uint64_t size;
   uint8_t type;
   uint8_t padding[7];
};

struct kbase_ioctl_get_context_id {
   int32_t id;
};

struct kbase_ioctl_mem_commit {
   uint64_t gpu_addr;
   uint64_t pages;
};

struct base_mem_aliasing_info {
   uint64_t handle;
   uint64_t offset;
   uint64_t length;
};

union kbase_ioctl_mem_alias {
   struct {
      uint64_t flags;
      uint64_t stride;
      uint64_t nents;
      uint64_t aliasing_info;
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
      uint64_t va_pages;
   } out;
};

enum base_mem_import_type {
   BASE_MEM_IMPORT_TYPE_INVALID     = 0,
   BASE_MEM_IMPORT_TYPE_UMP         = 1,
   BASE_MEM_IMPORT_TYPE_UMM         = 2,
   BASE_MEM_IMPORT_TYPE_USER_BUFFER = 3,
};

union kbase_ioctl_mem_import {
   struct {
      uint64_t flags;
      uint64_t phandle;
      uint32_t type;
      uint32_t padding;
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
      uint64_t va_pages;
   } out;
};

/* MediaTek SLC 40-byte MEM_IMPORT fallback layout (csf-v11/186) */
union kbase_ioctl_mem_import_mtk {
   struct {
      uint64_t flags;
      uint64_t phandle;
      uint32_t type;
      uint32_t padding;
      uint64_t vkAllocateMemoryCallID;
      uint64_t numOfPlanes : 8;
      uint64_t in_pbha_8bit : 8;
      uint64_t mtk_pad : 48;
   } in;
   struct {
      uint64_t flags;
      uint64_t gpu_va;
      uint64_t va_pages;
      uint64_t out_pbha_8bit;
   } out;
};

struct kbase_ioctl_mem_flags_change {
   uint64_t gpu_va;
   uint64_t flags;
   uint64_t mask;
};

struct kbase_ioctl_stream_create {
   char name[32];
   int32_t fd;
   uint32_t padding;
};

/* Note: in upstream mali_kbase_ioctl.h, stream_create is 32 bytes (name overwrites fd or union) */
union kbase_ioctl_stream_create_u {
   struct {
      char name[32];
   } in;
   struct {
      int32_t fd;
      uint32_t padding[7];
   } out;
};

struct kbase_ioctl_fence_validate {
   int32_t fd;
};

struct kbase_ioctl_cs_queue_register {
   uint64_t buffer_gpu_addr;
   uint32_t buffer_size;
   uint8_t priority;
   uint8_t padding[3];
};

struct kbase_ioctl_cs_queue_kick {
   uint64_t buffer_gpu_addr;
};

union kbase_ioctl_cs_queue_bind {
   struct {
      uint64_t buffer_gpu_addr;
      uint8_t group_handle;
      uint8_t csi_index;
      uint8_t padding[6];
   } in;
   struct {
      uint64_t mmap_handle;
   } out;
};

struct kbase_ioctl_cs_queue_register_ex {
   uint64_t buffer_gpu_addr;
   uint32_t buffer_size;
   uint8_t priority;
   uint8_t padding[3];
   uint64_t ex_offset_var_addr;
   uint64_t ex_buffer_base;
   uint32_t ex_buffer_size;
   uint8_t ex_event_size;
   uint8_t ex_event_state;
   uint8_t ex_padding[2];
};

struct kbase_ioctl_cs_queue_terminate {
   uint64_t buffer_gpu_addr;
};

union kbase_ioctl_cs_queue_group_create_1_6 {
   struct {
      uint64_t tiler_mask;
      uint64_t fragment_mask;
      uint64_t compute_mask;
      uint8_t cs_min;
      uint8_t priority;
      uint8_t tiler_max;
      uint8_t fragment_max;
      uint8_t compute_max;
      uint8_t padding[3];
   } in;
   struct {
      uint8_t group_handle;
      uint8_t padding[3];
      uint32_t group_uid;
   } out;
};

union kbase_ioctl_cs_queue_group_create_1_18 {
   struct {
      uint64_t tiler_mask;
      uint64_t fragment_mask;
      uint64_t compute_mask;
      uint8_t cs_min;
      uint8_t priority;
      uint8_t tiler_max;
      uint8_t fragment_max;
      uint8_t compute_max;
      uint8_t csi_handlers;
      uint8_t padding[2];
      uint64_t reserved;
   } in;
   struct {
      uint8_t group_handle;
      uint8_t padding[3];
      uint32_t group_uid;
   } out;
};

union kbase_ioctl_cs_queue_group_create {
   struct {
      uint64_t tiler_mask;
      uint64_t fragment_mask;
      uint64_t compute_mask;
      uint8_t cs_min;
      uint8_t priority;
      uint8_t tiler_max;
      uint8_t fragment_max;
      uint8_t compute_max;
      uint8_t csi_handlers;
      uint8_t neural_max;
      uint8_t cs_fault_report_enable;
      uint64_t neural_mask;
      uint64_t djoint_Scratch;
      uint64_t reserved[8];
   } in;
   struct {
      uint8_t group_handle;
      uint8_t padding[3];
      uint32_t group_uid;
   } out;
};

struct kbase_ioctl_cs_queue_group_term {
   uint8_t group_handle;
   uint8_t padding[7];
};

struct kbase_ioctl_kcpu_queue_new {
   uint8_t id;
   uint8_t padding[7];
};

struct kbase_ioctl_kcpu_queue_delete {
   uint8_t id;
   uint8_t padding[7];
};

struct kbase_ioctl_kcpu_queue_enqueue {
   uint64_t addr;
   uint32_t nr_commands;
   uint8_t id;
   uint8_t padding[3];
};

enum base_kcpu_command_type {
   BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL       = 0,
   BASE_KCPU_COMMAND_TYPE_FENCE_WAIT         = 1,
   BASE_KCPU_COMMAND_TYPE_CQS_WAIT           = 2,
   BASE_KCPU_COMMAND_TYPE_CQS_SET            = 3,
   BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION = 4,
   BASE_KCPU_COMMAND_TYPE_CQS_SET_OPERATION  = 5,
   BASE_KCPU_COMMAND_TYPE_MAP_IMPORT         = 6,
   BASE_KCPU_COMMAND_TYPE_UNMAP_IMPORT       = 7,
   BASE_KCPU_COMMAND_TYPE_UNMAP_IMPORT_FORCE = 8,
   BASE_KCPU_COMMAND_TYPE_JIT_ALLOC          = 9,
   BASE_KCPU_COMMAND_TYPE_JIT_FREE           = 10,
   BASE_KCPU_COMMAND_TYPE_GROUP_SUSPEND      = 11,
   BASE_KCPU_COMMAND_TYPE_ERROR_BARRIER      = 12,
};

#define BASEP_CQS_DATA_TYPE_U32 0U
#define BASEP_CQS_DATA_TYPE_U64 1U
#define BASEP_CQS_WAIT_OPERATION_LE 0U
#define BASEP_CQS_WAIT_OPERATION_GT 1U
#define BASEP_CQS_SET_OPERATION_ADD 0U
#define BASEP_CQS_SET_OPERATION_SET 1U

struct base_fence {
   struct {
      int32_t fd;
      int32_t stream_fd;
   } basep;
};

struct base_cqs_wait_operation_info {
   uint64_t addr;
   uint64_t val;
   uint8_t operation;
   uint8_t data_type;
   uint8_t padding[6];
};

struct base_cqs_set_operation_info {
   uint64_t addr;
   uint64_t val;
   uint8_t operation;
   uint8_t data_type;
   uint8_t padding[6];
};

struct base_kcpu_command {
   uint8_t type;
   uint8_t padding[7];
   union {
      struct {
         uint64_t fence;
      } fence;
      struct {
         uint64_t objs;
         uint32_t nr_objs;
         uint32_t padding;
      } cqs_wait_operation;
      struct {
         uint64_t objs;
         uint32_t nr_objs;
         uint32_t padding;
      } cqs_set_operation;
      uint64_t padding[2];
   } info;
};

union kbase_ioctl_cs_tiler_heap_init_1_13 {
   struct {
      uint32_t chunk_size;
      uint32_t initial_chunks;
      uint32_t max_chunks;
      uint16_t target_in_flight;
      uint8_t group_id;
      uint8_t padding;
   } in;
   struct {
      uint64_t gpu_heap_va;
      uint64_t first_chunk_va;
   } out;
};

union kbase_ioctl_cs_tiler_heap_init {
   struct {
      uint32_t chunk_size;
      uint32_t initial_chunks;
      uint32_t max_chunks;
      uint16_t target_in_flight;
      uint8_t group_id;
      uint8_t padding;
      uint64_t buf_desc_va;
   } in;
   struct {
      uint64_t gpu_heap_va;
      uint64_t first_chunk_va;
   } out;
};

struct kbase_ioctl_cs_tiler_heap_term {
   uint64_t gpu_heap_va;
};

union kbase_ioctl_cs_get_glb_iface {
   struct {
      uint32_t max_group_num;
      uint32_t max_total_stream_num;
      uint64_t groups_ptr;
      uint64_t streams_ptr;
   } in;
   struct {
      uint32_t glb_version;
      uint32_t features;
      uint32_t group_num;
      uint32_t max_total_stream_num;
      uint32_t total_stream_num;
      uint32_t instr_features;
   } out;
};

union kbase_ioctl_get_cpu_gpu_timeinfo {
   struct {
      uint32_t request_flags;
      uint32_t paddings[7];
   } in;
   struct {
      uint64_t sec;
      uint32_t nsec;
      uint32_t padding;
      uint64_t timestamp;
      uint64_t cycle_counter;
   } out;
};

struct kbase_ioctl_context_priority_check {
   uint8_t priority;
};

union kbase_ioctl_read_user_page {
   struct {
      uint32_t offset;
      uint32_t padding;
   } in;
   struct {
      uint64_t val_64;
   } out;
};

enum base_csf_notification_type {
   BASE_CSF_NOTIFICATION_EVENT = 0,
   BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR = 1,
   BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP = 2,
};

struct base_csf_notification {
   uint8_t type;
   uint8_t padding[7];
   struct {
      uint8_t handle;
      uint8_t padding[7];
      struct {
         uint32_t error_type;
         uint32_t padding;
         uint64_t exception_data;
      } error;
   } payload;
};

/* Exact KBASE_IOCTL_* definitions matching Pixel mali_kbase & r54p3 */
#define KBASE_IOCTL_VERSION_CHECK_RESERVED   _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS                _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_GET_GPUPROPS             _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)
#define KBASE_IOCTL_MEM_ALLOC                _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_QUERY                _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_IOCTL_MEM_FREE                 _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)
#define KBASE_IOCTL_GET_DDK_VERSION          _IOW(KBASE_IOCTL_TYPE, 13, struct kbase_ioctl_get_ddk_version)
#define KBASE_IOCTL_MEM_JIT_INIT             _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)
#define KBASE_IOCTL_MEM_SYNC                 _IOW(KBASE_IOCTL_TYPE, 15, struct kbase_ioctl_mem_sync)
#define KBASE_IOCTL_GET_CONTEXT_ID           _IOR(KBASE_IOCTL_TYPE, 17, struct kbase_ioctl_get_context_id)
#define KBASE_IOCTL_MEM_COMMIT               _IOW(KBASE_IOCTL_TYPE, 20, struct kbase_ioctl_mem_commit)
#define KBASE_IOCTL_MEM_ALIAS                _IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)
#define KBASE_IOCTL_MEM_IMPORT               _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)
#define KBASE_IOCTL_MEM_IMPORT_MTK           _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import_mtk)
#define KBASE_IOCTL_MEM_FLAGS_CHANGE         _IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_flags_change)
#define KBASE_IOCTL_STREAM_CREATE            _IOW(KBASE_IOCTL_TYPE, 24, union kbase_ioctl_stream_create_u)
#define KBASE_IOCTL_FENCE_VALIDATE           _IOW(KBASE_IOCTL_TYPE, 25, struct kbase_ioctl_fence_validate)
#define KBASE_IOCTL_CS_QUEUE_REGISTER        _IOW(KBASE_IOCTL_TYPE, 36, struct kbase_ioctl_cs_queue_register)
#define KBASE_IOCTL_CS_QUEUE_KICK            _IOW(KBASE_IOCTL_TYPE, 37, struct kbase_ioctl_cs_queue_kick)
#define KBASE_IOCTL_MEM_EXEC_INIT            _IOW(KBASE_IOCTL_TYPE, 38, struct kbase_ioctl_mem_exec_init)
#define KBASE_IOCTL_CS_QUEUE_BIND            _IOWR(KBASE_IOCTL_TYPE, 39, union kbase_ioctl_cs_queue_bind)
#define KBASE_IOCTL_CS_QUEUE_REGISTER_EX     _IOW(KBASE_IOCTL_TYPE, 40, struct kbase_ioctl_cs_queue_register_ex)
#define KBASE_IOCTL_CS_QUEUE_TERMINATE       _IOW(KBASE_IOCTL_TYPE, 41, struct kbase_ioctl_cs_queue_terminate)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 _IOWR(KBASE_IOCTL_TYPE, 42, union kbase_ioctl_cs_queue_group_create_1_6)
#define KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE _IOW(KBASE_IOCTL_TYPE, 43, struct kbase_ioctl_cs_queue_group_term)
#define KBASE_IOCTL_CS_EVENT_SIGNAL          _IO(KBASE_IOCTL_TYPE, 44)
#define KBASE_IOCTL_KCPU_QUEUE_CREATE        _IOR(KBASE_IOCTL_TYPE, 45, struct kbase_ioctl_kcpu_queue_new)
#define KBASE_IOCTL_KCPU_QUEUE_DELETE        _IOW(KBASE_IOCTL_TYPE, 46, struct kbase_ioctl_kcpu_queue_delete)
#define KBASE_IOCTL_KCPU_QUEUE_ENQUEUE       _IOW(KBASE_IOCTL_TYPE, 47, struct kbase_ioctl_kcpu_queue_enqueue)
#define KBASE_IOCTL_CS_TILER_HEAP_INIT       _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init)
#define KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13  _IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init_1_13)
#define KBASE_IOCTL_CS_TILER_HEAP_TERM       _IOW(KBASE_IOCTL_TYPE, 49, struct kbase_ioctl_cs_tiler_heap_term)
#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO     _IOWR(KBASE_IOCTL_TYPE, 50, union kbase_ioctl_get_cpu_gpu_timeinfo)
#define KBASE_IOCTL_CS_GET_GLB_IFACE         _IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)
#define KBASE_IOCTL_VERSION_CHECK            _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_CONTEXT_PRIORITY_CHECK   _IOWR(KBASE_IOCTL_TYPE, 54, struct kbase_ioctl_context_priority_check)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18 _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create_1_18)
#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE    _IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create)
#define KBASE_IOCTL_MEM_ALLOC_EX             _IOWR(KBASE_IOCTL_TYPE, 59, union kbase_ioctl_mem_alloc_ex)
#define KBASE_IOCTL_READ_USER_PAGE           _IOWR(KBASE_IOCTL_TYPE, 60, union kbase_ioctl_read_user_page)

#ifdef __cplusplus
}
#endif

#endif /* MALI_KBASE_UAPI_H */
