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

/* Sync Scope Values in SYNC_ADD/SET/WAIT bits [29:28] */
#define CSF_SYNC_SCOPE_CSG      0x0ULL
#define CSF_SYNC_SCOPE_SYSTEM   0x2ULL

/* Resource mask bits for REQ_RESOURCE (opcode 0x22) */
#define CSF_RES_COMPUTE         (1U << 0)
#define CSF_RES_FRAGMENT        (1U << 1)
#define CSF_RES_TILER           (1U << 2)
#define CSF_RES_IDVS            (1U << 3)

/* Ring buffer configuration */
#define ADAPTER_RINGBUF_SIZE      (64U * 1024U)
#define ADAPTER_RING_SLOT_QWORDS  24U
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

/* CSF Bytecode Normalizer & Ring Emitter (exposed for unit tests & OR-Tools verifier) */
struct adapter_cs_patch_stats {
   uint32_t total_instructions;
   uint32_t sync_scope_upgrades;
   uint32_t heap_op_nops;
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

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_KBASE_ADAPTER_H */
