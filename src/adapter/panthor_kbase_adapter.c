/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor-to-Kbase Decoupled UAPI Interception & Translation Layer
 * (`libpanthor_kbase_adapter.so`)
 *
 * Translates upstream Mesa 26.3.0 Panthor DRM UAPI (`DRM_IOCTL_PANTHOR_*`),
 * `drm_syncobj` (`DRM_IOCTL_SYNCOBJ_*`), GEM/PRIME (`DRM_IOCTL_GEM_CLOSE`,
 * `DRM_IOCTL_PRIME_*`), and `libdrm` calls into `/dev/mali0` ARM Mali Kbase
 * CSF UAPI operations (`KBASE_IOCTL_*`, `USER_IO` mmaps, KCPU/CQS queues).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "panthor_kbase_adapter.h"

/* Real libc / syscall pointers */
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*real_munmap)(void *, size_t) = NULL;

static void resolve_real_funcs(void)
{
   bool has_emu = (dlsym(RTLD_DEFAULT, "mali_kbase_emu_get_stats") != NULL);
   void *handle = has_emu ? RTLD_DEFAULT : RTLD_NEXT;
   if (!real_open) {
      real_open = dlsym(handle, "open");
      if (!real_open) real_open = dlsym(RTLD_DEFAULT, "open");
   }
   if (!real_close) {
      real_close = dlsym(handle, "close");
      if (!real_close) real_close = dlsym(RTLD_DEFAULT, "close");
   }
   if (!real_ioctl) {
      real_ioctl = dlsym(handle, "ioctl");
      if (!real_ioctl) real_ioctl = dlsym(RTLD_DEFAULT, "ioctl");
   }
   if (!real_mmap) {
      real_mmap = dlsym(handle, "mmap");
      if (!real_mmap) real_mmap = dlsym(RTLD_DEFAULT, "mmap");
   }
   if (!real_munmap) {
      real_munmap = dlsym(handle, "munmap");
      if (!real_munmap) real_munmap = dlsym(RTLD_DEFAULT, "munmap");
   }
}

/* Optional UAPI trace logging for formal verification & debugging */
static FILE *g_trace_fp = NULL;
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void trace_log(const char *fmt, ...)
{
   const char *path = getenv("PANVK_ADAPTER_TRACE_PATH");
   if (!path || !path[0])
      return;
   if (!g_trace_fp)
      g_trace_fp = fopen(path, "a");
   if (!g_trace_fp)
      return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(g_trace_fp, fmt, ap);
   va_end(ap);
   fflush(g_trace_fp);
}

/* Internal state for a translated Panthor->Kbase BO */
struct adapter_bo {
   bool in_use;
   uint32_t gem_handle;
   uint64_t size;
   uint32_t panthor_flags;
   uint64_t kbase_flags;
   uint64_t kbase_cookie_or_va;
   uint64_t gpu_va;
   void *cpu_ptr;
   size_t cpu_size;
   int dmabuf_fd;
   uint64_t dmabuf_ino;
   bool is_imported;
   bool is_same_va;
   bool is_csf_event;
   char label[64];
};

/* Internal state for an emulated DRM Syncobj (backed by CQS 64-bit timeline in CSF_EVENT mem) */
struct adapter_syncobj {
   bool in_use;
   uint32_t handle;
   uint64_t value;
   uint64_t cqs_gpu_va;
   uint64_t *cqs_cpu_ptr;
};

/* Subqueue state inside a CSG */
struct adapter_subqueue {
   bool bound;
   uint32_t csi_index;
   uint32_t ringbuf_size;
   uint64_t ring_gpu_va;
   uint64_t *ring_cpu_ptr;
   void *user_io_ptr; /* 3 * 4096 bytes: page 0 Doorbell, page 1 Input, page 2 Output */
   uint64_t insert_offset;
   uint64_t emitted_jobs;
   uint64_t seqno_gpu_va;
   volatile uint64_t *seqno_cpu_ptr;
   uint64_t cached_subq_ctx_va;
   uint8_t cached_subq_ctx_reg;
};

/* Scheduling group state */
struct adapter_group {
   bool in_use;
   uint32_t group_handle;
   uint8_t kbase_group_handle;
   uint32_t vm_id;
   uint32_t queue_count;
   struct adapter_subqueue subqueues[ADAPTER_MAX_SUBQUEUES];
   uint64_t active_tiler_heap_ctx_va;
};

/* Tiler heap state */
struct adapter_heap {
   bool in_use;
   uint32_t handle;
   uint64_t gpu_heap_va;
   uint64_t first_chunk_va;
   uint32_t chunk_size;
};

/* Per-context adapter state (maps a virtual Panthor DRM fd to a /dev/mali0 fd) */
struct adapter_ctx {
   bool active;
   int drm_fd;
   int mali_fd;
   uint16_t kbase_ver_major;
   uint16_t kbase_ver_minor;
   uint32_t arch_major;
   struct drm_panthor_gpu_info gpu_info;
   struct drm_panthor_csif_info csif_info;
   struct drm_panthor_mmu_info mmu_info;
   uint64_t user_mmio_offset;
   void *user_reg_page;
   uint8_t kcpu_queue_id;
   bool has_kcpu_queue;
   int stream_fd;

   /* CSF Event BO backing syncobjs & subqueue seqnos */
   uint64_t event_bo_gpu_va;
   uint8_t *event_bo_cpu;
   size_t event_bo_size;

   uint32_t next_vm_id;
   uint32_t next_gem_handle;
   uint32_t next_syncobj_handle;
   uint32_t next_group_handle;
   uint32_t next_heap_handle;

   struct adapter_bo bos[ADAPTER_MAX_BOS];
   struct adapter_syncobj syncobjs[ADAPTER_MAX_SYNCOBJS];
   struct adapter_group groups[ADAPTER_MAX_GROUPS];
   struct adapter_heap heaps[ADAPTER_MAX_HEAPS];
};

static struct adapter_ctx g_ctx;

/* Parse TLV stream from KBASE_IOCTL_GET_GPUPROPS into drm_panthor_gpu_info */
static void parse_kbase_gpuprops(struct adapter_ctx *ctx, const uint8_t *buf, size_t len)
{
   size_t off = 0;
   uint64_t raw_gpu_id = 0;
   /* Sensible defaults for Valhall CSF (Mali-G615 / G710) if a prop is omitted */
   ctx->gpu_info.gpu_id = (10U << 28) | (8U << 24) | (0U << 16);
   ctx->gpu_info.csf_id = (1U << 26) | (1U << 20);
   ctx->gpu_info.mmu_features = 0x2830; /* 40-bit VA, 48-bit PA */
   ctx->gpu_info.tiler_features = 0x809;
   ctx->gpu_info.max_threads = 2048;
   ctx->gpu_info.thread_max_workgroup_size = 1024;
   ctx->gpu_info.thread_max_barrier_size = 1024;
   ctx->gpu_info.shader_present = 0x3f;
   ctx->gpu_info.l2_present = 0x1;
   ctx->gpu_info.tiler_present = 0x1;

   while (off + 4 <= len) {
      uint32_t hdr;
      memcpy(&hdr, buf + off, 4);
      off += 4;
      uint32_t key = hdr >> 2;
      uint32_t sz_code = hdr & 0x3;
      size_t val_sz = (size_t)1U << sz_code;
      if (off + val_sz > len)
         break;
      uint64_t val = 0;
      memcpy(&val, buf + off, val_sz);
      off += val_sz;

      switch (key) {
      case KBASE_GPUPROP_RAW_GPU_ID:
         raw_gpu_id = val;
         if (val > 0xffffffffULL) {
            /* 64-bit Valhall v11+ RAW_GPU_ID -> 32-bit Panthor gpu_id encoding */
            uint32_t arch_major = (val >> 56) & 0xff;
            uint32_t arch_minor = (val >> 48) & 0xf;
            uint32_t arch_rev   = (val >> 40) & 0xf;
            uint32_t prod_major = (val >> 32) & 0xf;
            uint32_t ver_major  = (val >> 16) & 0xf;
            uint32_t ver_minor  = (val >> 8) & 0xff;
            uint32_t ver_status = val & 0xf;
            ctx->gpu_info.gpu_id = (arch_major << 28) | (arch_minor << 24) |
                                   (arch_rev << 20) | (prod_major << 16) |
                                   (ver_major << 12) | (ver_minor << 4) | ver_status;
         } else {
            ctx->gpu_info.gpu_id = (uint32_t)val;
         }
         break;
      case KBASE_GPUPROP_RAW_L2_FEATURES:
         ctx->gpu_info.l2_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_TILER_FEATURES:
         ctx->gpu_info.tiler_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_MEM_FEATURES:
         ctx->gpu_info.mem_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_MMU_FEATURES:
         ctx->gpu_info.mmu_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_THREADS:
      case KBASE_GPUPROP_MAX_THREADS:
         ctx->gpu_info.max_threads = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE:
      case KBASE_GPUPROP_MAX_WORKGROUP_SIZE:
         ctx->gpu_info.thread_max_workgroup_size = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE:
      case KBASE_GPUPROP_MAX_BARRIER_SIZE:
         ctx->gpu_info.thread_max_barrier_size = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_THREAD_FEATURES:
         ctx->gpu_info.thread_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0:
      case KBASE_GPUPROP_TEXTURE_FEATURES_0:
         ctx->gpu_info.texture_features[0] = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1:
      case KBASE_GPUPROP_TEXTURE_FEATURES_1:
         ctx->gpu_info.texture_features[1] = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2:
      case KBASE_GPUPROP_TEXTURE_FEATURES_2:
         ctx->gpu_info.texture_features[2] = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3:
         ctx->gpu_info.texture_features[3] = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_SHADER_PRESENT:
         ctx->gpu_info.shader_present = val;
         break;
      case KBASE_GPUPROP_RAW_L2_PRESENT:
         ctx->gpu_info.l2_present = val;
         break;
      case KBASE_GPUPROP_RAW_TILER_PRESENT:
         ctx->gpu_info.tiler_present = val;
         break;
      case KBASE_GPUPROP_RAW_CORE_FEATURES:
         ctx->gpu_info.core_features = (uint32_t)val;
         break;
      case KBASE_GPUPROP_RAW_GPU_FEATURES:
         ctx->gpu_info.gpu_features = val;
         break;
      default:
         break;
      }
   }
   (void)raw_gpu_id;
   ctx->arch_major = DRM_PANTHOR_ARCH_MAJOR(ctx->gpu_info.gpu_id);
   if (ctx->arch_major < 10)
      ctx->arch_major = 10;
}

/*
 * CSF Bytecode Normalizer:
 * Scans a PanVK CSF command stream in-place before submission to kbase:
 *  1. Extracts `subq_context_va` from the initial submit's `MOV48 r_ctx, va`.
 *  2. Upgrades `SYNC_ADD32` (0x25), `SYNC_SET32` (0x24), `SYNC_ADD64` (0x33),
 *     and `SYNC_SET64` (0x32) from `CSG` scope (0) to `SYSTEM` scope (2) +
 *     doorbell/notification bit so sibling subqueues and evicted CSGs wake up on kbase.
 *  3. Rewrites `HEAP_OPERATION` (0x1e) modes `VT_END` (1) and `FRAGMENT_COMPLETED` (2)
 *     to `NOP` (0) so kbase's OOM handler never terminates the CSG on `frag_end > vt_end`.
 */
void panthor_kbase_normalize_cs_stream(uint64_t *stream_cpu,
                                       uint32_t stream_size_bytes,
                                       bool is_initial_submit,
                                       struct adapter_cs_patch_stats *stats)
{
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!stream_cpu || stream_size_bytes < sizeof(uint64_t))
      return;

   uint32_t count = stream_size_bytes / sizeof(uint64_t);
   if (stats)
      stats->total_instructions = count;

   for (uint32_t i = 0; i < count; i++) {
      uint64_t insn = stream_cpu[i];
      uint64_t opcode = (insn >> 56) & 0xffULL;

      if (is_initial_submit && i == 0 && opcode == CSF_OP_MOVE48) {
         uint8_t dest_reg = (insn >> 48) & 0xffU;
         uint64_t imm48 = insn & 0xffffffffffffULL;
         if (stats) {
            stats->extracted_subq_ctx_reg = dest_reg;
            stats->extracted_subq_ctx_va = imm48;
         }
      }

      if (opcode == CSF_OP_SYNC_ADD32 || opcode == CSF_OP_SYNC_SET32 ||
          opcode == CSF_OP_SYNC_ADD64 || opcode == CSF_OP_SYNC_SET64) {
         uint64_t scope = (insn >> 28) & 0x3ULL;
         if (scope == CSF_SYNC_SCOPE_CSG) {
            insn &= ~(0x3ULL << 28);
            insn |= (CSF_SYNC_SCOPE_SYSTEM << 28);
            /* Also set notification bit (bit 24) so kbase wakes poll/CQS */
            insn |= (1ULL << 24);
            stream_cpu[i] = insn;
            if (stats)
               stats->sync_scope_upgrades++;
         }
      } else if (opcode == CSF_OP_HEAP_OPERATION) {
         /* Bits [29:28] encode heap op mode: 0 = VT_STARTED, 1 = VT_END, 2 = FRAG_END */
         uint64_t mode = (insn >> 28) & 0x3ULL;
         if (mode != 0) {
            stream_cpu[i] = 0ULL; /* NOP */
            if (stats)
               stats->heap_op_nops++;
         }
      }
   }
}

/*
 * Synthesize a complete kbase CSF ring-buffer slot equivalent to panthor_sched.c's
 * kernel ring emission + kbase userspace queue preamble (SET_STATE/SET_SB_ENTRY,
 * REQ_RESOURCE, subqueue context register restore, HEAP_SET, FLUSH_CACHE2, CALL,
 * WAIT, SYNC_ADD64.system, ERROR_BARRIER).
 */
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
                                    uint16_t all_sb_mask)
{
   size_t idx = 0;
   uint32_t scratch_reg = (cs_reg_count >= 64) ? (cs_reg_count - 4) : 60;
   uint32_t subq_ctx_reg = (cs_reg_count >= 96) ? 88 : 56;

   /* 1. Scoreboard setup for kbase userspace ring wrapper */
   if (arch_major >= 11) {
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (0ULL << 40) | 0U; /* SB_SEL_ENDPOINT = 0 */
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (1ULL << 40) | 1U; /* SB_MASK_WAIT = 1 */
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (2ULL << 40) | 2U; /* SB_SEL_OTHER = LS(2) */
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (3ULL << 40) | 3U; /* SB_SEL_DEFERRED = 3 */
   } else {
      ring_slot[idx++] = (CSF_OP_SET_SB_ENTRY << 56) | (0ULL << 40) | (2ULL << 32);
   }

   /* 2. REQ_RESOURCE for the subqueue */
   uint32_t res_mask = (subqueue_index == 0) ? (CSF_RES_IDVS | CSF_RES_TILER) :
                       (subqueue_index == 1) ? CSF_RES_FRAGMENT : CSF_RES_COMPUTE;
   ring_slot[idx++] = (CSF_OP_REQ_RESOURCE << 56) | (uint64_t)res_mask;

   /* 3. Restore subqueue context register across kicks */
   if (subq_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | ((uint64_t)subq_ctx_reg << 48) |
                         (subq_ctx_va & 0xffffffffffffULL);
   }

   /* 4. Bind current tiler heap context on VT and FRAG subqueues */
   if (subqueue_index != 2 && tiler_heap_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | ((uint64_t)scratch_reg << 48) |
                         (tiler_heap_ctx_va & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_HEAP_SET << 56) | ((uint64_t)scratch_reg << 40);
   }

   /* 5. Cache flush + CALL into PanVK's command stream BO (matches panthor_sched.c!) */
   if (stream_size > 0 && stream_addr != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | ((uint64_t)(scratch_reg + 2) << 48) |
                         (uint64_t)latest_flush;
      ring_slot[idx++] = (CSF_OP_FLUSH_CACHE2 << 56) | (1ULL << 48) |
                         ((uint64_t)(scratch_reg + 2) << 40);
      ring_slot[idx++] = (CSF_OP_WAIT << 56) | (1ULL << 16);
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | ((uint64_t)scratch_reg << 48) |
                         (stream_addr & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | ((uint64_t)(scratch_reg + 2) << 48) |
                         (uint64_t)stream_size;
      ring_slot[idx++] = (CSF_OP_CALL << 56) | ((uint64_t)scratch_reg << 40) |
                         ((uint64_t)(scratch_reg + 2) << 32);
   }

   /* 6. Wait for all scoreboard slots and signal subqueue seqno with SYSTEM scope */
   ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | ((uint64_t)scratch_reg << 48) |
                      (seqno_gpu_va & 0xffffffffffffULL);
   ring_slot[idx++] = (CSF_OP_WAIT << 56) | ((uint64_t)(all_sb_mask ? all_sb_mask : 0xffffU) << 16);
   ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | ((uint64_t)(scratch_reg + 2) << 48) | 1ULL;
   ring_slot[idx++] = (CSF_OP_SYNC_ADD64 << 56) | (1ULL << 52) |
                      ((uint64_t)scratch_reg << 40) |
                      ((uint64_t)(scratch_reg + 2) << 32) |
                      (CSF_SYNC_SCOPE_SYSTEM << 28) | (1ULL << 24);
   ring_slot[idx++] = (CSF_OP_ERROR_BARRIER << 56);

   while (idx < ADAPTER_RING_SLOT_QWORDS)
      ring_slot[idx++] = 0ULL; /* NOP padding to fixed 192-byte slot */

   return idx * sizeof(uint64_t);
}

/* Initialize a fresh /dev/mali0 context following strict kbase ordering rules */
static int init_kbase_context(struct adapter_ctx *ctx, int mali_fd)
{
   resolve_real_funcs();
   ctx->mali_fd = mali_fd;

   /* Step 1: KBASE_IOCTL_VERSION_CHECK (nr 52) — MUST be first ioctl on fd */
   struct kbase_ioctl_version_check ver = { .major = 1, .minor = 38 };
   if (real_ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0)
      return -1;
   ctx->kbase_ver_major = ver.major;
   ctx->kbase_ver_minor = ver.minor;
   trace_log("KBASE_IOCTL_VERSION_CHECK major=%u minor=%u\n", ver.major, ver.minor);

   /* Step 2: KBASE_IOCTL_SET_FLAGS (nr 1) — MUST follow VERSION_CHECK */
   struct kbase_ioctl_set_flags flags = {
      .create_flags = BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED
   };
   if (real_ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &flags) < 0)
      return -1;
   trace_log("KBASE_IOCTL_SET_FLAGS create_flags=0x%x\n", flags.create_flags);

   /* Step 3: KBASE_IOCTL_MEM_EXEC_INIT (nr 38) — MUST precede GPU_EX BO allocs */
   struct kbase_ioctl_mem_exec_init exec_init = { .va_pages = 0x80000ULL };
   if (real_ioctl(mali_fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec_init) < 0)
      return -1;
   trace_log("KBASE_IOCTL_MEM_EXEC_INIT va_pages=0x%llx\n",
             (unsigned long long)exec_init.va_pages);

   /* Step 4: KBASE_IOCTL_MEM_JIT_INIT (nr 14) */
   struct kbase_ioctl_mem_jit_init jit_init = {
      .va_pages = 0x100000ULL,
      .max_allocations = 255,
      .trim_level = 100,
      .group_id = 0,
      .phys_pages = 0x100000ULL,
   };
   real_ioctl(mali_fd, KBASE_IOCTL_MEM_JIT_INIT, &jit_init);
   trace_log("KBASE_IOCTL_MEM_JIT_INIT va_pages=0x%llx\n",
             (unsigned long long)jit_init.va_pages);

   /* Step 5: Two-pass KBASE_IOCTL_GET_GPUPROPS (nr 3) */
   struct kbase_ioctl_get_gpuprops gp = { .buffer = 0, .size = 0, .flags = 0 };
   int blob_len = real_ioctl(mali_fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
   if (blob_len > 0) {
      uint8_t *blob = calloc(1, (size_t)blob_len);
      gp.buffer = (uintptr_t)blob;
      gp.size = (uint32_t)blob_len;
      if (real_ioctl(mali_fd, KBASE_IOCTL_GET_GPUPROPS, &gp) >= 0)
         parse_kbase_gpuprops(ctx, blob, (size_t)blob_len);
      free(blob);
   } else {
      parse_kbase_gpuprops(ctx, NULL, 0);
   }
   trace_log("KBASE_IOCTL_GET_GPUPROPS gpu_id=0x%x arch=%u\n",
             ctx->gpu_info.gpu_id, ctx->arch_major);

   /* Step 6: KBASE_IOCTL_CS_GET_GLB_IFACE (nr 51) */
   union kbase_ioctl_cs_get_glb_iface glb = {0};
   if (real_ioctl(mali_fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb) >= 0) {
      ctx->csif_info.csg_slot_count = glb.out.group_num ? glb.out.group_num : 8;
      ctx->csif_info.cs_slot_count = glb.out.total_stream_num ? glb.out.total_stream_num : 24;
      ctx->csif_info.cs_reg_count = (ctx->arch_major >= 11) ? 96 : 64;
      ctx->csif_info.scoreboard_slot_count = 8;
      ctx->csif_info.unpreserved_cs_reg_count = 4;
   }
   trace_log("KBASE_IOCTL_CS_GET_GLB_IFACE csg=%u cs=%u regs=%u\n",
             ctx->csif_info.csg_slot_count, ctx->csif_info.cs_slot_count,
             ctx->csif_info.cs_reg_count);

   /* Step 7: Map CSF USER_REG page (cookie 47UL << 12) for LATEST_FLUSH reads */
   ctx->user_reg_page = real_mmap(NULL, 4096, PROT_READ, MAP_SHARED,
                                  mali_fd, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);
   if (ctx->user_reg_page == MAP_FAILED)
      ctx->user_reg_page = NULL;

   /* Step 8: Create KCPU queue & stream fd for CQS/sync_file operations */
   struct kbase_ioctl_kcpu_queue_new kcpu = {0};
   if (real_ioctl(mali_fd, KBASE_IOCTL_KCPU_QUEUE_CREATE, &kcpu) >= 0) {
      ctx->kcpu_queue_id = kcpu.id;
      ctx->has_kcpu_queue = true;
      trace_log("KBASE_IOCTL_KCPU_QUEUE_CREATE id=%u\n", kcpu.id);
   }
   union kbase_ioctl_stream_create_u strm = {0};
   snprintf(strm.in.name, sizeof(strm.in.name), "panvk-kbase-sync");
   if (real_ioctl(mali_fd, KBASE_IOCTL_STREAM_CREATE, &strm) >= 0)
      ctx->stream_fd = strm.out.fd;

   /* Step 9: Allocate CSF_EVENT BO for CQS syncobjs & subqueue seqnos */
   union kbase_ioctl_mem_alloc ev_alloc = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT | BASE_MEM_COHERENT_LOCAL,
      }
   };
   if (real_ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &ev_alloc) == 0) {
      ctx->event_bo_size = 16 * 4096;
      ctx->event_bo_cpu = real_mmap(NULL, ctx->event_bo_size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, mali_fd, ev_alloc.out.gpu_va);
      if (ctx->event_bo_cpu != MAP_FAILED) {
         ctx->event_bo_gpu_va = (uintptr_t)ctx->event_bo_cpu;
         memset(ctx->event_bo_cpu, 0, ctx->event_bo_size);
      }
   }

   ctx->mmu_info.page_size_bitmap = 0x201000ULL; /* 4KB | 2MB */
   ctx->next_vm_id = 1;
   ctx->next_gem_handle = 1;
   ctx->next_syncobj_handle = 1;
   ctx->next_group_handle = 1;
   ctx->next_heap_handle = 1;
   ctx->active = true;
   return 0;
}

int panthor_kbase_adapter_open(const char *path, int flags)
{
   resolve_real_funcs();
   pthread_mutex_lock(&g_lock);
   const char *mali_path = getenv("PANVK_KBASE_NODE_PATH");
   if (!mali_path || !mali_path[0])
      mali_path = "/dev/mali0";
   (void)path;
   int mali_fd = real_open(mali_path, flags | O_RDWR | O_CLOEXEC, 0);
   if (mali_fd < 0) {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   memset(&g_ctx, 0, sizeof(g_ctx));
   g_ctx.drm_fd = mali_fd;
   if (init_kbase_context(&g_ctx, mali_fd) < 0) {
      real_close(mali_fd);
      g_ctx.active = false;
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pthread_mutex_unlock(&g_lock);
   return mali_fd;
}

int panthor_kbase_adapter_dup(int fd)
{
   /*
    * Note: dup() on /dev/mali0 shares struct kbase_file whose VERSION_CHECK
    * can only run once. Our adapter tracks the context across dup'd fds or
    * opens a fresh /dev/mali0 context when required.
    */
   if (g_ctx.active && fd == g_ctx.drm_fd)
      return fd;
   return dup(fd);
}

int panthor_kbase_adapter_close(int fd)
{
   resolve_real_funcs();
   pthread_mutex_lock(&g_lock);
   if (g_ctx.active && fd == g_ctx.drm_fd) {
      if (g_ctx.has_kcpu_queue) {
         struct kbase_ioctl_kcpu_queue_delete del = { .id = g_ctx.kcpu_queue_id };
         real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_DELETE, &del);
      }
      if (g_ctx.user_reg_page)
         real_munmap(g_ctx.user_reg_page, 4096);
      g_ctx.active = false;
   }
   pthread_mutex_unlock(&g_lock);
   return real_close(fd);
}

bool panthor_kbase_adapter_is_virtual_drm_fd(int fd)
{
   return g_ctx.active && fd == g_ctx.drm_fd;
}

static struct adapter_bo *find_bo(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
      if (g_ctx.bos[i].in_use && g_ctx.bos[i].gem_handle == handle)
         return &g_ctx.bos[i];
   }
   return NULL;
}

static struct adapter_syncobj *find_syncobj(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
      if (g_ctx.syncobjs[i].in_use && g_ctx.syncobjs[i].handle == handle)
         return &g_ctx.syncobjs[i];
   }
   return NULL;
}

static struct adapter_group *find_group(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_GROUPS; i++) {
      if (g_ctx.groups[i].in_use && g_ctx.groups[i].group_handle == handle)
         return &g_ctx.groups[i];
   }
   return NULL;
}

int panthor_kbase_adapter_ioctl(int fd, unsigned long request, void *arg)
{
   resolve_real_funcs();
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return real_ioctl(fd, request, arg);

   pthread_mutex_lock(&g_lock);
   unsigned int nr = _IOC_NR(request);
   unsigned int type = _IOC_TYPE(request);
   int ret = 0;

   if (type != 'd') {
      pthread_mutex_unlock(&g_lock);
      return real_ioctl(fd, request, arg);
   }

   /* Standard DRM GEM / PRIME / SYNCOBJ IOCTLs */
   switch (nr) {
   case 0x00: { /* DRM_IOCTL_VERSION */
      struct drm_version *v = (struct drm_version *)arg;
      if (v) {
         v->version_major = 1;
         v->version_minor = 3;
         v->version_patchlevel = 0;
         const char *name = "panthor";
         const char *date = "20230801";
         const char *desc = "Panthor-to-Kbase UAPI Adapter";
         if (v->name && v->name_len > 0)
            strncpy(v->name, name, v->name_len);
         v->name_len = strlen(name);
         if (v->date && v->date_len > 0)
            strncpy(v->date, date, v->date_len);
         v->date_len = strlen(date);
         if (v->desc && v->desc_len > 0)
            strncpy(v->desc, desc, v->desc_len);
         v->desc_len = strlen(desc);
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x0c: { /* DRM_IOCTL_GET_CAP */
      struct { uint64_t capability; uint64_t value; } *cap = arg;
      if (cap)
         cap->value = 1ULL;
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x0d: { /* DRM_IOCTL_SET_CLIENT_CAP */
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc1: { /* DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD */
      struct drm_syncobj_handle *sh = (struct drm_syncobj_handle *)arg;
      if (sh) {
         sh->fd = (g_ctx.stream_fd >= 0) ? dup(g_ctx.stream_fd) : dup(g_ctx.mali_fd);
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc2: { /* DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE */
      struct drm_syncobj_handle *sh = (struct drm_syncobj_handle *)arg;
      if (sh) {
         for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
            if (!g_ctx.syncobjs[i].in_use) {
               g_ctx.syncobjs[i].in_use = true;
               g_ctx.syncobjs[i].handle = g_ctx.next_syncobj_handle++;
               g_ctx.syncobjs[i].value = 1ULL;
               sh->handle = g_ctx.syncobjs[i].handle;
               break;
            }
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x09: { /* DRM_IOCTL_GEM_CLOSE */
      struct drm_gem_close *cl = (struct drm_gem_close *)arg;
      struct adapter_bo *bo = find_bo(cl->handle);
      if (bo) {
         void *unmap_ptr = (bo->cpu_ptr && bo->is_same_va) ? bo->cpu_ptr : NULL;
         size_t unmap_sz = bo->cpu_size;
         uint64_t free_va = (!unmap_ptr && bo->gpu_va) ? bo->gpu_va : 0;
         uint32_t h = bo->gem_handle;
         uint64_t va = bo->gpu_va;
         if (bo->dmabuf_fd >= 0)
            real_close(bo->dmabuf_fd);
         memset(bo, 0, sizeof(*bo));
         pthread_mutex_unlock(&g_lock);
         if (unmap_ptr)
            real_munmap(unmap_ptr, unmap_sz);
         else if (free_va) {
            struct kbase_ioctl_mem_free mf = { .gpu_addr = free_va };
            real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_FREE, &mf);
         }
         trace_log("DRM_IOCTL_GEM_CLOSE handle=%u -> KBASE_IOCTL_MEM_FREE va=0x%llx\n",
                   h, (unsigned long long)va);
         return 0;
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x2d: { /* DRM_IOCTL_PRIME_HANDLE_TO_FD */
      struct drm_prime_handle *ph = (struct drm_prime_handle *)arg;
      struct adapter_bo *bo = find_bo(ph->handle);
      if (!bo) {
         pthread_mutex_unlock(&g_lock);
         errno = ENOENT;
         return -1;
      }
      if (bo->dmabuf_fd < 0) {
         int mfd = memfd_create("panvk-dmabuf-export", MFD_CLOEXEC);
         if (mfd >= 0 && ftruncate(mfd, (off_t)bo->size) == 0) {
            struct stat st;
            bo->dmabuf_fd = mfd;
            if (fstat(mfd, &st) == 0)
               bo->dmabuf_ino = (uint64_t)st.st_ino;
         }
      }
      ph->fd = dup(bo->dmabuf_fd);
      pthread_mutex_unlock(&g_lock);
      return (ph->fd >= 0) ? 0 : -1;
   }
   case 0x2e: { /* DRM_IOCTL_PRIME_FD_TO_HANDLE */
      struct drm_prime_handle *ph = (struct drm_prime_handle *)arg;
      struct stat st;
      bool have_stat = (fstat(ph->fd, &st) == 0);
      uint64_t sz = (have_stat && st.st_size > 0) ? (uint64_t)st.st_size : 65536;
      if (have_stat && st.st_ino != 0) {
         for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
            if (g_ctx.bos[i].in_use && g_ctx.bos[i].dmabuf_ino == (uint64_t)st.st_ino) {
               ph->handle = g_ctx.bos[i].gem_handle;
               pthread_mutex_unlock(&g_lock);
               return 0;
            }
         }
      }
      int fd_copy = dup(ph->fd);
      union kbase_ioctl_mem_import imp = {
         .in = {
            .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                     BASE_MEM_IMPORT_SHARED,
            .phandle = (uintptr_t)&fd_copy,
            .type = BASE_MEM_IMPORT_TYPE_UMM,
         }
      };
      if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT, &imp) < 0 && errno == ENOTTY) {
         /* Retry with 40-byte MediaTek SLC layout (csf-v11/186) */
         union kbase_ioctl_mem_import_mtk imp_mtk = {0};
         imp_mtk.in.flags = imp.in.flags;
         imp_mtk.in.phandle = (uintptr_t)&fd_copy;
         imp_mtk.in.type = BASE_MEM_IMPORT_TYPE_UMM;
         if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT_MTK, &imp_mtk) == 0) {
            imp.out.gpu_va = imp_mtk.out.gpu_va;
            imp.out.va_pages = imp_mtk.out.va_pages;
            imp.out.flags = imp_mtk.out.flags;
         }
      }
      for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
         if (!g_ctx.bos[i].in_use) {
            struct adapter_bo *bo = &g_ctx.bos[i];
            memset(bo, 0, sizeof(*bo));
            bo->in_use = true;
            bo->gem_handle = g_ctx.next_gem_handle++;
            bo->size = sz;
            bo->gpu_va = imp.out.gpu_va;
            bo->dmabuf_fd = fd_copy;
            bo->dmabuf_ino = have_stat ? (uint64_t)st.st_ino : 0;
            bo->is_imported = true;
            ph->handle = bo->gem_handle;
            trace_log("DRM_IOCTL_PRIME_FD_TO_HANDLE fd=%d -> KBASE_IOCTL_MEM_IMPORT va=0x%llx handle=%u\n",
                      ph->fd, (unsigned long long)bo->gpu_va, bo->gem_handle);
            pthread_mutex_unlock(&g_lock);
            return 0;
         }
      }
      close(fd_copy);
      pthread_mutex_unlock(&g_lock);
      errno = ENOMEM;
      return -1;
   }
   case 0xbf: { /* DRM_IOCTL_SYNCOBJ_CREATE */
      struct drm_syncobj_create *sc = (struct drm_syncobj_create *)arg;
      for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
         if (!g_ctx.syncobjs[i].in_use) {
            struct adapter_syncobj *so = &g_ctx.syncobjs[i];
            so->in_use = true;
            so->handle = g_ctx.next_syncobj_handle++;
            so->value = (sc->flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1ULL : 0ULL;
            if (g_ctx.event_bo_cpu) {
               size_t off = 1024 + i * 16;
               so->cqs_gpu_va = g_ctx.event_bo_gpu_va + off;
               so->cqs_cpu_ptr = (uint64_t *)(g_ctx.event_bo_cpu + off);
               *so->cqs_cpu_ptr = so->value;
            }
            sc->handle = so->handle;
            pthread_mutex_unlock(&g_lock);
            return 0;
         }
      }
      pthread_mutex_unlock(&g_lock);
      errno = ENOMEM;
      return -1;
   }
   case 0xc0: { /* DRM_IOCTL_SYNCOBJ_DESTROY */
      struct drm_syncobj_destroy *sd = (struct drm_syncobj_destroy *)arg;
      struct adapter_syncobj *so = find_syncobj(sd->handle);
      if (so)
         memset(so, 0, sizeof(*so));
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc3: /* DRM_IOCTL_SYNCOBJ_WAIT */
   case 0xca: { /* DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT */
      if (g_ctx.has_kcpu_queue && g_ctx.event_bo_gpu_va) {
         struct base_cqs_wait_operation_info winfo = {
            .addr = g_ctx.event_bo_gpu_va,
            .val = 0,
            .operation = BASEP_CQS_WAIT_OPERATION_GT,
            .data_type = BASEP_CQS_DATA_TYPE_U64,
         };
         struct base_kcpu_command cmd = {
            .type = BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION,
            .info.cqs_wait_operation = {
               .objs = (uintptr_t)&winfo,
               .nr_objs = 1,
            }
         };
         struct kbase_ioctl_kcpu_queue_enqueue enq = {
            .addr = (uintptr_t)&cmd,
            .nr_commands = 1,
            .id = g_ctx.kcpu_queue_id,
         };
         real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enq);
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc4: /* DRM_IOCTL_SYNCOBJ_RESET */
   case 0xc5: /* DRM_IOCTL_SYNCOBJ_SIGNAL */
   case 0xcb: /* DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL */
   case 0xcc: /* DRM_IOCTL_SYNCOBJ_TRANSFER */
   case 0xcd: /* DRM_IOCTL_SYNCOBJ_QUERY */
      pthread_mutex_unlock(&g_lock);
      return 0;
   default:
      break;
   }

   /* Panthor-specific IOCTLs (DRM_COMMAND_BASE = 0x40 .. 0x4f) */
   if (nr < DRM_COMMAND_BASE || nr >= DRM_COMMAND_BASE + 0x10) {
      pthread_mutex_unlock(&g_lock);
      return 0;
   }

   unsigned int panthor_id = nr - DRM_COMMAND_BASE;
   switch (panthor_id) {
   case DRM_PANTHOR_DEV_QUERY: {
      struct drm_panthor_dev_query *q = (struct drm_panthor_dev_query *)arg;
      switch (q->type) {
      case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_gpu_info);
         } else {
            memcpy((void *)(uintptr_t)q->pointer, &g_ctx.gpu_info,
                   q->size < sizeof(g_ctx.gpu_info) ? q->size : sizeof(g_ctx.gpu_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_csif_info);
         } else {
            memcpy((void *)(uintptr_t)q->pointer, &g_ctx.csif_info,
                   q->size < sizeof(g_ctx.csif_info) ? q->size : sizeof(g_ctx.csif_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_MMU_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_mmu_info);
         } else {
            memcpy((void *)(uintptr_t)q->pointer, &g_ctx.mmu_info,
                   q->size < sizeof(g_ctx.mmu_info) ? q->size : sizeof(g_ctx.mmu_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO: {
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_timestamp_info);
         } else {
            union kbase_ioctl_get_cpu_gpu_timeinfo ti = {0};
            real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &ti);
            struct drm_panthor_timestamp_info out_ti = {
               .timestamp_frequency = 26000000ULL,
               .current_timestamp = ti.out.timestamp,
               .cpu_timestamp_sec = ti.out.sec,
               .cpu_timestamp_nsec = ti.out.nsec,
               .cycle_count = ti.out.cycle_counter,
            };
            memcpy((void *)(uintptr_t)q->pointer, &out_ti,
                   q->size < sizeof(out_ti) ? q->size : sizeof(out_ti));
         }
         break;
      }
      case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO: {
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_group_priorities_info);
         } else {
            struct kbase_ioctl_context_priority_check pc = { .priority = 3 };
            real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CONTEXT_PRIORITY_CHECK, &pc);
            struct drm_panthor_group_priorities_info pi = { .allowed_mask = 0x7 };
            memcpy((void *)(uintptr_t)q->pointer, &pi,
                   q->size < sizeof(pi) ? q->size : sizeof(pi));
         }
         break;
      }
      default:
         break;
      }
      trace_log("DRM_PANTHOR_DEV_QUERY type=%u\n", q->type);
      break;
   }

   case DRM_PANTHOR_SET_USER_MMIO_OFFSET: {
      struct drm_panthor_set_user_mmio_offset *mo =
         (struct drm_panthor_set_user_mmio_offset *)arg;
      g_ctx.user_mmio_offset = mo->offset;
      trace_log("DRM_PANTHOR_SET_USER_MMIO_OFFSET offset=0x%llx\n",
                (unsigned long long)mo->offset);
      break;
   }

   case DRM_PANTHOR_VM_CREATE: {
      struct drm_panthor_vm_create *vc = (struct drm_panthor_vm_create *)arg;
      vc->id = g_ctx.next_vm_id++;
      if (!vc->user_va_range) vc->user_va_range = (1ULL << 39);
      trace_log("DRM_PANTHOR_VM_CREATE -> vm_id=%u\n", vc->id);
      break;
   }

   case DRM_PANTHOR_VM_DESTROY: {
      struct drm_panthor_vm_destroy *vd = (struct drm_panthor_vm_destroy *)arg;
      trace_log("DRM_PANTHOR_VM_DESTROY vm_id=%u\n", vd->id);
      break;
   }

   case DRM_PANTHOR_VM_GET_STATE: {
      struct drm_panthor_vm_get_state *vs = (struct drm_panthor_vm_get_state *)arg;
      vs->state = DRM_PANTHOR_VM_STATE_USABLE;
      break;
   }

   case DRM_PANTHOR_BO_CREATE: {
      struct drm_panthor_bo_create *bc = (struct drm_panthor_bo_create *)arg;
      uint64_t aligned_sz = (bc->size + 4095ULL) & ~4095ULL;
      if (aligned_sz == 0)
         aligned_sz = 4096ULL;
      uint64_t pages = aligned_sz >> 12;

      uint64_t kflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                        BASE_MEM_SAME_VA;
      if (bc->flags & DRM_PANTHOR_BO_WB_MMAP)
         kflags |= BASE_MEM_CACHED_CPU;
      if (aligned_sz <= 65536ULL)
         kflags |= BASE_MEM_CSF_EVENT;

      union kbase_ioctl_mem_alloc ma = {
         .in = {
            .va_pages = pages,
            .commit_pages = pages,
            .extension = 0,
            .flags = kflags,
         }
      };
      if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_ALLOC, &ma) < 0) {
         ret = -1;
         break;
      }
      /* Immediately map SAME_VA region in kbase so GPU VA == CPU VA is materialized */
      void *cpu = real_mmap(NULL, (size_t)aligned_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED, g_ctx.mali_fd, ma.out.gpu_va);
      if (cpu == MAP_FAILED) {
         ret = -1;
         break;
      }
      memset(cpu, 0, (size_t)aligned_sz);

      struct adapter_bo *bo = NULL;
      for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
         if (!g_ctx.bos[i].in_use) {
            bo = &g_ctx.bos[i];
            break;
         }
      }
      if (!bo) {
         ret = -1;
         break;
      }
      memset(bo, 0, sizeof(*bo));
      bo->in_use = true;
      bo->gem_handle = g_ctx.next_gem_handle++;
      bo->size = aligned_sz;
      bo->panthor_flags = bc->flags;
      bo->kbase_flags = kflags;
      bo->kbase_cookie_or_va = ma.out.gpu_va;
      bo->gpu_va = (uintptr_t)cpu;
      bo->cpu_ptr = cpu;
      bo->cpu_size = (size_t)aligned_sz;
      bo->dmabuf_fd = -1;
      bo->is_same_va = true;
      bo->is_csf_event = (kflags & BASE_MEM_CSF_EVENT) != 0;

      bc->handle = bo->gem_handle;
      bc->size = aligned_sz;
      trace_log("DRM_PANTHOR_BO_CREATE size=%llu -> KBASE_IOCTL_MEM_ALLOC handle=%u va=0x%llx\n",
                (unsigned long long)aligned_sz, bo->gem_handle,
                (unsigned long long)bo->gpu_va);
      break;
   }

   case DRM_PANTHOR_BO_MMAP_OFFSET: {
      struct drm_panthor_bo_mmap_offset *mo = (struct drm_panthor_bo_mmap_offset *)arg;
      /* Encode GEM handle into fake mmap offset above 0x1000000000ULL */
      mo->offset = 0x1000000000ULL | ((uint64_t)mo->handle << 12);
      break;
   }

   case DRM_PANTHOR_VM_BIND: {
      struct drm_panthor_vm_bind *vb = (struct drm_panthor_vm_bind *)arg;
      struct drm_panthor_vm_bind_op *ops =
         (struct drm_panthor_vm_bind_op *)(uintptr_t)vb->ops.array;
      for (uint32_t i = 0; i < vb->ops.count; i++) {
         uint32_t op_type = ops[i].flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;
         if (op_type == DRM_PANTHOR_VM_BIND_OP_TYPE_MAP) {
            struct adapter_bo *bo = find_bo(ops[i].bo_handle);
            if (bo) {
               if (ops[i].va == ~0ULL || ops[i].va == 0ULL) {
                  ops[i].va = bo->gpu_va + ops[i].bo_offset;
               } else if (!(ops[i].flags & DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC)) {
                  /* Executable shader BO in EXEC_VA zone */
                  bo->kbase_flags |= BASE_MEM_PROT_GPU_EX;
               }
               trace_log("DRM_PANTHOR_VM_BIND MAP bo=%u va=0x%llx -> kbase_va=0x%llx\n",
                         bo->gem_handle, (unsigned long long)ops[i].va,
                         (unsigned long long)bo->gpu_va);
            }
         }
      }
      break;
   }

   case DRM_PANTHOR_BO_SYNC: {
      struct drm_panthor_bo_sync *bs = (struct drm_panthor_bo_sync *)arg;
      struct drm_panthor_bo_sync_op *ops =
         (struct drm_panthor_bo_sync_op *)(uintptr_t)bs->ops.array;
      for (uint32_t i = 0; i < bs->ops.count; i++) {
         struct adapter_bo *bo = find_bo(ops[i].handle);
         if (bo && bo->cpu_ptr) {
            struct kbase_ioctl_mem_sync ms = {
               .handle = bo->gpu_va,
               .user_addr = (uintptr_t)bo->cpu_ptr + ops[i].offset,
               .size = ops[i].size,
               .type = (ops[i].type == DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH)
                          ? BASE_SYNCSET_OP_MSYNC
                          : (BASE_SYNCSET_OP_MSYNC | BASE_SYNCSET_OP_CSYNC),
            };
            real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_SYNC, &ms);
            trace_log("DRM_PANTHOR_BO_SYNC bo=%u -> KBASE_IOCTL_MEM_SYNC va=0x%llx size=%llu\n",
                      bo->gem_handle, (unsigned long long)bo->gpu_va,
                      (unsigned long long)ops[i].size);
         }
      }
      break;
   }

   case DRM_PANTHOR_BO_QUERY_INFO: {
      struct drm_panthor_bo_query_info *qi = (struct drm_panthor_bo_query_info *)arg;
      struct adapter_bo *bo = find_bo(qi->handle);
      if (bo) {
         qi->extra_flags = bo->is_imported ? DRM_PANTHOR_BO_IS_IMPORTED : 0;
         qi->create_flags = bo->panthor_flags;
      }
      break;
   }

   case DRM_PANTHOR_BO_SET_LABEL: {
      struct drm_panthor_bo_set_label *bl = (struct drm_panthor_bo_set_label *)arg;
      struct adapter_bo *bo = find_bo(bl->handle);
      if (bo && bl->label)
         snprintf(bo->label, sizeof(bo->label), "%s", (const char *)(uintptr_t)bl->label);
      break;
   }

   case DRM_PANTHOR_TILER_HEAP_CREATE: {
      struct drm_panthor_tiler_heap_create *hc =
         (struct drm_panthor_tiler_heap_create *)arg;
      union kbase_ioctl_cs_tiler_heap_init_1_13 hi13 = {
         .in = {
            .chunk_size = hc->chunk_size ? hc->chunk_size : 1048576U,
            .initial_chunks = hc->initial_chunk_count ? hc->initial_chunk_count : 4U,
            .max_chunks = hc->max_chunks ? hc->max_chunks : 200U,
            .target_in_flight = (uint16_t)(hc->target_in_flight ? hc->target_in_flight : 65535U),
            .group_id = 0,
         }
      };
      if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13, &hi13) < 0) {
         union kbase_ioctl_cs_tiler_heap_init hi24 = {
            .in = {
               .chunk_size = hi13.in.chunk_size,
               .initial_chunks = hi13.in.initial_chunks,
               .max_chunks = hi13.in.max_chunks,
               .target_in_flight = hi13.in.target_in_flight,
               .group_id = 0,
               .buf_desc_va = 0,
            }
         };
         if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &hi24) < 0) {
            ret = -1;
            break;
         }
         hi13.out.gpu_heap_va = hi24.out.gpu_heap_va;
         hi13.out.first_chunk_va = hi24.out.first_chunk_va;
      }

      for (size_t i = 0; i < ADAPTER_MAX_HEAPS; i++) {
         if (!g_ctx.heaps[i].in_use) {
            g_ctx.heaps[i].in_use = true;
            g_ctx.heaps[i].handle = g_ctx.next_heap_handle++;
            g_ctx.heaps[i].gpu_heap_va = hi13.out.gpu_heap_va;
            g_ctx.heaps[i].first_chunk_va = hi13.out.first_chunk_va;
            g_ctx.heaps[i].chunk_size = hc->chunk_size;
            hc->handle = g_ctx.heaps[i].handle;
            hc->tiler_heap_ctx_gpu_va = hi13.out.gpu_heap_va;
            hc->first_heap_chunk_gpu_va = hi13.out.first_chunk_va;
            /* Bind heap context to active groups on this VM */
            for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
               if (g_ctx.groups[g].in_use && g_ctx.groups[g].vm_id == hc->vm_id)
                  g_ctx.groups[g].active_tiler_heap_ctx_va = hi13.out.gpu_heap_va;
            }
            trace_log("DRM_PANTHOR_TILER_HEAP_CREATE -> KBASE_IOCTL_CS_TILER_HEAP_INIT heap_va=0x%llx first_chunk=0x%llx\n",
                      (unsigned long long)hc->tiler_heap_ctx_gpu_va,
                      (unsigned long long)hc->first_heap_chunk_gpu_va);
            break;
         }
      }
      break;
   }

   case DRM_PANTHOR_TILER_HEAP_DESTROY: {
      struct drm_panthor_tiler_heap_destroy *hd =
         (struct drm_panthor_tiler_heap_destroy *)arg;
      for (size_t i = 0; i < ADAPTER_MAX_HEAPS; i++) {
         if (g_ctx.heaps[i].in_use && g_ctx.heaps[i].handle == hd->handle) {
            struct kbase_ioctl_cs_tiler_heap_term ht = {
               .gpu_heap_va = g_ctx.heaps[i].gpu_heap_va
            };
            real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &ht);
            trace_log("DRM_PANTHOR_TILER_HEAP_DESTROY -> KBASE_IOCTL_CS_TILER_HEAP_TERM va=0x%llx\n",
                      (unsigned long long)ht.gpu_heap_va);
            memset(&g_ctx.heaps[i], 0, sizeof(g_ctx.heaps[i]));
            break;
         }
      }
      break;
   }

   case DRM_PANTHOR_GROUP_CREATE: {
      struct drm_panthor_group_create *gc = (struct drm_panthor_group_create *)arg;
      uint32_t qcnt = gc->queues.count;
      if (qcnt > ADAPTER_MAX_SUBQUEUES)
         qcnt = ADAPTER_MAX_SUBQUEUES;

      union kbase_ioctl_cs_queue_group_create_1_6 csg = {
         .in = {
            .tiler_mask = gc->tiler_core_mask ? gc->tiler_core_mask : 1ULL,
            .fragment_mask = gc->fragment_core_mask ? gc->fragment_core_mask : ~0ULL,
            .compute_mask = gc->compute_core_mask ? gc->compute_core_mask : ~0ULL,
            .cs_min = (uint8_t)(qcnt ? qcnt : 3),
            .priority = (uint8_t)(gc->priority <= 3 ? gc->priority : 1),
            .tiler_max = gc->max_tiler_cores ? gc->max_tiler_cores : 1,
            .fragment_max = gc->max_fragment_cores ? gc->max_fragment_cores : 64,
            .compute_max = gc->max_compute_cores ? gc->max_compute_cores : 64,
         }
      };
      if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &csg) < 0) {
         ret = -1;
         break;
      }

      struct adapter_group *grp = NULL;
      for (size_t i = 0; i < ADAPTER_MAX_GROUPS; i++) {
         if (!g_ctx.groups[i].in_use) {
            grp = &g_ctx.groups[i];
            break;
         }
      }
      if (!grp) {
         ret = -1;
         break;
      }
      memset(grp, 0, sizeof(*grp));
      grp->in_use = true;
      grp->group_handle = g_ctx.next_group_handle++;
      grp->kbase_group_handle = csg.out.group_handle;
      grp->vm_id = gc->vm_id;
      grp->queue_count = qcnt;

      /* Allocate SAME_VA ring buffer BO, register queue, bind queue, and mmap USER_IO for each subqueue */
      for (uint32_t q = 0; q < qcnt; q++) {
         struct adapter_subqueue *sq = &grp->subqueues[q];
         sq->csi_index = q;
         sq->ringbuf_size = ADAPTER_RINGBUF_SIZE;

         union kbase_ioctl_mem_alloc ra = {
            .in = {
               .va_pages = ADAPTER_RINGBUF_SIZE >> 12,
               .commit_pages = ADAPTER_RINGBUF_SIZE >> 12,
               .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_SAME_VA,
            }
         };
         if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_ALLOC, &ra) < 0) {
            ret = -1;
            break;
         }
         void *rcpu = real_mmap(NULL, ADAPTER_RINGBUF_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, g_ctx.mali_fd, ra.out.gpu_va);
         if (rcpu == MAP_FAILED) {
            ret = -1;
            break;
         }
         memset(rcpu, 0, ADAPTER_RINGBUF_SIZE);
         sq->ring_cpu_ptr = (uint64_t *)rcpu;
         sq->ring_gpu_va = (uintptr_t)rcpu;

         struct kbase_ioctl_cs_queue_register qreg = {
            .buffer_gpu_addr = sq->ring_gpu_va,
            .buffer_size = ADAPTER_RINGBUF_SIZE,
            .priority = 1,
         };
         if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &qreg) < 0) {
            ret = -1;
            break;
         }

         union kbase_ioctl_cs_queue_bind qbind = {
            .in = {
               .buffer_gpu_addr = sq->ring_gpu_va,
               .group_handle = grp->kbase_group_handle,
               .csi_index = (uint8_t)q,
            }
         };
         if (real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_BIND, &qbind) < 0) {
            ret = -1;
            break;
         }

         /* Strictly mandatory: mmap 3 USER_IO pages BEFORE first CS_QUEUE_KICK! */
         void *uio = real_mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, g_ctx.mali_fd, qbind.out.mmap_handle);
         if (uio == MAP_FAILED) {
            ret = -1;
            break;
         }
         sq->user_io_ptr = uio;
         sq->bound = true;

         if (g_ctx.event_bo_cpu) {
            size_t seq_off = q * 64;
            sq->seqno_gpu_va = g_ctx.event_bo_gpu_va + seq_off;
            sq->seqno_cpu_ptr = (volatile uint64_t *)(g_ctx.event_bo_cpu + seq_off);
            *sq->seqno_cpu_ptr = 0;
         }
      }

      gc->group_handle = grp->group_handle;
      trace_log("DRM_PANTHOR_GROUP_CREATE -> KBASE_IOCTL_CS_QUEUE_GROUP_CREATE handle=%u queues=%u\n",
                grp->group_handle, qcnt);
      break;
   }

   case DRM_PANTHOR_GROUP_DESTROY: {
      struct drm_panthor_group_destroy *gd = (struct drm_panthor_group_destroy *)arg;
      struct adapter_group *grp = find_group(gd->group_handle);
      if (grp) {
         for (uint32_t q = 0; q < grp->queue_count; q++) {
            struct adapter_subqueue *sq = &grp->subqueues[q];
            if (sq->bound) {
               if (sq->user_io_ptr)
                  real_munmap(sq->user_io_ptr, 3 * 4096);
               struct kbase_ioctl_cs_queue_terminate qt = {
                  .buffer_gpu_addr = sq->ring_gpu_va
               };
               real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &qt);
               if (sq->ring_cpu_ptr)
                  real_munmap(sq->ring_cpu_ptr, sq->ringbuf_size);
            }
         }
         struct kbase_ioctl_cs_queue_group_term gt = {
            .group_handle = grp->kbase_group_handle
         };
         real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &gt);
         trace_log("DRM_PANTHOR_GROUP_DESTROY -> KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE handle=%u\n",
                   grp->group_handle);
         memset(grp, 0, sizeof(*grp));
      }
      break;
   }

   case DRM_PANTHOR_GROUP_GET_STATE: {
      struct drm_panthor_group_get_state *gs =
         (struct drm_panthor_group_get_state *)arg;
      gs->state = 0;
      gs->fatal_queues = 0;
      break;
   }

   case DRM_PANTHOR_GROUP_SUBMIT: {
      struct drm_panthor_group_submit *gs = (struct drm_panthor_group_submit *)arg;
      struct adapter_group *grp = find_group(gs->group_handle);
      if (!grp) {
         ret = -1;
         break;
      }
      struct drm_panthor_queue_submit *qsubs =
         (struct drm_panthor_queue_submit *)(uintptr_t)gs->queue_submits.array;

      for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
         struct drm_panthor_queue_submit *qs = &qsubs[i];
         if (qs->queue_index >= grp->queue_count)
            continue;
         struct adapter_subqueue *sq = &grp->subqueues[qs->queue_index];
         bool is_initial = (sq->emitted_jobs == 0);

         /* 1. Normalize the submitted CSF command stream in-place */
         struct adapter_cs_patch_stats stats = {0};
         if (qs->stream_addr && qs->stream_size >= 8) {
            uint64_t *stream_cpu = (uint64_t *)(uintptr_t)qs->stream_addr;
            panthor_kbase_normalize_cs_stream(stream_cpu, qs->stream_size,
                                              is_initial, &stats);
            if (is_initial && stats.extracted_subq_ctx_va) {
               sq->cached_subq_ctx_va = stats.extracted_subq_ctx_va;
               sq->cached_subq_ctx_reg = stats.extracted_subq_ctx_reg;
            }
         }

         /* 2. Emit the CSF ring slot into the kbase userspace ring buffer */
         uint32_t ring_off = (uint32_t)(sq->insert_offset % sq->ringbuf_size);
         if (ring_off + ADAPTER_RING_SLOT_BYTES > sq->ringbuf_size) {
            memset((uint8_t *)sq->ring_cpu_ptr + ring_off, 0,
                   sq->ringbuf_size - ring_off);
            sq->insert_offset += (sq->ringbuf_size - ring_off);
            ring_off = 0;
         }
         uint64_t *slot_ptr =
            (uint64_t *)((uint8_t *)sq->ring_cpu_ptr + ring_off);
         size_t slot_bytes = panthor_kbase_emit_ring_slot(
            slot_ptr,
            g_ctx.arch_major,
            g_ctx.csif_info.cs_reg_count,
            qs->queue_index,
            sq->cached_subq_ctx_va,
            grp->active_tiler_heap_ctx_va,
            qs->stream_addr,
            qs->stream_size,
            qs->latest_flush,
            sq->seqno_gpu_va,
            0xffffU);

         sq->insert_offset += slot_bytes;
         sq->emitted_jobs++;

         /* 3. Publish CS_INSERT in USER_IO Input Page (+0x1000), ring Doorbell (+0x0000), and kick */
         volatile uint64_t *input_page =
            (volatile uint64_t *)((uint8_t *)sq->user_io_ptr + CS_USER_IO_INPUT_PAGE_OFFSET);
         volatile uint32_t *doorbell_page =
            (volatile uint32_t *)((uint8_t *)sq->user_io_ptr + CS_USER_IO_DOORBELL_OFFSET);

         input_page[0] = sq->insert_offset;
         __atomic_thread_fence(__ATOMIC_SEQ_CST);
         doorbell_page[0] = 1U;
         __atomic_thread_fence(__ATOMIC_SEQ_CST);

         struct kbase_ioctl_cs_queue_kick kick = {
            .buffer_gpu_addr = sq->ring_gpu_va
         };
         real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick);

         /* 4. Update signaled drm_syncobjs */
         struct drm_panthor_sync_op *sops =
            (struct drm_panthor_sync_op *)(uintptr_t)qs->syncs.array;
         for (uint32_t s = 0; s < qs->syncs.count; s++) {
            if (sops[s].flags & DRM_PANTHOR_SYNC_OP_SIGNAL) {
               struct adapter_syncobj *so = find_syncobj(sops[s].handle);
               if (so) {
                  so->value = sops[s].timeline_value ? sops[s].timeline_value : 1ULL;
                  if (so->cqs_cpu_ptr)
                     *so->cqs_cpu_ptr = so->value;
               }
            }
         }

         trace_log("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u -> KBASE_IOCTL_CS_QUEUE_KICK insert=%llu\n",
                   qs->queue_index, (unsigned long long)qs->stream_addr,
                   qs->stream_size, stats.sync_scope_upgrades,
                   stats.heap_op_nops, (unsigned long long)sq->insert_offset);
      }
      break;
   }

   default:
      break;
   }

   pthread_mutex_unlock(&g_lock);
   return ret;
}

void *panthor_kbase_adapter_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   resolve_real_funcs();
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return real_mmap(addr, length, prot, flags, fd, offset);

   pthread_mutex_lock(&g_lock);
   uint64_t uoff = (uint64_t)offset;

   /* Case 1: Mapping DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET */
   if (uoff == DRM_PANTHOR_USER_MMIO_OFFSET_64BIT ||
       uoff == DRM_PANTHOR_USER_MMIO_OFFSET_32BIT ||
       (g_ctx.user_mmio_offset && uoff == g_ctx.user_mmio_offset)) {
      void *res = g_ctx.user_reg_page;
      if (!res)
         res = real_mmap(addr, length, PROT_READ, MAP_SHARED,
                         g_ctx.mali_fd, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);
      pthread_mutex_unlock(&g_lock);
      return res ? res : MAP_FAILED;
   }

   /* Case 2: Mapping a GEM BO offset returned by DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET */
   if ((uoff & 0x1000000000ULL) == 0x1000000000ULL) {
      uint32_t handle = (uint32_t)((uoff >> 12) & 0xfffffU);
      struct adapter_bo *bo = find_bo(handle);
      if (bo) {
         if (bo->cpu_ptr) {
            void *ptr = bo->cpu_ptr;
            pthread_mutex_unlock(&g_lock);
            return ptr;
         }
         if (bo->dmabuf_fd >= 0) {
            void *ptr = real_mmap(addr, length, prot, flags, bo->dmabuf_fd, 0);
            if (ptr != MAP_FAILED) {
               bo->cpu_ptr = ptr;
               bo->cpu_size = length;
            }
            pthread_mutex_unlock(&g_lock);
            return ptr;
         }
      }
   }

   pthread_mutex_unlock(&g_lock);
   return real_mmap(addr, length, prot, flags, g_ctx.mali_fd, offset);
}

int panthor_kbase_adapter_munmap(void *addr, size_t length)
{
   resolve_real_funcs();
   pthread_mutex_lock(&g_lock);
   /*
    * Guard against the kbase SAME_VA munmap trap (mali_kbase_mem_linux.c:2805):
    * Calling munmap() on a SAME_VA BO tears down the GPU MMU page tables in kbase!
    * Keep the SAME_VA mapping pinned until DRM_IOCTL_GEM_CLOSE.
    */
   for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
      if (g_ctx.bos[i].in_use && g_ctx.bos[i].is_same_va &&
          g_ctx.bos[i].cpu_ptr == addr) {
         trace_log("ADAPTER_MUNMAP_GUARD pinned SAME_VA bo=%u addr=%p len=%zu\n",
                   g_ctx.bos[i].gem_handle, addr, length);
         pthread_mutex_unlock(&g_lock);
         return 0;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return real_munmap(addr, length);
}

/* ============================================================================
 * LD_PRELOAD & libdrm Interception Wrappers
 * ============================================================================ */

drmVersionPtr drmGetVersion(int fd)
{
   if (panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      drmVersionPtr v = calloc(1, sizeof(*v));
      v->version_major = 1;
      v->version_minor = 3;
      v->version_patchlevel = 0;
      v->name = strdup("panthor");
      v->name_len = strlen(v->name);
      v->date = strdup("20230801");
      v->date_len = strlen(v->date);
      v->desc = strdup("Panthor-to-Kbase UAPI Adapter");
      v->desc_len = strlen(v->desc);
      return v;
   }
   drmVersionPtr (*next_fn)(int) = dlsym(RTLD_NEXT, "drmGetVersion");
   return next_fn ? next_fn(fd) : NULL;
}

void drmFreeVersion(drmVersionPtr ptr)
{
   if (!ptr)
      return;
   drmVersionPtr v = ptr;
   if (v->name && strcmp(v->name, "panthor") == 0) {
      free(v->name);
      free(v->date);
      free(v->desc);
      free(v);
      return;
   }
   void (*next_fn)(drmVersionPtr) = dlsym(RTLD_NEXT, "drmFreeVersion");
   if (next_fn)
      next_fn(ptr);
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
   return panthor_kbase_adapter_ioctl(fd, request, arg);
}

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct drm_syncobj_create arg = { .flags = flags };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   return ret;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy arg = { .handle = handle };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array arg = {
      .handles = (uintptr_t)handles,
      .count_handles = handle_count,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &arg);
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled)
{
   struct drm_syncobj_wait arg = {
      .handles = (uintptr_t)handles,
      .count_handles = num_handles,
      .timeout_nsec = timeout_nsec,
      .flags = flags,
   };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &arg);
   if (ret == 0 && first_signaled)
      *first_signaled = arg.first_signaled;
   return ret;
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                           unsigned num_handles, int64_t timeout_nsec,
                           unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_timeline_wait arg = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .count_handles = num_handles,
      .timeout_nsec = timeout_nsec,
      .flags = flags,
   };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg);
   if (ret == 0 && first_signaled)
      *first_signaled = arg.first_signaled;
   return ret;
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   struct drm_syncobj_transfer arg = {
      .src_handle = src_handle,
      .dst_handle = dst_handle,
      .src_point = src_point,
      .dst_point = dst_point,
      .flags = flags,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &arg);
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   struct drm_prime_handle arg = { .handle = handle, .flags = flags, .fd = -1 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
   if (ret == 0 && prime_fd)
      *prime_fd = arg.fd;
   return ret;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   struct drm_prime_handle arg = { .fd = prime_fd };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   return ret;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)handle;
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return -1;
   pthread_mutex_lock(&g_lock);
   if (g_ctx.has_kcpu_queue) {
      struct base_fence fence = { .basep = { .fd = -1, .stream_fd = g_ctx.stream_fd } };
      struct base_kcpu_command cmd = {
         .type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL,
         .info.fence = { .fence = (uintptr_t)&fence }
      };
      struct kbase_ioctl_kcpu_queue_enqueue enq = {
         .addr = (uintptr_t)&cmd,
         .nr_commands = 1,
         .id = g_ctx.kcpu_queue_id,
      };
      real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enq);
      if (sync_file_fd)
         *sync_file_fd = (fence.basep.fd >= 0) ? fence.basep.fd : dup(g_ctx.mali_fd);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   pthread_mutex_unlock(&g_lock);
   return -1;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   (void)handle;
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return -1;
   pthread_mutex_lock(&g_lock);
   if (g_ctx.has_kcpu_queue && sync_file_fd >= 0) {
      struct base_fence fence = { .basep = { .fd = sync_file_fd, .stream_fd = g_ctx.stream_fd } };
      struct base_kcpu_command cmd = {
         .type = BASE_KCPU_COMMAND_TYPE_FENCE_WAIT,
         .info.fence = { .fence = (uintptr_t)&fence }
      };
      struct kbase_ioctl_kcpu_queue_enqueue enq = {
         .addr = (uintptr_t)&cmd,
         .nr_commands = 1,
         .id = g_ctx.kcpu_queue_id,
      };
      real_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enq);
   }
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   struct { uint64_t capability; uint64_t value; } arg = { .capability = capability, .value = 0 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_GET_CAP, &arg);
   if (ret == 0 && value)
      *value = arg.value;
   return ret;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
   (void)fd; (void)capability; (void)value;
   return 0;
}

int drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct drm_gem_close arg = { .handle = handle };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}

int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
   unsigned long req = _IOC(_IOC_WRITE, 'd', DRM_COMMAND_BASE + drmCommandIndex, size);
   return panthor_kbase_adapter_ioctl(fd, req, data);
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array arg = {
      .handles = (uintptr_t)handles,
      .count_handles = handle_count,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &arg);
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             uint64_t *points, uint32_t handle_count)
{
   struct drm_syncobj_timeline_array arg = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .count_handles = handle_count,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &arg);
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
   struct drm_syncobj_timeline_array arg = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .count_handles = handle_count,
   };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &arg);
   if (ret == 0 && points) {
      for (uint32_t i = 0; i < handle_count; i++) {
         if (points[i] == 0)
            points[i] = 1ULL;
      }
   }
   return ret;
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                     uint32_t handle_count, uint32_t flags)
{
   (void)flags;
   return drmSyncobjQuery(fd, handles, points, handle_count);
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   struct drm_syncobj_handle arg = { .handle = handle, .fd = -1 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &arg);
   if (ret == 0 && obj_fd)
      *obj_fd = arg.fd;
   return ret;
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   struct drm_syncobj_handle arg = { .fd = obj_fd, .handle = 0 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   return ret;
}
