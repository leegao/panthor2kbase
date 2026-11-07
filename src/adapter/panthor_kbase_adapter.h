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

#ifdef __cplusplus
extern "C" {
#endif

/* CSF Opcode Constants (Valhall v10-v14 CS Frontend) */
#define CSF_OP_NOP              0x00ULL
#define CSF_OP_MOVE48           0x01ULL
#define CSF_OP_MOVE32           0x02ULL
#define CSF_OP_WAIT             0x03ULL
#define CSF_OP_FLUSH_CACHE2     0x04ULL
#define CSF_OP_SET_SB_ENTRY     0x14ULL
#define CSF_OP_SET_STATE        0x15ULL
#define CSF_OP_SHARED_SB_INC    0x1bULL
#define CSF_OP_SHARED_SB_DEC    0x1cULL
#define CSF_OP_HEAP_OPERATION   0x1eULL
#define CSF_OP_HEAP_SET         0x1fULL
#define CSF_OP_CALL             0x20ULL
#define CSF_OP_JUMP             0x21ULL
#define CSF_OP_REQ_RESOURCE     0x22ULL
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
#define CSF_OP_ERROR_BARRIER    0x2fULL
#define CSF_OP_SYNC_SET64       0x32ULL
#define CSF_OP_SYNC_ADD64       0x33ULL
#define CSF_OP_SYNC_WAIT64      0x34ULL
#define CSF_OP_LOAD_MULTIPLE    0x38ULL
#define CSF_OP_STORE_MULTIPLE   0x39ULL

/* Hardware Valhall v10-v14 CS Opcodes from Mesa src/panfrost/genxml/v11.xml */
#define HW_CSF_OP_NOP                   0x00ULL /*  0 */
#define HW_CSF_OP_MOVE48                0x01ULL /*  1 */
#define HW_CSF_OP_MOVE32                0x02ULL /*  2 */
#define HW_CSF_OP_WAIT                  0x03ULL /*  3 */
#define HW_CSF_OP_RUN_COMPUTE           0x04ULL /*  4 */
#define HW_CSF_OP_RUN_TILING            0x05ULL /*  5 (v10) */
#define HW_CSF_OP_RUN_IDVS              0x06ULL /*  6 */
#define HW_CSF_OP_RUN_FRAGMENT          0x07ULL /*  7 */
#define HW_CSF_OP_RUN_FULLSCREEN        0x08ULL /*  8 */
#define HW_CSF_OP_FINISH_TILING         0x09ULL /*  9 */
#define HW_CSF_OP_FINISH_FRAGMENT       0x0bULL /* 11 */
#define HW_CSF_OP_ADD_IMM32             0x10ULL /* 16 */
#define HW_CSF_OP_ADD_IMM64             0x11ULL /* 17 */
#define HW_CSF_OP_LOAD_MULTIPLE         0x14ULL /* 20 */
#define HW_CSF_OP_STORE_MULTIPLE        0x15ULL /* 21 */
#define HW_CSF_OP_BRANCH                0x16ULL /* 22 */
#define HW_CSF_OP_SET_SB_ENTRY          0x17ULL /* 23 */
#define HW_CSF_OP_PROGRESS_WAIT         0x18ULL /* 24 */
#define HW_CSF_OP_SET_EXCEPTION_HANDLER 0x19ULL /* 25 */
#define HW_CSF_OP_NEXT_SB_ENTRY         0x1aULL /* 26 */
#define HW_CSF_OP_SET_STATE             0x1bULL /* 27 */
#define HW_CSF_OP_SET_STATE_IMM32       0x1cULL /* 28 */
#define HW_CSF_OP_SHARED_SB_INC         0x1eULL /* 30 */
#define HW_CSF_OP_SHARED_SB_DEC         0x1fULL /* 31 */
#define HW_CSF_OP_CALL                  0x20ULL /* 32 */
#define HW_CSF_OP_JUMP                  0x21ULL /* 33 */
#define HW_CSF_OP_REQ_RESOURCE          0x22ULL /* 34 */
#define HW_CSF_OP_FLUSH_CACHE2          0x24ULL /* 36 */
#define HW_CSF_OP_SYNC_ADD32            0x25ULL /* 37 */
#define HW_CSF_OP_SYNC_SET32            0x26ULL /* 38 */
#define HW_CSF_OP_SYNC_WAIT32           0x27ULL /* 39 */
#define HW_CSF_OP_STORE_STATE           0x28ULL /* 40 */
#define HW_CSF_OP_PROT_REGION           0x29ULL /* 41 */
#define HW_CSF_OP_PROGRESS_STORE        0x2aULL /* 42 */
#define HW_CSF_OP_PROGRESS_LOAD         0x2bULL /* 43 */
#define HW_CSF_OP_RUN_COMPUTE_INDIRECT  0x2cULL /* 44 */
#define HW_CSF_OP_ERROR_BARRIER         0x2fULL /* 47 */
#define HW_CSF_OP_HEAP_SET              0x30ULL /* 48 */
#define HW_CSF_OP_HEAP_OPERATION        0x31ULL /* 49 */
#define HW_CSF_OP_TRACE_POINT           0x32ULL /* 50 */
#define HW_CSF_OP_SYNC_ADD64            0x33ULL /* 51 */
#define HW_CSF_OP_SYNC_SET64            0x34ULL /* 52 */
#define HW_CSF_OP_SYNC_WAIT64           0x35ULL /* 53 */

/* Hardware Sync Scope Values in SYNC_ADD/SET bits [2:1] (v11.xml) */
#define HW_CSF_SYNC_SCOPE_SYSTEM        0x0ULL
#define HW_CSF_SYNC_SCOPE_CSG           0x2ULL

/* Legacy/Synthetic Sync Scope Values in bits [29:28] */
#define CSF_SYNC_SCOPE_CSG              0x0ULL
#define CSF_SYNC_SCOPE_SYSTEM           0x2ULL

/* Resource mask bits for REQ_RESOURCE (opcode 0x22) */
#define CSF_RES_COMPUTE         (1U << 0)
#define CSF_RES_FRAGMENT        (1U << 1)
#define CSF_RES_TILER           (1U << 2)
#define CSF_RES_IDVS            (1U << 3)

/* Per-subqueue 64-byte event_bo cell layout (matches panvk-kbase-android Patch 007) */
#define ADAPTER_SEQNO_CELL_STRIDE            64U
#define ADAPTER_SEQNO_VALUE_OFFSET           0U   /* u64: SYNC_ADD64 seqno */
#define ADAPTER_SEQNO_ERROR_OFFSET           8U   /* u64: SYNC_ADD64 error word */
#define ADAPTER_SEQNO_LS_COPY_OFFSET         16U  /* u64: STORE_MULTIPLE copy of seqno */
#define ADAPTER_SEQNO_PRE_CALL_OFFSET        24U  /* u64: written right before CALL */
#define ADAPTER_SEQNO_POST_CALL_OFFSET       32U  /* u64: written right after CALL */
#define ADAPTER_SEQNO_POST_WAIT_OFFSET       40U  /* u64: written right after WAIT #all_sb_mask */
#define ADAPTER_SEQNO_STREAM_PROGRESS_OFFSET 48U  /* u64: written inside CS stream */

/* Ring buffer configuration */
#define ADAPTER_RINGBUF_SIZE      (64U * 1024U)
#define ADAPTER_RING_SLOT_QWORDS  32U
#define ADAPTER_RING_SLOT_BYTES   (ADAPTER_RING_SLOT_QWORDS * sizeof(uint64_t))
#define ADAPTER_MAX_SUBQUEUES     4U
#define ADAPTER_MAX_BOS           1024U
#define ADAPTER_MAX_SYNCOBJS      512U
#define ADAPTER_MAX_GROUPS        32U
#define ADAPTER_MAX_HEAPS         32U
#define ADAPTER_MAX_VIRTUAL_FDS   64U

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

/* Thread-local hint for pan_kmod_bo_alloc flags (EXECUTABLE / GPU_UNCACHED) */
void panthor_kbase_adapter_hint_bo_flags(uint32_t kmod_flags);
uint64_t panthor_kbase_adapter_get_bo_gpu_va(uint32_t gem_handle);

/* CSF Bytecode Normalizer, Ring Emitter & Diagnostic Inspector */
struct adapter_cs_patch_stats {
   uint32_t total_instructions;
   uint32_t sync_scope_upgrades;
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

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_KBASE_ADAPTER_H */
