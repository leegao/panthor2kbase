/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor DRM / CSF / Valhall ISA Capture & Local/On-Device Replay Engine
 *
 * Captures a self-contained binary trace (.pandrmcap) of:
 *   - All DRM_IOCTL_PANTHOR_* and DRM_IOCTL_SYNCOBJ_* / GEM / PRIME operations
 *   - Pre-submit BO memory snapshots (command streams, UBOs, VBOs, descriptor tables)
 *   - Transitive closure of CSF command stream chunks across CALL (0x20) & JUMP (0x21)
 *   - Valhall shader ISA binaries in executable BOs (BASE_MEM_PROT_GPU_EX)
 *   - Texture descriptor metadata (with optional stubbed payload mode for compact traces)
 *
 * Replays captured .pandrmcap traces deterministically against libpanthor_kbase_adapter
 * (backed by libmali_kbase_emu.so on x86-64 host or /dev/mali0 on Android hardware)
 * with automatic handle & SAME_VA GPU address relocation and CSF invariant auditing.
 */
#ifndef PANTHOR_DRM_CAPTURE_REPLAY_H
#define PANTHOR_DRM_CAPTURE_REPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "panthor_kbase_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANDRM_TRACE_MAGIC         "PANDRMC1"
#define PANDRM_TRACE_VERSION       1U

/* Header flags */
#define PANDRM_FLAG_STUB_TEXTURES  (1U << 0)
#define PANDRM_FLAG_CAPTURE_ISA    (1U << 1)
#define PANDRM_FLAG_POST_NORM_CSF  (1U << 2)

/* Record types in .pandrmcap binary stream */
enum pandrm_record_type {
   PANDRM_REC_NOP                 = 0,
   PANDRM_REC_DEV_QUERY           = 1,
   PANDRM_REC_VM_CREATE           = 2,
   PANDRM_REC_VM_DESTROY          = 3,
   PANDRM_REC_BO_CREATE           = 4,
   PANDRM_REC_VM_BIND             = 5,
   PANDRM_REC_TILER_HEAP_CREATE   = 6,
   PANDRM_REC_TILER_HEAP_DESTROY  = 7,
   PANDRM_REC_GROUP_CREATE        = 8,
   PANDRM_REC_GROUP_DESTROY       = 9,
   PANDRM_REC_SYNCOBJ_CREATE      = 10,
   PANDRM_REC_SYNCOBJ_DESTROY     = 11,
   PANDRM_REC_SYNCOBJ_OP          = 12,
   PANDRM_REC_BO_DATA_SNAPSHOT    = 13,
   PANDRM_REC_CSF_STREAM_CLOSURE  = 14,
   PANDRM_REC_ISA_SHADER_BLOB     = 15,
   PANDRM_REC_TEX_META            = 16,
   PANDRM_REC_GROUP_SUBMIT        = 17,
   PANDRM_REC_GEM_CLOSE           = 18,
};

/* 64-byte file header at offset 0 of every .pandrmcap file */
struct pandrm_trace_file_header {
   char     magic[8];              /* "PANDRMC1" */
   uint32_t version;               /* PANDRM_TRACE_VERSION (1) */
   uint32_t flags;                 /* PANDRM_FLAG_* bitmask */
   uint32_t gpu_id;                /* e.g. 0x72120000 / 0xa8670000 */
   uint32_t arch_major;            /* e.g. 10, 11, 12 */
   uint32_t cs_reg_count;          /* 96 */
   uint32_t scoreboard_slot_count; /* 8 */
   uint64_t capture_timestamp_ns;  /* CLOCK_MONOTONIC timestamp at capture start */
   uint64_t reserved[3];
};

/* 32-byte record header preceding every record payload */
struct pandrm_record_header {
   uint32_t record_type;           /* enum pandrm_record_type */
   uint32_t payload_bytes;         /* byte length immediately following this header */
   uint64_t seq_id;                /* monotonically increasing record index */
   uint64_t fnv1a64_checksum;      /* 64-bit FNV-1a checksum over payload */
   uint32_t fd_tag;                /* virtual DRM FD during capture */
   int32_t  ioctl_ret;             /* return value of the captured operation */
};

/* Payload structures for each record type */
struct pandrm_rec_dev_query {
   uint32_t query_type;
   uint32_t query_size;
   /* Followed by query_size bytes of query output struct */
};

struct pandrm_rec_vm_create {
   uint32_t vm_id;
   uint32_t flags;
   uint64_t user_va_range;
};

struct pandrm_rec_vm_destroy {
   uint32_t vm_id;
   uint32_t pad;
};

struct pandrm_rec_bo_create {
   uint32_t gem_handle;
   uint32_t panthor_flags;
   uint32_t kmod_flags;
   uint32_t is_executable;
   uint64_t size;
   uint64_t kbase_flags;
   uint64_t gpu_va;
};

struct pandrm_rec_vm_bind_header {
   uint32_t vm_id;
   uint32_t flags;
   uint32_t op_count;
   uint32_t pad;
   /* Followed by op_count * struct drm_panthor_vm_bind_op */
};

struct pandrm_rec_tiler_heap_create {
   uint32_t vm_id;
   uint32_t initial_chunk_count;
   uint32_t chunk_size;
   uint32_t max_chunks;
   uint32_t target_in_flight;
   uint32_t handle;
   uint64_t tiler_heap_ctx_gpu_va;
   uint64_t first_heap_chunk_gpu_va;
};

struct pandrm_rec_tiler_heap_destroy {
   uint32_t handle;
   uint32_t pad;
};

struct pandrm_rec_group_create {
   uint32_t vm_id;
   uint32_t queues_count;
   uint8_t  max_compute_cores;
   uint8_t  max_fragment_cores;
   uint8_t  max_tiler_cores;
   uint8_t  priority;
   uint32_t group_handle;
   uint64_t compute_core_mask;
   uint64_t fragment_core_mask;
   uint64_t tiler_core_mask;
   struct drm_panthor_queue_create queues[ADAPTER_MAX_SUBQUEUES];
};

struct pandrm_rec_group_destroy {
   uint32_t group_handle;
   uint32_t pad;
};

struct pandrm_rec_syncobj_create {
   uint32_t handle;
   uint32_t flags;
};

struct pandrm_rec_syncobj_destroy {
   uint32_t handle;
   uint32_t pad;
};

enum pandrm_syncobj_op_kind {
   PANDRM_SYNCOBJ_WAIT            = 1,
   PANDRM_SYNCOBJ_TIMELINE_WAIT   = 2,
   PANDRM_SYNCOBJ_RESET           = 3,
   PANDRM_SYNCOBJ_SIGNAL          = 4,
   PANDRM_SYNCOBJ_TIMELINE_SIGNAL = 5,
   PANDRM_SYNCOBJ_QUERY           = 6,
   PANDRM_SYNCOBJ_TRANSFER        = 7,
};

struct pandrm_rec_syncobj_op {
   uint32_t op_kind;       /* enum pandrm_syncobj_op_kind */
   uint32_t count_handles;
   uint32_t flags;
   uint32_t dst_handle;    /* used for TRANSFER */
   uint64_t dst_point;     /* used for TRANSFER */
   uint32_t handles[16];
   uint64_t points[16];
};

struct pandrm_rec_bo_data_snapshot {
   uint32_t gem_handle;
   uint32_t is_executable;
   uint64_t gpu_va;
   uint64_t offset;
   uint64_t byte_size;
   uint64_t content_fnv1a64;
   /* Followed by byte_size raw bytes */
};

struct pandrm_rec_csf_stream_closure {
   uint32_t group_handle;
   uint32_t queue_index;
   uint32_t chunk_index;
   uint16_t call_depth;
   uint16_t is_jump_target;
   uint64_t gpu_va;
   uint32_t size_bytes;
   uint32_t pad;
   uint64_t pre_norm_fnv1a64;
   uint64_t post_norm_fnv1a64;
   /* Followed by size_bytes raw CSF instruction bytes (uint64_t qwords) */
};

struct pandrm_rec_isa_shader_blob {
   uint32_t gem_handle;
   uint32_t shader_size;
   uint64_t shader_gpu_va;
   uint64_t isa_fnv1a64;
   /* Followed by shader_size raw Valhall ISA bytes */
};

struct pandrm_rec_tex_meta {
   uint32_t gem_handle;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t format;
   uint32_t stride;
   uint32_t is_stubbed;
   uint32_t payload_bytes;
   uint64_t tex_gpu_va;
   uint64_t tex_fnv1a64;
   /* Followed by payload_bytes (0 when is_stubbed == 1) */
};

struct pandrm_rec_queue_submit_entry {
   uint32_t queue_index;
   uint32_t stream_size;
   uint64_t stream_addr;
   uint32_t latest_flush;
   uint32_t sync_count;
   struct drm_panthor_sync_op syncs[16];
};

struct pandrm_rec_group_submit {
   uint32_t group_handle;
   uint32_t queue_submit_count;
   /* Followed by queue_submit_count * struct pandrm_rec_queue_submit_entry */
};

struct pandrm_rec_gem_close {
   uint32_t gem_handle;
   uint32_t pad;
};

/* Replay options and verification summary */
struct pandrm_replay_options {
   bool verbose;
   bool dump_csf;
   bool verify_invariants;
   bool bug_catching_mode;
};

struct pandrm_replay_summary {
   uint64_t total_records_replayed;
   uint64_t total_bo_creates;
   uint64_t total_bo_snapshots_restored;
   uint64_t total_va_relocations_applied;
   uint64_t total_csf_chunks_verified;
   uint64_t total_isa_blobs_restored;
   uint64_t total_tex_metas_restored;
   uint64_t total_group_submits;
   uint64_t total_syncobj_waits;
   uint32_t csf_invariant_violations;
   int      final_status;
};

/* Adapter Capture Lifecycle & Hook API */
bool panthor_drm_capture_is_active(void);
int  panthor_drm_capture_start(const char *filepath, uint32_t flags);
void panthor_drm_capture_stop(void);
void panthor_drm_capture_init_from_env(uint32_t gpu_id, uint32_t arch_major,
                                       uint32_t cs_reg_count, uint32_t sb_slot_count);

/* Called by panthor_kbase_adapter_ioctl after each Panthor/DRM ioctl */
void panthor_drm_capture_record_ioctl(int fd, unsigned long request, void *arg, int ret);

/* Called by DRM_PANTHOR_GROUP_SUBMIT to snapshot BOs, CSF transitive closure, ISA & texture metadata */
void panthor_drm_capture_record_pre_submit(uint32_t group_handle,
                                           const struct drm_panthor_queue_submit *qsubs,
                                           uint32_t qsub_count);

void panthor_drm_capture_record_csf_chunk(uint32_t group_handle,
                                          uint32_t queue_index,
                                          uint32_t chunk_index,
                                          uint16_t call_depth,
                                          uint16_t is_jump_target,
                                          uint64_t gpu_va,
                                          uint32_t size_bytes,
                                          uint64_t pre_norm_hash,
                                          uint64_t post_norm_hash,
                                          const uint64_t *qwords);

/* Local & On-Device Trace Replay API */
int panthor_drm_replay_trace_file(const char *filepath,
                                  const struct pandrm_replay_options *opts,
                                  struct pandrm_replay_summary *out_summary);

/*
 * 64-bit FNV-1a hash helper shared with CSF worklist chunk hash cache:
 * Uses 64-bit word-aligned register loads (`ldr xN`) when `data` is 8-byte aligned
 * so Write-Combine (`MT_NORMAL_NC`) command stream buffers are read once per 8-byte
 * CSF instruction instead of issuing 8 uncached byte-by-byte (`ldrb`) bus transactions,
 * while computing the exact bit-identical FNV-1a 64-bit digest.
 */
static inline uint64_t panthor_fnv1a64_qword_step(uint64_t h, uint64_t w)
{
   h ^= (w & 0xffULL);         h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 8) & 0xffULL);  h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 16) & 0xffULL); h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 24) & 0xffULL); h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 32) & 0xffULL); h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 40) & 0xffULL); h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 48) & 0xffULL); h *= UINT64_C(0x100000001b3);
   h ^= ((w >> 56) & 0xffULL); h *= UINT64_C(0x100000001b3);
   return h;
}

static inline uint64_t panthor_fnv1a64_bytes(const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   uint64_t h = UINT64_C(0xcbf29ce484222325);
   size_t i = 0;
   if (len >= 8U && (((uintptr_t)p & 7U) == 0U)) {
      const uint64_t *qw = (const uint64_t *)p;
      size_t qcount = len >> 3;
      for (size_t q = 0; q < qcount; q++)
         h = panthor_fnv1a64_qword_step(h, qw[q]);
      i = qcount << 3;
   }
   for (; i < len; i++) {
      h ^= (uint64_t)p[i];
      h *= UINT64_C(0x100000001b3);
   }
   return h;
}

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_DRM_CAPTURE_REPLAY_H */
