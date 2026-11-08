/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor-to-Kbase Decoupled UAPI Interception & Translation Layer
 * Translates upstream Mesa 26.3.0 Panthor DRM UAPI + drm_syncobj + dma-buf
 * calls into /dev/mali0 ARM Mali Kbase CSF UAPI operations.
 */
#ifndef PANTHOR_KBASE_ADAPTER_H
#define PANTHOR_KBASE_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "drm.h"
#include "panthor_drm.h"
#include "mali_kbase_uapi.h"
#include "mali_csf_kbase_spec_autogen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CSF Opcode Constants (Valhall v10-v14 CS Frontend) */
#define CSF_OP_NOP              MALI_CSF_OP_NOP
#define CSF_OP_MOVE48           MALI_CSF_OP_MOVE48
#define CSF_OP_MOVE32           MALI_CSF_OP_MOVE32
#define CSF_OP_WAIT             MALI_CSF_OP_WAIT
#define CSF_OP_FLUSH_CACHE2     0x04ULL
#define CSF_OP_SET_SB_ENTRY     0x14ULL
#define CSF_OP_SET_STATE        0x15ULL
#define CSF_OP_SHARED_SB_INC    0x1bULL
#define CSF_OP_SHARED_SB_DEC    0x1cULL
#define CSF_OP_HEAP_OPERATION   0x1eULL
#define CSF_OP_HEAP_SET         0x1fULL
#define CSF_OP_CALL             MALI_CSF_OP_CALL
#define CSF_OP_JUMP             MALI_CSF_OP_JUMP
#define CSF_OP_REQ_RESOURCE     MALI_CSF_OP_REQ_RESOURCE
#define CSF_OP_SYNC_SET32       0x24ULL
#define CSF_OP_SYNC_ADD32       0x25ULL
#define CSF_OP_SYNC_WAIT32      0x26ULL
#define CSF_OP_RUN_COMPUTE      0x27ULL
#define CSF_OP_RUN_TILER        0x28ULL
#define CSF_OP_RUN_IDVS         0x29ULL
#define CSF_OP_RUN_FRAGMENT     0x2aULL
#define CSF_OP_RUN_FULLSCREEN   0x2bULL
#define CSF_OP_FINISH_TILING    0x2cULL
#define CSF_OP_FINISH_FRAGMENT  0x2dULL
#define CSF_OP_ERROR_BARRIER    MALI_CSF_OP_ERROR_BARRIER
#define CSF_OP_SYNC_SET64       0x32ULL
#define CSF_OP_SYNC_ADD64       0x33ULL
#define CSF_OP_SYNC_WAIT64      0x34ULL
#define CSF_OP_LOAD_MULTIPLE    0x38ULL
#define CSF_OP_STORE_MULTIPLE   0x39ULL

/* Hardware Valhall v10-v14 CS Opcodes from Mesa src/panfrost/genxml/v11.xml (via mali_csf_kbase_spec_autogen.h) */
#define HW_CSF_OP_NOP                   MALI_CSF_OP_NOP
#define HW_CSF_OP_MOVE48                MALI_CSF_OP_MOVE48
#define HW_CSF_OP_MOVE32                MALI_CSF_OP_MOVE32
#define HW_CSF_OP_WAIT                  MALI_CSF_OP_WAIT
#define HW_CSF_OP_RUN_COMPUTE           MALI_CSF_OP_RUN_COMPUTE
#define HW_CSF_OP_RUN_TILING            0x05ULL /* 5 (v10) */
#define HW_CSF_OP_RUN_IDVS              MALI_CSF_OP_RUN_IDVS
#define HW_CSF_OP_RUN_FRAGMENT          MALI_CSF_OP_RUN_FRAGMENT
#define HW_CSF_OP_RUN_FULLSCREEN        MALI_CSF_OP_RUN_FULLSCREEN
#define HW_CSF_OP_FINISH_TILING         MALI_CSF_OP_FINISH_TILING
#define HW_CSF_OP_FINISH_FRAGMENT       MALI_CSF_OP_FINISH_FRAGMENT
#define HW_CSF_OP_ADD_IMM32             MALI_CSF_OP_ADD_IMMEDIATE32
#define HW_CSF_OP_ADD_IMM64             MALI_CSF_OP_ADD_IMMEDIATE64
#define HW_CSF_OP_LOAD_MULTIPLE         MALI_CSF_OP_LOAD_MULTIPLE
#define HW_CSF_OP_STORE_MULTIPLE        MALI_CSF_OP_STORE_MULTIPLE
#define HW_CSF_OP_BRANCH                MALI_CSF_OP_BRANCH
#define HW_CSF_OP_SET_SB_ENTRY          MALI_CSF_OP_SET_SB_ENTRY
#define HW_CSF_OP_PROGRESS_WAIT         MALI_CSF_OP_PROGRESS_WAIT
#define HW_CSF_OP_SET_EXCEPTION_HANDLER MALI_CSF_OP_SET_EXCEPTION_HANDLER
#define HW_CSF_OP_NEXT_SB_ENTRY         MALI_CSF_OP_NEXT_SB_ENTRY
#define HW_CSF_OP_SET_STATE             MALI_CSF_OP_SET_STATE
#define HW_CSF_OP_SET_STATE_IMM32       MALI_CSF_OP_SET_STATE_IMM32
#define HW_CSF_OP_SHARED_SB_INC         MALI_CSF_OP_SHARED_SB_INC
#define HW_CSF_OP_SHARED_SB_DEC         MALI_CSF_OP_SHARED_SB_DEC
#define HW_CSF_OP_CALL                  MALI_CSF_OP_CALL
#define HW_CSF_OP_JUMP                  MALI_CSF_OP_JUMP
#define HW_CSF_OP_REQ_RESOURCE          MALI_CSF_OP_REQ_RESOURCE
#define HW_CSF_OP_FLUSH_CACHE2          MALI_CSF_OP_FLUSH_CACHE2
#define HW_CSF_OP_SYNC_ADD32            MALI_CSF_OP_SYNC_ADD32
#define HW_CSF_OP_SYNC_SET32            MALI_CSF_OP_SYNC_SET32
#define HW_CSF_OP_SYNC_WAIT32           MALI_CSF_OP_SYNC_WAIT32
#define HW_CSF_OP_STORE_STATE           MALI_CSF_OP_STORE_STATE
#define HW_CSF_OP_PROT_REGION           MALI_CSF_OP_PROT_REGION
#define HW_CSF_OP_PROGRESS_STORE        MALI_CSF_OP_PROGRESS_STORE
#define HW_CSF_OP_PROGRESS_LOAD         MALI_CSF_OP_PROGRESS_LOAD
#define HW_CSF_OP_RUN_COMPUTE_INDIRECT  MALI_CSF_OP_RUN_COMPUTE_INDIRECT
#define HW_CSF_OP_ERROR_BARRIER         MALI_CSF_OP_ERROR_BARRIER
#define HW_CSF_OP_HEAP_SET              MALI_CSF_OP_HEAP_SET
#define HW_CSF_OP_HEAP_OPERATION        MALI_CSF_OP_HEAP_OPERATION
#define HW_CSF_OP_TRACE_POINT           MALI_CSF_OP_TRACE_POINT
#define HW_CSF_OP_SYNC_ADD64            MALI_CSF_OP_SYNC_ADD64
#define HW_CSF_OP_SYNC_SET64            MALI_CSF_OP_SYNC_SET64
#define HW_CSF_OP_SYNC_WAIT64           MALI_CSF_OP_SYNC_WAIT64

/* Hardware Sync Scope Values in SYNC_ADD/SET bits [2:1] (v11.xml) */
#define HW_CSF_SYNC_SCOPE_SYSTEM        MALI_CS_SYNC_SCOPE_SYSTEM
#define HW_CSF_SYNC_SCOPE_CSG           MALI_CS_SYNC_SCOPE_CSG

/* Legacy/Synthetic Sync Scope Values in bits [29:28] */
#define CSF_SYNC_SCOPE_CSG              0x0ULL
#define CSF_SYNC_SCOPE_SYSTEM           0x2ULL

/* Resource mask bits for REQ_RESOURCE (opcode 0x22) */
#define CSF_RES_COMPUTE         (1U << 0)
#define CSF_RES_FRAGMENT        (1U << 1)
#define CSF_RES_TILER           (1U << 2)
#define CSF_RES_IDVS            (1U << 3)

/*
 * Per-subqueue 64-byte cell layout:
 *  - `event_bo` (BASE_MEM_CSF_EVENT) holds ONLY [0..15] (`seqno` + `error`), accessed
 *    exclusively by `SYNC_ADD64.system` (never by LOAD_MULTIPLE/STORE_MULTIPLE).
 *  - `diag_bo` (BASE_MEM_COHERENT_LOCAL) holds [16..55] (`ls_copy`, `pre_call`,
 *    `post_call`, `post_wait`, `stream_progress`), accessed by `STORE_MULTIPLE`.
 * Separating `event_bo` and `diag_bo` guarantees zero L2 shareability conflicts (0x88).
 */
#define ADAPTER_SEQNO_CELL_STRIDE            64U
#define ADAPTER_SEQNO_VALUE_OFFSET           0U   /* u64: SYNC_ADD64 seqno (in event_bo) */
#define ADAPTER_SEQNO_ERROR_OFFSET           8U   /* u64: SYNC_ADD64 error word (in event_bo) */
#define ADAPTER_SEQNO_LS_COPY_OFFSET         16U  /* u64: STORE_MULTIPLE copy of seqno (in diag_bo) */
#define ADAPTER_SEQNO_PRE_CALL_OFFSET        24U  /* u64: written right before CALL (in diag_bo) */
#define ADAPTER_SEQNO_POST_CALL_OFFSET       32U  /* u64: written right after CALL (in diag_bo) */
#define ADAPTER_SEQNO_POST_WAIT_OFFSET       40U  /* u64: written right after WAIT #all_sb_mask (in diag_bo) */
#define ADAPTER_SEQNO_STREAM_PROGRESS_OFFSET 48U  /* u64: written inside CS stream (in diag_bo) */

/* Offset in event_bo where per-group redirected PanVK syncobjs & ringbuf syncs are stored */
#define ADAPTER_EVENT_BO_SYNCOBJS_BASE_OFFSET 0x8000U
#define ADAPTER_EVENT_BO_GROUP_STRIDE         0x0200U /* 512B per group (8 x 64B cache lines) */

/* Ring buffer configuration */
#define ADAPTER_RINGBUF_SIZE          (64U * 1024U)
#define ADAPTER_RING_SLOT_QWORDS      24U
#define ADAPTER_RING_SLOT_BYTES       (ADAPTER_RING_SLOT_QWORDS * sizeof(uint64_t))
#define ADAPTER_RING_SLOT_MAX_QWORDS  64U
#define ADAPTER_RING_SLOT_MAX_BYTES   (ADAPTER_RING_SLOT_MAX_QWORDS * sizeof(uint64_t))
#define ADAPTER_FLIGHT_LOG_ENTRIES    16U
#define ADAPTER_MAX_SUBQUEUES         4U
#define ADAPTER_MAX_BOS               2048U
#define ADAPTER_MAX_SYNCOBJS          1024U
#define ADAPTER_MAX_GROUPS            32U
#define ADAPTER_MAX_HEAPS             32U
#define ADAPTER_MAX_VIRTUAL_FDS       64U

/* Public entrypoints usable by direct Mesa kmod patch and tests */
int panthor_kbase_adapter_open(const char *path, int flags);
int panthor_kbase_adapter_dup(int fd);
void panthor_kbase_adapter_set_dup_tracking(bool enable);
bool panthor_kbase_adapter_get_dup_tracking(void);
int panthor_kbase_adapter_close(int fd);
bool panthor_kbase_adapter_is_virtual_drm_fd(int fd);
int panthor_kbase_adapter_ioctl(int fd, unsigned long request, void *arg);
void *panthor_kbase_adapter_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int panthor_kbase_adapter_munmap(void *addr, size_t length);

/* Thread-local hint for pan_kmod_bo_alloc flags (EXECUTABLE / GPU_UNCACHED / CSF_EVENT) */
void panthor_kbase_adapter_hint_bo_flags(uint32_t kmod_flags);
uint64_t panthor_kbase_adapter_get_bo_gpu_va(uint32_t gem_handle);

/* CSF Bytecode Normalizer, Ring Emitter & Diagnostic Inspector */
struct adapter_cs_patch_stats {
   uint32_t total_instructions;
   uint32_t sync_scope_upgrades;
   uint32_t deferred_sync_waits;
   uint32_t heap_op_nops;
   uint32_t finish_frag_stat_clears;
   uint64_t extracted_subq_ctx_va;
   uint8_t  extracted_subq_ctx_reg;
};

void panthor_kbase_normalize_cs_stream(uint64_t *stream_cpu,
                                       uint32_t stream_size_bytes,
                                       bool is_initial_submit,
                                       struct adapter_cs_patch_stats *stats);

size_t panthor_kbase_emit_ring_slot(uint64_t *ring_slot,
                                    uint32_t arch_major,
                                    uint32_t cs_reg_count,
                                    uint32_t subqueue_index,
                                    uint64_t subq_ctx_va,
                                    uint64_t tiler_heap_ctx_va,
                                    uint64_t stream_addr,
                                    uint32_t stream_size,
                                    uint32_t latest_flush,
                                    uint64_t seqno_gpu_va,
                                    uint16_t all_sb_mask);

size_t panthor_kbase_emit_ring_slot_ex(uint64_t *ring_slot,
                                       uint32_t arch_major,
                                       uint32_t cs_reg_count,
                                       uint32_t subqueue_index,
                                       uint64_t subq_ctx_va,
                                       uint64_t tiler_heap_ctx_va,
                                       uint64_t stream_addr,
                                       uint32_t stream_size,
                                       uint32_t latest_flush,
                                       uint64_t seqno_gpu_va,
                                       uint16_t all_sb_mask,
                                       uint64_t job_seqno,
                                       bool emit_breadcrumbs);

const char *panthor_kbase_csf_opcode_name(uint8_t opcode);
void panthor_kbase_dump_csf_diagnostics(const char *reason);

/* Periodic Performance, Latency & Resource Telemetry Snapshot */
struct adapter_telemetry_snapshot {
   uint64_t total_group_submits;
   uint64_t total_subqueue_jobs[ADAPTER_MAX_SUBQUEUES];
   uint64_t completed_subqueue_seqno[ADAPTER_MAX_SUBQUEUES];
   uint64_t ring_wraps[ADAPTER_MAX_SUBQUEUES];
   uint64_t total_syncobj_waits;
   uint64_t fast_spin_syncobj_waits;
   uint64_t poll_sleep_syncobj_waits;
   uint32_t active_bos;
   uint32_t cached_cpu_bos;
   uint32_t imported_bos;
   uint64_t active_bo_bytes;
   uint32_t active_syncobjs;
   double   window_fps;
   double   avg_submit_us;
   double   max_submit_us;
   double   avg_cs_norm_us;
   double   avg_cache_clean_us;
   double   avg_kick_us;
   double   avg_sync_wait_us;
   double   max_sync_wait_us;
};

void panthor_kbase_get_telemetry_snapshot(struct adapter_telemetry_snapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_KBASE_ADAPTER_H */
