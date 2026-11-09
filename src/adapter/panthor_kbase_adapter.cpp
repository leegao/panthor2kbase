/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor-to-Kbase Decoupled UAPI Interception & Translation Layer (C++17)
 * (`libpanthor_kbase_adapter.so` / `src/panfrost/uapi_adapter/panthor_kbase_adapter.cpp`)
 *
 * Translates upstream Mesa 26.3.0 Panthor DRM UAPI (`DRM_IOCTL_PANTHOR_*`),
 * `drm_syncobj` (`DRM_IOCTL_SYNCOBJ_*`), GEM/PRIME (`DRM_IOCTL_GEM_CLOSE`,
 * `DRM_IOCTL_PRIME_*`), and `libdrm` calls into `/dev/mali0` ARM Mali Kbase
 * CSF UAPI operations (`KBASE_IOCTL_*`, `USER_IO` mmaps, KCPU/CQS queues).
 *
 * Key Invariants Verified Against `mali_kbase_mt6878_r44.ko` (CSF v1.21, Mali-G615 MC2)
 * and `libGLES_mali.r44p1.so`:
 *  1. Exact `BASE_MEM_*` bit flags (`BASE_MEM_SAME_VA = 1<<13`,
 *     `BASE_MEM_COHERENT_LOCAL = 1<<11`, `BASE_MEM_COHERENT_SYSTEM = 1<<10`,
 *     `BASE_MEM_CACHED_CPU = 1<<12`, `BASE_MEM_CSF_EVENT = 1<<19`).
 *  2. Mutual exclusion of `BASE_MEM_CSF_EVENT` (which forces `BASE_MEM_COHERENT_SYSTEM`)
 *     and `BASE_MEM_COHERENT_LOCAL` (`(~flags & 0xc00) == 0 -> -ENOMEM` at `kbase_mem_alloc+0x90`).
 *  3. Context bring-up sequence: `VERSION_CHECK (52)` -> `SET_FLAGS (1)` ->
 *     `mmap(BASE_MEM_MAP_TRACKING_HANDLE = 3<<12)` -> `GET_GPUPROPS (3)` ->
 *     `MEM_JIT_INIT (14)` -> `MEM_EXEC_INIT (38)` -> `CS_GET_GLB_IFACE (51)` ->
 *     `mmap(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE = 47<<12)` -> `KCPU_QUEUE_CREATE (45)` ->
 *     `STREAM_CREATE (24, fd = ret)` -> `MEM_ALLOC (event_bo)`.
 *  4. Full `v11` (Mali-G615 `0xB803` / `0xB8A3`, Mali-G715 `0xB802`, Immortalis-G715 `0xB800`)
 *     support with `core_features = 4` normalization and automatic `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
 *  5. Proper virtual DRM FD table & refcounting across `dup()` / `F_DUPFD_CLOEXEC` / `close()`
 *     so logical device destruction never closes the physical device's `/dev/mali0` context.
 *  6. High-FPS CSF anti-stall fixes & comprehensive CSF ring/subqueue/syncobj diagnostic inspector:
 *     - Clear bit 0 (`Increment Fragment Completed`) on `FINISH_FRAGMENT` (0x0b) while NOPing
 *       `HEAP_OPERATION(VT_END/FRAG_END)` (0x31) so tiler heap chunks are recycled on every frame
 *       while `vt_end == 0` and `frag_end == 0` (preventing `kbase_queue_oom_event` termination).
 *     - Request `BASE_CSF_TILER_OOM_EXCEPTION_FLAG` via 40-byte `KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18`
 *       (or 112-byte layout) before 32-byte `1_6`, and default `initial_chunks >= 32`.
 *     - Proper deadline-based `DRM_IOCTL_SYNCOBJ_WAIT` (no premature 50ms fake-success return),
 *       with `BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR` decoding and full CSF state dumps.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "panthor_kbase_adapter.h"
#include "panthor_drm_capture_replay.h"
#include "log.h"

namespace {

/* Real libc / syscall pointers */
typedef int (*open_fn_t)(const char *, int, ...);
typedef int (*close_fn_t)(int);
typedef int (*ioctl_fn_t)(int, unsigned long, ...);
typedef void *(*mmap_fn_t)(void *, size_t, int, int, int, off_t);
typedef int (*munmap_fn_t)(void *, size_t);

static open_fn_t   real_open   = nullptr;
static close_fn_t  real_close  = nullptr;
static ioctl_fn_t  real_ioctl  = nullptr;
static mmap_fn_t   real_mmap   = nullptr;
static munmap_fn_t real_munmap = nullptr;

static void resolve_real_funcs()
{
   bool has_emu = (dlsym(RTLD_DEFAULT, "mali_kbase_emu_get_stats") != nullptr);
   void *handle = has_emu ? RTLD_DEFAULT : RTLD_NEXT;
   if (!real_open) {
      real_open = reinterpret_cast<open_fn_t>(dlsym(handle, "open"));
      if (!real_open)
         real_open = reinterpret_cast<open_fn_t>(dlsym(RTLD_DEFAULT, "open"));
   }
   if (!real_close) {
      real_close = reinterpret_cast<close_fn_t>(dlsym(handle, "close"));
      if (!real_close)
         real_close = reinterpret_cast<close_fn_t>(dlsym(RTLD_DEFAULT, "close"));
   }
   if (!real_ioctl) {
      real_ioctl = reinterpret_cast<ioctl_fn_t>(dlsym(handle, "ioctl"));
      if (!real_ioctl)
         real_ioctl = reinterpret_cast<ioctl_fn_t>(dlsym(RTLD_DEFAULT, "ioctl"));
   }
   if (!real_mmap) {
      real_mmap = reinterpret_cast<mmap_fn_t>(dlsym(handle, "mmap"));
      if (!real_mmap)
         real_mmap = reinterpret_cast<mmap_fn_t>(dlsym(RTLD_DEFAULT, "mmap"));
   }
   if (!real_munmap) {
      real_munmap = reinterpret_cast<munmap_fn_t>(dlsym(handle, "munmap"));
      if (!real_munmap)
         real_munmap = reinterpret_cast<munmap_fn_t>(dlsym(RTLD_DEFAULT, "munmap"));
   }
}

static uint64_t get_monotonic_ns()
{
   struct timespec ts = {0, 0};
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(ts.tv_nsec);
}

/* Optional UAPI trace logging for formal verification & replay against mali_kbase.ko */
static FILE *g_trace_fp = nullptr;
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static thread_local uint32_t g_tls_next_bo_kmod_flags = 0;

/* Matching Mesa's pan_kmod_bo_flags in src/panfrost/lib/kmod/pan_kmod.h */
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_EXECUTABLE      = (1U << 0);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_ALLOC_ON_FAULT  = (1U << 1);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_NO_MMAP         = (1U << 2);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_EXPORTED        = (1U << 3);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_IMPORTED        = (1U << 4);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_GPU_UNCACHED    = (1U << 5);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_WB_MMAP         = (1U << 6);
[[maybe_unused]] constexpr uint32_t ADAPTER_KMOD_BO_FLAG_CSF_EVENT       = (1U << 7);

static void trace_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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
   uint32_t kmod_flags;
   uint64_t kbase_flags;
   uint64_t kbase_cookie_or_va;
   uint64_t gpu_va;
   void *cpu_ptr;
   size_t cpu_size;
   void *gpu_mapping;
   size_t gpu_mapping_size;
   void *alias_ptr;
   size_t alias_size;
   uint64_t alias_gpu_va;
   int dmabuf_fd;
   uint64_t dmabuf_ino;
   bool is_imported;
   bool is_same_va;
   bool is_executable;
   bool is_csf_event;
   char label[64];
};

/* Internal state for an emulated DRM Syncobj (backed by CQS 64-bit timeline in CSF_EVENT mem) */
struct adapter_syncobj {
   bool in_use;
   uint32_t handle;
   uint64_t value;
   uint64_t completed_value;
   uint64_t cqs_gpu_va;
   uint64_t *cqs_cpu_ptr;
   uint32_t target_group_handle;
   uint64_t target_subq_seqno[ADAPTER_MAX_SUBQUEUES];
   uint32_t last_target_group_handle;
   uint64_t last_target_subq_seqno[ADAPTER_MAX_SUBQUEUES];
};

static inline void kbase_cpu_cache_clean_range(const void *ptr, size_t size)
{
   if (!ptr || size == 0)
      return;
#if defined(__aarch64__)
   uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(63);
   uintptr_t end   = (reinterpret_cast<uintptr_t>(ptr) + size + 63) & ~static_cast<uintptr_t>(63);
   for (uintptr_t line = start; line < end; line += 64)
      __asm__ volatile("dc cvac, %0" : : "r"(line) : "memory");
   __asm__ volatile("dsb sy" : : : "memory");
#else
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static inline void kbase_cpu_cache_invalidate_range(const void *ptr, size_t size)
{
   if (!ptr || size == 0)
      return;
#if defined(__aarch64__)
   uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(63);
   uintptr_t end   = (reinterpret_cast<uintptr_t>(ptr) + size + 63) & ~static_cast<uintptr_t>(63);
   for (uintptr_t line = start; line < end; line += 64)
      __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
   __asm__ volatile("dsb sy" : : : "memory");
#else
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* Subqueue state inside a CSG */
struct adapter_flight_entry {
   uint64_t job_seqno;
   uint32_t ring_off;
   uint32_t slot_bytes;
   uint32_t wrap_nop_bytes;
   uint64_t insert_before;
   uint64_t insert_after;
   uint64_t extract_at_submit;
   uint64_t stream_va;
   uint32_t stream_size;
   uint64_t submit_ts_ns;
};

struct adapter_subqueue {
   bool bound;
   uint8_t kbase_group_handle;
   uint32_t csi_index;
   uint32_t ringbuf_size;
   uint32_t ring_alloc_size;
   uint64_t ring_gpu_va;
   uint64_t *ring_cpu_ptr;
   void *user_io_ptr; /* 3 * 4096 bytes: page 0 Doorbell, page 1 Input, page 2 Output */
   uint64_t insert_offset;
   uint64_t emitted_jobs;
   uint64_t ring_wraps;
   uint64_t seqno_gpu_va;
   volatile uint64_t *seqno_cpu_ptr;
   uint64_t diag_gpu_va;
   volatile uint64_t *diag_cpu_ptr;
   uint64_t cached_subq_ctx_va;
   uint8_t cached_subq_ctx_reg;
   uint64_t last_stream_va;
   uint32_t last_stream_size;
   uint32_t last_ring_slot_offset;
   uint32_t last_ring_slot_bytes;
   uint64_t job_stream_va[64];
   uint32_t job_stream_size[64];
   struct adapter_flight_entry flight_log[ADAPTER_FLIGHT_LOG_ENTRIES];
   /* 32-way per-subqueue compiler-style CS chunk hash cache (full-chunk 64-bit FNV-1a digest
    * + outgoing CALL/JUMP CFG edges for transitive closure traversal) */
   struct {
      uint64_t va;
      uint32_t sz;
      uint64_t post_norm_fnv1a64;
      uint32_t edge_count;
      struct {
         uint64_t target_va;
         uint32_t target_sz;
         uint8_t  is_call;
      } edges[8];
   } norm_cache[32];
   uint32_t norm_cache_hand;
};

/* Scheduling group state */
struct adapter_group {
   bool in_use;
   uint32_t group_handle;
   uint8_t kbase_group_handle;
   uint8_t csi_handlers;
   uint32_t group_create_layout;
   bool single_csg_mode;
   uint32_t vm_id;
   uint32_t queue_count;
   uint32_t fatal_state;
   uint32_t fatal_queues;
   bool dumped_fatal_diag;
   struct adapter_subqueue subqueues[ADAPTER_MAX_SUBQUEUES];
   uint64_t active_tiler_heap_ctx_va;
   uint64_t redirected_syncobjs_va;
   uint64_t redirected_ringbuf_sync_va;
};

/* Tiler heap state */
struct adapter_heap {
   bool in_use;
   uint32_t handle;
   uint32_t vm_id;
   uint64_t gpu_heap_va;
   uint64_t first_chunk_va;
   uint32_t chunk_size;
   uint32_t initial_chunks;
   uint32_t max_chunks;
};

/* Per-context adapter state (maps virtual Panthor DRM fds to a /dev/mali0 context) */
struct adapter_ctx {
   bool active;
   int drm_fd;
   int mali_fd;
   dev_t mali_dev;
   ino_t mali_ino;
   dev_t mali_rdev;
   int virtual_fds[ADAPTER_MAX_VIRTUAL_FDS];
   int open_refcount;
   uint16_t kbase_ver_major;
   uint16_t kbase_ver_minor;
   uint32_t arch_major;
   struct drm_panthor_gpu_info gpu_info;
   struct drm_panthor_csif_info csif_info;
   struct drm_panthor_mmu_info mmu_info;
   uint64_t user_mmio_offset;
   void *tracking_page;
   void *user_reg_page;
   uint8_t kcpu_queue_id;
   bool has_kcpu_queue;
   int stream_fd;

   /* CSF Event BO (BASE_MEM_CSF_EVENT) backing ONLY CQS/SYNC_ADD/SYNC_WAIT cells */
   uint64_t event_bo_gpu_va;
   uint8_t *event_bo_cpu;
   size_t event_bo_size;

   /* Coherent Local Diagnostic BO (BASE_MEM_COHERENT_LOCAL) backing STORE_MULTIPLE breadcrumbs */
   uint64_t diag_bo_gpu_va;
   uint8_t *diag_bo_cpu;
   size_t diag_bo_size;

   /* 16 MiB Shadow CS Ring BO (BASE_MEM_COHERENT_LOCAL) for immutable in-flight CS snapshots */
   uint64_t shadow_cs_bo_gpu_va;
   uint8_t *shadow_cs_bo_cpu;
   size_t shadow_cs_bo_size;
   size_t shadow_cs_write_off;

   uint32_t next_vm_id;
   uint32_t next_gem_handle;
   uint32_t next_syncobj_handle;
   uint32_t next_group_handle;
   uint32_t next_heap_handle;

   uint32_t max_bo_slot_used;
   uint32_t bo_alloc_cursor;
   uint32_t max_syncobj_slot_used;
   uint32_t syncobj_alloc_cursor;

   /* Cumulative & Windowed Telemetry Counters */
   uint64_t total_group_submits;
   uint64_t munmap_guard_hits;
   uint64_t total_syncobj_waits;
   uint64_t fast_spin_syncobj_waits;
   uint64_t poll_sleep_syncobj_waits;
   uint64_t window_start_ns;
   uint64_t window_submits;
   uint64_t window_submit_ns_sum;
   uint64_t window_submit_ns_max;
   uint64_t window_cs_norm_ns_sum;
   uint64_t window_cache_clean_ns_sum;
   uint64_t window_kick_ns_sum;
   uint64_t window_sync_waits;
   uint64_t window_sync_wait_ns_sum;
   uint64_t window_sync_wait_ns_max;
   double   last_window_fps;
   double   last_avg_submit_us;
   double   last_max_submit_us;
   double   last_avg_cs_norm_us;
   double   last_avg_cache_clean_us;
   double   last_avg_kick_us;
   double   last_avg_sync_wait_us;
   double   last_max_sync_wait_us;

   struct adapter_bo bos[ADAPTER_MAX_BOS];
   struct adapter_syncobj syncobjs[ADAPTER_MAX_SYNCOBJS];
   struct adapter_group groups[ADAPTER_MAX_GROUPS];
   struct adapter_heap heaps[ADAPTER_MAX_HEAPS];
};

static struct adapter_ctx g_ctx;

static bool is_csf_breadcrumbs_enabled()
{
   static int cached = -1;
   if (cached < 0) {
      const char *env = getenv("PANTHOR_ADAPTER_CSF_BREADCRUMBS");
      cached = (!env || !env[0] || atoi(env) != 0) ? 1 : 0;
   }
   return cached != 0;
}

static bool is_single_csg_enabled(uint32_t arch_major)
{
   const char *env = getenv("PANTHOR_ADAPTER_SINGLE_CSG");
   if (env && env[0])
      return atoi(env) != 0;
   const char *ir_env = getenv("PANVK_KBASE_TILER_IR");
   if (ir_env && ir_env[0])
      return atoi(ir_env) != 0;
   /*
    * Default to 1-CSG incremental rendering on PAN_ARCH 11..13 (Mali-G615/G715/G720/G925),
    * matching panvk-kbase-android patches 0161, 0167, 0174:
    *  - All 3 subqueues (VT=0, FRAG=1, COMPUTE=2) share one CSG with
    *    BASE_CSF_TILER_OOM_EXCEPTION_FLAG so hardware MALI_CS_SYNC_SCOPE_CSG
    *    and the FRAG TILER_OOM exception handler work seamlessly.
    */
   return (arch_major >= 11 && arch_major <= 13);
}

static void register_virtual_fd_locked(int fd)
{
   if (fd < 0)
      return;
   for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++) {
      if (g_ctx.virtual_fds[i] == fd)
         return;
   }
   for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++) {
      if (g_ctx.virtual_fds[i] < 0) {
         g_ctx.virtual_fds[i] = fd;
         g_ctx.open_refcount++;
         return;
      }
   }
}

static bool unregister_virtual_fd_locked(int fd)
{
   if (fd < 0)
      return false;
   for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++) {
      if (g_ctx.virtual_fds[i] == fd) {
         g_ctx.virtual_fds[i] = -1;
         if (g_ctx.open_refcount > 0)
            g_ctx.open_refcount--;
         return true;
      }
   }
   return false;
}

static struct adapter_bo *find_bo(uint32_t handle)
{
   if (handle == 0)
      return nullptr;
   size_t direct_idx = static_cast<size_t>((handle - 1U) % ADAPTER_MAX_BOS);
   if (g_ctx.bos[direct_idx].in_use && g_ctx.bos[direct_idx].gem_handle == handle)
      return &g_ctx.bos[direct_idx];
   size_t limit = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
   if (limit > ADAPTER_MAX_BOS)
      limit = ADAPTER_MAX_BOS;
   for (size_t i = 0; i < limit; i++) {
      if (g_ctx.bos[i].in_use && g_ctx.bos[i].gem_handle == handle)
         return &g_ctx.bos[i];
   }
   return nullptr;
}

static struct adapter_bo *find_bo_by_va(uint64_t va)
{
   if (va == 0)
      return nullptr;
   size_t limit = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
   if (limit > ADAPTER_MAX_BOS)
      limit = ADAPTER_MAX_BOS;
   static size_t s_mru_bo_idx = 0;
   size_t mru = __atomic_load_n(&s_mru_bo_idx, __ATOMIC_RELAXED);
   if (mru < limit && g_ctx.bos[mru].in_use) {
      if (g_ctx.bos[mru].gpu_va != 0 &&
          va >= g_ctx.bos[mru].gpu_va &&
          va < g_ctx.bos[mru].gpu_va + g_ctx.bos[mru].size)
         return &g_ctx.bos[mru];
      if (g_ctx.bos[mru].alias_gpu_va != 0 &&
          va >= g_ctx.bos[mru].alias_gpu_va &&
          va < g_ctx.bos[mru].alias_gpu_va + g_ctx.bos[mru].alias_size)
         return &g_ctx.bos[mru];
   }
   for (size_t i = 0; i < limit; i++) {
      if (g_ctx.bos[i].in_use) {
         if (g_ctx.bos[i].gpu_va != 0 &&
             va >= g_ctx.bos[i].gpu_va &&
             va < g_ctx.bos[i].gpu_va + g_ctx.bos[i].size) {
            __atomic_store_n(&s_mru_bo_idx, i, __ATOMIC_RELAXED);
            return &g_ctx.bos[i];
         }
         if (g_ctx.bos[i].alias_gpu_va != 0 &&
             va >= g_ctx.bos[i].alias_gpu_va &&
             va < g_ctx.bos[i].alias_gpu_va + g_ctx.bos[i].alias_size) {
            __atomic_store_n(&s_mru_bo_idx, i, __ATOMIC_RELAXED);
            return &g_ctx.bos[i];
         }
      }
   }
   return nullptr;
}

static void *resolve_gpu_va_to_cpu(uint64_t va, size_t min_bytes)
{
   if (va == 0)
      return nullptr;
   if (g_ctx.event_bo_cpu &&
       va >= g_ctx.event_bo_gpu_va &&
       va + min_bytes <= g_ctx.event_bo_gpu_va + g_ctx.event_bo_size) {
      return g_ctx.event_bo_cpu + (va - g_ctx.event_bo_gpu_va);
   }
   if (g_ctx.diag_bo_cpu &&
       va >= g_ctx.diag_bo_gpu_va &&
       va + min_bytes <= g_ctx.diag_bo_gpu_va + g_ctx.diag_bo_size) {
      return g_ctx.diag_bo_cpu + (va - g_ctx.diag_bo_gpu_va);
   }
   if (g_ctx.shadow_cs_bo_cpu &&
       va >= g_ctx.shadow_cs_bo_gpu_va &&
       va + min_bytes <= g_ctx.shadow_cs_bo_gpu_va + g_ctx.shadow_cs_bo_size) {
      return g_ctx.shadow_cs_bo_cpu + (va - g_ctx.shadow_cs_bo_gpu_va);
   }
   struct adapter_bo *bo = find_bo_by_va(va);
   if (bo && bo->cpu_ptr && bo->cpu_ptr != MAP_FAILED) {
      uint64_t base = (bo->alias_gpu_va && va >= bo->alias_gpu_va &&
                       va < bo->alias_gpu_va + bo->alias_size)
                         ? bo->alias_gpu_va
                         : bo->gpu_va;
      uint64_t off = (va - base) % bo->size;
      if (off + min_bytes <= bo->size)
         return reinterpret_cast<uint8_t *>(bo->cpu_ptr) + off;
   }
   return nullptr;
}

static struct adapter_syncobj *find_syncobj(uint32_t handle)
{
   if (handle == 0)
      return nullptr;
   size_t direct_idx = static_cast<size_t>((handle - 1U) % ADAPTER_MAX_SYNCOBJS);
   if (g_ctx.syncobjs[direct_idx].in_use && g_ctx.syncobjs[direct_idx].handle == handle)
      return &g_ctx.syncobjs[direct_idx];
   size_t limit = g_ctx.max_syncobj_slot_used ? g_ctx.max_syncobj_slot_used : ADAPTER_MAX_SYNCOBJS;
   if (limit > ADAPTER_MAX_SYNCOBJS)
      limit = ADAPTER_MAX_SYNCOBJS;
   for (size_t i = 0; i < limit; i++) {
      if (g_ctx.syncobjs[i].in_use && g_ctx.syncobjs[i].handle == handle)
         return &g_ctx.syncobjs[i];
   }
   return nullptr;
}

static struct adapter_group *find_group(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_GROUPS; i++) {
      if (g_ctx.groups[i].in_use && g_ctx.groups[i].group_handle == handle)
         return &g_ctx.groups[i];
   }
   return nullptr;
}

static int alloc_kbase_mem(int mali_fd,
                           uint64_t va_pages,
                           uint64_t commit_pages,
                           uint64_t extension,
                           uint64_t in_flags,
                           uint64_t *out_flags,
                           uint64_t *out_gpu_va)
{
   /* Never combine BASE_MEM_CSF_EVENT (which forces BASE_MEM_COHERENT_SYSTEM)
    * with BASE_MEM_COHERENT_LOCAL, as mali_kbase.ko rejects COHERENT_SYSTEM | COHERENT_LOCAL. */
   if (in_flags & BASE_MEM_CSF_EVENT)
      in_flags &= ~BASE_MEM_COHERENT_LOCAL;

   /* Strip any kernel-only bits (0xa0) defensively */
   in_flags &= ~BASEP_MEM_FLAGS_KERNEL_ONLY;

   if (g_ctx.kbase_ver_major > 1 ||
       (g_ctx.kbase_ver_major == 1 && g_ctx.kbase_ver_minor >= 9)) {
      union kbase_ioctl_mem_alloc_ex req_ex;
      memset(&req_ex, 0, sizeof(req_ex));
      req_ex.in.va_pages = va_pages;
      req_ex.in.commit_pages = commit_pages;
      req_ex.in.extension = extension;
      req_ex.in.flags = in_flags;
      req_ex.in.fixed_address = 0;
      if (adapter_ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC_EX, &req_ex) == 0) {
         if (out_flags)
            *out_flags = req_ex.out.flags;
         if (out_gpu_va)
            *out_gpu_va = req_ex.out.gpu_va;
         return 0;
      }
   }

   union kbase_ioctl_mem_alloc req;
   memset(&req, 0, sizeof(req));
   req.in.va_pages = va_pages;
   req.in.commit_pages = commit_pages;
   req.in.extension = extension;
   req.in.flags = in_flags;
   if (adapter_ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &req) < 0)
      return -1;
   if (out_flags)
      *out_flags = req.out.flags;
   if (out_gpu_va)
      *out_gpu_va = req.out.gpu_va;
   return 0;
}

/* Parse TLV stream from KBASE_IOCTL_GET_GPUPROPS into drm_panthor_gpu_info */
static void parse_kbase_gpuprops(struct adapter_ctx *ctx, const uint8_t *buf, size_t len)
{
   PANTHOR2KBASE_LOG_SCOPE("parse_kbase_gpuprops", "len=%zu", len);
   size_t off = 0;
   uint64_t raw_gpu_id = 0;
   uint32_t prop_product_id = 0;
   uint32_t prop_major_rev = 0;
   uint32_t prop_minor_rev = 0;
   uint32_t prop_status = 0;

   /* Sensible defaults for Valhall CSF (Mali-G615 / G710) if a prop is omitted */
   ctx->gpu_info.gpu_id = (10U << 28) | (8U << 24) | (7U << 16);
   ctx->gpu_info.csf_id = (1U << 26) | (1U << 20);
   ctx->gpu_info.mmu_features = 0x2830U; /* 48-bit VA (0x30), 40-bit PA (0x28) */
   ctx->gpu_info.tiler_features = 0x809U;
   ctx->gpu_info.max_threads = 2048U;
   ctx->gpu_info.thread_max_workgroup_size = 1024U;
   ctx->gpu_info.thread_max_barrier_size = 1024U;
   ctx->gpu_info.shader_present = 0x3fULL;
   ctx->gpu_info.l2_present = 0x1ULL;
   ctx->gpu_info.tiler_present = 0x1ULL;

   while (buf && off + 4 <= len) {
      uint32_t hdr = 0;
      memcpy(&hdr, buf + off, 4);
      off += 4;
      uint32_t key = hdr >> 2;
      uint32_t sz_code = hdr & 0x3U;
      size_t val_sz = static_cast<size_t>(1U) << sz_code;
      if (off + val_sz > len)
         break;
      uint64_t val = 0;
      memcpy(&val, buf + off, val_sz);
      off += val_sz;

      switch (key) {
      case KBASE_GPUPROP_PRODUCT_ID:
         prop_product_id = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_VERSION_STATUS:
         prop_status = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_MINOR_REVISION:
         prop_minor_rev = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_MAJOR_REVISION:
         prop_major_rev = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_GPU_ID:
         raw_gpu_id = val;
         if (val > 0xffffffffULL) {
            /* 64-bit Valhall v11+ RAW_GPU_ID -> 32-bit Panthor gpu_id encoding */
            uint32_t arch_major = (val >> 56) & 0xffU;
            uint32_t arch_minor = (val >> 48) & 0xfU;
            uint32_t arch_rev   = (val >> 40) & 0xfU;
            uint32_t prod_major = (val >> 32) & 0xfU;
            uint32_t ver_major  = (val >> 16) & 0xfU;
            uint32_t ver_minor  = (val >> 8)  & 0xffU;
            uint32_t ver_status = val & 0xfU;
            ctx->gpu_info.gpu_id = (arch_major << 28) | (arch_minor << 24) |
                                   (arch_rev << 20)   | (prod_major << 16) |
                                   (ver_major << 12)  | (ver_minor << 4)   | ver_status;
         } else {
            ctx->gpu_info.gpu_id = static_cast<uint32_t>(val);
         }
         break;
      case KBASE_GPUPROP_RAW_L2_FEATURES:
         ctx->gpu_info.l2_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_TILER_FEATURES:
         ctx->gpu_info.tiler_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_MEM_FEATURES:
         ctx->gpu_info.mem_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_MMU_FEATURES:
         ctx->gpu_info.mmu_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_THREADS:
      case KBASE_GPUPROP_MAX_THREADS:
         ctx->gpu_info.max_threads = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE:
      case KBASE_GPUPROP_MAX_WORKGROUP_SIZE:
         ctx->gpu_info.thread_max_workgroup_size = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE:
      case KBASE_GPUPROP_MAX_BARRIER_SIZE:
         ctx->gpu_info.thread_max_barrier_size = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_THREAD_FEATURES:
         ctx->gpu_info.thread_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0:
      case KBASE_GPUPROP_TEXTURE_FEATURES_0:
         ctx->gpu_info.texture_features[0] = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1:
      case KBASE_GPUPROP_TEXTURE_FEATURES_1:
         ctx->gpu_info.texture_features[1] = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2:
      case KBASE_GPUPROP_TEXTURE_FEATURES_2:
         ctx->gpu_info.texture_features[2] = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3:
      case KBASE_GPUPROP_TEXTURE_FEATURES_3:
         ctx->gpu_info.texture_features[3] = static_cast<uint32_t>(val);
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
         ctx->gpu_info.core_features = static_cast<uint32_t>(val);
         break;
      case KBASE_GPUPROP_RAW_GPU_FEATURES:
         ctx->gpu_info.gpu_features = val;
         break;
      default:
         break;
      }
   }

   if (!raw_gpu_id && prop_product_id) {
      ctx->gpu_info.gpu_id = ((prop_product_id & 0xffffU) << 16) |
                             ((prop_major_rev & 0xfU) << 12) |
                             ((prop_minor_rev & 0xffU) << 4) |
                             (prop_status & 0xfU);
   }

   ctx->arch_major = DRM_PANTHOR_ARCH_MAJOR(ctx->gpu_info.gpu_id);
   if (ctx->arch_major < 10)
      ctx->arch_major = 10;

   if (ctx->arch_major == 11) {
      setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
   setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "0", 0);
      uint32_t prod_major = DRM_PANTHOR_PRODUCT_MAJOR(ctx->gpu_info.gpu_id);
      if (prod_major != 2 && prod_major != 3) {
         uint32_t mapped_prod = (prod_major == 0) ? 2U : 3U;
         ctx->gpu_info.gpu_id = (11U << 28) | (8U << 24) |
                                (mapped_prod << 16) |
                                (ctx->gpu_info.gpu_id & 0xffffU);
      }
      if ((ctx->gpu_info.core_features & 0xffU) != 4U) {
         ctx->gpu_info.core_features = (ctx->gpu_info.core_features & ~0xffU) | 4U;
      }
   }

   LOG_INFO("parse_kbase_gpuprops: raw_gpu_id=0x%llx -> panthor_gpu_id=0x%x (arch=%u) core_features=0x%x shader_present=0x%llx mmu_features=0x%x",
            static_cast<unsigned long long>(raw_gpu_id),
            ctx->gpu_info.gpu_id, ctx->arch_major,
            ctx->gpu_info.core_features,
            static_cast<unsigned long long>(ctx->gpu_info.shader_present),
            ctx->gpu_info.mmu_features);
}

/* Initialize a fresh /dev/mali0 context following strict mali_kbase.ko ordering rules */
static int init_kbase_context(struct adapter_ctx *ctx, int mali_fd)
{
   PANTHOR2KBASE_LOG_SCOPE("init_kbase_context", "mali_fd=%d", mali_fd);
   resolve_real_funcs();
   ctx->mali_fd = mali_fd;
   ctx->stream_fd = -1;

   setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
   setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "0", 0);

   /* Step 1: KBASE_IOCTL_VERSION_CHECK (nr 52) — MUST be first ioctl on fd */
   struct kbase_ioctl_version_check ver = { 0, 0 };
   if (adapter_ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0)
      return -1;
   ctx->kbase_ver_major = ver.major;
   ctx->kbase_ver_minor = ver.minor;

   /* Step 2: KBASE_IOCTL_SET_FLAGS (nr 1) — MUST follow VERSION_CHECK */
   struct kbase_ioctl_set_flags flags = { .create_flags = BASE_CONTEXT_CREATE_FLAG_NONE };
   if (adapter_ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &flags) < 0)
      return -1;

   /* Step 3: Map BASE_MEM_MAP_TRACKING_HANDLE (3ULL << 12) — required before any BO alloc */
   ctx->tracking_page = real_mmap(nullptr, 4096, PROT_NONE, MAP_SHARED,
                                  mali_fd, static_cast<off_t>(BASE_MEM_MAP_TRACKING_HANDLE));
   if (ctx->tracking_page == MAP_FAILED) {
      LOG_WARN("init_kbase_context: mmap(BASE_MEM_MAP_TRACKING_HANDLE) returned MAP_FAILED (errno=%d)", errno);
      ctx->tracking_page = nullptr;
   }

   /* Step 4: Two-pass KBASE_IOCTL_GET_GPUPROPS (nr 3) */
   struct kbase_ioctl_get_gpuprops gp = { 0, 0, 0 };
   int blob_len = adapter_ioctl(mali_fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
   if (blob_len > 0) {
      uint8_t *blob = static_cast<uint8_t *>(calloc(1, static_cast<size_t>(blob_len)));
      if (blob) {
         gp.buffer = reinterpret_cast<uintptr_t>(blob);
         gp.size = static_cast<uint32_t>(blob_len);
         if (adapter_ioctl(mali_fd, KBASE_IOCTL_GET_GPUPROPS, &gp) >= 0)
            parse_kbase_gpuprops(ctx, blob, static_cast<size_t>(blob_len));
         free(blob);
      }
   } else {
      parse_kbase_gpuprops(ctx, nullptr, 0);
   }

   /*
    * Step 5: KBASE_IOCTL_MEM_JIT_INIT (nr 14) — MUST run BEFORE any KBASE_IOCTL_MEM_ALLOC!
    */
   struct kbase_ioctl_mem_jit_init jit_init;
   memset(&jit_init, 0, sizeof(jit_init));
   jit_init.va_pages = 1ULL << 25;
   jit_init.max_allocations = 255;
   jit_init.trim_level = 0;
   jit_init.group_id = 0;
   jit_init.phys_pages = 1ULL << 25;
   adapter_ioctl(mali_fd, KBASE_IOCTL_MEM_JIT_INIT, &jit_init);

   /* Step 6: KBASE_IOCTL_MEM_EXEC_INIT (nr 38) — precede GPU_EX BO allocs */
   struct kbase_ioctl_mem_exec_init exec_init = { .va_pages = 0x100000ULL };
   adapter_ioctl(mali_fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec_init);

   /* Step 7: Two-pass KBASE_IOCTL_CS_GET_GLB_IFACE (nr 51) */
   union kbase_ioctl_cs_get_glb_iface glb;
   memset(&glb, 0, sizeof(glb));
   if (adapter_ioctl(mali_fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb) >= 0) {
      uint32_t group_num = glb.out.group_num ? glb.out.group_num : 8U;
      uint32_t total_stream_num = glb.out.total_stream_num ? glb.out.total_stream_num : 24U;
      uint32_t cs_slot_count = 8U;
      uint32_t scoreboard_slot_count = 8U;
      uint32_t cs_reg_count = (ctx->arch_major >= 12) ? 128U : 96U;

      struct basep_cs_group_control *groups =
         static_cast<struct basep_cs_group_control *>(calloc(group_num, sizeof(*groups)));
      struct basep_cs_stream_control *streams =
         static_cast<struct basep_cs_stream_control *>(calloc(total_stream_num, sizeof(*streams)));
      if (groups && streams) {
         union kbase_ioctl_cs_get_glb_iface glb_fill;
         memset(&glb_fill, 0, sizeof(glb_fill));
         glb_fill.in.max_group_num = group_num;
         glb_fill.in.max_total_stream_num = total_stream_num;
         glb_fill.in.groups_ptr = reinterpret_cast<uintptr_t>(groups);
         glb_fill.in.streams_ptr = reinterpret_cast<uintptr_t>(streams);
         if (adapter_ioctl(mali_fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &glb_fill) >= 0) {
            if (groups[0].stream_num >= 3 && groups[0].stream_num <= 16)
               cs_slot_count = groups[0].stream_num;
            uint32_t sb = (streams[0].features >> 8) & 0xffU;
            if (sb >= 4 && sb <= 16)
               scoreboard_slot_count = sb;
         }
      }
      free(groups);
      free(streams);

      ctx->csif_info.csg_slot_count = group_num;
      ctx->csif_info.cs_slot_count = cs_slot_count;
      ctx->csif_info.cs_reg_count = cs_reg_count;
      ctx->csif_info.scoreboard_slot_count = scoreboard_slot_count;
      ctx->csif_info.unpreserved_cs_reg_count = 4U;
   } else {
      ctx->csif_info.csg_slot_count = 8U;
      ctx->csif_info.cs_slot_count = 8U;
      ctx->csif_info.cs_reg_count = (ctx->arch_major >= 12) ? 128U : 96U;
      ctx->csif_info.scoreboard_slot_count = 8U;
      ctx->csif_info.unpreserved_cs_reg_count = 4U;
   }

   /* Step 8: Map CSF USER_REG page (cookie 47UL << 12) for LATEST_FLUSH reads */
   ctx->user_reg_page = real_mmap(nullptr, 4096, PROT_READ, MAP_SHARED,
                                  mali_fd, static_cast<off_t>(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE));
   if (ctx->user_reg_page == MAP_FAILED)
      ctx->user_reg_page = nullptr;

   /* Step 9: Create KCPU queue & stream fd for CQS/sync_file operations */
   struct kbase_ioctl_kcpu_queue_new kcpu;
   memset(&kcpu, 0, sizeof(kcpu));
   if (adapter_ioctl(mali_fd, KBASE_IOCTL_KCPU_QUEUE_CREATE, &kcpu) >= 0) {
      ctx->kcpu_queue_id = kcpu.id;
      ctx->has_kcpu_queue = true;
   }
   union kbase_ioctl_stream_create_u strm;
   memset(&strm, 0, sizeof(strm));
   snprintf(strm.in.name, sizeof(strm.in.name), "panvk-kbase-sync");
   int strm_ret = adapter_ioctl(mali_fd, KBASE_IOCTL_STREAM_CREATE, &strm);
   if (strm_ret >= 0) {
      ctx->stream_fd = (strm_ret > 0) ? strm_ret : strm.out.fd;
   }

   LOG_INFO("init_kbase_context: arch=%u kbase_ver=%u.%u cs_regs=%u sb_slots=%u has_kcpu=%d stream_fd=%d",
            ctx->arch_major, ctx->kbase_ver_major, ctx->kbase_ver_minor,
            ctx->csif_info.cs_reg_count, ctx->csif_info.scoreboard_slot_count,
            ctx->has_kcpu_queue, ctx->stream_fd);

   /*
    * Step 10: Allocate CSF_EVENT BO for CQS syncobjs & subqueue 64-byte breadcrumb cells.
    */
   uint64_t ev_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                       BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                       BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT;
   uint64_t ev_out_flags = 0;
   uint64_t ev_cookie = 0;
   if (alloc_kbase_mem(mali_fd, 32, 32, 0, ev_flags, &ev_out_flags, &ev_cookie) == 0) {
      ctx->event_bo_size = 32 * 4096;
      ctx->event_bo_cpu = static_cast<uint8_t *>(
         real_mmap(nullptr, ctx->event_bo_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, mali_fd, static_cast<off_t>(ev_cookie)));
      if (ctx->event_bo_cpu != MAP_FAILED) {
         ctx->event_bo_gpu_va = reinterpret_cast<uintptr_t>(ctx->event_bo_cpu);
         memset(ctx->event_bo_cpu, 0, ctx->event_bo_size);
         kbase_cpu_cache_clean_range(ctx->event_bo_cpu, ctx->event_bo_size);
      } else {
         ctx->event_bo_cpu = nullptr;
      }
   }

   /*
    * Step 10b: Allocate separate BASE_MEM_COHERENT_LOCAL diag_bo for STORE_MULTIPLE
    * breadcrumbs so LS stores never touch the BASE_MEM_CSF_EVENT cache lines in event_bo!
    */
   uint64_t diag_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                         BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                         BASE_MEM_SAME_VA | BASE_MEM_COHERENT_LOCAL;
   uint64_t diag_out_flags = 0;
   uint64_t diag_cookie = 0;
   if (alloc_kbase_mem(mali_fd, 4, 4, 0, diag_flags, &diag_out_flags, &diag_cookie) == 0) {
      ctx->diag_bo_size = 4 * 4096;
      ctx->diag_bo_cpu = static_cast<uint8_t *>(
         real_mmap(nullptr, ctx->diag_bo_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, mali_fd, static_cast<off_t>(diag_cookie)));
      if (ctx->diag_bo_cpu != MAP_FAILED) {
         ctx->diag_bo_gpu_va = reinterpret_cast<uintptr_t>(ctx->diag_bo_cpu);
         memset(ctx->diag_bo_cpu, 0, ctx->diag_bo_size);
         kbase_cpu_cache_clean_range(ctx->diag_bo_cpu, ctx->diag_bo_size);
      } else {
         ctx->diag_bo_cpu = nullptr;
      }
   }

   /* Step 10c: Allocate 8 MiB (2048 pages) SAME_VA Shadow CS Ring BO so dynamic command
    * streams cannot be overwritten in-place while in flight on the GPU. */
   const char *sh_env = getenv("PANTHOR_ADAPTER_SHADOW_CS");
   bool enable_shadow_cs = (!sh_env || !sh_env[0] || atoi(sh_env) != 0);
   if (enable_shadow_cs) {
      uint64_t sh_pages = 2048ULL; /* 8 MiB */
      uint64_t sh_out_flags = 0;
      uint64_t sh_cookie = 0;
      if (alloc_kbase_mem(mali_fd, sh_pages, sh_pages, 0, diag_flags, &sh_out_flags, &sh_cookie) == 0) {
         ctx->shadow_cs_bo_size = static_cast<size_t>(sh_pages * 4096ULL);
         ctx->shadow_cs_bo_cpu = static_cast<uint8_t *>(
            real_mmap(nullptr, ctx->shadow_cs_bo_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, mali_fd, static_cast<off_t>(sh_cookie)));
         if (ctx->shadow_cs_bo_cpu != MAP_FAILED) {
            ctx->shadow_cs_bo_gpu_va = reinterpret_cast<uintptr_t>(ctx->shadow_cs_bo_cpu);
            ctx->shadow_cs_write_off = 0;
         } else {
            ctx->shadow_cs_bo_cpu = nullptr;
            ctx->shadow_cs_bo_size = 0;
         }
      }
   }

   ctx->mmu_info.page_size_bitmap = 0x1000ULL; /* 4KB base page size for SAME_VA */
   ctx->next_vm_id = 1;
   ctx->next_gem_handle = 1;
   ctx->next_syncobj_handle = 1;
   ctx->next_group_handle = 1;
   ctx->next_heap_handle = 1;
   ctx->active = true;
   return 0;
}

static const char *cs_fault_exception_name(uint8_t exc)
{
   switch (exc) {
   case 0x00: return "NONE";
   case 0x05: return "KABOOM";
   case 0x0f: return "CS_RESOURCE_TERMINATED";
   case 0x48: return "CS_BUS_FAULT";
   case 0x49: return "CS_INVALID_INSTRUCTION";
   case 0x4a: return "CS_CALL_STACK_OVERFLOW";
   case 0x4b: return "CS_INHERIT_FAULT";
   case 0x50: return "INSTR_INVALID_PC";
   case 0x51: return "INSTR_INVALID_ENC";
   case 0x55: return "INSTR_BARRIER_FAULT";
   case 0x58: return "DATA_INVALID_FAULT";
   case 0x59: return "TILE_RANGE_FAULT";
   case 0x5a: return "ADDR_RANGE_FAULT";
   case 0x5b: return "IMPRECISE_FAULT";
   case 0x60: return "OUT_OF_MEMORY (TILER_OOM)";
   case 0x69: return "RESOURCE_EVICTION_TIMEOUT";
   case 0x80: return "GPU_BUS_FAULT";
   case 0x88: return "GPU_SHAREABILITY_FAULT";
   case 0x89: return "SYSTEM_SHAREABILITY_FAULT";
   case 0x8a: return "GPU_CACHE_WB_BUS_FAULT";
   case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4:
      return "MMU_TRANSLATION_FAULT";
   case 0xc8: case 0xc9: case 0xca: case 0xcb:
      return "MMU_PERMISSION_FAULT";
   case 0xd9: case 0xda: case 0xdb:
      return "MMU_ACCESS_FLAG_FAULT";
   case 0xe0: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
      return "MMU_ADDRESS_SIZE_FAULT";
   case 0xe8: case 0xe9: case 0xea: case 0xeb:
      return "MMU_MEMORY_ATTRIBUTE_FAULT";
   default:   return "UNKNOWN_CSF_FAULT";
   }
}

static void decode_csf_insn(uint64_t insn, char *buf, size_t buflen)
{
   uint8_t op = static_cast<uint8_t>((insn >> 56) & 0xffU);
   const char *op_name = panthor_kbase_csf_opcode_name(op);
   switch (op) {
   case HW_CSF_OP_NOP:
      snprintf(buf, buflen, "NOP");
      break;
   case HW_CSF_OP_MOVE48:
      snprintf(buf, buflen, "MOVE48 r%u, 0x%llx",
               static_cast<unsigned>((insn >> 48) & 0xffU),
               static_cast<unsigned long long>(insn & 0xffffffffffffULL));
      break;
   case HW_CSF_OP_MOVE32:
      snprintf(buf, buflen, "MOVE32 r%u, 0x%x",
               static_cast<unsigned>((insn >> 48) & 0xffU),
               static_cast<unsigned>(insn & 0xffffffffU));
      break;
   case HW_CSF_OP_WAIT:
      snprintf(buf, buflen, "WAIT mask=0x%04x (prog_inc=%u)",
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<unsigned>((insn >> 32) & 0x1U));
      break;
   case HW_CSF_OP_CALL:
   case HW_CSF_OP_JUMP:
      snprintf(buf, buflen, "%s addr_reg=r%u, len_reg=r%u",
               op_name,
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 32) & 0xffU));
      break;
   case HW_CSF_OP_REQ_RESOURCE:
      snprintf(buf, buflen, "REQ_RESOURCE mask=0x%x (comp=%u frag=%u tiler=%u idvs=%u)",
               static_cast<unsigned>(insn & 0xfU),
               static_cast<unsigned>((insn >> 0) & 1U),
               static_cast<unsigned>((insn >> 1) & 1U),
               static_cast<unsigned>((insn >> 2) & 1U),
               static_cast<unsigned>((insn >> 3) & 1U));
      break;
   case HW_CSF_OP_FLUSH_CACHE2:
      snprintf(buf, buflen, "FLUSH_CACHE2 wait=0x%04x flush_reg=r%u sig_slot=%u mode=0x%03x",
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 48) & 0xfU),
               static_cast<unsigned>(insn & 0xfffU));
      break;
   case HW_CSF_OP_SYNC_ADD32:
   case HW_CSF_OP_SYNC_SET32:
   case HW_CSF_OP_SYNC_ADD64:
   case HW_CSF_OP_SYNC_SET64:
      snprintf(buf, buflen, "%s addr=r%u data=r%u wait=0x%04x sig_slot=%u defer=%u scope=%s(%u) err_prop=%u",
               op_name,
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 32) & 0xffU),
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<unsigned>((insn >> 48) & 0xfU),
               static_cast<unsigned>((insn >> 52) & 0x1U),
               (((insn >> 1) & 0x3U) == 0) ? "SYSTEM" : "CSG",
               static_cast<unsigned>((insn >> 1) & 0x3U),
               static_cast<unsigned>(insn & 0x1U));
      break;
   case HW_CSF_OP_SYNC_WAIT32:
   case HW_CSF_OP_SYNC_WAIT64: {
      uint32_t cond = static_cast<uint32_t>((insn >> 28) & 0xfU);
      const char *cond_str = (cond == 0) ? "LE" : (cond == 1) ? "GT" : (cond == 5) ? "GE" : "?";
      snprintf(buf, buflen, "%s addr=r%u ref=r%u cond=%s(%u) err_rej=%u",
               op_name,
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 32) & 0xffU),
               cond_str, cond,
               static_cast<unsigned>(insn & 0x1U));
      break;
   }
   case HW_CSF_OP_FINISH_FRAGMENT:
      snprintf(buf, buflen, "FINISH_FRAGMENT first=r%u last=r%u wait=0x%04x sig_slot=%u defer=%u inc_frag=%u",
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 32) & 0xffU),
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<unsigned>((insn >> 48) & 0xfU),
               static_cast<unsigned>((insn >> 52) & 0x1U),
               static_cast<unsigned>(insn & 0x1U));
      break;
   case HW_CSF_OP_HEAP_OPERATION:
      snprintf(buf, buflen, "HEAP_OPERATION mode=%u wait=0x%04x sig_slot=%u defer=%u",
               static_cast<unsigned>((insn >> 32) & 0x7U),
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<unsigned>((insn >> 48) & 0xfU),
               static_cast<unsigned>((insn >> 52) & 0x1U));
      break;
   case HW_CSF_OP_SET_STATE_IMM32:
      snprintf(buf, buflen, "SET_STATE_IMM32 state=%u val=0x%x",
               static_cast<unsigned>((insn >> 32) & 0xffU),
               static_cast<unsigned>(insn & 0xffffffffU));
      break;
   case HW_CSF_OP_LOAD_MULTIPLE:
   case HW_CSF_OP_STORE_MULTIPLE:
      snprintf(buf, buflen, "%s base_reg=r%u addr=r%u mask=0x%04x offset=%d",
               op_name,
               static_cast<unsigned>((insn >> 48) & 0xffU),
               static_cast<unsigned>((insn >> 40) & 0xffU),
               static_cast<unsigned>((insn >> 16) & 0xffffU),
               static_cast<int>(static_cast<int16_t>(insn & 0xffffU)));
      break;
   default:
      snprintf(buf, buflen, "%s (0x%016llx)",
               op_name, static_cast<unsigned long long>(insn));
      break;
   }
}

static bool handle_mali_csf_notification_locked(const uint8_t *raw, ssize_t len)
{
   if (!raw || len < static_cast<ssize_t>(sizeof(struct base_csf_notification)))
      return false;
   const auto *notif = reinterpret_cast<const struct base_csf_notification *>(raw);
   if (notif->type == BASE_CSF_NOTIFICATION_EVENT) {
      LOG_DEBUG("CSF_NOTIFICATION: BASE_CSF_NOTIFICATION_EVENT");
      return false;
   }
   if (notif->type == BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP) {
      LOG_WARN("CSF_NOTIFICATION: BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP cmd=%u",
               notif->payload.dump.cmd);
      return false;
   }
   if (notif->type == BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR) {
      uint8_t grp_h = notif->payload.csg_error.handle;
      uint8_t err_type = notif->payload.csg_error.error.error_type;
      uint64_t sideband = notif->payload.csg_error.error.payload.fatal_queue.sideband;
      uint32_t status = notif->payload.csg_error.error.payload.fatal_queue.status;
      uint8_t csi_idx = notif->payload.csg_error.error.payload.fatal_queue.csi_index;
      uint8_t exc_type = static_cast<uint8_t>(status & 0xffU);
      const char *etype_str =
         (err_type == BASE_GPU_QUEUE_GROUP_ERROR_FATAL) ? "GROUP_FATAL" :
         (err_type == BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL) ? "QUEUE_FATAL" :
         (err_type == BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT) ? "PROGRESS_TIMEOUT" :
         (err_type == BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM) ? "TILER_HEAP_OOM" : "UNKNOWN";

      LOG_ERROR("CSF_NOTIFICATION: GPU_QUEUE_GROUP_ERROR kbase_group=%u type=%s(%u) csi=%u status=0x%08x (exc=0x%02x %s) sideband=0x%016llx",
                grp_h, etype_str, err_type, csi_idx, status, exc_type,
                cs_fault_exception_name(exc_type),
                static_cast<unsigned long long>(sideband));

      for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
         if (!g_ctx.groups[g].in_use)
            continue;
         for (uint32_t q = 0; q < g_ctx.groups[g].queue_count; q++) {
            if (g_ctx.groups[g].subqueues[q].kbase_group_handle == grp_h) {
               g_ctx.groups[g].fatal_state = (err_type == BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT)
                                                ? DRM_PANTHOR_GROUP_STATE_TIMEDOUT
                                                : DRM_PANTHOR_GROUP_STATE_FATAL_FAULT;
               g_ctx.groups[g].fatal_queues |= (1U << q);
            }
         }
      }
      return true;
   }
   return false;
}

static int create_kbase_csg_with_fallback(uint64_t tiler_mask,
                                          uint64_t fragment_mask,
                                          uint64_t compute_mask,
                                          uint8_t cs_min,
                                          uint8_t priority,
                                          uint8_t tiler_max,
                                          uint8_t fragment_max,
                                          uint8_t compute_max,
                                          uint8_t csi_handlers,
                                          uint8_t *out_group_handle,
                                          uint32_t *out_layout)
{
   /*
    * Try 40-byte (1.18) and 112-byte (1.19-1.25+) layouts FIRST when
    * csi_handlers != 0 (BASE_CSF_TILER_OOM_EXCEPTION_FLAG), because
    * the 32-byte 1.6 compat ioctl drops csi_handlers!
    */
   uint8_t prio_candidates[2] = { priority, 1U };
   int n_prios = (priority == 1U) ? 1 : 2;

   for (int p = 0; p < n_prios; p++) {
      uint8_t prio = prio_candidates[p];
      if (csi_handlers != 0) {
         union kbase_ioctl_cs_queue_group_create_1_18 csg40;
         memset(&csg40, 0, sizeof(csg40));
         csg40.in.tiler_mask = tiler_mask;
         csg40.in.fragment_mask = fragment_mask;
         csg40.in.compute_mask = compute_mask;
         csg40.in.cs_min = cs_min;
         csg40.in.priority = prio;
         csg40.in.tiler_max = tiler_max;
         csg40.in.fragment_max = fragment_max;
         csg40.in.compute_max = compute_max;
         csg40.in.csi_handlers = csi_handlers;
         if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18, &csg40) == 0) {
            *out_group_handle = csg40.out.group_handle;
            if (out_layout)
               *out_layout = 40U;
            return 0;
         }

         union kbase_ioctl_cs_queue_group_create_112 csg112;
         memset(&csg112, 0, sizeof(csg112));
         csg112.in.tiler_mask = tiler_mask;
         csg112.in.fragment_mask = fragment_mask;
         csg112.in.compute_mask = compute_mask;
         csg112.in.cs_min = cs_min;
         csg112.in.priority = prio;
         csg112.in.tiler_max = tiler_max;
         csg112.in.fragment_max = fragment_max;
         csg112.in.compute_max = compute_max;
         csg112.in.csi_handlers = csi_handlers;
         if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_112, &csg112) == 0) {
            *out_group_handle = csg112.out.group_handle;
            if (out_layout)
               *out_layout = 112U;
            return 0;
         }
      }

      union kbase_ioctl_cs_queue_group_create_1_6 csg32;
      memset(&csg32, 0, sizeof(csg32));
      csg32.in.tiler_mask = tiler_mask;
      csg32.in.fragment_mask = fragment_mask;
      csg32.in.compute_mask = compute_mask;
      csg32.in.cs_min = cs_min;
      csg32.in.priority = prio;
      csg32.in.tiler_max = tiler_max;
      csg32.in.fragment_max = fragment_max;
      csg32.in.compute_max = compute_max;
      if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &csg32) == 0) {
         *out_group_handle = csg32.out.group_handle;
         if (out_layout)
            *out_layout = 32U;
         return 0;
      }
   }
   return -1;
}

} /* anonymous namespace */

extern "C" {

const char *panthor_kbase_csf_opcode_name(uint8_t opcode)
{
   switch (opcode) {
   case 0x00: return "NOP";
   case 0x01: return "MOVE48";
   case 0x02: return "MOVE32";
   case 0x03: return "WAIT";
   case 0x04: return "RUN_COMPUTE";
   case 0x05: return "RUN_TILING";
   case 0x06: return "RUN_IDVS";
   case 0x07: return "RUN_FRAGMENT";
   case 0x08: return "RUN_FULLSCREEN";
   case 0x09: return "FINISH_TILING";
   case 0x0b: return "FINISH_FRAGMENT";
   case 0x10: return "ADD_IMM32";
   case 0x11: return "ADD_IMM64";
   case 0x12: return "COMPARE_SELECT32";
   case 0x13: return "LOGIC_OP32";
   case 0x14: return "LOAD_MULTIPLE";
   case 0x15: return "STORE_MULTIPLE";
   case 0x16: return "BRANCH";
   case 0x17: return "SET_SB_ENTRY";
   case 0x18: return "PROGRESS_WAIT";
   case 0x19: return "SET_EXCEPTION_HANDLER";
   case 0x1a: return "NEXT_SB_ENTRY";
   case 0x1b: return "SET_STATE";
   case 0x1c: return "SET_STATE_IMM32";
   case 0x1e: return "SHARED_SB_INC";
   case 0x1f: return "SHARED_SB_DEC";
   case 0x20: return "CALL";
   case 0x21: return "JUMP";
   case 0x22: return "REQ_RESOURCE";
   case 0x24: return "FLUSH_CACHE2";
   case 0x25: return "SYNC_ADD32";
   case 0x26: return "SYNC_SET32";
   case 0x27: return "SYNC_WAIT32";
   case 0x28: return "STORE_STATE";
   case 0x29: return "PROT_REGION";
   case 0x2a: return "PROGRESS_STORE";
   case 0x2b: return "PROGRESS_LOAD";
   case 0x2c: return "RUN_COMPUTE_INDIRECT";
   case 0x2f: return "ERROR_BARRIER";
   case 0x30: return "HEAP_SET";
   case 0x31: return "HEAP_OPERATION";
   case 0x32: return "TRACE_POINT";
   case 0x33: return "SYNC_ADD64";
   case 0x34: return "SYNC_SET64";
   case 0x35: return "SYNC_WAIT64";
   default:   return "CSF_OP_UNKNOWN";
   }
}

void panthor_kbase_dump_csf_diagnostics(const char *reason)
{
   pthread_mutex_lock(&g_lock);
   LOG_WARN("================================================================================");
   LOG_WARN(" CSF SUBQUEUE & SYNCOBJ DIAGNOSTIC SNAPSHOT (reason: %s)", reason ? reason : "manual");
   LOG_WARN("================================================================================");

   if (g_ctx.event_bo_cpu && g_ctx.event_bo_size > 0)
      kbase_cpu_cache_invalidate_range(g_ctx.event_bo_cpu, g_ctx.event_bo_size);

   static const char *const sq_names[4] = { "VT(0)", "FRAG(1)", "COMPUTE(2)", "SUBQ(3)" };

   for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
      struct adapter_group *grp = &g_ctx.groups[g];
      if (!grp->in_use)
         continue;

      LOG_WARN("Group handle=%u kbase_csg=%u layout=%uB single_csg=%d csi_handlers=0x%x heap_ctx=0x%llx fatal_state=0x%x",
               grp->group_handle, grp->kbase_group_handle, grp->group_create_layout,
               grp->single_csg_mode ? 1 : 0, grp->csi_handlers,
               static_cast<unsigned long long>(grp->active_tiler_heap_ctx_va),
               grp->fatal_state);

      for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
         struct adapter_subqueue *sq = &grp->subqueues[q];
         if (!sq->bound)
            continue;

         uint64_t cs_insert = 0, cs_extract_init = 0, cs_extract = 0;
         uint32_t cs_active = 0, cs_fault = 0, cs_fatal = 0;
         uint64_t cs_fault_info = 0, cs_fatal_info = 0;
         uint32_t heap_vt_start = 0, heap_vt_end = 0, heap_frag_end = 0;

         if (sq->user_io_ptr) {
            const auto *in_u64 = reinterpret_cast<const volatile uint64_t *>(
               static_cast<const uint8_t *>(sq->user_io_ptr) + CS_USER_IO_INPUT_PAGE_OFFSET);
            const auto *out_u64 = reinterpret_cast<const volatile uint64_t *>(
               static_cast<const uint8_t *>(sq->user_io_ptr) + CS_USER_IO_OUTPUT_PAGE_OFFSET);
            const auto *out_u32 = reinterpret_cast<const volatile uint32_t *>(
               static_cast<const uint8_t *>(sq->user_io_ptr) + CS_USER_IO_OUTPUT_PAGE_OFFSET);

            cs_insert       = in_u64[0];
            cs_extract_init = in_u64[1];
            cs_extract      = out_u64[0];
            cs_active       = out_u32[2];
            cs_fault        = out_u32[0x80 / 4];
            cs_fatal        = out_u32[0x84 / 4];
            cs_fault_info   = out_u64[0x88 / 8];
            cs_fatal_info   = out_u64[0x90 / 8];
            heap_vt_start   = out_u32[0xc0 / 4];
            heap_vt_end     = out_u32[0xc4 / 4];
            heap_frag_end   = out_u32[0xcc / 4];
         }

         uint64_t seqno_val = 0, seqno_err = 0, ls_copy = 0;
         uint64_t pre_call = 0, post_call = 0, post_wait = 0, stream_prog = 0;
         if (sq->seqno_cpu_ptr) {
            kbase_cpu_cache_invalidate_range(const_cast<uint64_t *>(sq->seqno_cpu_ptr), 64);
            const auto *cell = reinterpret_cast<const volatile uint64_t *>(sq->seqno_cpu_ptr);
            seqno_val   = cell[ADAPTER_SEQNO_VALUE_OFFSET / 8];
            seqno_err   = cell[ADAPTER_SEQNO_ERROR_OFFSET / 8];
         }
         const volatile uint64_t *dcell = sq->diag_cpu_ptr ? sq->diag_cpu_ptr : sq->seqno_cpu_ptr;
         if (dcell) {
            kbase_cpu_cache_invalidate_range(const_cast<uint64_t *>(dcell), 64);
            ls_copy     = dcell[ADAPTER_SEQNO_LS_COPY_OFFSET / 8];
            pre_call    = dcell[ADAPTER_SEQNO_PRE_CALL_OFFSET / 8];
            post_call   = dcell[ADAPTER_SEQNO_POST_CALL_OFFSET / 8];
            post_wait   = dcell[ADAPTER_SEQNO_POST_WAIT_OFFSET / 8];
            stream_prog = dcell[ADAPTER_SEQNO_STREAM_PROGRESS_OFFSET / 8];
         }

         uint64_t active_job_seq = seqno_val + 1;
         const char *stage = "IDLE/COMPLETE";
         if (seqno_val < sq->emitted_jobs) {
            if (cs_extract >= cs_insert && post_call >= active_job_seq && post_wait < active_job_seq)
               stage = "RING_WRAP_TRUNCATION_ANOMALY (CS_EXTRACT==CS_INSERT after POST_CALL without POST_WAIT/SEQNO)";
            else if (cs_extract < cs_insert && pre_call < active_job_seq)
               stage = "BLOCKED_BEFORE_CALL (ringbuffer/REQ_RESOURCE/FLUSH_CACHE2 or CSG off-slot)";
            else if (pre_call >= active_job_seq && post_call < active_job_seq)
               stage = "BLOCKED_INSIDE_CS_STREAM (SYNC_WAIT32/64 or WAIT inside command buffer)";
            else if (post_call >= active_job_seq && post_wait < active_job_seq)
               stage = "BLOCKED_AT_POST_CALL_WAIT (waiting for RUN_IDVS/FRAGMENT/COMPUTE/FINISH_FRAGMENT)";
            else if (post_wait >= active_job_seq && seqno_val < active_job_seq)
               stage = "BLOCKED_AT_RING_SYNC_ADD64";
            else
               stage = "PENDING_COMPLETION";
         }

         LOG_WARN("  [%s] csg=%u csi=%u jobs=%llu seqno=%llu(err=0x%llx) ls=%llu pre=%llu post=%llu wait=%llu prog=0x%llx",
                  sq_names[q], sq->kbase_group_handle, sq->csi_index,
                  static_cast<unsigned long long>(sq->emitted_jobs),
                  static_cast<unsigned long long>(seqno_val),
                  static_cast<unsigned long long>(seqno_err),
                  static_cast<unsigned long long>(ls_copy),
                  static_cast<unsigned long long>(pre_call),
                  static_cast<unsigned long long>(post_call),
                  static_cast<unsigned long long>(post_wait),
                  static_cast<unsigned long long>(stream_prog));
         LOG_WARN("          CS_INSERT=%llu(ring_off=%u) CS_EXTRACT=%llu(ring_off=%u, init=%llu) CS_ACTIVE=%u heap(vt_s=%u vt_e=%u frag_e=%u) -> %s",
                  static_cast<unsigned long long>(cs_insert),
                  sq->ringbuf_size ? static_cast<unsigned>(cs_insert % sq->ringbuf_size) : 0U,
                  static_cast<unsigned long long>(cs_extract),
                  sq->ringbuf_size ? static_cast<unsigned>(cs_extract % sq->ringbuf_size) : 0U,
                  static_cast<unsigned long long>(cs_extract_init),
                  cs_active, heap_vt_start, heap_vt_end, heap_frag_end, stage);

         if (cs_fault != 0 || cs_fatal != 0) {
            uint8_t f_exc = static_cast<uint8_t>(cs_fault & 0xffU);
            uint8_t fat_exc = static_cast<uint8_t>(cs_fatal & 0xffU);
            LOG_ERROR("          CS_FAULT=0x%08x (%s, info=0x%llx) CS_FATAL=0x%08x (%s, info=0x%llx)",
                      cs_fault, cs_fault_exception_name(f_exc),
                      static_cast<unsigned long long>(cs_fault_info),
                      cs_fatal, cs_fault_exception_name(fat_exc),
                      static_cast<unsigned long long>(cs_fatal_info));
         }

         /* Inspect subqueue context (syncobjs, render.desc_ringbuf & tiler_oom_ctx) */
         if (sq->cached_subq_ctx_va != 0) {
            auto *ctx_u64 = static_cast<uint64_t *>(resolve_gpu_va_to_cpu(sq->cached_subq_ctx_va, 80));
            if (ctx_u64) {
               kbase_cpu_cache_invalidate_range(ctx_u64, 128);
               uint64_t syncobjs_va = ctx_u64[0];
               uint64_t rb_sync_va  = ctx_u64[3];
               uint64_t rb_addr_va  = ctx_u64[4];
               uint64_t tiler_hp_va = ctx_u64[5];
               auto *so_u64 = static_cast<uint64_t *>(resolve_gpu_va_to_cpu(syncobjs_va, 48));
               uint32_t rb_sync_val = 0;
               if (rb_sync_va != 0) {
                  auto *rb_u32 = static_cast<uint32_t *>(resolve_gpu_va_to_cpu(rb_sync_va, 4));
                  if (rb_u32) {
                     kbase_cpu_cache_invalidate_range(rb_u32, 64);
                     rb_sync_val = rb_u32[0];
                  }
               }
               if (so_u64) {
                  kbase_cpu_cache_invalidate_range(so_u64, 64);
                  LOG_WARN("          queue->syncobjs(0x%llx): VT={seq=%llu,err=0x%llx} FRAG={seq=%llu,err=0x%llx} COMP={seq=%llu,err=0x%llx} desc_rb_sync(0x%llx)=0x%x(addr=0x%llx) heap=0x%llx",
                           static_cast<unsigned long long>(syncobjs_va),
                           static_cast<unsigned long long>(so_u64[0]),
                           static_cast<unsigned long long>(so_u64[1]),
                           static_cast<unsigned long long>(so_u64[2]),
                           static_cast<unsigned long long>(so_u64[3]),
                           static_cast<unsigned long long>(so_u64[4]),
                           static_cast<unsigned long long>(so_u64[5]),
                           static_cast<unsigned long long>(rb_sync_va),
                           rb_sync_val,
                           static_cast<unsigned long long>(rb_addr_va),
                           static_cast<unsigned long long>(tiler_hp_va));
               }
            }
         }

         /* Dump recent ringbuffer flight recorder entries for this subqueue */
         if (sq->emitted_jobs > 0) {
            uint64_t now_ts = get_monotonic_ns();
            uint64_t start_j = (sq->emitted_jobs > 6) ? (sq->emitted_jobs - 5) : 1;
            LOG_WARN("          Ringbuffer Flight Recorder (last %llu jobs on %s, ring_size=%u):",
                     static_cast<unsigned long long>(sq->emitted_jobs - start_j + 1),
                     sq_names[q], sq->ringbuf_size);
            for (uint64_t j = start_j; j <= sq->emitted_jobs; j++) {
               const struct adapter_flight_entry *fe = &sq->flight_log[j % ADAPTER_FLIGHT_LOG_ENTRIES];
               if (fe->job_seqno != j)
                  continue;
               double age_ms = (now_ts >= fe->submit_ts_ns)
                                  ? (static_cast<double>(now_ts - fe->submit_ts_ns) / 1e6)
                                  : 0.0;
               LOG_WARN("            job=%llu ring_off=%u..%u (%uB, wrap_nop=%uB) insert=%llu->%llu extract_at_sub=%llu stream=0x%llx(%uB) age=%.2fms",
                        static_cast<unsigned long long>(fe->job_seqno),
                        fe->ring_off, fe->ring_off + fe->slot_bytes, fe->slot_bytes,
                        fe->wrap_nop_bytes,
                        static_cast<unsigned long long>(fe->insert_before),
                        static_cast<unsigned long long>(fe->insert_after),
                        static_cast<unsigned long long>(fe->extract_at_submit),
                        static_cast<unsigned long long>(fe->stream_va),
                        fe->stream_size, age_ms);
               if (seqno_val < sq->emitted_jobs && j > seqno_val + 1) {
                  uint64_t act_va = sq->job_stream_va[(seqno_val + 1) & 63U];
                  uint32_t act_sz = sq->job_stream_size[(seqno_val + 1) & 63U];
                  if (act_va != 0 && fe->stream_va == act_va) {
                     LOG_ERROR("            [IN-FLIGHT STREAM OVERWRITE DETECTED] active job=%llu stream=0x%llx(%uB) was reused by later job=%llu stream=0x%llx(%uB) before GPU completion!",
                               static_cast<unsigned long long>(seqno_val + 1),
                               static_cast<unsigned long long>(act_va), act_sz,
                               static_cast<unsigned long long>(fe->job_seqno),
                               static_cast<unsigned long long>(fe->stream_va), fe->stream_size);
                  }
               }
            }
         }

         /* Disassemble primary ringbuffer slot & active/unfinished secondary CS stream */
         uint64_t active_job = seqno_val + 1;
         if (seqno_val < sq->emitted_jobs && sq->ring_cpu_ptr) {
            const struct adapter_flight_entry *afe = &sq->flight_log[active_job % ADAPTER_FLIGHT_LOG_ENTRIES];
            if (afe->job_seqno == active_job &&
                afe->ring_off + afe->slot_bytes <= sq->ring_alloc_size) {
               const auto *ring_qwords = reinterpret_cast<const uint64_t *>(
                  reinterpret_cast<const uint8_t *>(sq->ring_cpu_ptr) + afe->ring_off);
               uint32_t rq_count = afe->slot_bytes / 8;
               LOG_WARN("          Primary Ringbuffer Slot (job=%llu) on %s (ring_va=0x%llx+0x%x, %u insns):",
                        static_cast<unsigned long long>(active_job), sq_names[q],
                        static_cast<unsigned long long>(sq->ring_gpu_va),
                        afe->ring_off, rq_count);
               for (uint32_t r_idx = 0; r_idx < rq_count && r_idx < 64; r_idx++) {
                  char rdis[160];
                  decode_csf_insn(ring_qwords[r_idx], rdis, sizeof(rdis));
                  LOG_WARN("            [ring+%02u] off=%u: 0x%016llx  %s",
                           r_idx, afe->ring_off + r_idx * 8,
                           static_cast<unsigned long long>(ring_qwords[r_idx]), rdis);
               }
            }
         }

         uint64_t dump_va = (seqno_val < sq->emitted_jobs && sq->job_stream_va[active_job & 63U] != 0)
                               ? sq->job_stream_va[active_job & 63U]
                               : sq->last_stream_va;
         uint32_t dump_sz = (seqno_val < sq->emitted_jobs && sq->job_stream_size[active_job & 63U] != 0)
                               ? sq->job_stream_size[active_job & 63U]
                               : sq->last_stream_size;
         if (seqno_val < sq->emitted_jobs && dump_va != 0 && dump_sz >= 8) {
            auto *cs_ptr = static_cast<const uint64_t *>(
               resolve_gpu_va_to_cpu(dump_va, dump_sz));
            if (cs_ptr) {
               uint32_t n_insns = dump_sz / 8;
               LOG_WARN("          Active CS stream (job=%llu) on %s (va=0x%llx, %u insns):",
                        static_cast<unsigned long long>(active_job),
                        sq_names[q],
                        static_cast<unsigned long long>(dump_va), n_insns);
               uint64_t shadow_regs[128] = {0};
               if (sq->cached_subq_ctx_va != 0) {
                  uint8_t creg = sq->cached_subq_ctx_reg ? sq->cached_subq_ctx_reg : 90U;
                  if (creg + 1 < 128) {
                     shadow_regs[creg]     = sq->cached_subq_ctx_va & 0xffffffffULL;
                     shadow_regs[creg + 1] = sq->cached_subq_ctx_va >> 32;
                  }
               }
               for (uint32_t idx = 0; idx < n_insns && idx < 256; idx++) {
                  uint64_t insn = cs_ptr[idx];
                  uint8_t op = static_cast<uint8_t>((insn >> 56) & 0xffU);
                  char dis[160];
                  decode_csf_insn(insn, dis, sizeof(dis));
                  char extra[160];
                  extra[0] = '\0';

                  if (op == HW_CSF_OP_MOVE48) {
                     uint8_t rd = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                     uint64_t imm = insn & 0xffffffffffffULL;
                     if (rd + 1 < 128) {
                        shadow_regs[rd]     = imm & 0xffffffffULL;
                        shadow_regs[rd + 1] = imm >> 32;
                     }
                  } else if (op == HW_CSF_OP_MOVE32) {
                     uint8_t rd = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                     if (rd < 128)
                        shadow_regs[rd] = insn & 0xffffffffULL;
                  } else if (op == HW_CSF_OP_ADD_IMM32) {
                     uint8_t rd = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                     uint8_t rs = static_cast<uint8_t>((insn >> 40) & 0x7fU);
                     int32_t imm = static_cast<int32_t>(insn & 0xffffffffULL);
                     if (rd < 128 && rs < 128)
                        shadow_regs[rd] = static_cast<uint32_t>(shadow_regs[rs] + imm);
                  } else if (op == HW_CSF_OP_ADD_IMM64) {
                     uint8_t rd = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                     uint8_t rs = static_cast<uint8_t>((insn >> 40) & 0x7fU);
                     int32_t imm = static_cast<int32_t>(insn & 0xffffffffULL);
                     if (rd + 1 < 128 && rs + 1 < 128) {
                        uint64_t base_v = (shadow_regs[rs + 1] << 32) | (shadow_regs[rs] & 0xffffffffULL);
                        uint64_t res_v = static_cast<uint64_t>( static_cast<int64_t>(base_v) + imm );
                        shadow_regs[rd]     = res_v & 0xffffffffULL;
                        shadow_regs[rd + 1] = res_v >> 32;
                     }
                  } else if (op == HW_CSF_OP_LOAD_MULTIPLE) {
                     uint8_t base_r = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                     uint8_t addr_r = static_cast<uint8_t>((insn >> 40) & 0x7fU);
                     uint16_t mask  = static_cast<uint16_t>((insn >> 16) & 0xffffU);
                     int16_t off    = static_cast<int16_t>(insn & 0xffffU);
                     if (addr_r + 1 < 128) {
                        uint64_t src_va = ((shadow_regs[addr_r + 1] << 32) | (shadow_regs[addr_r] & 0xffffffffULL)) + off;
                        const auto *src_u32 = static_cast<const uint32_t *>(resolve_gpu_va_to_cpu(src_va, 64));
                        if (src_u32) {
                           uint32_t m_idx = 0;
                           for (uint32_t b = 0; b < 16; b++) {
                              if (mask & (1U << b)) {
                                 if (base_r + b < 128)
                                    shadow_regs[base_r + b] = src_u32[m_idx];
                                 m_idx++;
                              }
                           }
                        }
                     }
                  } else if (op == HW_CSF_OP_SYNC_WAIT32 || op == HW_CSF_OP_SYNC_WAIT64) {
                     uint8_t addr_r = static_cast<uint8_t>((insn >> 40) & 0x7fU);
                     uint8_t ref_r  = static_cast<uint8_t>((insn >> 32) & 0x7fU);
                     if (addr_r + 1 < 128 && ref_r < 128) {
                        uint64_t w_va = (shadow_regs[addr_r + 1] << 32) | (shadow_regs[addr_r] & 0xffffffffULL);
                        if (op == HW_CSF_OP_SYNC_WAIT32) {
                           uint32_t ref_v = static_cast<uint32_t>(shadow_regs[ref_r]);
                           auto *w_ptr = static_cast<uint32_t *>(resolve_gpu_va_to_cpu(w_va, 4));
                           if (w_ptr) {
                              kbase_cpu_cache_invalidate_range(w_ptr, 64);
                              snprintf(extra, sizeof(extra), "  <-- [target_va=0x%llx live_mem=%u ref=%u]",
                                       static_cast<unsigned long long>(w_va), w_ptr[0], ref_v);
                           }
                        } else if (ref_r + 1 < 128) {
                           uint64_t ref_v = (shadow_regs[ref_r + 1] << 32) | (shadow_regs[ref_r] & 0xffffffffULL);
                           auto *w_ptr = static_cast<uint64_t *>(resolve_gpu_va_to_cpu(w_va, 8));
                           if (w_ptr) {
                              kbase_cpu_cache_invalidate_range(w_ptr, 64);
                              snprintf(extra, sizeof(extra), "  <-- [target_va=0x%llx live_mem=%llu ref=%llu]",
                                       static_cast<unsigned long long>(w_va),
                                       static_cast<unsigned long long>(w_ptr[0]),
                                       static_cast<unsigned long long>(ref_v));
                           }
                        }
                     }
                  }

                  LOG_WARN("            [%02u] +0x%03x: 0x%016llx  %s%s",
                           idx, idx * 8,
                           static_cast<unsigned long long>(insn), dis, extra);
               }
            }
         }
      }
   }
   LOG_WARN("  Telemetry: submits=%llu waits=%llu(fast=%llu,poll=%llu) fps=%.1f submit_us(avg=%.1f,max=%.1f) wait_us(avg=%.1f,max=%.1f)",
            static_cast<unsigned long long>(g_ctx.total_group_submits),
            static_cast<unsigned long long>(g_ctx.total_syncobj_waits),
            static_cast<unsigned long long>(g_ctx.fast_spin_syncobj_waits),
            static_cast<unsigned long long>(g_ctx.poll_sleep_syncobj_waits),
            g_ctx.last_window_fps,
            g_ctx.last_avg_submit_us, g_ctx.last_max_submit_us,
            g_ctx.last_avg_sync_wait_us, g_ctx.last_max_sync_wait_us);
   LOG_WARN("================================================================================");
   pthread_mutex_unlock(&g_lock);
}

void panthor_kbase_get_telemetry_snapshot(struct adapter_telemetry_snapshot *out)
{
   if (!out)
      return;
   pthread_mutex_lock(&g_lock);
   memset(out, 0, sizeof(*out));
   out->total_group_submits      = g_ctx.total_group_submits;
   out->total_syncobj_waits      = g_ctx.total_syncobj_waits;
   out->fast_spin_syncobj_waits  = g_ctx.fast_spin_syncobj_waits;
   out->poll_sleep_syncobj_waits = g_ctx.poll_sleep_syncobj_waits;
   out->window_fps               = g_ctx.last_window_fps;
   out->avg_submit_us            = g_ctx.last_avg_submit_us;
   out->max_submit_us            = g_ctx.last_max_submit_us;
   out->avg_cs_norm_us           = g_ctx.last_avg_cs_norm_us;
   out->avg_cache_clean_us       = g_ctx.last_avg_cache_clean_us;
   out->avg_kick_us              = g_ctx.last_avg_kick_us;
   out->avg_sync_wait_us         = g_ctx.last_avg_sync_wait_us;
   out->max_sync_wait_us         = g_ctx.last_max_sync_wait_us;

   size_t bo_limit = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
   if (bo_limit > ADAPTER_MAX_BOS)
      bo_limit = ADAPTER_MAX_BOS;
   for (size_t i = 0; i < bo_limit; i++) {
      if (g_ctx.bos[i].in_use) {
         out->active_bos++;
         out->active_bo_bytes += g_ctx.bos[i].size;
         if (g_ctx.bos[i].kbase_flags & BASE_MEM_CACHED_CPU)
            out->cached_cpu_bos++;
         if (g_ctx.bos[i].is_imported)
            out->imported_bos++;
      }
   }
   size_t so_limit = g_ctx.max_syncobj_slot_used ? g_ctx.max_syncobj_slot_used : ADAPTER_MAX_SYNCOBJS;
   if (so_limit > ADAPTER_MAX_SYNCOBJS)
      so_limit = ADAPTER_MAX_SYNCOBJS;
   for (size_t i = 0; i < so_limit; i++) {
      if (g_ctx.syncobjs[i].in_use)
         out->active_syncobjs++;
   }
   for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
      if (!g_ctx.groups[g].in_use)
         continue;
      for (uint32_t q = 0; q < g_ctx.groups[g].queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
         const struct adapter_subqueue *sq = &g_ctx.groups[g].subqueues[q];
         out->total_subqueue_jobs[q] += sq->emitted_jobs;
         out->ring_wraps[q]          += sq->ring_wraps;
         if (sq->seqno_cpu_ptr)
            out->completed_subqueue_seqno[q] = *sq->seqno_cpu_ptr;
      }
   }
   pthread_mutex_unlock(&g_lock);
}

void panthor_kbase_adapter_hint_bo_flags(uint32_t kmod_flags)
{
   g_tls_next_bo_kmod_flags = kmod_flags;
}

uint64_t panthor_kbase_adapter_get_bo_gpu_va(uint32_t gem_handle)
{
   pthread_mutex_lock(&g_lock);
   struct adapter_bo *bo = find_bo(gem_handle);
   uint64_t va = bo ? (bo->alias_gpu_va ? bo->alias_gpu_va : bo->gpu_va) : 0ULL;
   pthread_mutex_unlock(&g_lock);
   return va;
}

/*
 * CSF Bytecode Normalizer:
 * Scans a PanVK CSF command stream in-place before submission to kbase:
 *  1. Extracts `subq_context_va` from the initial submit's `MOV48 r_ctx, va`.
 *  2. Upgrades `SYNC_ADD32` (0x25), `SYNC_SET32` (0x26), `SYNC_ADD64` (0x33),
 *     and `SYNC_SET64` (0x34) `Scope` at bits [2:1] from `CSG` (2) to `SYSTEM` (0)
 *     so cross-CSG `SYNC_WAIT`s wake up even when subqueues run in separate CSGs.
 *  3. Rewrites `HEAP_OPERATION` (0x31) modes `VT_END` (1) and `FRAGMENT_COMPLETED` (3)
 *     to `WAIT`/`NOP` so `vt_end` and `frag_end` stay `0`.
 *  4. Clears bit 0 (`Increment Fragment Completed`) of `FINISH_FRAGMENT` (0x0b)
 *     so tiler heap chunks are recycled back to the heap free list at the end of
 *     every render pass while `frag_end` remains `0` (`frag_end <= vt_end`),
 *     preventing `mali_kbase_csf.c:handle_oom_event` from terminating the CSG!
 */
static bool is_bug_catching_mode_enabled(void)
{
   const char *env = getenv("PANTHOR_ADAPTER_BUG_CATCHING_MODE");
   return (env && env[0] && atoi(env) != 0);
}

void panthor_kbase_normalize_cs_stream(uint64_t *stream_cpu,
                                       uint32_t stream_size_bytes,
                                       bool is_initial_submit,
                                       struct adapter_cs_patch_stats *stats)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_normalize_cs_stream",
                           "stream_cpu=%p, size=%u, initial=%d",
                           static_cast<void *>(stream_cpu), stream_size_bytes, is_initial_submit);
   if (stats)
      memset(stats, 0, sizeof(*stats));
   if (!stream_cpu || stream_size_bytes < sizeof(uint64_t))
      return;

   uint32_t count = stream_size_bytes / sizeof(uint64_t);
   if (stats)
      stats->total_instructions = count;

   /* Track whether SB_ID(DEFERRED_SYNC) (slot 1) could have an unretired
    * asynchronous SYNC_ADD/SET in flight on this stream (doco._zve6aI38CdoizoSSnm2oA):
    * Starts true at command buffer entry (in case a prior command buffer on the
    * same CSI stream left a deferred sync in flight), clears whenever any
    * instruction waits on MALI_PANVK_SB_MASK_DEFERRED_SYNC (slot 1), and sets
    * whenever SYNC_ADD32/64 or SYNC_SET32/64 signals slot 1. */
   bool may_have_unwaited_deferred_sync = true;
   const uint64_t def_wait_bit = static_cast<uint64_t>(MALI_PANVK_SB_MASK_DEFERRED_SYNC)
                                 << MALI_CSF_INS_WAIT_WAIT_MASK_SHIFT;

   for (uint32_t i = 0; i < count; i++) {
      uint64_t insn = stream_cpu[i];
      uint64_t opcode = (insn >> 56) & 0xffULL;

      if ((opcode == MALI_CSF_OP_WAIT || opcode == MALI_CSF_OP_FINISH_FRAGMENT ||
           opcode == MALI_CSF_OP_FLUSH_CACHE2) &&
          (insn & def_wait_bit) != 0ULL) {
         may_have_unwaited_deferred_sync = false;
      }

      if (is_initial_submit && i == 0 && opcode == CSF_OP_MOVE48) {
         uint8_t dest_reg = static_cast<uint8_t>((insn >> 48) & 0xffU);
         uint64_t imm48 = insn & 0xffffffffffffULL;
         if (stats) {
            stats->extracted_subq_ctx_reg = dest_reg;
            stats->extracted_subq_ctx_va = imm48;
         }
      }

      /*
       * 1. Real Valhall v10.xml/v11.xml SYNC_ADD32 (37=0x25), SYNC_SET32 (38=0x26),
       *    SYNC_ADD64 (51=0x33), SYNC_SET64 (52=0x34):
       *    In genxml v10.xml/v11.xml, `Scope` is at bits [2:1] (`0 = System`, `2 = CSG`),
       *    `Data` register is at bits [39:32], `Address` register is at bits [47:40],
       *    and `Wait Mask` is at bits [31:16].
       *    CRITICAL: Always `continue` for real hardware SYNC_ADD/SET instructions
       *    (`((insn >> 32) & 0xffffULL) != 0ULL`) even when `hw_scope` is already `0`
       *    (e.g. when a command buffer is resubmitted), so they NEVER fall through to
       *    the legacy synthetic test-vector path and never corrupt `Wait Mask` to 0x2100!
       */
      /*
       * 0. Eliminate finish_cs error-propagation LOAD_MULTIPLE on queue->syncobjs (+8, +24, +40):
       *    At the end of every command buffer (finish_cs in panvk_vX_cmd_buffer.c:115), PanVK emits:
       *      [i+0] LOAD_MULTIPLE rX, [r90 + 0], mask=0x0003
       *      [i+1] WAIT mask=0x0001
       *      [i+2] LOAD_MULTIPLE rY, [rX + 8/24/40], mask=0x0001
       *      [i+3] WAIT mask=0x0001
       *      [i+4] BRANCH rY, eqz, +2
       *      [i+5] STORE_MULTIPLE rY, [r90 + 12], mask=0x0001
       *      [i+6] WAIT mask=0x0001
       *    Executing a non-atomic LS LOAD_MULTIPLE on queue->syncobjs (+8/+24/+40) while sibling
       *    subqueues execute atomic SYNC_ADD64 on +0/+16/+32 of the same 64-byte cache line triggers
       *    0x88 GPU_SHAREABILITY_FAULT on non-coherent MT6878 (Mali-G615 r44p1).
       */
      if (i + 6U < count &&
          opcode == MALI_CSF_OP_LOAD_MULTIPLE &&
          ((stream_cpu[i + 1U] >> 56) & 0xffULL) == MALI_CSF_OP_WAIT &&
          ((stream_cpu[i + 2U] >> 56) & 0xffULL) == MALI_CSF_OP_LOAD_MULTIPLE &&
          ((stream_cpu[i + 3U] >> 56) & 0xffULL) == MALI_CSF_OP_WAIT &&
          ((stream_cpu[i + 4U] >> 56) & 0xffULL) == MALI_CSF_OP_BRANCH &&
          ((stream_cpu[i + 5U] >> 56) & 0xffULL) == MALI_CSF_OP_STORE_MULTIPLE &&
          ((stream_cpu[i + 6U] >> 56) & 0xffULL) == MALI_CSF_OP_WAIT) {
         uint64_t lm2 = stream_cpu[i + 2U];
         uint16_t lm2_mask = static_cast<uint16_t>((lm2 >> 16) & 0xffffU);
         int16_t  lm2_off  = static_cast<int16_t>(lm2 & 0xffffU);
         uint64_t sm5 = stream_cpu[i + 5U];
         int16_t  sm5_off  = static_cast<int16_t>(sm5 & 0xffffU);
         if (lm2_mask == 0x0001U && (lm2_off == 8 || lm2_off == 24 || lm2_off == 40) && sm5_off == 12) {
            for (uint32_t k = 0; k < 7U; k++)
               stream_cpu[i + k] = 0ULL; /* NOP */
            i += 6U;
            continue;
         }
      }

      struct mali_csf_sync_op_fields sync_f;
      if (mali_csf_unpack_sync_op(insn, &sync_f)) {
         if ((insn & def_wait_bit) != 0ULL)
            may_have_unwaited_deferred_sync = false;
         if (sync_f.is_deferred)
            may_have_unwaited_deferred_sync = true;
         uint64_t hw_scope = sync_f.scope;
         const char *force_sys_env = getenv("PANTHOR_ADAPTER_FORCE_SYSTEM_SCOPE");
         bool force_sys = (force_sys_env && force_sys_env[0] && atoi(force_sys_env) != 0);
         bool keep_csg_scope = (!force_sys && g_ctx.active && g_ctx.groups[0].in_use &&
                                g_ctx.groups[0].single_csg_mode);
         if (keep_csg_scope) {
            /*
             * On non-coherent MT6878 (system_coherency = COHERENCY_NONE), dev->mempools.rw
             * has KBASE_REG_GPU_CACHED | KBASE_REG_SHARE_IN (SH = 10, Inner-Shareable) and
             * NO page ever gets KBASE_REG_SHARE_BOTH (SH = 11, Outer-Shareable).
             * Matching libGLES_mali.r44p1.so (0x1a6c81c, 0x1a728bc), keep Scope = CSG (2)
             * (Inner-Shareable) when all subqueues share one CSG (single_csg_mode == true)!
             */
            if (hw_scope != MALI_CS_SYNC_SCOPE_CSG && ((insn >> 40) & 0xffULL) >= 64ULL) {
               insn = (insn & ~MALI_CSF_INS_SYNC_ADD32_SCOPE_MASK) |
                      (static_cast<uint64_t>(MALI_CS_SYNC_SCOPE_CSG) << MALI_CSF_INS_SYNC_ADD32_SCOPE_SHIFT);
               stream_cpu[i] = insn;
            }
            if (((insn >> 40) & 0xffULL) >= 64ULL)
               continue;
         } else if (hw_scope == MALI_CS_SYNC_SCOPE_CSG /* 2 = CSG */) {
            insn &= ~MALI_CSF_INS_SYNC_ADD32_SCOPE_MASK; /* 0 = System */
            stream_cpu[i] = insn;
            if (stats)
               stats->sync_scope_upgrades++;
            continue;
         }
         if (((insn >> 40) & 0xffULL) >= 64ULL)
            continue;
      }

      /*
       * 1b. Real Valhall v10.xml/v11.xml FLUSH_CACHE2 (36=0x24):
       *     Opcode 0x24 is FLUSH_CACHE2 in hardware (and CSF_OP_SYNC_SET32 only in legacy
       *     synthetic unit-test vectors where bits [47:32] and [11:0] are 0).
       *     Always `continue` on real FLUSH_CACHE2 so `Wait Mask` is never corrupted!
       */
      if (opcode == MALI_CSF_OP_FLUSH_CACHE2 &&
          (((insn >> 32) & 0xffffULL) != 0ULL || (insn & 0xfffULL) != 0ULL)) {
         /* Ensure the preceding flush_id zero-init uses 64-bit MOVE48 (0x01) instead of
          * 32-bit MOVE32 (0x02) when targeting an even register (e.g. r66), so r67 never
          * retains a stale 0x0000005f high address word (preventing 0x0000005f00000010 MMU fault)! */
         if (i > 0U) {
            uint64_t prev_insn = stream_cpu[i - 1U];
            uint64_t prev_op = (prev_insn >> 56) & 0xffULL;
            uint8_t prev_dst = static_cast<uint8_t>((prev_insn >> 48) & 0xffU);
            if (prev_op == CSF_OP_MOVE32 && (prev_dst & 1U) == 0U && (prev_insn & 0xffffffffULL) == 0ULL) {
               stream_cpu[i - 1U] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(prev_dst) << 48);
            }
         }
         continue;
      }

      /*
       * 1c. Real Valhall v10.xml/v11.xml SYNC_WAIT32 (39=0x27) & SYNC_WAIT64 (53=0x35):
       *     Drain `SB_ID(DEFERRED_SYNC)` (scoreboard slot 1) before entering any
       *     cross-subqueue SYNC_WAIT32/64 (panvk-kbase-android Patch 0134), 100% outside
       *     `mesa_overlay`! In `cs_builder.h`, `cs_src64(b, addr)` always emits a
       *     `WAIT` (0x03) on `SB_ID(LS)` (slot 0) 1..3 instructions before `SYNC_WAIT32/64`.
       *     We OR `MALI_PANVK_SB_MASK_DEFERRED_SYNC` (bit 1 -> 0x00020000) into that `WAIT`.
       */
      if ((opcode == MALI_CSF_OP_SYNC_WAIT32 || opcode == MALI_CSF_OP_SYNC_WAIT64) &&
          ((insn >> 32) & 0xffffULL) != 0ULL) {
         (void)may_have_unwaited_deferred_sync;
         uint32_t min_j = (i >= 4U) ? (i - 4U) : 0U;
         for (uint32_t j = i; j > min_j; j--) {
            uint64_t prev = stream_cpu[j - 1U];
            if (((prev >> 56) & 0xffULL) == MALI_CSF_OP_WAIT) {
               if ((prev & def_wait_bit) == 0ULL) {
                  stream_cpu[j - 1U] = prev | def_wait_bit;
                  if (stats)
                     stats->deferred_sync_waits++;
               }
               may_have_unwaited_deferred_sync = false;
               break;
            }
         }
         continue;
      }

      /*
       * 2. Real Valhall v10.xml/v11.xml HEAP_OPERATION (49=0x31):
       *    `Operation` is at bits [34:32] (`0 = VT_START`, `1 = VT_END`, `3 = FRAG_COMPLETED`)
       *    and `Wait Mask` is at bits [31:16]. Rewrite non-VT_START heap ops to WAIT/NOP.
       */
      if (opcode == MALI_CSF_OP_HEAP_OPERATION) {
         uint64_t hw_mode = (insn & MALI_CSF_INS_HEAP_OPERATION_OPERATION_MASK) >>
                            MALI_CSF_INS_HEAP_OPERATION_OPERATION_SHIFT;
         if (hw_mode != MALI_CS_HEAP_OPERATION_VERTEX_TILER_STARTED) {
            uint64_t wait_mask = insn & MALI_CSF_INS_HEAP_OPERATION_WAIT_MASK_MASK;
            stream_cpu[i] = wait_mask ? ((MALI_CSF_OP_WAIT << 56) | wait_mask) : 0ULL;
            if (stats)
               stats->heap_op_nops++;
            continue;
         }
         if (((insn >> 28) & 0x3ULL) == 0ULL)
            continue;
      }

      /*
       * 3. Real Valhall v10.xml/v11.xml FINISH_FRAGMENT (11=0x0b):
       *    Bit 0 is `Increment Fragment Completed`. On Kbase (`mali_kbase_csf.c:1977`),
       *    if `vt_end == 0` (from step 2) and `frag_end > 0`, `handle_oom_event`
       *    rejects tiler chunk growth with `frag_end > vt_end -> -EINVAL` and
       *    terminates the queue group! Clearing bit 0 recycles the tiler heap
       *    chunks (`first_heap_chunk..last_heap_chunk`) without incrementing `frag_end`.
       */
      if (opcode == MALI_CSF_OP_FINISH_FRAGMENT &&
          (insn & MALI_CSF_INS_FINISH_FRAGMENT_INCREMENT_FRAGMENT_COMPLETED_MASK) != 0ULL &&
          ((insn >> 1) & 0x7fffULL) == 0ULL) {
         insn &= ~MALI_CSF_INS_FINISH_FRAGMENT_INCREMENT_FRAGMENT_COMPLETED_MASK;
         stream_cpu[i] = insn;
         if (stats)
            stats->finish_frag_stat_clears++;
         continue;
      }

      /*
       * 4. Legacy/synthetic test vector normalization (only when registers bits [47:32] == 0):
       */
      if ((opcode == CSF_OP_SYNC_ADD32 || opcode == CSF_OP_SYNC_SET32 ||
           opcode == CSF_OP_SYNC_ADD64 || opcode == CSF_OP_SYNC_SET64) &&
          ((insn >> 40) & 0xffULL) < 64ULL &&
          (opcode != CSF_OP_SYNC_SET32 || (insn & 0xffULL) == 0ULL)) {
         uint64_t scope = (insn >> 28) & 0x3ULL;
         if (scope == CSF_SYNC_SCOPE_CSG) {
            insn &= ~(0x3ULL << 28);
            insn |= (CSF_SYNC_SCOPE_SYSTEM << 28);
            insn |= (1ULL << 24);
            stream_cpu[i] = insn;
            if (stats)
               stats->sync_scope_upgrades++;
         }
      } else if (opcode == CSF_OP_HEAP_OPERATION && ((insn >> 48) & 0xffULL) == 0ULL) {
         uint64_t mode = (insn >> 28) & 0x3ULL;
         if (mode != 0) {
            stream_cpu[i] = 0ULL; /* NOP */
            if (stats)
               stats->heap_op_nops++;
         }
      }
   }
}

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
                                       bool emit_breadcrumbs)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_emit_ring_slot_ex",
                           "arch=%u, subq=%u, stream_addr=0x%llx, size=%u, seqno=%llu",
                           arch_major, subqueue_index,
                           static_cast<unsigned long long>(stream_addr), stream_size,
                           static_cast<unsigned long long>(job_seqno));
   size_t idx = 0;
   uint32_t call_addr_reg = (cs_reg_count >= 64) ? (cs_reg_count - 4) : 60;
   uint32_t call_size_reg = call_addr_reg + 2;
   uint32_t flush_id_reg  = call_addr_reg + 2;
   /* On cs_reg_count == 96 (nr_kernel_registers = 4), cs_subqueue_ctx_reg is r90 (96 - 6 = 90 = 0x5a) */
   uint32_t subq_ctx_reg  = (cs_reg_count >= 96) ? (cs_reg_count - 6) : 56;
   uint16_t sb_mask       = all_sb_mask ? all_sb_mask : 0x00ffU;

   /*
    * Resolve diagnostic breadcrumb target VA:
    * When g_ctx.diag_bo_gpu_va is allocated (BASE_MEM_COHERENT_LOCAL), ALWAYS write
    * STORE_MULTIPLE breadcrumbs (`pre_call`, `post_call`, `post_wait`, `ls_copy`) into
    * `diag_bo` instead of `event_bo` (BASE_MEM_CSF_EVENT) so we never mix Non-Shareable
    * LS stores and Shareable `SYNC_ADD64.system` atomics on the same 64-byte L2 cache line!
    */
   uint64_t diag_va = seqno_gpu_va;
   if (g_ctx.diag_bo_gpu_va != 0 && seqno_gpu_va >= g_ctx.event_bo_gpu_va &&
       seqno_gpu_va < g_ctx.event_bo_gpu_va + g_ctx.event_bo_size) {
      diag_va = g_ctx.diag_bo_gpu_va + (seqno_gpu_va - g_ctx.event_bo_gpu_va);
   }

   auto emit_store64_imm = [&](uint64_t dst_va, uint64_t val, int16_t offset) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_addr_reg) << 48) |
                         (dst_va & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_size_reg) << 48) |
                         (val & 0xffffffffffffULL);
      ring_slot[idx++] = (HW_CSF_OP_STORE_MULTIPLE << 56) |
                         (static_cast<uint64_t>(call_size_reg) << 48) |
                         (static_cast<uint64_t>(call_addr_reg) << 40) |
                         (0x3ULL << 16) |
                         (static_cast<uint16_t>(offset) & 0xffffULL);
      ring_slot[idx++] = (CSF_OP_WAIT << 56) | (1ULL << 16);
   };

   if (arch_major >= 11) {
      ring_slot[idx++] = (HW_CSF_OP_SET_STATE_IMM32 << 56) | (0ULL << 32) | 0x03ULL;
      ring_slot[idx++] = (HW_CSF_OP_SET_STATE_IMM32 << 56) | (9ULL << 32) | 0x08ULL;
      ring_slot[idx++] = (HW_CSF_OP_SET_STATE_IMM32 << 56) | (1ULL << 32) | 0x00ULL;
      ring_slot[idx++] = (HW_CSF_OP_SET_STATE_IMM32 << 56) | (2ULL << 32) | 0x01ULL;
      ring_slot[idx++] = (HW_CSF_OP_SET_STATE_IMM32 << 56) | (8ULL << 32) | 0xf0ULL;
   } else {
      /* Keep CSF_OP_SET_SB_ENTRY at ring_slot[0] for v10 + unit_tests_edge_cases */
      ring_slot[idx++] = (CSF_OP_SET_SB_ENTRY << 56) | 2ULL;
   }

   uint32_t res_mask = (subqueue_index == 0) ? (CSF_RES_IDVS | CSF_RES_TILER) :
                       (subqueue_index == 1) ? CSF_RES_FRAGMENT : CSF_RES_COMPUTE;
   ring_slot[idx++] = (HW_CSF_OP_REQ_RESOURCE << 56) | static_cast<uint64_t>(res_mask);

   if (subq_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(subq_ctx_reg) << 48) |
                         (subq_ctx_va & 0xffffffffffffULL);
   }

   if (subqueue_index != 2 && tiler_heap_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_addr_reg) << 48) |
                         (tiler_heap_ctx_va & 0xffffffffffffULL);
      ring_slot[idx++] = (HW_CSF_OP_HEAP_SET << 56) | (static_cast<uint64_t>(call_addr_reg) << 40);
   }

   if (stream_size > 0 && stream_addr != 0) {
      /* Pre-CALL cache flush: L2=CLEAN_INV(3), LSC=CLEAN_INV(3), Other=INVALIDATE(2) (0x233), signal_slot=0 */
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | (static_cast<uint64_t>(flush_id_reg) << 48) |
                         static_cast<uint64_t>(latest_flush);
      ring_slot[idx++] = (HW_CSF_OP_FLUSH_CACHE2 << 56) |
                         (0ULL << 48) |
                         (static_cast<uint64_t>(flush_id_reg) << 40) |
                         (2ULL << 8) | (3ULL << 4) | 3ULL;
      ring_slot[idx++] = (CSF_OP_WAIT << 56) | (1ULL << 16);

      if (emit_breadcrumbs && diag_va != 0 && job_seqno != 0)
         emit_store64_imm(diag_va, job_seqno, ADAPTER_SEQNO_PRE_CALL_OFFSET);

      /* Call command stream */
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_addr_reg) << 48) |
                         (stream_addr & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | (static_cast<uint64_t>(call_size_reg) << 48) |
                         static_cast<uint64_t>(stream_size);
      ring_slot[idx++] = (HW_CSF_OP_CALL << 56) | (static_cast<uint64_t>(call_addr_reg) << 40) |
                         (static_cast<uint64_t>(call_size_reg) << 32);

      if (emit_breadcrumbs && diag_va != 0 && job_seqno != 0)
         emit_store64_imm(diag_va, job_seqno, ADAPTER_SEQNO_POST_CALL_OFFSET);

      /* Wait for all scoreboard slots */
      ring_slot[idx++] = (CSF_OP_WAIT << 56) | (static_cast<uint64_t>(sb_mask) << 16);

      if (emit_breadcrumbs && diag_va != 0 && job_seqno != 0) {
         emit_store64_imm(diag_va, job_seqno, ADAPTER_SEQNO_POST_WAIT_OFFSET);
         emit_store64_imm(diag_va, job_seqno, ADAPTER_SEQNO_LS_COPY_OFFSET);
      }
   }

   /*
    * Increment subqueue seqno via v10.xml/v11.xml CS SYNC_ADD64 (opcode 51 = 0x33):
    *   bit 0       = Error Propagate (1)
    *   bits [2:1]  = Scope (0 = MALI_CS_SYNC_SCOPE_SYSTEM)
    *   bits [31:16]= Wait Mask (0)
    *   bits [39:32]= Data register (call_size_reg)
    *   bits [47:40]= Address register (call_addr_reg)
    *   bits [51:48]= Signal slot (0)
    *   bit 52      = Defer Mode (0 = Defer Immediate)
    * Followed by ERROR_BARRIER (0x2f) and 64-byte cacheline NOP padding (Patch 007).
    * Never emit LOAD_MULTIPLE/STORE_MULTIPLE on `seqno_gpu_va` (`event_bo`)!
    */
   if (job_seqno != 0 && diag_va != 0 && diag_va != seqno_gpu_va) {
      /*
       * Runtime Kbase submission (job_seqno != 0):
       *  1. Write job_seqno to seqno_gpu_va (event_bo, Non-Cacheable DRAM, SH=00) via
       *     non-atomic STORE_MULTIPLE + WAIT(1<<16) so the CPU sees *sq->seqno_cpu_ptr
       *     immediately without executing an atomic SYNC_ADD on a Non-Shareable page.
       *  2. Execute 32-bit SYNC_ADD32 (0x25) with Scope = CSG (2, Inner-Shareable = 0x4)
       *     on diag_va + 56 (diag_bo, BASE_MEM_COHERENT_LOCAL, SH=10 Inner-Shareable) to
       *     fire the hardware CSF_NOTIFICATION_EVENT interrupt with 100% matching PTE shareability!
       */
      emit_store64_imm(seqno_gpu_va, job_seqno, ADAPTER_SEQNO_VALUE_OFFSET);
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_addr_reg) << 48) |
                         ((diag_va + 56ULL) & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | (static_cast<uint64_t>(call_size_reg) << 48) | 1ULL;
      ring_slot[idx++] = (HW_CSF_OP_SYNC_ADD32 << 56) |
                         (0ULL << 48) |
                         (static_cast<uint64_t>(call_addr_reg) << 40) |
                         (static_cast<uint64_t>(call_size_reg) << 32) |
                         (static_cast<uint64_t>(MALI_CS_SYNC_SCOPE_CSG) << MALI_CSF_INS_SYNC_ADD32_SCOPE_SHIFT) |
                         1ULL;
      ring_slot[idx++] = (HW_CSF_OP_ERROR_BARRIER << 56);
   } else {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_addr_reg) << 48) |
                         (seqno_gpu_va & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(call_size_reg) << 48) | 1ULL;
      ring_slot[idx++] = (HW_CSF_OP_SYNC_ADD64 << 56) |
                         (0ULL << 48) |
                         (static_cast<uint64_t>(call_addr_reg) << 40) |
                         (static_cast<uint64_t>(call_size_reg) << 32) |
                         1ULL;
      ring_slot[idx++] = (HW_CSF_OP_ERROR_BARRIER << 56);
   }

   while (idx < ADAPTER_RING_SLOT_QWORDS)
      ring_slot[idx++] = 0ULL;
   /* Always pad ring entries to a multiple of 64 bytes (8 qwords) for the CSF prefetcher */
   while ((idx & 7U) != 0U)
      ring_slot[idx++] = 0ULL;

   return idx * sizeof(uint64_t);
}

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
   return panthor_kbase_emit_ring_slot_ex(
      ring_slot, arch_major, cs_reg_count, subqueue_index,
      subq_ctx_va, tiler_heap_ctx_va, stream_addr, stream_size,
      latest_flush, seqno_gpu_va, all_sb_mask, 0ULL, false);
}

int panthor_kbase_adapter_open(const char *path, int flags)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_open", "path=%s, flags=0x%x",
                           path ? path : "(null)", flags);
   resolve_real_funcs();
   pthread_mutex_lock(&g_lock);

   /* If an active context already exists, return a new duplicated virtual FD */
   if (g_ctx.active && g_ctx.mali_fd >= 0) {
      int dup_fd = fcntl(g_ctx.mali_fd, F_DUPFD_CLOEXEC, 3);
      if (dup_fd < 0)
         dup_fd = dup(g_ctx.mali_fd);
      if (dup_fd >= 0) {
         register_virtual_fd_locked(dup_fd);
         LOG_INFO("adapter_open: reusing active kbase context (mali_fd=%d -> dup_fd=%d, refcnt=%d)",
                  g_ctx.mali_fd, dup_fd, g_ctx.open_refcount);
         pthread_mutex_unlock(&g_lock);
         return dup_fd;
      }
   }

   const char *mali_path = getenv("PANVK_KBASE_NODE_PATH");
   if (!mali_path || !mali_path[0])
      mali_path = "/dev/mali0";
   (void)path;

   int raw_fd = real_open(mali_path, flags | O_RDWR | O_CLOEXEC, 0);
   if (raw_fd < 0) {
      LOG_ERROR("adapter_open: real_open(%s) failed: %s", mali_path, strerror(errno));
      pthread_mutex_unlock(&g_lock);
      return -1;
   }

   int priv_mali_fd = fcntl(raw_fd, F_DUPFD_CLOEXEC, 3);
   if (priv_mali_fd < 0)
      priv_mali_fd = raw_fd;

   memset(&g_ctx, 0, sizeof(g_ctx));
   for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++)
      g_ctx.virtual_fds[i] = -1;
   g_ctx.drm_fd = raw_fd;
   g_ctx.mali_fd = priv_mali_fd;

   struct stat st;
   if (fstat(priv_mali_fd, &st) == 0) {
      g_ctx.mali_dev = st.st_dev;
      g_ctx.mali_ino = st.st_ino;
      g_ctx.mali_rdev = st.st_rdev;
   }

   if (init_kbase_context(&g_ctx, priv_mali_fd) < 0) {
      LOG_ERROR("adapter_open: init_kbase_context failed for fd=%d", priv_mali_fd);
      if (priv_mali_fd != raw_fd)
         real_close(priv_mali_fd);
      real_close(raw_fd);
      g_ctx.active = false;
      pthread_mutex_unlock(&g_lock);
      return -1;
   }

   register_virtual_fd_locked(raw_fd);
   if (priv_mali_fd != raw_fd)
      register_virtual_fd_locked(priv_mali_fd);

   uint32_t cap_gpu_id = g_ctx.gpu_info.gpu_id;
   uint32_t cap_arch   = g_ctx.arch_major;
   uint32_t cap_regs   = g_ctx.csif_info.cs_reg_count;
   uint32_t cap_sbs    = g_ctx.csif_info.scoreboard_slot_count;
   pthread_mutex_unlock(&g_lock);
   panthor_drm_capture_init_from_env(cap_gpu_id, cap_arch, cap_regs, cap_sbs);
   LOG_INFO("adapter_open: initialized mali_fd=%d user_fd=%d refcnt=%d",
            priv_mali_fd, raw_fd, g_ctx.open_refcount);
   return raw_fd;
}

static bool g_dup_tracking_enabled = true;
static bool g_dup_tracking_env_checked = false;

void panthor_kbase_adapter_set_dup_tracking(bool enable)
{
   pthread_mutex_lock(&g_lock);
   g_dup_tracking_enabled = enable;
   g_dup_tracking_env_checked = true;
   pthread_mutex_unlock(&g_lock);
}

bool panthor_kbase_adapter_get_dup_tracking(void)
{
   pthread_mutex_lock(&g_lock);
   if (!g_dup_tracking_env_checked) {
      const char *env = getenv("PANTHOR_KBASE_DUP_TRACKING");
      if (env)
         g_dup_tracking_enabled = (atoi(env) != 0);
      g_dup_tracking_env_checked = true;
   }
   bool ret = g_dup_tracking_enabled;
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int panthor_kbase_adapter_dup(int fd)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_dup", "fd=%d", fd);
   bool track_dup = panthor_kbase_adapter_get_dup_tracking();
   pthread_mutex_lock(&g_lock);
   if (panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      if (dup_fd < 0)
         dup_fd = dup(fd);
      if (dup_fd >= 0) {
         register_virtual_fd_locked(dup_fd);
         LOG_DEBUG("adapter_dup (dup_tracking=%d): fd=%d -> dup_fd=%d (refcnt=%d)",
                   track_dup ? 1 : 0, fd, dup_fd, g_ctx.open_refcount);
         pthread_mutex_unlock(&g_lock);
         return dup_fd;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return dup(fd);
}

static thread_local bool tls_in_adapter_close = false;
static thread_local bool tls_in_adapter_mmap = false;

static void *safe_real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
   tls_in_adapter_mmap = true;
   void *res = real_mmap(addr, length, prot, flags, fd, offset);
   tls_in_adapter_mmap = false;
   return res;
}

int panthor_kbase_adapter_close(int fd)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_close", "fd=%d", fd);
   resolve_real_funcs();
   if (tls_in_adapter_close)
      return real_close(fd);
   pthread_mutex_lock(&g_lock);
   if (g_ctx.active && panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      if (fd == g_ctx.mali_fd && g_ctx.open_refcount > 1) {
         LOG_DEBUG("adapter_close: keeping internal mali_fd=%d alive (refcnt=%d)",
                   fd, g_ctx.open_refcount);
         pthread_mutex_unlock(&g_lock);
         return 0;
      }
      unregister_virtual_fd_locked(fd);
      int remaining_threshold = (g_ctx.mali_fd >= 0 && g_ctx.mali_fd != fd) ? 1 : 0;
      if (g_ctx.open_refcount <= remaining_threshold) {
         LOG_INFO("adapter_close: last virtual FD %d closed; tearing down kbase context (mali_fd=%d)",
                  fd, g_ctx.mali_fd);
         g_ctx.active = false;
         tls_in_adapter_close = true;
         if (g_ctx.has_kcpu_queue) {
            struct kbase_ioctl_kcpu_queue_delete del = { .id = g_ctx.kcpu_queue_id, .padding = {0} };
            adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_DELETE, &del);
         }
         if (g_ctx.stream_fd >= 0 && g_ctx.stream_fd != g_ctx.mali_fd)
            real_close(g_ctx.stream_fd);
         if (g_ctx.shadow_cs_bo_cpu)
            real_munmap(g_ctx.shadow_cs_bo_cpu, g_ctx.shadow_cs_bo_size);
         if (g_ctx.diag_bo_cpu)
            real_munmap(g_ctx.diag_bo_cpu, g_ctx.diag_bo_size);
         if (g_ctx.event_bo_cpu)
            real_munmap(g_ctx.event_bo_cpu, g_ctx.event_bo_size);
         if (g_ctx.user_reg_page)
            real_munmap(g_ctx.user_reg_page, 4096);
         if (g_ctx.tracking_page)
            real_munmap(g_ctx.tracking_page, 4096);
         if (g_ctx.mali_fd >= 0 && g_ctx.mali_fd != fd)
            real_close(g_ctx.mali_fd);
         tls_in_adapter_close = false;
      }
   }
   pthread_mutex_unlock(&g_lock);
   tls_in_adapter_close = true;
   int rc = real_close(fd);
   tls_in_adapter_close = false;
   return rc;
}

bool panthor_kbase_adapter_is_virtual_drm_fd(int fd)
{
   if (fd < 0 || !g_ctx.active || tls_in_adapter_close)
      return false;
   if (fd == g_ctx.stream_fd)
      return false;
   if (fd == g_ctx.drm_fd || fd == g_ctx.mali_fd)
      return true;
   for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++) {
      if (g_ctx.virtual_fds[i] == fd)
         return true;
   }
   struct stat st;
   if (g_ctx.mali_rdev != 0 && fstat(fd, &st) == 0 && st.st_rdev == g_ctx.mali_rdev) {
      pthread_mutex_lock(&g_lock);
      register_virtual_fd_locked(fd);
      pthread_mutex_unlock(&g_lock);
      return true;
   }
   return false;
}

int panthor_kbase_adapter_ioctl(int fd, unsigned long request, void *arg)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_ioctl", "fd=%d, req=%s(0x%lx)",
                           fd, panthor2kbase_ioctl_req_name(request), request);
   resolve_real_funcs();
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      LOG_DEBUG("ioctl passthrough fd=%d req=%s", fd, panthor2kbase_ioctl_req_name(request));
      int rc = real_ioctl(fd, request, arg);
      if (rc < 0 && _IOC_TYPE(request) == 'b') {
         unsigned int bnr = _IOC_NR(request);
         if (bnr == 0 /* DMA_BUF_IOCTL_SYNC */ ||
             bnr == 3 /* DMA_BUF_IOCTL_IMPORT_SYNC_FILE */) {
            errno = 0;
            return 0;
         }
         if (bnr == 2 /* DMA_BUF_IOCTL_EXPORT_SYNC_FILE */ && arg != nullptr && g_ctx.active) {
            struct dmabuf_export_sync_arg { uint32_t flags; int32_t fd; };
            auto *es = static_cast<struct dmabuf_export_sync_arg *>(arg);
            es->fd = (g_ctx.stream_fd >= 0) ? dup(g_ctx.stream_fd) : dup(g_ctx.mali_fd);
            if (es->fd >= 0) {
               errno = 0;
               return 0;
            }
         }
      }
      return rc;
   }

   pthread_mutex_lock(&g_lock);
   unsigned int nr = _IOC_NR(request);
   unsigned int type = _IOC_TYPE(request);
   int ret = 0;

   bool is_hot_ioctl = (type == 'd') &&
      (nr == 0xbf || nr == 0xc0 || nr == 0xc3 || nr == 0xc4 || nr == 0xc5 ||
       nr == 0xca || nr == 0xcb || nr == 0xcc || nr == 0xcd ||
       nr == (DRM_COMMAND_BASE + DRM_PANTHOR_GROUP_SUBMIT) ||
       nr == (DRM_COMMAND_BASE + DRM_PANTHOR_GROUP_GET_STATE) ||
       nr == (DRM_COMMAND_BASE + DRM_PANTHOR_VM_GET_STATE) ||
       nr == (DRM_COMMAND_BASE + DRM_PANTHOR_BO_SYNC));
   if (is_hot_ioctl && g_ctx.total_group_submits >= 5ULL) {
      LOG_DEBUG("ioctl intercepted: req=%s(0x%lx) fd=%d nr=%u type=%c",
                panthor2kbase_ioctl_req_name(request), request, fd, nr,
                type ? static_cast<char>(type) : '?');
   } else {
      LOG_INFO("ioctl intercepted: req=%s(0x%lx) fd=%d nr=%u type=%c",
               panthor2kbase_ioctl_req_name(request), request, fd, nr,
               type ? static_cast<char>(type) : '?');
   }
   panthor2kbase_log_ioctl_struct_in(request, arg);

   if (type != 'd') {
      pthread_mutex_unlock(&g_lock);
      return real_ioctl(fd, request, arg);
   }

   /* Standard DRM GEM / PRIME / SYNCOBJ IOCTLs */
   switch (nr) {
   case 0x00: { /* DRM_IOCTL_VERSION */
      struct drm_version *v = static_cast<struct drm_version *>(arg);
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
      LOG_INFO("DRM_IOCTL_VERSION -> major=1 minor=3 patch=0");
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x0c: { /* DRM_IOCTL_GET_CAP */
      struct drm_get_cap_arg { uint64_t capability; uint64_t value; };
      auto *cap = static_cast<struct drm_get_cap_arg *>(arg);
      if (cap)
         cap->value = 1ULL;
      LOG_INFO("DRM_IOCTL_GET_CAP capability=%llu -> value=%llu",
               static_cast<unsigned long long>(cap ? cap->capability : 0),
               static_cast<unsigned long long>(cap ? cap->value : 0));
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x0d: { /* DRM_IOCTL_SET_CLIENT_CAP */
      LOG_INFO("DRM_IOCTL_SET_CLIENT_CAP");
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc1: { /* DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD */
      auto *sh = static_cast<struct drm_syncobj_handle *>(arg);
      if (!sh) {
         pthread_mutex_unlock(&g_lock);
         errno = EINVAL;
         return -1;
      }
      struct adapter_syncobj *so = find_syncobj(sh->handle);
      if ((sh->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) != 0) {
         if (!so || (so->value == 0ULL && so->target_group_handle == 0)) {
            pthread_mutex_unlock(&g_lock);
            errno = EINVAL;
            return -1;
         }
         /* If exported as a sync_file while GPU work is in flight, wait briefly or ensure fence is valid */
         if (so->target_group_handle != 0) {
            struct adapter_group *grp = find_group(so->target_group_handle);
            if (grp) {
               for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
                  struct adapter_subqueue *sq = &grp->subqueues[q];
                  uint64_t need = so->target_subq_seqno[q];
                  if (need > 0 && sq->seqno_cpu_ptr) {
                     uint64_t w_start = get_monotonic_ns();
                     while (*sq->seqno_cpu_ptr < need && (get_monotonic_ns() - w_start) < 50000000ULL) {
                        kbase_cpu_cache_invalidate_range(
                           const_cast<uint64_t *>(sq->seqno_cpu_ptr), ADAPTER_SEQNO_CELL_STRIDE);
                        if (*sq->seqno_cpu_ptr >= need)
                           break;
                        usleep(10);
                     }
                  }
               }
            }
         }
      }
      sh->fd = (g_ctx.stream_fd >= 0) ? dup(g_ctx.stream_fd) : dup(g_ctx.mali_fd);
      LOG_DEBUG("DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD handle=%u flags=0x%x -> fd=%d",
                sh->handle, sh->flags, sh->fd);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc2: { /* DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE */
      auto *sh = static_cast<struct drm_syncobj_handle *>(arg);
      if (sh) {
         for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
            if (!g_ctx.syncobjs[i].in_use) {
               g_ctx.syncobjs[i].in_use = true;
               g_ctx.syncobjs[i].handle = g_ctx.next_syncobj_handle++;
               g_ctx.syncobjs[i].value = 1ULL;
               if (i + 1 > g_ctx.max_syncobj_slot_used)
                  g_ctx.max_syncobj_slot_used = static_cast<uint32_t>(i + 1);
               sh->handle = g_ctx.syncobjs[i].handle;
               break;
            }
         }
      }
      LOG_INFO("DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE -> handle=%u", sh ? sh->handle : 0);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x09: { /* DRM_IOCTL_GEM_CLOSE */
      auto *cl = static_cast<struct drm_gem_close *>(arg);
      struct adapter_bo *bo = find_bo(cl->handle);
      if (bo) {
         void *alias_ptr = bo->alias_ptr;
         size_t alias_sz = bo->alias_size;
         void *gpu_map_ptr = bo->gpu_mapping;
         size_t gpu_map_sz = bo->gpu_mapping_size;
         void *unmap_ptr = (bo->cpu_ptr && bo->is_same_va) ? bo->cpu_ptr : nullptr;
         void *exec_cpu_ptr = (bo->cpu_ptr && !bo->is_same_va) ? bo->cpu_ptr : nullptr;
         size_t unmap_sz = bo->cpu_size;
         uint64_t free_va = (!bo->is_same_va && !gpu_map_ptr && bo->gpu_va) ? bo->gpu_va : 0;
         uint32_t h = bo->gem_handle;
         uint64_t va = bo->gpu_va;
         if (bo->dmabuf_fd >= 0)
            real_close(bo->dmabuf_fd);
         memset(bo, 0, sizeof(*bo));
         pthread_mutex_unlock(&g_lock);
         if (alias_ptr && alias_sz > 0)
            real_munmap(alias_ptr, alias_sz);
         if (gpu_map_ptr && gpu_map_ptr != unmap_ptr && gpu_map_ptr != exec_cpu_ptr && gpu_map_sz > 0)
            real_munmap(gpu_map_ptr, gpu_map_sz);
         if (unmap_ptr) {
            real_munmap(unmap_ptr, unmap_sz);
         } else {
            if (exec_cpu_ptr)
               real_munmap(exec_cpu_ptr, unmap_sz);
            if (free_va) {
               struct kbase_ioctl_mem_free mf = { .gpu_addr = free_va };
               adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_FREE, &mf);
            }
         }
         LOG_INFO("DRM_IOCTL_GEM_CLOSE handle=%u -> freed va=0x%llx",
                  h, static_cast<unsigned long long>(va));
         return 0;
      }
      LOG_WARN("DRM_IOCTL_GEM_CLOSE handle=%u not found", cl ? cl->handle : 0);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0x2d: { /* DRM_IOCTL_PRIME_HANDLE_TO_FD */
      auto *ph = static_cast<struct drm_prime_handle *>(arg);
      struct adapter_bo *bo = find_bo(ph->handle);
      if (!bo) {
         LOG_WARN("DRM_IOCTL_PRIME_HANDLE_TO_FD handle=%u not found", ph ? ph->handle : 0);
         pthread_mutex_unlock(&g_lock);
         errno = ENOENT;
         return -1;
      }
      if (bo->dmabuf_fd < 0) {
         int mfd = memfd_create("panvk-dmabuf-export", MFD_CLOEXEC);
         if (mfd >= 0 && ftruncate(mfd, static_cast<off_t>(bo->size)) == 0) {
            struct stat st;
            bo->dmabuf_fd = mfd;
            if (fstat(mfd, &st) == 0)
               bo->dmabuf_ino = static_cast<uint64_t>(st.st_ino);
         }
      }
      ph->fd = dup(bo->dmabuf_fd);
      LOG_INFO("DRM_IOCTL_PRIME_HANDLE_TO_FD handle=%u -> fd=%d",
               ph ? ph->handle : 0, ph ? ph->fd : -1);
      pthread_mutex_unlock(&g_lock);
      return (ph->fd >= 0) ? 0 : -1;
   }
   case 0x2e: { /* DRM_IOCTL_PRIME_FD_TO_HANDLE */
      auto *ph = static_cast<struct drm_prime_handle *>(arg);
      struct stat st;
      bool have_stat = (fstat(ph->fd, &st) == 0);
      uint64_t sz = (have_stat && st.st_size > 0) ? static_cast<uint64_t>(st.st_size) : 65536ULL;
      if (have_stat && st.st_ino != 0) {
         for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
            if (g_ctx.bos[i].in_use && g_ctx.bos[i].dmabuf_ino == static_cast<uint64_t>(st.st_ino)) {
               LOG_INFO("DRM_IOCTL_PRIME_FD_TO_HANDLE fd=%d -> existing handle=%u",
                        ph->fd, g_ctx.bos[i].gem_handle);
               ph->handle = g_ctx.bos[i].gem_handle;
               pthread_mutex_unlock(&g_lock);
               return 0;
            }
         }
      }
      int fd_copy = dup(ph->fd);
      union kbase_ioctl_mem_import imp;
      memset(&imp, 0, sizeof(imp));
      imp.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                     BASE_MEM_IMPORT_SHARED | BASE_MEM_COHERENT_SYSTEM |
                     BASE_MEM_COHERENT_LOCAL;
      imp.in.phandle = reinterpret_cast<uintptr_t>(&fd_copy);
      imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;

      int ret_imp = adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT, &imp);
      if (ret_imp < 0 && errno == ENOTTY) {
         union kbase_ioctl_mem_import_mtk imp_mtk;
         memset(&imp_mtk, 0, sizeof(imp_mtk));
         imp_mtk.in.flags = imp.in.flags;
         imp_mtk.in.phandle = reinterpret_cast<uintptr_t>(&fd_copy);
         imp_mtk.in.type = BASE_MEM_IMPORT_TYPE_UMM;
         ret_imp = adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT_MTK, &imp_mtk);
         if (ret_imp == 0) {
            imp.out.gpu_va = imp_mtk.out.gpu_va;
            imp.out.va_pages = imp_mtk.out.va_pages;
            imp.out.flags = imp_mtk.out.flags;
         }
      }
      if (ret_imp < 0) {
         close(fd_copy);
         pthread_mutex_unlock(&g_lock);
         return -1;
      }

      uint64_t bo_sz = imp.out.va_pages ? (imp.out.va_pages * 4096ULL) : sz;
      uint64_t resolved_gpu_va = imp.out.gpu_va;
      void *gpu_map_ptr = nullptr;
      void *imported_cpu_ptr = nullptr;
      if (imp.out.flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)) {
         void *gpu_map = real_mmap(nullptr, static_cast<size_t>(bo_sz),
                                   PROT_READ | PROT_WRITE, MAP_SHARED,
                                   g_ctx.mali_fd, static_cast<off_t>(imp.out.gpu_va));
         if (gpu_map != MAP_FAILED) {
            resolved_gpu_va = reinterpret_cast<uintptr_t>(gpu_map);
            gpu_map_ptr = gpu_map;
            imported_cpu_ptr = gpu_map;
         }
      }
      if (!imported_cpu_ptr && fd_copy >= 0) {
         void *dmabuf_cpu = real_mmap(nullptr, static_cast<size_t>(bo_sz),
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      fd_copy, 0);
         if (dmabuf_cpu != MAP_FAILED)
            imported_cpu_ptr = dmabuf_cpu;
      }

      for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
         if (!g_ctx.bos[i].in_use) {
            if (i + 1 > g_ctx.max_bo_slot_used)
               g_ctx.max_bo_slot_used = static_cast<uint32_t>(i + 1);
            struct adapter_bo *bo = &g_ctx.bos[i];
            memset(bo, 0, sizeof(*bo));
            bo->in_use = true;
            bo->gem_handle = g_ctx.next_gem_handle++;
            bo->size = bo_sz;
            bo->kbase_flags = imp.out.flags;
            bo->gpu_va = resolved_gpu_va;
            bo->gpu_mapping = gpu_map_ptr;
            bo->gpu_mapping_size = gpu_map_ptr ? static_cast<size_t>(bo_sz) : 0;
            bo->cpu_ptr = imported_cpu_ptr;
            bo->cpu_size = static_cast<size_t>(bo_sz);
            bo->dmabuf_fd = fd_copy;
            bo->dmabuf_ino = have_stat ? static_cast<uint64_t>(st.st_ino) : 0;
            bo->is_imported = true;
            bo->is_same_va = (imported_cpu_ptr != nullptr && imported_cpu_ptr == gpu_map_ptr);
            ph->handle = bo->gem_handle;
            LOG_INFO("DRM_IOCTL_PRIME_FD_TO_HANDLE fd=%d -> KBASE_IOCTL_MEM_IMPORT va=0x%llx handle=%u",
                     ph->fd, static_cast<unsigned long long>(bo->gpu_va), bo->gem_handle);
            pthread_mutex_unlock(&g_lock);
            return 0;
         }
      }
      close(fd_copy);
      LOG_ERROR("DRM_IOCTL_PRIME_FD_TO_HANDLE fd=%d: no free BO slot", ph->fd);
      pthread_mutex_unlock(&g_lock);
      errno = ENOMEM;
      return -1;
   }
   case 0xbf: { /* DRM_IOCTL_SYNCOBJ_CREATE */
      auto *sc = static_cast<struct drm_syncobj_create *>(arg);
      uint32_t next_h = g_ctx.next_syncobj_handle ? g_ctx.next_syncobj_handle : 1U;
      size_t pref_idx = static_cast<size_t>((next_h - 1U) % ADAPTER_MAX_SYNCOBJS);
      for (size_t step = 0; step < ADAPTER_MAX_SYNCOBJS; step++) {
         size_t i = (pref_idx + step) % ADAPTER_MAX_SYNCOBJS;
         if (!g_ctx.syncobjs[i].in_use) {
            if (i + 1 > g_ctx.max_syncobj_slot_used)
               g_ctx.max_syncobj_slot_used = static_cast<uint32_t>(i + 1);
            struct adapter_syncobj *so = &g_ctx.syncobjs[i];
            memset(so, 0, sizeof(*so));
            so->in_use = true;
            so->handle = g_ctx.next_syncobj_handle++;
            so->value = (sc->flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1ULL : 0ULL;
            so->completed_value = so->value;
            if (g_ctx.event_bo_cpu) {
               size_t off = 2048 + i * 16;
               so->cqs_gpu_va = g_ctx.event_bo_gpu_va + off;
               so->cqs_cpu_ptr = reinterpret_cast<uint64_t *>(g_ctx.event_bo_cpu + off);
               *so->cqs_cpu_ptr = so->value;
            }
            sc->handle = so->handle;
            if (g_ctx.total_group_submits < 5ULL)
               LOG_INFO("DRM_IOCTL_SYNCOBJ_CREATE -> handle=%u signaled=%d",
                        so->handle, (so->value != 0));
            else
               LOG_DEBUG("DRM_IOCTL_SYNCOBJ_CREATE -> handle=%u signaled=%d",
                         so->handle, (so->value != 0));
            pthread_mutex_unlock(&g_lock);
            panthor_drm_capture_record_ioctl(fd, request, arg, 0);
            return 0;
         }
      }
      LOG_ERROR("DRM_IOCTL_SYNCOBJ_CREATE: no free syncobj slot");
      pthread_mutex_unlock(&g_lock);
      errno = ENOMEM;
      return -1;
   }
   case 0xc0: { /* DRM_IOCTL_SYNCOBJ_DESTROY */
      auto *sd = static_cast<struct drm_syncobj_destroy *>(arg);
      struct adapter_syncobj *so = find_syncobj(sd->handle);
      if (so)
         memset(so, 0, sizeof(*so));
      if (g_ctx.total_group_submits < 5ULL)
         LOG_INFO("DRM_IOCTL_SYNCOBJ_DESTROY handle=%u", sd ? sd->handle : 0);
      else
         LOG_DEBUG("DRM_IOCTL_SYNCOBJ_DESTROY handle=%u", sd ? sd->handle : 0);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc3: /* DRM_IOCTL_SYNCOBJ_WAIT */
   case 0xca: { /* DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT */
      uint64_t handles_ptr = 0;
      uint64_t points_ptr = 0;
      uint32_t count_handles = 0;
      uint32_t wait_flags = 0;
      int64_t timeout_nsec = 0;
      uint32_t *first_signaled_out = nullptr;
      if (nr == 0xc3) {
         auto *sw = static_cast<struct drm_syncobj_wait *>(arg);
         if (sw) {
            handles_ptr = sw->handles;
            count_handles = sw->count_handles;
            wait_flags = sw->flags;
            timeout_nsec = sw->timeout_nsec;
            sw->first_signaled = 0;
            first_signaled_out = &sw->first_signaled;
         }
      } else {
         auto *tw = static_cast<struct drm_syncobj_timeline_wait *>(arg);
         if (tw) {
            handles_ptr = tw->handles;
            points_ptr = tw->points;
            count_handles = tw->count_handles;
            wait_flags = tw->flags;
            timeout_nsec = tw->timeout_nsec;
            tw->first_signaled = 0;
            first_signaled_out = &tw->first_signaled;
         }
      }

      if (!handles_ptr || count_handles == 0 || count_handles > 64) {
         pthread_mutex_unlock(&g_lock);
         return 0;
      }

      uint32_t local_handles[64];
      uint64_t local_points[64];
      const auto *handles_in = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(handles_ptr));
      const auto *points_in  = reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(points_ptr));
      for (uint32_t i = 0; i < count_handles; i++) {
         local_handles[i] = handles_in[i];
         local_points[i]  = (nr == 0xca && points_in && points_in[i] > 0ULL) ? points_in[i] : 1ULL;
      }

      uint32_t target_grp_id = 0;
      uint64_t needed_seqno[ADAPTER_MAX_SUBQUEUES] = {0};
      uint32_t first_sig_idx = 0;
      bool wait_all = (wait_flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0 || count_handles == 1U;
      bool wait_avail = (wait_flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0;

      /* Dynamically evaluate whether all (or any) requested syncobjs have:
       *   1. Been signaled/submitted to at least local_points[i] (so->value >= req_val), AND
       *   2. Completed their associated GPU subqueue jobs (unless WAIT_AVAILABLE is set).
       * Crucially, this prevents returning 0 (VK_SUCCESS) on unsignaled fences/semaphores
       * when bcn_layer's cleanup thread or DXVK/Mesa polls or waits before submission! */
      auto check_all_done = [&]() -> bool {
         target_grp_id = 0;
         memset(needed_seqno, 0, sizeof(needed_seqno));
         bool any_ready = false;
         bool all_ready = true;
         for (uint32_t i = 0; i < count_handles; i++) {
            struct adapter_syncobj *so = find_syncobj(local_handles[i]);
            if (!so) {
               if (!any_ready) first_sig_idx = i;
               any_ready = true;
               continue;
            }
            bool this_ready = true;
            if (so->value < local_points[i]) {
               this_ready = false;
            } else if (!wait_avail && so->target_group_handle != 0) {
               struct adapter_group *g = find_group(so->target_group_handle);
               if (g) {
                  for (uint32_t q = 0; q < g->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
                     struct adapter_subqueue *sq = &g->subqueues[q];
                     uint64_t need = so->target_subq_seqno[q];
                     if (need > 0 && sq->seqno_cpu_ptr) {
                        kbase_cpu_cache_invalidate_range(
                           const_cast<uint64_t *>(sq->seqno_cpu_ptr), ADAPTER_SEQNO_CELL_STRIDE);
                        if (*sq->seqno_cpu_ptr < need) {
                           this_ready = false;
                           target_grp_id = so->target_group_handle;
                           if (need > needed_seqno[q])
                              needed_seqno[q] = need;
                        }
                     }
                  }
               }
               if (this_ready)
                  so->completed_value = so->value;
            }
            if (this_ready) {
               if (!any_ready) first_sig_idx = i;
               any_ready = true;
            } else {
               all_ready = false;
            }
         }
         return wait_all ? all_ready : any_ready;
      };

      if (!check_all_done()) {
         if (timeout_nsec == 0) {
            pthread_mutex_unlock(&g_lock);
            errno = ETIME;
            return -1;
         }

         uint64_t start_ns = get_monotonic_ns();
         uint64_t max_wait_ns = 5000000000ULL; /* 5s per watchdog window */
         const char *env_to = getenv("PANTHOR_ADAPTER_WAIT_TIMEOUT_MS");
         if (env_to && env_to[0] && atoi(env_to) > 0)
            max_wait_ns = static_cast<uint64_t>(atoi(env_to)) * 1000000ULL;

         /* DRM_IOCTL_SYNCOBJ_WAIT / TIMELINE_WAIT uses absolute CLOCK_MONOTONIC timeout_nsec
          * (or INT64_MAX for infinite wait). Also support small relative timeouts gracefully. */
         uint64_t caller_budget_ns = max_wait_ns;
         if (timeout_nsec > 0 && timeout_nsec != INT64_MAX) {
            uint64_t abs_to = static_cast<uint64_t>(timeout_nsec);
            if (abs_to <= start_ns) {
               /* Already expired absolute CLOCK_MONOTONIC deadline */
               pthread_mutex_unlock(&g_lock);
               errno = ETIME;
               return -1;
            }
            uint64_t rem_ns = abs_to - start_ns;
            if (rem_ns < caller_budget_ns)
               caller_budget_ns = rem_ns;
         }

         static uint64_t fast_spin_budget_ns = UINT64_MAX;
         if (fast_spin_budget_ns == UINT64_MAX) {
            const char *fs_env = getenv("PANTHOR_ADAPTER_FAST_SPIN_US");
            uint64_t us = (fs_env && *fs_env) ? strtoull(fs_env, nullptr, 0) : 200ULL;
            fast_spin_budget_ns = us * 1000ULL;
         }
         bool used_poll_sleep = false;
         uint32_t spin_iters = 0;
         while (!check_all_done()) {
            uint64_t now_ns = get_monotonic_ns();
            uint64_t elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0ULL;
            if (elapsed_ns >= fast_spin_budget_ns || elapsed_ns >= caller_budget_ns)
               break;
            int mfd = g_ctx.mali_fd;
            pthread_mutex_unlock(&g_lock);
            ++spin_iters;
            if ((spin_iters & 15U) == 0U && mfd >= 0) {
               struct pollfd pfd = { .fd = mfd, .events = POLLIN, .revents = 0 };
               if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                  uint8_t notif[64];
                  ssize_t rlen = read(mfd, notif, sizeof(notif));
                  pthread_mutex_lock(&g_lock);
                  if (rlen >= static_cast<ssize_t>(sizeof(struct base_csf_notification)) &&
                      handle_mali_csf_notification_locked(notif, rlen)) {
                     pthread_mutex_unlock(&g_lock);
                     panthor_kbase_dump_csf_diagnostics("CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR");
                     pthread_mutex_lock(&g_lock);
                     break;
                  }
                  continue;
               }
            }
            if (elapsed_ns < 150000ULL) {
#if defined(__aarch64__)
               __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__)
               __builtin_ia32_pause();
#endif
               if ((spin_iters & 7U) == 0U)
                  sched_yield();
            } else {
               usleep(10);
            }
            pthread_mutex_lock(&g_lock);
         }

         uint64_t last_kick_ns = start_ns;
         uint64_t last_diag_ns = start_ns;
         uint32_t stall_dump_count = 0;
         bool dumped_stall = false;

         while (!check_all_done()) {
            used_poll_sleep = true;
            uint64_t now_ns = get_monotonic_ns();
            uint64_t elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0ULL;

            struct adapter_group *grp = target_grp_id ? find_group(target_grp_id) : nullptr;
            if (grp && grp->fatal_state != 0) {
               if (!grp->dumped_fatal_diag) {
                  grp->dumped_fatal_diag = true;
                  pthread_mutex_unlock(&g_lock);
                  panthor_kbase_dump_csf_diagnostics("SYNCOBJ_WAIT group fatal_state != 0");
                  pthread_mutex_lock(&g_lock);
               }
               break;
            }
            /* Re-kick unfinished subqueues after 15ms (and every 25ms thereafter) so normal 16.6ms frames
             * never trigger spurious KBASE_IOCTL_CS_EVENT_SIGNAL / KBASE_IOCTL_CS_QUEUE_KICK storms that
             * contend with Android SurfaceFlinger & hwcomposer KCPU queues. */
            if (grp && (now_ns - last_kick_ns >= 25000000ULL || (last_kick_ns == start_ns && elapsed_ns >= 15000000ULL))) {
               last_kick_ns = now_ns;
               adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_EVENT_SIGNAL, nullptr);
               for (uint32_t q = 0; q < grp->queue_count; q++) {
                  struct adapter_subqueue *sq = &grp->subqueues[q];
                  if (needed_seqno[q] > 0 && sq->seqno_cpu_ptr && *sq->seqno_cpu_ptr < needed_seqno[q]) {
                     if (sq->user_io_ptr) {
                        auto *doorbell_page = reinterpret_cast<volatile uint32_t *>(
                           static_cast<uint8_t *>(sq->user_io_ptr) + CS_USER_IO_DOORBELL_OFFSET);
                        doorbell_page[0] = 1U;
                        __atomic_thread_fence(__ATOMIC_SEQ_CST);
                     }
                     struct kbase_ioctl_cs_queue_kick kick = { .buffer_gpu_addr = sq->ring_gpu_va };
                     adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick);
                  }
               }
            }

            /* Configurable stall diagnostic threshold via PANTHOR_ADAPTER_STALL_DUMP_MS
             * (default 100 ms; 0 disables periodic stall dumps). */
            static uint64_t stall_dump_ns = UINT64_MAX;
            if (stall_dump_ns == UINT64_MAX) {
               const char *s_env = getenv("PANTHOR_ADAPTER_STALL_DUMP_MS");
               uint64_t ms = (s_env && *s_env) ? strtoull(s_env, nullptr, 0) : 100ULL;
               stall_dump_ns = ms * 1000000ULL;
            }
            uint64_t eff_stall_ns = (stall_dump_count == 0U) ? stall_dump_ns : (stall_dump_ns * 10ULL);
            if (grp && stall_dump_ns > 0ULL && stall_dump_count < 2U && (now_ns - last_diag_ns >= eff_stall_ns)) {
               last_diag_ns = now_ns;
               stall_dump_count++;
               dumped_stall = true;
               pthread_mutex_unlock(&g_lock);
               panthor_kbase_dump_csf_diagnostics("SYNCOBJ_WAIT stall threshold exceeded");
               pthread_mutex_lock(&g_lock);
            }

            if (elapsed_ns >= caller_budget_ns) {
               if (grp && !dumped_stall && stall_dump_ns > 0ULL && elapsed_ns >= stall_dump_ns) {
                  pthread_mutex_unlock(&g_lock);
                  panthor_kbase_dump_csf_diagnostics("SYNCOBJ_WAIT timeout");
                  pthread_mutex_lock(&g_lock);
               }
               pthread_mutex_unlock(&g_lock);
               errno = ETIME;
               return -1;
            }

            int mfd = g_ctx.mali_fd;
            pthread_mutex_unlock(&g_lock);
            if (mfd >= 0 && target_grp_id != 0) {
               struct pollfd pfd = { .fd = mfd, .events = POLLIN, .revents = 0 };
               int prc = poll(&pfd, 1, 1);
               uint8_t notif[64];
               ssize_t rlen = 0;
               if (prc > 0 && (pfd.revents & POLLIN)) {
                  rlen = read(mfd, notif, sizeof(notif));
               }
               pthread_mutex_lock(&g_lock);
               if (rlen >= static_cast<ssize_t>(sizeof(struct base_csf_notification))) {
                  if (handle_mali_csf_notification_locked(notif, rlen)) {
                     pthread_mutex_unlock(&g_lock);
                     panthor_kbase_dump_csf_diagnostics("CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR");
                     pthread_mutex_lock(&g_lock);
                     break;
                  }
               }
            } else {
               usleep(50);
               pthread_mutex_lock(&g_lock);
            }
         }

         uint64_t wait_done_ns = get_monotonic_ns();
         uint64_t wait_dur_ns = (wait_done_ns >= start_ns) ? (wait_done_ns - start_ns) : 0ULL;
         g_ctx.total_syncobj_waits++;
         if (used_poll_sleep)
            g_ctx.poll_sleep_syncobj_waits++;
         else
            g_ctx.fast_spin_syncobj_waits++;
         g_ctx.window_sync_waits++;
         g_ctx.window_sync_wait_ns_sum += wait_dur_ns;
         if (wait_dur_ns > g_ctx.window_sync_wait_ns_max)
            g_ctx.window_sync_wait_ns_max = wait_dur_ns;
      } else {
         g_ctx.total_syncobj_waits++;
         g_ctx.fast_spin_syncobj_waits++;
      }

      if (first_signaled_out)
         *first_signaled_out = first_sig_idx;

      /* Invalidate CPU cache on imported swapchain dma-buf BOs ONLY when we waited on a GPU group */
#if defined(__aarch64__)
      if (target_grp_id != 0) {
         size_t bo_lim = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
         if (bo_lim > ADAPTER_MAX_BOS)
            bo_lim = ADAPTER_MAX_BOS;
         for (size_t b = 0; b < bo_lim; b++) {
            if (g_ctx.bos[b].in_use && g_ctx.bos[b].is_imported && g_ctx.bos[b].cpu_ptr)
               kbase_cpu_cache_invalidate_range(g_ctx.bos[b].cpu_ptr, g_ctx.bos[b].cpu_size);
         }
      }
#endif
      LOG_DEBUG("DRM_IOCTL_SYNCOBJ_WAIT/TIMELINE_WAIT count=%u -> OK", count_handles);
      pthread_mutex_unlock(&g_lock);
      panthor_drm_capture_record_ioctl(fd, request, arg, 0);
      return 0;
   }
   case 0xc4: { /* DRM_IOCTL_SYNCOBJ_RESET */
      auto *sa = static_cast<struct drm_syncobj_array *>(arg);
      if (sa && sa->handles && sa->count_handles <= 64) {
         const auto *handles = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(sa->handles));
         for (uint32_t i = 0; i < sa->count_handles; i++) {
            struct adapter_syncobj *so = find_syncobj(handles[i]);
            if (so) {
               so->value = 0;
               so->completed_value = 0;
               so->target_group_handle = 0;
               memset(so->target_subq_seqno, 0, sizeof(so->target_subq_seqno));
               if (so->cqs_cpu_ptr)
                  *so->cqs_cpu_ptr = 0;
            }
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc5: { /* DRM_IOCTL_SYNCOBJ_SIGNAL */
      auto *sa = static_cast<struct drm_syncobj_array *>(arg);
      if (sa && sa->handles && sa->count_handles <= 64) {
         const auto *handles = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(sa->handles));
         for (uint32_t i = 0; i < sa->count_handles; i++) {
            struct adapter_syncobj *so = find_syncobj(handles[i]);
            if (so) {
               so->value = 1ULL;
               so->completed_value = 1ULL;
               so->target_group_handle = 0;
               memset(so->target_subq_seqno, 0, sizeof(so->target_subq_seqno));
               if (so->cqs_cpu_ptr)
                  *so->cqs_cpu_ptr = 1ULL;
            }
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xcb: { /* DRM_IOCTL_SYNCOBJ_QUERY (0xCB in drm.h!) */
      auto *ta = static_cast<struct drm_syncobj_timeline_array *>(arg);
      if (ta && ta->handles && ta->points && ta->count_handles <= 64) {
         const auto *handles = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(ta->handles));
         auto *points        = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(ta->points));
         for (uint32_t i = 0; i < ta->count_handles; i++) {
            struct adapter_syncobj *so = find_syncobj(handles[i]);
            if (!so) {
               points[i] = 0ULL;
               continue;
            }
            if ((ta->flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) != 0 || so->target_group_handle == 0) {
               points[i] = so->value;
            } else {
               bool gpu_done = true;
               struct adapter_group *grp = find_group(so->target_group_handle);
               if (grp) {
                  for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
                     struct adapter_subqueue *sq = &grp->subqueues[q];
                     uint64_t need = so->target_subq_seqno[q];
                     if (need > 0 && sq->seqno_cpu_ptr) {
                        kbase_cpu_cache_invalidate_range(
                           const_cast<uint64_t *>(sq->seqno_cpu_ptr), ADAPTER_SEQNO_CELL_STRIDE);
                        if (*sq->seqno_cpu_ptr < need) {
                           gpu_done = false;
                           break;
                        }
                     }
                  }
               }
               if (gpu_done)
                  so->completed_value = so->value;
               points[i] = gpu_done ? so->value : so->completed_value;
            }
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xcc: { /* DRM_IOCTL_SYNCOBJ_TRANSFER */
      auto *tr = static_cast<struct drm_syncobj_transfer *>(arg);
      if (tr) {
         struct adapter_syncobj *src = find_syncobj(tr->src_handle);
         struct adapter_syncobj *dst = find_syncobj(tr->dst_handle);
         if (dst) {
            if (!src || (src->value == 0ULL && src->target_group_handle == 0 && src->last_target_group_handle == 0)) {
               dst->value = 0ULL;
               dst->completed_value = 0ULL;
               dst->target_group_handle = 0;
               memset(dst->target_subq_seqno, 0, sizeof(dst->target_subq_seqno));
            } else {
               dst->value = tr->dst_point ? tr->dst_point : (src->value ? src->value : 1ULL);
               uint32_t grp_h = src->target_group_handle ? src->target_group_handle : src->last_target_group_handle;
               if (grp_h != 0) {
                  dst->target_group_handle = grp_h;
                  dst->last_target_group_handle = grp_h;
                  for (uint32_t q = 0; q < ADAPTER_MAX_SUBQUEUES; q++) {
                     uint64_t seq = src->target_subq_seqno[q] ? src->target_subq_seqno[q] : src->last_target_subq_seqno[q];
                     if (seq > dst->target_subq_seqno[q])
                        dst->target_subq_seqno[q] = seq;
                     if (seq > dst->last_target_subq_seqno[q])
                        dst->last_target_subq_seqno[q] = seq;
                  }
               } else {
                  dst->completed_value = dst->value;
               }
            }
            if (dst->cqs_cpu_ptr)
               *dst->cqs_cpu_ptr = dst->value;
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xcd: { /* DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL (0xCD in drm.h!) */
      auto *ta = static_cast<struct drm_syncobj_timeline_array *>(arg);
      if (ta && ta->handles && ta->points && ta->count_handles <= 64) {
         const auto *handles = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(ta->handles));
         const auto *points  = reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(ta->points));
         for (uint32_t i = 0; i < ta->count_handles; i++) {
            struct adapter_syncobj *so = find_syncobj(handles[i]);
            if (so) {
               so->value = points[i];
               so->completed_value = points[i];
               so->target_group_handle = 0;
               memset(so->target_subq_seqno, 0, sizeof(so->target_subq_seqno));
               if (so->cqs_cpu_ptr)
                  *so->cqs_cpu_ptr = points[i];
            }
         }
      }
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   default:
      if (nr < DRM_COMMAND_BASE || nr >= DRM_COMMAND_BASE + 0x10)
         LOG_WARN("ioctl fd=%d req=%s(0x%lx) nr=0x%x: unhandled drm ioctl",
                  fd, panthor2kbase_ioctl_req_name(request), request, nr);
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
      auto *q = static_cast<struct drm_panthor_dev_query *>(arg);
      switch (q->type) {
      case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_gpu_info);
         } else {
            memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(q->pointer)), &g_ctx.gpu_info,
                   q->size < sizeof(g_ctx.gpu_info) ? q->size : sizeof(g_ctx.gpu_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_csif_info);
         } else {
            memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(q->pointer)), &g_ctx.csif_info,
                   q->size < sizeof(g_ctx.csif_info) ? q->size : sizeof(g_ctx.csif_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_MMU_INFO:
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_mmu_info);
         } else {
            memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(q->pointer)), &g_ctx.mmu_info,
                   q->size < sizeof(g_ctx.mmu_info) ? q->size : sizeof(g_ctx.mmu_info));
         }
         break;
      case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO: {
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_timestamp_info);
         } else {
            union kbase_ioctl_get_cpu_gpu_timeinfo ti;
            memset(&ti, 0, sizeof(ti));
            adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &ti);
            struct drm_panthor_timestamp_info out_ti{};
            out_ti.timestamp_frequency = 26000000ULL;
            out_ti.current_timestamp = ti.out.timestamp;
            out_ti.cycle_count = ti.out.cycle_counter;
            out_ti.cpu_timestamp_sec = ti.out.sec;
            out_ti.cpu_timestamp_nsec = ti.out.nsec;
            memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(q->pointer)), &out_ti,
                   q->size < sizeof(out_ti) ? q->size : sizeof(out_ti));
         }
         break;
      }
      case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO: {
         if (!q->pointer) {
            q->size = sizeof(struct drm_panthor_group_priorities_info);
         } else {
            struct kbase_ioctl_context_priority_check pc = { .priority = 3 };
            adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CONTEXT_PRIORITY_CHECK, &pc);
            struct drm_panthor_group_priorities_info pi = { .allowed_mask = 0x7, .pad = {0, 0, 0} };
            memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(q->pointer)), &pi,
                   q->size < sizeof(pi) ? q->size : sizeof(pi));
         }
         break;
      }
      default:
         break;
      }
      LOG_INFO("DRM_PANTHOR_DEV_QUERY type=%u size=%u", q->type, q->size);
      trace_log("DRM_PANTHOR_DEV_QUERY type=%u\n", q->type);
      break;
   }

   case DRM_PANTHOR_SET_USER_MMIO_OFFSET: {
      auto *mo = static_cast<struct drm_panthor_set_user_mmio_offset *>(arg);
      g_ctx.user_mmio_offset = mo->offset;
      LOG_INFO("DRM_PANTHOR_SET_USER_MMIO_OFFSET offset=0x%llx",
               static_cast<unsigned long long>(mo->offset));
      trace_log("DRM_PANTHOR_SET_USER_MMIO_OFFSET offset=0x%llx\n",
                static_cast<unsigned long long>(mo->offset));
      break;
   }

   case DRM_PANTHOR_VM_CREATE: {
      auto *vc = static_cast<struct drm_panthor_vm_create *>(arg);
      vc->id = g_ctx.next_vm_id++;
      if (!vc->user_va_range)
         vc->user_va_range = (1ULL << 39);
      LOG_INFO("DRM_PANTHOR_VM_CREATE -> vm_id=%u user_va_range=0x%llx",
               vc->id, static_cast<unsigned long long>(vc->user_va_range));
      trace_log("DRM_PANTHOR_VM_CREATE -> vm_id=%u\n", vc->id);
      break;
   }

   case DRM_PANTHOR_VM_DESTROY: {
      auto *vd = static_cast<struct drm_panthor_vm_destroy *>(arg);
      LOG_INFO("DRM_PANTHOR_VM_DESTROY vm_id=%u", vd->id);
      trace_log("DRM_PANTHOR_VM_DESTROY vm_id=%u\n", vd->id);
      break;
   }

   case DRM_PANTHOR_VM_GET_STATE: {
      auto *vs = static_cast<struct drm_panthor_vm_get_state *>(arg);
      vs->state = DRM_PANTHOR_VM_STATE_USABLE;
      LOG_DEBUG("DRM_PANTHOR_VM_GET_STATE -> state=USABLE");
      break;
   }

   case DRM_PANTHOR_BO_CREATE: {
      auto *bc = static_cast<struct drm_panthor_bo_create *>(arg);
      uint32_t kmod_flags = g_tls_next_bo_kmod_flags | (bc->flags >> 16);
      g_tls_next_bo_kmod_flags = 0;
      bc->flags &= 0xffffU;

      uint64_t aligned_sz = (bc->size + 4095ULL) & ~4095ULL;
      if (aligned_sz == 0)
         aligned_sz = 4096ULL;
      uint64_t pages = aligned_sz >> 12;

      bool is_exec = (kmod_flags & ADAPTER_KMOD_BO_FLAG_EXECUTABLE) != 0;
      uint64_t kflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_COHERENT_LOCAL;
      if (is_exec) {
         kflags |= BASE_MEM_PROT_GPU_EX;
      } else {
         kflags |= BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
      }
      if (!is_exec && ((bc->flags & DRM_PANTHOR_BO_WB_MMAP) || (kmod_flags & ADAPTER_KMOD_BO_FLAG_WB_MMAP)))
         kflags |= BASE_MEM_CACHED_CPU;
      if (kmod_flags & ADAPTER_KMOD_BO_FLAG_GPU_UNCACHED)
         kflags |= BASE_MEM_UNCACHED_GPU;

      uint64_t out_flags = 0;
      uint64_t out_cookie_or_va = 0;
      int alloc_rc = -1;
      /*
       * CRITICAL INVARIANT (mali_kbase_mem.c:2505, 2515 & mali_kbase_mem_linux.c:2080):
       * On non-CPU-coherent SoCs (MT6878 where system_coherency == COHERENCY_NONE == 31),
       * `BASE_MEM_CSF_EVENT` strips `KBASE_REG_GPU_CACHED` and `KBASE_REG_SHARE_IN`, mapping
       * the BO in the GPU MMU as Non-Shareable (`SH=00`, `AS_MEMATTR_INDEX_NON_CACHEABLE`).
       * Setting `BASE_MEM_CSF_EVENT` on general BOs causes:
       *   1. `KBASE_IOCTL_MEM_ALIAS` (`kbase_mem_alias`) to fail with `-ENOMEM` (`errno=12`)
       *      because `!(aliasing_reg->flags & KBASE_REG_GPU_CACHED)`.
       *   2. `GPU_SHAREABILITY_FAULT` (`0x88`, `status=0x00c00488`) in the L2 cache when
       *      a cache line is accessed with both Shareable and Non-Shareable transactions.
       * Therefore, ONLY set `BASE_MEM_CSF_EVENT` when `ADAPTER_KMOD_BO_FLAG_CSF_EVENT` is
       * explicitly requested; all PanVK subqueue/ringbuf syncobjs are redirected into
       * `g_ctx.event_bo` automatically during `DRM_PANTHOR_GROUP_SUBMIT`!
       */
      if (!is_exec && (kmod_flags & ADAPTER_KMOD_BO_FLAG_CSF_EVENT) != 0) {
         uint64_t ev_flags = (kflags & ~(BASE_MEM_COHERENT_LOCAL | BASE_MEM_CACHED_CPU | BASE_MEM_UNCACHED_GPU)) |
                             BASE_MEM_CSF_EVENT;
         alloc_rc = alloc_kbase_mem(g_ctx.mali_fd, pages, pages, 0, ev_flags, &out_flags, &out_cookie_or_va);
         if (alloc_rc == 0)
            kflags = ev_flags;
      }
      if (alloc_rc < 0 &&
          alloc_kbase_mem(g_ctx.mali_fd, pages, pages, 0, kflags, &out_flags, &out_cookie_or_va) < 0) {
         LOG_ERROR("DRM_PANTHOR_BO_CREATE: alloc_kbase_mem(pages=%llu, flags=0x%llx) failed: %s",
                   static_cast<unsigned long long>(pages),
                   static_cast<unsigned long long>(kflags), strerror(errno));
         ret = -1;
         break;
      }

      void *cpu = real_mmap(nullptr, static_cast<size_t>(aligned_sz), PROT_READ | PROT_WRITE,
                            MAP_SHARED, g_ctx.mali_fd, static_cast<off_t>(out_cookie_or_va));
      if (cpu == MAP_FAILED) {
         LOG_ERROR("DRM_PANTHOR_BO_CREATE: real_mmap(offset=0x%llx, size=%llu) failed: %s",
                   static_cast<unsigned long long>(out_cookie_or_va),
                   static_cast<unsigned long long>(aligned_sz), strerror(errno));
         struct kbase_ioctl_mem_free mf = { .gpu_addr = out_cookie_or_va };
         adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_FREE, &mf);
         ret = -1;
         break;
      }
      /* mali_kbase.ko already zeroes newly allocated physical pages in the kernel; only zero
       * small control/ring/sync BOs (<= 64 KiB) + the first 4 KiB header in userspace so
       * 64 MiB / 128 MiB DXVK heaps do not dirty 32,768+ CPU cache lines or stall for 75 ms! */
      if (aligned_sz <= 65536ULL) {
         memset(cpu, 0, static_cast<size_t>(aligned_sz));
      } else {
         memset(cpu, 0, 4096U);
      }

      struct adapter_bo *bo = nullptr;
      uint32_t next_bh = g_ctx.next_gem_handle ? g_ctx.next_gem_handle : 1U;
      size_t pref_b = static_cast<size_t>((next_bh - 1U) % ADAPTER_MAX_BOS);
      for (size_t step = 0; step < ADAPTER_MAX_BOS; step++) {
         size_t i = (pref_b + step) % ADAPTER_MAX_BOS;
         if (!g_ctx.bos[i].in_use) {
            if (i + 1 > g_ctx.max_bo_slot_used)
               g_ctx.max_bo_slot_used = static_cast<uint32_t>(i + 1);
            bo = &g_ctx.bos[i];
            break;
         }
      }
      if (!bo) {
         real_munmap(cpu, static_cast<size_t>(aligned_sz));
         ret = -1;
         break;
      }
      memset(bo, 0, sizeof(*bo));
      bo->in_use = true;
      bo->gem_handle = g_ctx.next_gem_handle++;
      bo->size = aligned_sz;
      bo->panthor_flags = bc->flags;
      bo->kmod_flags = kmod_flags;
      bo->kbase_flags = out_flags ? out_flags : kflags;
      bo->kbase_cookie_or_va = out_cookie_or_va;
      bo->is_same_va = !is_exec && ((bo->kbase_flags & BASE_MEM_SAME_VA) != 0);
      bo->is_executable = is_exec;
      bo->is_csf_event = (bo->kbase_flags & BASE_MEM_CSF_EVENT) != 0;
      bo->gpu_va = bo->is_same_va ? reinterpret_cast<uintptr_t>(cpu) : out_cookie_or_va;
      bo->cpu_ptr = cpu;
      bo->cpu_size = static_cast<size_t>(aligned_sz);
      bo->dmabuf_fd = -1;

      bc->handle = bo->gem_handle;
      bc->size = aligned_sz;
      LOG_INFO("DRM_PANTHOR_BO_CREATE size=%llu -> handle=%u va=0x%llx cpu=%p flags=0x%llx exec=%d csf_ev=%d",
               static_cast<unsigned long long>(aligned_sz), bo->gem_handle,
               static_cast<unsigned long long>(bo->gpu_va), cpu,
               static_cast<unsigned long long>(bo->kbase_flags), is_exec, bo->is_csf_event);
      trace_log("DRM_PANTHOR_BO_CREATE size=%llu -> KBASE_IOCTL_MEM_ALLOC handle=%u va=0x%llx\n",
                static_cast<unsigned long long>(aligned_sz), bo->gem_handle,
                static_cast<unsigned long long>(bo->gpu_va));
      break;
   }

   case DRM_PANTHOR_BO_MMAP_OFFSET: {
      auto *mo = static_cast<struct drm_panthor_bo_mmap_offset *>(arg);
      mo->offset = 0x1000000000ULL | (static_cast<uint64_t>(mo->handle) << 12);
      LOG_DEBUG("DRM_PANTHOR_BO_MMAP_OFFSET handle=%u -> offset=0x%llx",
                mo->handle, static_cast<unsigned long long>(mo->offset));
      break;
   }

   case DRM_PANTHOR_VM_BIND: {
      auto *vb = static_cast<struct drm_panthor_vm_bind *>(arg);
      auto *ops = reinterpret_cast<struct drm_panthor_vm_bind_op *>(static_cast<uintptr_t>(vb->ops.array));
      LOG_INFO("DRM_PANTHOR_VM_BIND op_count=%u", vb->ops.count);

      /* Handle 2-op contiguous circular wraparound mapping (init_render_desc_ringbuf) via KBASE_IOCTL_MEM_ALIAS */
      if (vb->ops.count == 2 &&
          (ops[0].flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK) == DRM_PANTHOR_VM_BIND_OP_TYPE_MAP &&
          (ops[1].flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK) == DRM_PANTHOR_VM_BIND_OP_TYPE_MAP &&
          ops[0].bo_handle != 0 && ops[0].bo_handle == ops[1].bo_handle &&
          ops[0].bo_offset == 0 && ops[1].bo_offset == 0 &&
          ops[0].size == ops[1].size) {
         struct adapter_bo *bo = find_bo(ops[0].bo_handle);
         if (bo) {
            uint64_t page_cnt = ops[0].size >> 12;
            struct base_mem_aliasing_info ai[2] = {
               { .handle = bo->gpu_va, .offset = 0, .length = page_cnt },
               { .handle = bo->gpu_va, .offset = 0, .length = page_cnt },
            };
            uint64_t total_alias_sz = ops[0].size * 2;
            for (unsigned attempt = 0; attempt < 16; attempt++) {
               union kbase_ioctl_mem_alias alias_req;
               memset(&alias_req, 0, sizeof(alias_req));
               alias_req.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                                    BASE_MEM_PROT_CPU_RD | BASE_MEM_COHERENT_LOCAL;
               alias_req.in.stride = page_cnt;
               alias_req.in.nents = 2;
               alias_req.in.aliasing_info = reinterpret_cast<uintptr_t>(ai);
               if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_ALIAS, &alias_req) != 0 ||
                   alias_req.out.gpu_va == 0) {
                  break;
               }
               void *aptr = real_mmap(nullptr, static_cast<size_t>(total_alias_sz),
                                      PROT_READ, MAP_SHARED, g_ctx.mali_fd,
                                      static_cast<off_t>(alias_req.out.gpu_va));
               if (aptr == MAP_FAILED) {
                  struct kbase_ioctl_mem_free free_req = { .gpu_addr = alias_req.out.gpu_va };
                  adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_FREE, &free_req);
                  break;
               }
               uint64_t ava = reinterpret_cast<uintptr_t>(aptr);
               if ((ava >> 32) == ((ava + total_alias_sz - 1) >> 32)) {
                  bo->alias_ptr = aptr;
                  bo->alias_size = static_cast<size_t>(total_alias_sz);
                  bo->alias_gpu_va = ava;
                  break;
               }
               real_munmap(aptr, static_cast<size_t>(total_alias_sz));
            }
            uint64_t base_va = bo->alias_gpu_va ? bo->alias_gpu_va : bo->gpu_va;
            ops[0].va = base_va;
            ops[1].va = base_va + ops[0].size;
            LOG_INFO("DRM_PANTHOR_VM_BIND 2x ALIAS MAP bo=%u -> va=0x%llx",
                     bo->gem_handle, static_cast<unsigned long long>(base_va));
            trace_log("DRM_PANTHOR_VM_BIND MAP bo=%u va=0x%llx -> kbase_va=0x%llx\n",
                      bo->gem_handle, static_cast<unsigned long long>(ops[0].va),
                      static_cast<unsigned long long>(base_va));
            break;
         }
      }

      for (uint32_t i = 0; i < vb->ops.count; i++) {
         uint32_t op_type = ops[i].flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;
         if (op_type == DRM_PANTHOR_VM_BIND_OP_TYPE_MAP) {
            struct adapter_bo *bo = find_bo(ops[i].bo_handle);
            if (bo) {
               if (((ops[i].flags & (DRM_PANTHOR_VM_BIND_OP_MAP_READONLY | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC)) == DRM_PANTHOR_VM_BIND_OP_MAP_READONLY) && !bo->is_executable) {
                  uint64_t pages = bo->size >> 12;
                  uint64_t exec_kflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                                         BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX |
                                         BASE_MEM_COHERENT_LOCAL;
                  uint64_t exec_out_flags = 0;
                  uint64_t exec_gpu_va = 0;
                  if (alloc_kbase_mem(g_ctx.mali_fd, pages, pages, 0, exec_kflags,
                                      &exec_out_flags, &exec_gpu_va) == 0 &&
                      exec_gpu_va != 0) {
                     uint8_t *saved = nullptr;
                     if (bo->cpu_ptr && bo->cpu_size > 0) {
                        saved = static_cast<uint8_t *>(malloc(bo->cpu_size));
                        if (saved)
                           memcpy(saved, bo->cpu_ptr, bo->cpu_size);
                     }
                     void *new_cpu = real_mmap(bo->cpu_ptr, bo->cpu_size,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED | (bo->cpu_ptr ? MAP_FIXED : 0),
                                               g_ctx.mali_fd,
                                               static_cast<off_t>(exec_gpu_va));
                     if (new_cpu != MAP_FAILED) {
                        if (saved) {
                           memcpy(new_cpu, saved, bo->cpu_size);
                           kbase_cpu_cache_clean_range(new_cpu, bo->cpu_size);
                        }
                        bo->cpu_ptr = new_cpu;
                        bo->gpu_va = exec_gpu_va;
                        bo->kbase_cookie_or_va = exec_gpu_va;
                        bo->kbase_flags = exec_out_flags ? exec_out_flags : exec_kflags;
                        bo->is_executable = true;
                        bo->is_same_va = false;
                        LOG_INFO("DRM_PANTHOR_VM_BIND upgraded bo=%u to EXEC_VA va=0x%llx cpu=%p",
                                 bo->gem_handle, static_cast<unsigned long long>(bo->gpu_va), bo->cpu_ptr);
                     }
                     if (saved)
                        free(saved);
                  }
               }
               ops[i].va = bo->gpu_va + ops[i].bo_offset;
               LOG_INFO("DRM_PANTHOR_VM_BIND MAP bo=%u -> kbase_va=0x%llx",
                        bo->gem_handle, static_cast<unsigned long long>(ops[i].va));
               trace_log("DRM_PANTHOR_VM_BIND MAP bo=%u va=0x%llx -> kbase_va=0x%llx\n",
                         bo->gem_handle, static_cast<unsigned long long>(ops[i].va),
                         static_cast<unsigned long long>(bo->gpu_va));
            }
         }
      }
      break;
   }

   case DRM_PANTHOR_BO_SYNC: {
      auto *bs = static_cast<struct drm_panthor_bo_sync *>(arg);
      auto *ops = reinterpret_cast<struct drm_panthor_bo_sync_op *>(static_cast<uintptr_t>(bs->ops.array));
      LOG_INFO("DRM_PANTHOR_BO_SYNC op_count=%u", bs->ops.count);
      bool touched_csf_event = false;
      for (uint32_t i = 0; i < bs->ops.count; i++) {
         struct adapter_bo *bo = find_bo(ops[i].handle);
         if (bo && bo->cpu_ptr) {
            uint8_t *sync_ptr = reinterpret_cast<uint8_t *>(bo->cpu_ptr) + ops[i].offset;
            size_t sync_sz = static_cast<size_t>(ops[i].size);
            if (ops[i].type == DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH) {
               kbase_cpu_cache_clean_range(sync_ptr, sync_sz);
               if (bo->is_csf_event)
                  touched_csf_event = true;
            } else {
               kbase_cpu_cache_invalidate_range(sync_ptr, sync_sz);
            }
            struct kbase_ioctl_mem_sync ms;
            memset(&ms, 0, sizeof(ms));
            ms.handle = bo->gpu_va;
            ms.user_addr = reinterpret_cast<uintptr_t>(bo->cpu_ptr) + ops[i].offset;
            ms.size = ops[i].size;
            ms.type = (ops[i].type == DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH)
                         ? BASE_SYNCSET_OP_MSYNC
                         : (BASE_SYNCSET_OP_MSYNC | BASE_SYNCSET_OP_CSYNC);
            adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_SYNC, &ms);
            LOG_INFO("DRM_PANTHOR_BO_SYNC bo=%u -> KBASE_IOCTL_MEM_SYNC va=0x%llx size=%llu",
                     bo->gem_handle, static_cast<unsigned long long>(bo->gpu_va),
                     static_cast<unsigned long long>(ops[i].size));
         }
      }
      if (touched_csf_event)
         adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_EVENT_SIGNAL, nullptr);
      break;
   }

   case DRM_PANTHOR_BO_QUERY_INFO: {
      auto *qi = static_cast<struct drm_panthor_bo_query_info *>(arg);
      struct adapter_bo *bo = find_bo(qi->handle);
      if (bo) {
         qi->extra_flags = bo->is_imported ? DRM_PANTHOR_BO_IS_IMPORTED : 0;
         qi->create_flags = bo->panthor_flags;
         LOG_DEBUG("DRM_PANTHOR_BO_QUERY_INFO handle=%u imported=%d flags=0x%x",
                   bo->gem_handle, bo->is_imported, bo->panthor_flags);
      } else {
         LOG_WARN("DRM_PANTHOR_BO_QUERY_INFO handle=%u not found", qi ? qi->handle : 0);
      }
      break;
   }

   case DRM_PANTHOR_BO_SET_LABEL: {
      auto *bl = static_cast<struct drm_panthor_bo_set_label *>(arg);
      struct adapter_bo *bo = find_bo(bl->handle);
      if (bo && bl->label)
         snprintf(bo->label, sizeof(bo->label), "%s",
                  reinterpret_cast<const char *>(static_cast<uintptr_t>(bl->label)));
      LOG_DEBUG("DRM_PANTHOR_BO_SET_LABEL handle=%u label=%s",
                bl ? bl->handle : 0,
                bl && bl->label ? reinterpret_cast<const char *>(static_cast<uintptr_t>(bl->label)) : "(null)");
      break;
   }

   case DRM_PANTHOR_TILER_HEAP_CREATE: {
      auto *hc = static_cast<struct drm_panthor_tiler_heap_create *>(arg);
      bool bug_mode = is_bug_catching_mode_enabled();
      uint32_t max_chunks = bug_mode
                               ? (hc->max_chunks ? (hc->max_chunks < 4U ? hc->max_chunks : 4U) : 4U)
                               : ((hc->max_chunks >= 200U) ? hc->max_chunks : 200U);
      const char *env_mc = getenv("PANTHOR_ADAPTER_MAX_TILER_CHUNKS");
      if (env_mc && env_mc[0] && atoi(env_mc) > 0)
         max_chunks = static_cast<uint32_t>(atoi(env_mc));

      uint32_t init_chunks = hc->initial_chunk_count ? hc->initial_chunk_count : (bug_mode ? 2U : 5U);
      const char *env_ic = getenv("PANTHOR_ADAPTER_INITIAL_HEAP_CHUNKS");
      if (!env_ic || !env_ic[0])
         env_ic = getenv("PANTHOR_ADAPTER_INITIAL_TILER_CHUNKS");
      if (env_ic && env_ic[0] && atoi(env_ic) > 0) {
         init_chunks = static_cast<uint32_t>(atoi(env_ic));
      } else if (bug_mode) {
         init_chunks = 2U;
      } else if (init_chunks < 16U) {
         init_chunks = (max_chunks < 16U) ? max_chunks : 16U;
      }
      if (init_chunks > max_chunks)
         init_chunks = max_chunks;

      union kbase_ioctl_cs_tiler_heap_init_1_13 hi13;
      memset(&hi13, 0, sizeof(hi13));
      hi13.in.chunk_size = hc->chunk_size ? hc->chunk_size : 1048576U;
      hi13.in.initial_chunks = init_chunks;
      hi13.in.max_chunks = max_chunks;
      hi13.in.target_in_flight = static_cast<uint16_t>(hc->target_in_flight ? hc->target_in_flight : 65535U);
      hi13.in.group_id = 0;

      uint32_t used_init_chunks = init_chunks;
      uint32_t chunk_sz = hc->chunk_size ? hc->chunk_size : 1048576U;
      uint16_t target_if = static_cast<uint16_t>(hc->target_in_flight ? hc->target_in_flight : 65535U);
      if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13, &hi13) < 0) {
         used_init_chunks = hc->initial_chunk_count ? hc->initial_chunk_count : 5U;
         memset(&hi13, 0, sizeof(hi13));
         hi13.in.chunk_size = chunk_sz;
         hi13.in.initial_chunks = used_init_chunks;
         hi13.in.max_chunks = max_chunks;
         hi13.in.target_in_flight = target_if;
         hi13.in.group_id = 0;
         if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13, &hi13) < 0) {
            union kbase_ioctl_cs_tiler_heap_init hi24;
            memset(&hi24, 0, sizeof(hi24));
            hi24.in.chunk_size = chunk_sz;
            hi24.in.initial_chunks = used_init_chunks;
            hi24.in.max_chunks = max_chunks;
            hi24.in.target_in_flight = target_if;
            hi24.in.group_id = 0;
            hi24.in.buf_desc_va = 0;
            if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &hi24) < 0) {
               LOG_ERROR("DRM_PANTHOR_TILER_HEAP_CREATE: KBASE_IOCTL_CS_TILER_HEAP_INIT failed");
               ret = -1;
               break;
            }
            hi13.out.gpu_heap_va = hi24.out.gpu_heap_va;
            hi13.out.first_chunk_va = hi24.out.first_chunk_va;
         }
      }

      bool stored = false;
      for (size_t i = 0; i < ADAPTER_MAX_HEAPS; i++) {
         if (!g_ctx.heaps[i].in_use) {
            g_ctx.heaps[i].in_use = true;
            g_ctx.heaps[i].handle = g_ctx.next_heap_handle++;
            g_ctx.heaps[i].vm_id = hc->vm_id;
            g_ctx.heaps[i].gpu_heap_va = hi13.out.gpu_heap_va;
            g_ctx.heaps[i].first_chunk_va = hi13.out.first_chunk_va;
            g_ctx.heaps[i].chunk_size = chunk_sz;
            g_ctx.heaps[i].initial_chunks = used_init_chunks;
            g_ctx.heaps[i].max_chunks = max_chunks;
            hc->handle = g_ctx.heaps[i].handle;
            hc->tiler_heap_ctx_gpu_va = hi13.out.gpu_heap_va;
            hc->first_heap_chunk_gpu_va = hi13.out.first_chunk_va;
            for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
               if (g_ctx.groups[g].in_use && g_ctx.groups[g].vm_id == hc->vm_id)
                  g_ctx.groups[g].active_tiler_heap_ctx_va = hi13.out.gpu_heap_va;
            }
            LOG_INFO("DRM_PANTHOR_TILER_HEAP_CREATE -> KBASE_IOCTL_CS_TILER_HEAP_INIT heap_va=0x%llx first_chunk=0x%llx initial=%u max=%u handle=%u",
                     static_cast<unsigned long long>(hc->tiler_heap_ctx_gpu_va),
                     static_cast<unsigned long long>(hc->first_heap_chunk_gpu_va),
                     used_init_chunks, max_chunks, hc->handle);
            trace_log("DRM_PANTHOR_TILER_HEAP_CREATE -> KBASE_IOCTL_CS_TILER_HEAP_INIT heap_va=0x%llx first_chunk=0x%llx\n",
                      static_cast<unsigned long long>(hc->tiler_heap_ctx_gpu_va),
                      static_cast<unsigned long long>(hc->first_heap_chunk_gpu_va));
            stored = true;
            break;
         }
      }
      if (!stored)
         LOG_ERROR("DRM_PANTHOR_TILER_HEAP_CREATE: no free heap slot");
      break;
   }

   case DRM_PANTHOR_TILER_HEAP_DESTROY: {
      auto *hd = static_cast<struct drm_panthor_tiler_heap_destroy *>(arg);
      bool found = false;
      for (size_t i = 0; i < ADAPTER_MAX_HEAPS; i++) {
         if (g_ctx.heaps[i].in_use && g_ctx.heaps[i].handle == hd->handle) {
            struct kbase_ioctl_cs_tiler_heap_term ht = { .gpu_heap_va = g_ctx.heaps[i].gpu_heap_va };
            adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &ht);
            LOG_INFO("DRM_PANTHOR_TILER_HEAP_DESTROY -> KBASE_IOCTL_CS_TILER_HEAP_TERM va=0x%llx handle=%u",
                     static_cast<unsigned long long>(ht.gpu_heap_va), hd->handle);
            memset(&g_ctx.heaps[i], 0, sizeof(g_ctx.heaps[i]));
            found = true;
            break;
         }
      }
      if (!found)
         LOG_WARN("DRM_PANTHOR_TILER_HEAP_DESTROY handle=%u not found", hd ? hd->handle : 0);
      break;
   }

   case DRM_PANTHOR_GROUP_CREATE: {
      auto *gc = static_cast<struct drm_panthor_group_create *>(arg);
      uint32_t qcnt = gc->queues.count;
      if (qcnt > ADAPTER_MAX_SUBQUEUES)
         qcnt = ADAPTER_MAX_SUBQUEUES;

      bool single_csg = is_single_csg_enabled(g_ctx.arch_major);
      uint8_t csi_handlers = BASE_CSF_TILER_OOM_EXCEPTION_FLAG;
      uint64_t tiler_mask = gc->tiler_core_mask ? gc->tiler_core_mask : 1ULL;
      uint64_t frag_mask = gc->fragment_core_mask ? gc->fragment_core_mask : ~0ULL;
      uint64_t comp_mask = gc->compute_core_mask ? gc->compute_core_mask : ~0ULL;
      uint8_t tiler_max = gc->max_tiler_cores ? gc->max_tiler_cores : 1U;
      uint8_t frag_max = gc->max_fragment_cores ? gc->max_fragment_cores : 64U;
      uint8_t comp_max = gc->max_compute_cores ? gc->max_compute_cores : 64U;

      /* Map Panthor group priority to Kbase CSG priority, defaulting to
       * BASE_QUEUE_GROUP_PRIORITY_MEDIUM (1) so high-FPS game workloads share
       * the Mali-G615 CSF scheduler fairly with Android SurfaceFlinger & hwcomposer
       * instead of preempting and starving system KCPU fences at PRIORITY_HIGH (0). */
      const char *hp_env = getenv("PANTHOR_ADAPTER_ALLOW_HIGH_PRIO");
      bool allow_high_prio = (hp_env && hp_env[0] && atoi(hp_env) != 0);
      uint8_t kbase_csg_prio = BASE_QUEUE_GROUP_PRIORITY_MEDIUM;
      if (gc->priority == PANTHOR_GROUP_PRIORITY_LOW) {
         kbase_csg_prio = BASE_QUEUE_GROUP_PRIORITY_LOW;
      } else if (allow_high_prio && gc->priority >= PANTHOR_GROUP_PRIORITY_HIGH) {
         kbase_csg_prio = BASE_QUEUE_GROUP_PRIORITY_HIGH;
      }

      uint8_t primary_csg_handle = 0;
      uint32_t used_layout = 0;
      if (create_kbase_csg_with_fallback(
             tiler_mask, frag_mask, comp_mask,
             static_cast<uint8_t>(qcnt ? qcnt : 3U),
             kbase_csg_prio,
             tiler_max, frag_max, comp_max,
             csi_handlers, &primary_csg_handle, &used_layout) < 0) {
         LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: KBASE_IOCTL_CS_QUEUE_GROUP_CREATE failed");
         ret = -1;
         break;
      }

      struct adapter_group *grp = nullptr;
      size_t g_slot = 0;
      for (size_t i = 0; i < ADAPTER_MAX_GROUPS; i++) {
         if (!g_ctx.groups[i].in_use) {
            grp = &g_ctx.groups[i];
            g_slot = i;
            break;
         }
      }
      if (!grp) {
         LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: no free group slot");
         ret = -1;
         break;
      }
      memset(grp, 0, sizeof(*grp));
      grp->in_use = true;
      grp->group_handle = g_ctx.next_group_handle++;
      grp->kbase_group_handle = primary_csg_handle;
      grp->csi_handlers = (used_layout >= 40U) ? csi_handlers : 0U;
      grp->group_create_layout = used_layout;
      grp->single_csg_mode = single_csg;
      grp->vm_id = gc->vm_id;
      grp->queue_count = qcnt;
      for (size_t h = 0; h < ADAPTER_MAX_HEAPS; h++) {
         if (g_ctx.heaps[h].in_use &&
             (g_ctx.heaps[h].vm_id == grp->vm_id || grp->active_tiler_heap_ctx_va == 0))
            grp->active_tiler_heap_ctx_va = g_ctx.heaps[h].gpu_heap_va;
      }

      /* Allocate SAME_VA ring buffer BO, register queue, bind queue, and mmap USER_IO for each subqueue */
      for (uint32_t q = 0; q < qcnt; q++) {
         struct adapter_subqueue *sq = &grp->subqueues[q];
         sq->csi_index = q;
         sq->ringbuf_size = ADAPTER_RINGBUF_SIZE;
         sq->ring_alloc_size = ADAPTER_RINGBUF_SIZE + 4096U; /* +4 KiB trailing guard page */

         uint64_t rflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                           BASE_MEM_PROT_GPU_RD | BASE_MEM_SAME_VA | BASE_MEM_COHERENT_LOCAL;
         uint64_t rout_flags = 0;
         uint64_t rcookie = 0;
         uint64_t rpages = sq->ring_alloc_size >> 12;
         if (alloc_kbase_mem(g_ctx.mali_fd, rpages, rpages, 0, rflags, &rout_flags, &rcookie) < 0) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: ring alloc failed for queue %u", q);
            ret = -1;
            break;
         }
         void *rcpu = real_mmap(nullptr, sq->ring_alloc_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, g_ctx.mali_fd, static_cast<off_t>(rcookie));
         if (rcpu == MAP_FAILED) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: ring mmap failed for queue %u", q);
            ret = -1;
            break;
         }
         memset(rcpu, 0, sq->ring_alloc_size);
         sq->ring_cpu_ptr = static_cast<uint64_t *>(rcpu);
         sq->ring_gpu_va = reinterpret_cast<uintptr_t>(rcpu);

         struct kbase_ioctl_cs_queue_register qreg;
         memset(&qreg, 0, sizeof(qreg));
         qreg.buffer_gpu_addr = sq->ring_gpu_va;
         qreg.buffer_size = ADAPTER_RINGBUF_SIZE;
         qreg.priority = 1;
         if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &qreg) < 0) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: KBASE_IOCTL_CS_QUEUE_REGISTER failed for queue %u", q);
            ret = -1;
            break;
         }

         uint8_t subq_group_handle = grp->kbase_group_handle;
         uint8_t subq_csi_index = static_cast<uint8_t>(q);
         if (!single_csg && q > 0) {
            uint8_t separate_handle = 0;
            if (create_kbase_csg_with_fallback(
                   tiler_mask, frag_mask, comp_mask, 1U,
                   kbase_csg_prio,
                   tiler_max, frag_max, comp_max,
                   csi_handlers, &separate_handle, nullptr) == 0) {
               subq_group_handle = separate_handle;
               subq_csi_index = 0;
            }
         }
         sq->kbase_group_handle = subq_group_handle;
         sq->csi_index = subq_csi_index;

         union kbase_ioctl_cs_queue_bind qbind;
         memset(&qbind, 0, sizeof(qbind));
         qbind.in.buffer_gpu_addr = sq->ring_gpu_va;
         qbind.in.group_handle = subq_group_handle;
         qbind.in.csi_index = subq_csi_index;
         if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_BIND, &qbind) < 0) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: KBASE_IOCTL_CS_QUEUE_BIND failed for queue %u", q);
            ret = -1;
            break;
         }

         /* Strictly mandatory: mmap 3 USER_IO pages BEFORE first CS_QUEUE_KICK! */
         void *uio = real_mmap(nullptr, 3 * 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, g_ctx.mali_fd, static_cast<off_t>(qbind.out.mmap_handle));
         if (uio == MAP_FAILED) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: USER_IO mmap failed for queue %u", q);
            ret = -1;
            break;
         }
         sq->user_io_ptr = uio;
         sq->bound = true;

         /* Isolate seqno & breadcrumb cells per (g_slot, q) across all 8 groups (32 * 64B = 2048B)
          * so multi-device / multi-group apps (DOG WITCH.exe, Machick 2.exe, bcn_layer + DXVK)
          * never overwrite or alias another group's seqno cell in event_bo or diag_bo! */
         if (g_ctx.event_bo_cpu) {
            size_t seq_off = (g_slot * ADAPTER_MAX_SUBQUEUES + q) * ADAPTER_SEQNO_CELL_STRIDE;
            sq->seqno_gpu_va = g_ctx.event_bo_gpu_va + seq_off;
            sq->seqno_cpu_ptr = reinterpret_cast<volatile uint64_t *>(g_ctx.event_bo_cpu + seq_off);
            memset(g_ctx.event_bo_cpu + seq_off, 0, ADAPTER_SEQNO_CELL_STRIDE);
            kbase_cpu_cache_clean_range(g_ctx.event_bo_cpu + seq_off, ADAPTER_SEQNO_CELL_STRIDE);
         }
         if (g_ctx.diag_bo_cpu) {
            size_t diag_off = (g_slot * ADAPTER_MAX_SUBQUEUES + q) * ADAPTER_SEQNO_CELL_STRIDE;
            sq->diag_gpu_va = g_ctx.diag_bo_gpu_va + diag_off;
            sq->diag_cpu_ptr = reinterpret_cast<volatile uint64_t *>(g_ctx.diag_bo_cpu + diag_off);
            memset(g_ctx.diag_bo_cpu + diag_off, 0, ADAPTER_SEQNO_CELL_STRIDE);
            kbase_cpu_cache_clean_range(g_ctx.diag_bo_cpu + diag_off, ADAPTER_SEQNO_CELL_STRIDE);
         }
      }

      gc->group_handle = grp->group_handle;
      LOG_INFO("DRM_PANTHOR_GROUP_CREATE -> KBASE_IOCTL_CS_QUEUE_GROUP_CREATE handle=%u kbase_group=%u layout=%uB single_csg=%d csi_handlers=0x%x queues=%u",
               grp->group_handle, grp->kbase_group_handle, grp->group_create_layout,
               grp->single_csg_mode ? 1 : 0, grp->csi_handlers, qcnt);
      trace_log("DRM_PANTHOR_GROUP_CREATE -> KBASE_IOCTL_CS_QUEUE_GROUP_CREATE handle=%u queues=%u\n",
                grp->group_handle, qcnt);
      break;
   }

   case DRM_PANTHOR_GROUP_DESTROY: {
      auto *gd = static_cast<struct drm_panthor_group_destroy *>(arg);
      struct adapter_group *grp = find_group(gd->group_handle);
      if (grp) {
         for (uint32_t q = 0; q < grp->queue_count; q++) {
            struct adapter_subqueue *sq = &grp->subqueues[q];
            if (sq->bound) {
               if (sq->user_io_ptr)
                  real_munmap(sq->user_io_ptr, 3 * 4096);
               struct kbase_ioctl_cs_queue_terminate qt = { .buffer_gpu_addr = sq->ring_gpu_va };
               adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &qt);
               if (sq->ring_cpu_ptr)
                  real_munmap(sq->ring_cpu_ptr, sq->ring_alloc_size ? sq->ring_alloc_size : sq->ringbuf_size);
               if (sq->kbase_group_handle != 0 && sq->kbase_group_handle != grp->kbase_group_handle) {
                  struct kbase_ioctl_cs_queue_group_term sgt = {
                     .group_handle = sq->kbase_group_handle,
                     .padding = {0}
                  };
                  adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &sgt);
               }
            }
         }
         struct kbase_ioctl_cs_queue_group_term gt = {
            .group_handle = grp->kbase_group_handle,
            .padding = {0}
         };
         adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &gt);
         LOG_INFO("DRM_PANTHOR_GROUP_DESTROY -> KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE handle=%u",
                  grp->group_handle);
         memset(grp, 0, sizeof(*grp));
      } else {
         LOG_WARN("DRM_PANTHOR_GROUP_DESTROY handle=%u not found", gd ? gd->group_handle : 0);
      }
      break;
   }

   case DRM_PANTHOR_GROUP_GET_STATE: {
      auto *gs = static_cast<struct drm_panthor_group_get_state *>(arg);
      struct adapter_group *grp = gs ? find_group(gs->group_handle) : nullptr;
      if (grp) {
         gs->state = grp->fatal_state;
         gs->fatal_queues = grp->fatal_queues;
      } else if (gs) {
         gs->state = 0;
         gs->fatal_queues = 0;
      }
      LOG_DEBUG("DRM_PANTHOR_GROUP_GET_STATE -> state=0x%x fatal_queues=0x%x",
                gs ? gs->state : 0, gs ? gs->fatal_queues : 0);
      break;
   }

   case DRM_PANTHOR_GROUP_SUBMIT: {
      auto *gs = static_cast<struct drm_panthor_group_submit *>(arg);
      struct adapter_group *grp = find_group(gs->group_handle);
      if (!grp) {
         LOG_ERROR("DRM_PANTHOR_GROUP_SUBMIT handle=%u not found", gs ? gs->group_handle : 0);
         ret = -1;
         break;
      }
      auto *qsubs = reinterpret_cast<struct drm_panthor_queue_submit *>(
         static_cast<uintptr_t>(gs->queue_submits.array));
      if (panthor_drm_capture_is_active() && qsubs && gs->queue_submits.count > 0) {
         panthor_drm_capture_record_pre_submit(grp->group_handle, qsubs, gs->queue_submits.count);
      }

      uint64_t submit_t0_ns = get_monotonic_ns();
      if (g_ctx.window_start_ns == 0ULL)
         g_ctx.window_start_ns = submit_t0_ns;

      if (g_ctx.total_group_submits < 5ULL)
         LOG_INFO("DRM_PANTHOR_GROUP_SUBMIT handle=%u queue_submits=%u",
                  grp->group_handle, gs->queue_submits.count);
      else
         LOG_DEBUG("DRM_PANTHOR_GROUP_SUBMIT handle=%u queue_submits=%u",
                   grp->group_handle, gs->queue_submits.count);

      /* Flush CPU caches ONLY for CPU-cached (BASE_MEM_CACHED_CPU) writable BOs (or all BOs if
       * PANTHOR_ADAPTER_FULL_BO_CACHE_CLEAN=1). Write-Combined BOs (!BASE_MEM_CACHED_CPU) only
       * require a single dsb sy barrier to drain the CPU write-combine buffer! */
      uint64_t clean_t0_ns = get_monotonic_ns();
#if defined(__aarch64__)
      {
         static int full_clean = -1;
         if (full_clean < 0) {
            const char *fc_env = getenv("PANTHOR_ADAPTER_FULL_BO_CACHE_CLEAN");
            full_clean = (fc_env && fc_env[0] && atoi(fc_env) != 0) ? 1 : 0;
         }
         size_t bo_lim = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
         if (bo_lim > ADAPTER_MAX_BOS)
            bo_lim = ADAPTER_MAX_BOS;
         for (size_t b = 0; b < bo_lim; b++) {
            if (g_ctx.bos[b].in_use && g_ctx.bos[b].cpu_ptr &&
                !g_ctx.bos[b].is_imported && !g_ctx.bos[b].is_executable &&
                g_ctx.bos[b].size <= (4U * 1024U * 1024U) &&
                (full_clean || (g_ctx.bos[b].kbase_flags & BASE_MEM_CACHED_CPU) != 0)) {
               kbase_cpu_cache_clean_range(g_ctx.bos[b].cpu_ptr, g_ctx.bos[b].size);
            }
         }
         __asm__ volatile("dsb sy" : : : "memory");
      }
#else
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
      uint64_t clean_t1_ns = get_monotonic_ns();
      uint64_t submit_norm_ns = 0ULL;

      bool breadcrumbs = is_csf_breadcrumbs_enabled();
      uint32_t touched_subqueues = 0;
      uint64_t group_dep_seqno[ADAPTER_MAX_SUBQUEUES] = {0};
      for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++)
         group_dep_seqno[q] = grp->subqueues[q].emitted_jobs;

      /* Pre-scan all WAIT sync_ops across the entire DRM_PANTHOR_GROUP_SUBMIT batch */
      for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
         struct drm_panthor_queue_submit *qs = &qsubs[i];
         auto *sops = reinterpret_cast<struct drm_panthor_sync_op *>(
            static_cast<uintptr_t>(qs->syncs.array));
         if (!sops)
            continue;
         for (uint32_t s = 0; s < qs->syncs.count; s++) {
            if ((sops[s].flags & DRM_PANTHOR_SYNC_OP_SIGNAL) == 0) {
               struct adapter_syncobj *wso = find_syncobj(sops[s].handle);
               if (wso) {
                  for (uint32_t q = 0; q < ADAPTER_MAX_SUBQUEUES; q++) {
                     uint64_t wseq = wso->target_subq_seqno[q] ? wso->target_subq_seqno[q] : wso->last_target_subq_seqno[q];
                     if (wseq > group_dep_seqno[q])
                        group_dep_seqno[q] = wseq;
                  }
               }
            }
         }
      }

      for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
         struct drm_panthor_queue_submit *qs = &qsubs[i];
         if (qs->queue_index >= grp->queue_count)
            continue;
         struct adapter_subqueue *sq = &grp->subqueues[qs->queue_index];

         if (qs->stream_size == 0 || qs->stream_addr == 0) {
            continue;
         }

         bool is_initial = (sq->emitted_jobs == 0);

         /* Fast-path for 8-byte (1-qword) pure-NOP streams emitted by PanVK */
         if (!is_initial && qs->stream_size == 8U && qs->stream_addr != 0ULL) {
            const auto *nop_check = reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(qs->stream_addr));
            if (nop_check[0] == 0ULL) {
               LOG_DEBUG("DRM_PANTHOR_GROUP_SUBMIT q=%u pure-NOP stream (sz=8) elided -> bound syncs to jobs=%llu",
                         qs->queue_index, static_cast<unsigned long long>(sq->emitted_jobs));
               continue;
            }
         }

         /* 1. Normalize the submitted CSF command stream in-place (with 8-way static CB signature cache)
          *    and optionally snapshot dynamic streams into shadow_cs_bo so in-flight CS memory can never
          *    be overwritten by early pool resets. */
         uint64_t norm_t0_ns = get_monotonic_ns();
         struct adapter_cs_patch_stats stats;
         memset(&stats, 0, sizeof(stats));
         uint64_t effective_stream_va = qs->stream_addr;
         if (qs->stream_addr && qs->stream_size >= 8) {
            auto *stream_cpu = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(qs->stream_addr));
            uint32_t n_qw = qs->stream_size / sizeof(uint64_t);
            bool cache_hit = false;
            {
               panthor_kbase_normalize_cs_stream(stream_cpu, qs->stream_size, is_initial, &stats);
            if (is_initial && stats.extracted_subq_ctx_va) {
               sq->cached_subq_ctx_va = stats.extracted_subq_ctx_va;
               sq->cached_subq_ctx_reg = stats.extracted_subq_ctx_reg;

               /*
                * Zero-`mesa_overlay` CSF_EVENT Syncobj Redirection:
                * Upstream PanVK sub-allocates `queue->syncobjs` (at offset +0 of
                * `struct panvk_cs_subqueue_context`) and `render.desc_ringbuf.syncobj`
                * (at offset +24) inside `dev->mempools.rw` (a 2 MiB `BASE_MEM_COHERENT_LOCAL` BO).
                * On Kbase (`mali_kbase_mem_linux.c:133` & `mtk_platform_qinspect.c:67`),
                * cross-subqueue `SYNC_WAIT32/64` and `SYNC_ADD32/64.system` words must live
                * in `BASE_MEM_CSF_EVENT` memory (`g_ctx.event_bo`) on isolated 64-byte cache lines
                * that are never touched by `LOAD_MULTIPLE`/`STORE_MULTIPLE`.
                * Redirect `subq_ctx->syncobjs` and `subq_ctx->render.desc_ringbuf.syncobj`
                * in-place into dedicated 64-byte-aligned cells in `g_ctx.event_bo`!
                */
               const char *redir_env = getenv("PANTHOR_ADAPTER_REDIRECT_SYNCOBJS");
               bool allow_redir = (redir_env && redir_env[0]) ? (atoi(redir_env) != 0) : (!grp->single_csg_mode);
               if (allow_redir && g_ctx.event_bo_cpu &&
                   g_ctx.event_bo_size >= ADAPTER_EVENT_BO_SYNCOBJS_BASE_OFFSET +
                                          ADAPTER_MAX_GROUPS * ADAPTER_EVENT_BO_GROUP_STRIDE) {
                  auto *subq_ctx_u64 = static_cast<uint64_t *>(
                     resolve_gpu_va_to_cpu(sq->cached_subq_ctx_va, 64));
                  if (subq_ctx_u64) {
                     uint32_t g_slot = (grp->group_handle > 0)
                                          ? ((grp->group_handle - 1U) % ADAPTER_MAX_GROUPS)
                                          : 0U;
                     size_t grp_ev_off = ADAPTER_EVENT_BO_SYNCOBJS_BASE_OFFSET +
                                         g_slot * ADAPTER_EVENT_BO_GROUP_STRIDE;
                     if (grp->redirected_syncobjs_va == 0) {
                        uint8_t *ev_sync_cpu = g_ctx.event_bo_cpu + grp_ev_off;
                        memset(ev_sync_cpu, 0, 256);
                        auto *ev_sync_u64 = reinterpret_cast<uint64_t *>(ev_sync_cpu);
                        /* Copy initial values from original queue->syncobjs if valid, ensuring seqno >= 1 */
                        uint64_t orig_syncobjs_va = subq_ctx_u64[MALI_PANVK_SUBQ_CTX_SYNCOBJS_OFFSET / 8];
                        auto *orig_sync_u64 = static_cast<const uint64_t *>(
                           resolve_gpu_va_to_cpu(orig_syncobjs_va, 48));
                        for (uint32_t s_idx = 0; s_idx < 3; s_idx++) {
                           uint64_t init_seq = orig_sync_u64 ? orig_sync_u64[s_idx * 2] : 1ULL;
                           ev_sync_u64[s_idx * 2]     = (init_seq != 0ULL) ? init_seq : 1ULL;
                           ev_sync_u64[s_idx * 2 + 1] = 0ULL;
                        }
                        /* Also initialize tessellation free sync at +128 to 1 (Patch 082) */
                        *reinterpret_cast<uint32_t *>(ev_sync_cpu + 128) = 1U;
                        kbase_cpu_cache_clean_range(ev_sync_cpu, 256);
                        grp->redirected_syncobjs_va = g_ctx.event_bo_gpu_va + grp_ev_off;
                     }
                     subq_ctx_u64[MALI_PANVK_SUBQ_CTX_SYNCOBJS_OFFSET / 8] = grp->redirected_syncobjs_va;

                     uint64_t orig_rb_sync_va = subq_ctx_u64[MALI_PANVK_SUBQ_CTX_RINGBUF_SYNCOBJ_OFFSET / 8];
                     if (orig_rb_sync_va != 0) {
                        if (grp->redirected_ringbuf_sync_va == 0) {
                           uint8_t *ev_rb_cpu = g_ctx.event_bo_cpu + grp_ev_off + 256;
                           memset(ev_rb_cpu, 0, 64);
                           auto *orig_rb_u32 = static_cast<const uint32_t *>(
                              resolve_gpu_va_to_cpu(orig_rb_sync_va, 8));
                           uint32_t init_rb = (orig_rb_u32 && orig_rb_u32[0] != 0)
                                                 ? orig_rb_u32[0]
                                                 : MALI_PANVK_RENDER_DESC_RINGBUF_SIZE;
                           *reinterpret_cast<uint32_t *>(ev_rb_cpu) = init_rb;
                           kbase_cpu_cache_clean_range(ev_rb_cpu, 64);
                           grp->redirected_ringbuf_sync_va = g_ctx.event_bo_gpu_va + grp_ev_off + 256;
                        }
                        subq_ctx_u64[MALI_PANVK_SUBQ_CTX_RINGBUF_SYNCOBJ_OFFSET / 8] =
                           grp->redirected_ringbuf_sync_va;
                     }
                     kbase_cpu_cache_clean_range(subq_ctx_u64, 64);
                  }
               }
            }
            /* Compiler-style Worklist Algorithm over the transitive closure of reachable
             * CSF chunks (CSF_OP_CALL bounded by MALI_CSF_MAX_CALL_STACK_DEPTH = 8,
             * CSF_OP_JUMP unbounded tail chain) with Full-Chunk 64-bit FNV-1a Hashing
             * (doco.DxclyE60VXtoVPV24H0N7g, doco.YWt5K_8F13z7sSe1GC0DZw, doco.HQxWytIKZO52FEwYGM2FPQ, doco.fdxkj9JdapjjMg1-Mt1DZQ). */
            {
               bool bug_mode = is_bug_catching_mode_enabled();
               uint64_t root_post_hash = panthor_fnv1a64_bytes(stream_cpu, qs->stream_size);
               if (panthor_drm_capture_is_active()) {
                  panthor_drm_capture_record_csf_chunk(
                     grp->group_handle, qs->queue_index, 0U, 0U, 0U,
                     qs->stream_addr, qs->stream_size, root_post_hash, root_post_hash, stream_cpu);
               }

               struct cs_work_item {
                  uint64_t va;
                  uint32_t sz;
                  uint16_t call_depth;
                  uint16_t is_jump;
               };
               cs_work_item worklist[64];
               uint32_t wl_head = 0U, wl_tail = 0U;
               uint64_t visited_va[64];
               uint32_t visited_sz[64];
               uint32_t visited_cnt = 0U;
               uint32_t chunk_seq = 0U;

               auto mark_visited = [&](uint64_t v, uint32_t s_bytes) -> bool {
                  for (uint32_t vi = 0; vi < visited_cnt; vi++) {
                     if (visited_va[vi] == v && visited_sz[vi] == s_bytes)
                        return false;
                  }
                  if (visited_cnt < 64U) {
                     visited_va[visited_cnt] = v;
                     visited_sz[visited_cnt] = s_bytes;
                     visited_cnt++;
                     return true;
                  }
                  return false;
               };

               auto scan_and_cache_edges = [&](uint64_t chunk_va, uint64_t *chunk_cpu, uint32_t chunk_sz,
                                               uint16_t cur_call_depth, uint64_t post_hash) {
                  uint32_t c_qwords = chunk_sz / sizeof(uint64_t);
                  uint64_t reg_file[128] = {0};
                  uint32_t slot_idx = (sq->norm_cache_hand++) & 31U;
                  for (uint32_t ci = 0; ci < 32U; ci++) {
                     if (sq->norm_cache[ci].va == chunk_va && sq->norm_cache[ci].sz == chunk_sz) {
                        slot_idx = ci;
                        break;
                     }
                  }
                  auto &ce = sq->norm_cache[slot_idx];
                  ce.va = chunk_va;
                  ce.sz = chunk_sz;
                  ce.post_norm_fnv1a64 = post_hash;
                  ce.edge_count = 0U;

                  for (uint32_t idx = 0; idx < c_qwords; idx++) {
                     uint64_t insn = chunk_cpu[idx];
                     uint8_t op = static_cast<uint8_t>((insn >> 56) & 0xffULL);
                     if (op == MALI_CSF_OP_MOVE48) {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                        reg_file[dst] = insn & MALI_CSF_INS_MOVE48_IMMEDIATE_MASK;
                     } else if (op == MALI_CSF_OP_MOVE32) {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0x7fU);
                        reg_file[dst] = insn & MALI_CSF_INS_MOVE32_IMMEDIATE_MASK;
                     } else {
                        uint8_t cj_op = 0, addr_reg = 0, len_reg = 0;
                        if (mali_csf_unpack_call_or_jump(insn, &cj_op, &addr_reg, &len_reg) &&
                            addr_reg < 128U && len_reg < 128U) {
                           uint64_t t_va = reg_file[addr_reg];
                           uint32_t t_sz = static_cast<uint32_t>(reg_file[len_reg]);
                           if (t_va != 0 && t_sz >= sizeof(uint64_t) && t_sz <= (4U * 1024U * 1024U)) {
                              bool is_call_op = (cj_op == MALI_CSF_OP_CALL);
                              if (!is_call_op || (cur_call_depth + 1U < MALI_CSF_MAX_CALL_STACK_DEPTH)) {
                                 if (ce.edge_count < 8U) {
                                    ce.edges[ce.edge_count].target_va = t_va;
                                    ce.edges[ce.edge_count].target_sz = t_sz;
                                    ce.edges[ce.edge_count].is_call = is_call_op ? 1U : 0U;
                                    ce.edge_count++;
                                 }
                                 if (wl_tail < 64U && mark_visited(t_va, t_sz)) {
                                    worklist[wl_tail++] = cs_work_item{
                                       .va = t_va,
                                       .sz = t_sz,
                                       .call_depth = static_cast<uint16_t>(is_call_op ? (cur_call_depth + 1U) : cur_call_depth),
                                       .is_jump = static_cast<uint16_t>(is_call_op ? 0U : 1U)
                                    };
                                 }
                              }
                           }
                        }
                     }
                  }
               };

               mark_visited(qs->stream_addr, qs->stream_size);
               scan_and_cache_edges(qs->stream_addr, stream_cpu, qs->stream_size, 0U, root_post_hash);

               while (wl_head < wl_tail) {
                  cs_work_item item = worklist[wl_head++];
                  void *target_raw = resolve_gpu_va_to_cpu(item.va, item.sz);
                  if (!target_raw)
                     continue;
                  auto *target_cpu = static_cast<uint64_t *>(target_raw);
                  uint64_t pre_h = panthor_fnv1a64_bytes(target_cpu, item.sz);
                  bool sub_hit = false;
                  if (!bug_mode) {
                     for (uint32_t ci = 0; ci < 32U; ci++) {
                        const auto &ce = sq->norm_cache[ci];
                        if (ce.va == item.va && ce.sz == item.sz && ce.post_norm_fnv1a64 == pre_h) {
                           sub_hit = true;
                           for (uint32_t ei = 0; ei < ce.edge_count; ei++) {
                              bool is_c = (ce.edges[ei].is_call != 0U);
                              if (!is_c || (item.call_depth + 1U < MALI_CSF_MAX_CALL_STACK_DEPTH)) {
                                 if (wl_tail < 64U && mark_visited(ce.edges[ei].target_va, ce.edges[ei].target_sz)) {
                                    worklist[wl_tail++] = cs_work_item{
                                       .va = ce.edges[ei].target_va,
                                       .sz = ce.edges[ei].target_sz,
                                       .call_depth = static_cast<uint16_t>(is_c ? (item.call_depth + 1U) : item.call_depth),
                                       .is_jump = static_cast<uint16_t>(is_c ? 0U : 1U)
                                    };
                                 }
                              }
                           }
                           break;
                        }
                     }
                  }
                  uint64_t post_h = pre_h;
                  if (!sub_hit) {
                     struct adapter_cs_patch_stats sub_stats{};
                     panthor_kbase_normalize_cs_stream(target_cpu, item.sz, false, &sub_stats);
                     stats.sync_scope_upgrades += sub_stats.sync_scope_upgrades;
                     stats.deferred_sync_waits += sub_stats.deferred_sync_waits;
                     stats.heap_op_nops += sub_stats.heap_op_nops;
                     stats.finish_frag_stat_clears += sub_stats.finish_frag_stat_clears;
                     kbase_cpu_cache_clean_range(target_cpu, item.sz);
                     post_h = panthor_fnv1a64_bytes(target_cpu, item.sz);
                     scan_and_cache_edges(item.va, target_cpu, item.sz, item.call_depth, post_h);
                  }
                  if (panthor_drm_capture_is_active()) {
                     panthor_drm_capture_record_csf_chunk(
                        grp->group_handle, qs->queue_index, ++chunk_seq,
                        item.call_depth, item.is_jump, item.va, item.sz,
                        pre_h, post_h, target_cpu);
                  }
               }
            }
               kbase_cpu_cache_clean_range(stream_cpu, qs->stream_size);
            }

            /* Snapshot single-chunk primary CS streams (<= 32 KiB without relative JUMP) into
             * g_ctx.shadow_cs_bo_cpu (unless PANTHOR_ADAPTER_BUG_CATCHING_MODE=1 is set). */
            if (!is_initial && !cache_hit && !is_bug_catching_mode_enabled() && g_ctx.shadow_cs_bo_cpu &&
                qs->stream_size <= 32768U && g_ctx.shadow_cs_bo_size >= 65536U) {
               bool has_jump = false;
               for (uint32_t idx = 0; idx < n_qw; idx++) {
                  uint64_t op = (stream_cpu[idx] >> 56) & 0xffULL;
                  if (op == CSF_OP_JUMP) {
                     has_jump = true;
                     break;
                  }
               }
               if (!has_jump) {
                  size_t aligned_sz = (static_cast<size_t>(qs->stream_size) + 63U) & ~static_cast<size_t>(63U);
                  if (g_ctx.shadow_cs_write_off + aligned_sz > g_ctx.shadow_cs_bo_size)
                     g_ctx.shadow_cs_write_off = 0;
                  uint8_t *dst_shadow = g_ctx.shadow_cs_bo_cpu + g_ctx.shadow_cs_write_off;
                  memcpy(dst_shadow, stream_cpu, qs->stream_size);
                  kbase_cpu_cache_clean_range(dst_shadow, qs->stream_size);
                  effective_stream_va = g_ctx.shadow_cs_bo_gpu_va + g_ctx.shadow_cs_write_off;
                  g_ctx.shadow_cs_write_off += aligned_sz;
               }
            }
         }
         submit_norm_ns += (get_monotonic_ns() - norm_t0_ns);

         /* 2. Ensure the 64 KiB ring buffer has space (never overwrite unextracted CSF commands) */
         uint64_t extract_at_sub = 0;
         if (sq->user_io_ptr) {
            const auto *out_u64 = reinterpret_cast<const volatile uint64_t *>(
               static_cast<const uint8_t *>(sq->user_io_ptr) + CS_USER_IO_OUTPUT_PAGE_OFFSET);
            for (unsigned wait_ring = 0; wait_ring < 500; wait_ring++) {
               extract_at_sub = out_u64[0];
               if ((sq->insert_offset + ADAPTER_RING_SLOT_MAX_BYTES * 2) - extract_at_sub <= sq->ringbuf_size)
                  break;
               struct kbase_ioctl_cs_queue_kick kick = { .buffer_gpu_addr = sq->ring_gpu_va };
               adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick);
               usleep(1000);
            }
         }

         /*
          * 3. Stage the CSF ring slot into `staged_slot` first to obtain the exact `slot_bytes`
          *    (192B without breadcrumbs, 320B with breadcrumbs), then check whether
          *    `ring_off + slot_bytes > sq->ringbuf_size` BEFORE writing to `sq->ring_cpu_ptr`!
          *    This prevents any ring slot from straddling the 64 KiB ringbuffer boundary
          *    (which caused Job 410 at ring_off=65344 to overflow bytes 192..319 past 65536).
          */
         uint64_t next_job_seqno = sq->emitted_jobs + 1;
         uint64_t staged_slot[ADAPTER_RING_SLOT_MAX_QWORDS];
         memset(staged_slot, 0, sizeof(staged_slot));
         size_t slot_bytes = panthor_kbase_emit_ring_slot_ex(
            staged_slot,
            g_ctx.arch_major,
            g_ctx.csif_info.cs_reg_count,
            qs->queue_index,
            sq->cached_subq_ctx_va,
            grp->active_tiler_heap_ctx_va,
            effective_stream_va,
            qs->stream_size,
            qs->latest_flush,
            sq->seqno_gpu_va,
            static_cast<uint16_t>(g_ctx.csif_info.scoreboard_slot_count
               ? ((1U << g_ctx.csif_info.scoreboard_slot_count) - 1U)
               : 0x00ffU),
            next_job_seqno,
            breadcrumbs);

         uint64_t insert_before = sq->insert_offset;
         uint32_t ring_off = static_cast<uint32_t>(sq->insert_offset % sq->ringbuf_size);
         size_t wrap_nop_bytes = 0;
         if (ring_off + slot_bytes > sq->ringbuf_size) {
            uint8_t *wrap_ptr = reinterpret_cast<uint8_t *>(sq->ring_cpu_ptr) + ring_off;
            wrap_nop_bytes = sq->ringbuf_size - ring_off;
            memset(wrap_ptr, 0, wrap_nop_bytes);
            kbase_cpu_cache_clean_range(wrap_ptr, wrap_nop_bytes);
            sq->insert_offset += wrap_nop_bytes;
            sq->ring_wraps++;
            LOG_DEBUG("DRM_PANTHOR_GROUP_SUBMIT q=%u job=%llu ring wrap #%llu at off=%u (+%zu > %u): NOP-padded %zuB -> ring_off=0",
                      qs->queue_index, static_cast<unsigned long long>(next_job_seqno),
                      static_cast<unsigned long long>(sq->ring_wraps),
                      ring_off, slot_bytes, sq->ringbuf_size, wrap_nop_bytes);
            ring_off = 0;
         }
         auto *slot_ptr = reinterpret_cast<uint64_t *>(
            reinterpret_cast<uint8_t *>(sq->ring_cpu_ptr) + ring_off);
         memcpy(slot_ptr, staged_slot, slot_bytes);

         /* Drain CPU cache / write-combine buffer so CSF MCU reads valid instructions from DRAM */
         kbase_cpu_cache_clean_range(slot_ptr, slot_bytes);

         sq->last_stream_va = effective_stream_va;
         sq->last_stream_size = qs->stream_size;
         sq->job_stream_va[next_job_seqno & 63U] = effective_stream_va;
         sq->job_stream_size[next_job_seqno & 63U] = qs->stream_size;
         sq->last_ring_slot_offset = ring_off;
         sq->last_ring_slot_bytes = static_cast<uint32_t>(slot_bytes);
         sq->insert_offset += slot_bytes;
         sq->emitted_jobs = next_job_seqno;
         if (qs->queue_index < ADAPTER_MAX_SUBQUEUES && next_job_seqno > group_dep_seqno[qs->queue_index])
            group_dep_seqno[qs->queue_index] = next_job_seqno;

         struct adapter_flight_entry *fe = &sq->flight_log[next_job_seqno % ADAPTER_FLIGHT_LOG_ENTRIES];
         fe->job_seqno = next_job_seqno;
         fe->ring_off = ring_off;
         fe->slot_bytes = static_cast<uint32_t>(slot_bytes);
         fe->wrap_nop_bytes = static_cast<uint32_t>(wrap_nop_bytes);
         fe->insert_before = insert_before;
         fe->insert_after = sq->insert_offset;
         fe->extract_at_submit = extract_at_sub;
         fe->stream_va = qs->stream_addr;
         fe->stream_size = qs->stream_size;
         fe->submit_ts_ns = get_monotonic_ns();
         touched_subqueues |= (1U << qs->queue_index);

         /* 4. Update signaled drm_syncobjs */
         auto *sops = reinterpret_cast<struct drm_panthor_sync_op *>(
            static_cast<uintptr_t>(qs->syncs.array));
         for (uint32_t s = 0; s < qs->syncs.count; s++) {
            if (sops[s].flags & DRM_PANTHOR_SYNC_OP_SIGNAL) {
               struct adapter_syncobj *so = find_syncobj(sops[s].handle);
               if (so) {
                  so->value = sops[s].timeline_value ? sops[s].timeline_value : 1ULL;
                  so->target_group_handle = grp->group_handle;
                  so->last_target_group_handle = grp->group_handle;
                  if (qs->queue_index < ADAPTER_MAX_SUBQUEUES) {
                     so->target_subq_seqno[qs->queue_index] = sq->emitted_jobs;
                     so->last_target_subq_seqno[qs->queue_index] = sq->emitted_jobs;
                  }
                  if (so->cqs_cpu_ptr)
                     *so->cqs_cpu_ptr = so->value;
               }
            }
         }

         if (g_ctx.total_group_submits < 5ULL) {
            LOG_INFO("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u fin_frag_clears=%u -> insert=%llu jobs=%llu",
                     qs->queue_index, static_cast<unsigned long long>(qs->stream_addr),
                     qs->stream_size, stats.sync_scope_upgrades,
                     stats.heap_op_nops, stats.finish_frag_stat_clears,
                     static_cast<unsigned long long>(sq->insert_offset),
                     static_cast<unsigned long long>(sq->emitted_jobs));
         } else {
            LOG_DEBUG("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u fin_frag_clears=%u -> insert=%llu jobs=%llu",
                      qs->queue_index, static_cast<unsigned long long>(qs->stream_addr),
                      qs->stream_size, stats.sync_scope_upgrades,
                      stats.heap_op_nops, stats.finish_frag_stat_clears,
                      static_cast<unsigned long long>(sq->insert_offset),
                      static_cast<unsigned long long>(sq->emitted_jobs));
         }
         trace_log("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u -> KBASE_IOCTL_CS_QUEUE_KICK insert=%llu\n",
                   qs->queue_index, static_cast<unsigned long long>(qs->stream_addr),
                   qs->stream_size, stats.sync_scope_upgrades,
                   stats.heap_op_nops, static_cast<unsigned long long>(sq->insert_offset));
      }

      /* Pass 2: Propagate the full group_dep_seqno[0..queue_count-1] across ALL subqueues
       * to every SIGNAL sync_op in this DRM_PANTHOR_GROUP_SUBMIT batch (including sync-only
       * and pure-NOP qsubmits), so waiting on any fence/semaphore signaled by this batch
       * always waits for all active subqueues (VT, FRAG, COMPUTE) to finish! */
      for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
         if (grp->subqueues[q].emitted_jobs > group_dep_seqno[q])
            group_dep_seqno[q] = grp->subqueues[q].emitted_jobs;
      }
      for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
         struct drm_panthor_queue_submit *qs = &qsubs[i];
         auto *sops = reinterpret_cast<struct drm_panthor_sync_op *>(
            static_cast<uintptr_t>(qs->syncs.array));
         if (!sops)
            continue;
         for (uint32_t s = 0; s < qs->syncs.count; s++) {
            if (sops[s].flags & DRM_PANTHOR_SYNC_OP_SIGNAL) {
               struct adapter_syncobj *so = find_syncobj(sops[s].handle);
               if (so) {
                  so->value = sops[s].timeline_value ? sops[s].timeline_value : 1ULL;
                  so->target_group_handle = grp->group_handle;
                  so->last_target_group_handle = grp->group_handle;
                  for (uint32_t q = 0; q < grp->queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
                     if (group_dep_seqno[q] > so->target_subq_seqno[q])
                        so->target_subq_seqno[q] = group_dep_seqno[q];
                     if (group_dep_seqno[q] > so->last_target_subq_seqno[q])
                        so->last_target_subq_seqno[q] = group_dep_seqno[q];
                  }
                  if (so->cqs_cpu_ptr)
                     *so->cqs_cpu_ptr = so->value;
               }
            }
         }
      }

      /*
       * 5. Phase 2 (Publish & Kick): After ALL subqueues' ring slots are emitted and
       * cache-cleaned, publish CS_INSERT in USER_IO Input Page (+0x1000), ring Doorbell (+0x0000),
       * and call KBASE_IOCTL_CS_QUEUE_KICK for each touched subqueue.
       */
      uint64_t kick_t0_ns = get_monotonic_ns();
      for (uint32_t q = 0; q < grp->queue_count; q++) {
         if (!(touched_subqueues & (1U << q)))
            continue;
         struct adapter_subqueue *sq = &grp->subqueues[q];
         auto *input_page = reinterpret_cast<volatile uint64_t *>(
             static_cast<uint8_t *>(sq->user_io_ptr) + CS_USER_IO_INPUT_PAGE_OFFSET);
         auto *doorbell_page = reinterpret_cast<volatile uint32_t *>(
            static_cast<uint8_t *>(sq->user_io_ptr) + CS_USER_IO_DOORBELL_OFFSET);

         input_page[0] = sq->insert_offset;
         __atomic_thread_fence(__ATOMIC_SEQ_CST);
         doorbell_page[0] = 1U;
         __atomic_thread_fence(__ATOMIC_SEQ_CST);

         struct kbase_ioctl_cs_queue_kick kick = { .buffer_gpu_addr = sq->ring_gpu_va };
         adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick);
      }
      uint64_t submit_t1_ns = get_monotonic_ns();
      uint64_t submit_dur_ns = (submit_t1_ns >= submit_t0_ns) ? (submit_t1_ns - submit_t0_ns) : 0ULL;
      uint64_t clean_dur_ns  = (clean_t1_ns >= clean_t0_ns) ? (clean_t1_ns - clean_t0_ns) : 0ULL;
      uint64_t kick_dur_ns   = (submit_t1_ns >= kick_t0_ns) ? (submit_t1_ns - kick_t0_ns) : 0ULL;

      g_ctx.total_group_submits++;
      g_ctx.window_submits++;
      g_ctx.window_submit_ns_sum      += submit_dur_ns;
      if (submit_dur_ns > g_ctx.window_submit_ns_max)
         g_ctx.window_submit_ns_max = submit_dur_ns;
      g_ctx.window_cs_norm_ns_sum     += submit_norm_ns;
      g_ctx.window_cache_clean_ns_sum += clean_dur_ns;
      g_ctx.window_kick_ns_sum        += kick_dur_ns;

      static uint64_t telemetry_interval = UINT64_MAX;
      if (telemetry_interval == UINT64_MAX) {
         const char *ti_env = getenv("PANTHOR_ADAPTER_TELEMETRY_INTERVAL");
         telemetry_interval = (ti_env && *ti_env) ? strtoull(ti_env, nullptr, 0) : 200ULL;
      }
      if (telemetry_interval > 0ULL && (g_ctx.total_group_submits % telemetry_interval) == 0ULL) {
         double win_sec = (submit_t1_ns > g_ctx.window_start_ns)
                             ? (static_cast<double>(submit_t1_ns - g_ctx.window_start_ns) / 1e9)
                             : 1e-6;
         double win_submits = static_cast<double>(g_ctx.window_submits ? g_ctx.window_submits : 1ULL);
         double win_waits   = static_cast<double>(g_ctx.window_sync_waits ? g_ctx.window_sync_waits : 1ULL);
         g_ctx.last_window_fps         = (win_submits * 0.5) / win_sec;
         g_ctx.last_avg_submit_us      = (static_cast<double>(g_ctx.window_submit_ns_sum) / 1e3) / win_submits;
         g_ctx.last_max_submit_us      = static_cast<double>(g_ctx.window_submit_ns_max) / 1e3;
         g_ctx.last_avg_cs_norm_us     = (static_cast<double>(g_ctx.window_cs_norm_ns_sum) / 1e3) / win_submits;
         g_ctx.last_avg_cache_clean_us = (static_cast<double>(g_ctx.window_cache_clean_ns_sum) / 1e3) / win_submits;
         g_ctx.last_avg_kick_us        = (static_cast<double>(g_ctx.window_kick_ns_sum) / 1e3) / win_submits;
         g_ctx.last_avg_sync_wait_us   = g_ctx.window_sync_waits
                                            ? ((static_cast<double>(g_ctx.window_sync_wait_ns_sum) / 1e3) / win_waits)
                                            : 0.0;
         g_ctx.last_max_sync_wait_us   = static_cast<double>(g_ctx.window_sync_wait_ns_max) / 1e3;

         uint32_t act_bos = 0, cached_bos = 0, imp_bos = 0, act_so = 0;
         uint64_t act_bo_bytes = 0;
         size_t bo_lim = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
         if (bo_lim > ADAPTER_MAX_BOS) bo_lim = ADAPTER_MAX_BOS;
         for (size_t b = 0; b < bo_lim; b++) {
            if (g_ctx.bos[b].in_use) {
               act_bos++;
               act_bo_bytes += g_ctx.bos[b].size;
               if (g_ctx.bos[b].kbase_flags & BASE_MEM_CACHED_CPU) cached_bos++;
               if (g_ctx.bos[b].is_imported) imp_bos++;
            }
         }
         size_t so_lim = g_ctx.max_syncobj_slot_used ? g_ctx.max_syncobj_slot_used : ADAPTER_MAX_SYNCOBJS;
         if (so_lim > ADAPTER_MAX_SYNCOBJS) so_lim = ADAPTER_MAX_SYNCOBJS;
         for (size_t s_i = 0; s_i < so_lim; s_i++) {
            if (g_ctx.syncobjs[s_i].in_use) act_so++;
         }

         double rss_mb = 0.0;
         FILE *sfp = fopen("/proc/self/statm", "r");
         if (sfp) {
            unsigned long vmsz_pages = 0, rss_pages = 0;
            if (fscanf(sfp, "%lu %lu", &vmsz_pages, &rss_pages) == 2)
               rss_mb = (static_cast<double>(rss_pages) * 4096.0) / (1024.0 * 1024.0);
            fclose(sfp);
         }
         uint64_t j0 = grp->subqueues[0].emitted_jobs;
         uint64_t s0 = grp->subqueues[0].seqno_cpu_ptr ? *grp->subqueues[0].seqno_cpu_ptr : 0ULL;
         uint64_t s1 = (grp->queue_count > 1 && grp->subqueues[1].seqno_cpu_ptr) ? *grp->subqueues[1].seqno_cpu_ptr : 0ULL;
         uint64_t s2 = (grp->queue_count > 2 && grp->subqueues[2].seqno_cpu_ptr) ? *grp->subqueues[2].seqno_cpu_ptr : 0ULL;
         LOG_INFO("[TELEMETRY] submits=%llu jobs(vt=%llu,seq=%llu/%llu/%llu) fps=%.1f submit_us(avg=%.1f,max=%.1f: norm=%.1f,clean=%.1f,kick=%.1f) wait_us(avg=%.1f,max=%.1f,fast=%llu,poll=%llu) bos=%u(%.1fMB,cached=%u,imp=%u,rss=%.1fMB,guards=%llu) syncobjs=%u wraps=%llu/%llu/%llu",
                  static_cast<unsigned long long>(g_ctx.total_group_submits),
                  static_cast<unsigned long long>(j0),
                  static_cast<unsigned long long>(s0),
                  static_cast<unsigned long long>(s1),
                  static_cast<unsigned long long>(s2),
                  g_ctx.last_window_fps,
                  g_ctx.last_avg_submit_us, g_ctx.last_max_submit_us,
                  g_ctx.last_avg_cs_norm_us, g_ctx.last_avg_cache_clean_us, g_ctx.last_avg_kick_us,
                  g_ctx.last_avg_sync_wait_us, g_ctx.last_max_sync_wait_us,
                  static_cast<unsigned long long>(g_ctx.fast_spin_syncobj_waits),
                  static_cast<unsigned long long>(g_ctx.poll_sleep_syncobj_waits),
                  act_bos, static_cast<double>(act_bo_bytes) / (1024.0 * 1024.0), cached_bos, imp_bos,
                  rss_mb, static_cast<unsigned long long>(g_ctx.munmap_guard_hits),
                  act_so,
                  static_cast<unsigned long long>(grp->subqueues[0].ring_wraps),
                  static_cast<unsigned long long>(grp->queue_count > 1 ? grp->subqueues[1].ring_wraps : 0ULL),
                  static_cast<unsigned long long>(grp->queue_count > 2 ? grp->subqueues[2].ring_wraps : 0ULL));

         g_ctx.window_start_ns           = submit_t1_ns;
         g_ctx.window_submits            = 0;
         g_ctx.window_submit_ns_sum      = 0;
         g_ctx.window_submit_ns_max      = 0;
         g_ctx.window_cs_norm_ns_sum     = 0;
         g_ctx.window_cache_clean_ns_sum = 0;
         g_ctx.window_kick_ns_sum        = 0;
         g_ctx.window_sync_waits         = 0;
         g_ctx.window_sync_wait_ns_sum   = 0;
         g_ctx.window_sync_wait_ns_max   = 0;
      }
      break;
   }

   default:
      LOG_WARN("ioctl fd=%d req=%s(0x%lx): unhandled panthor ioctl id=%u",
               fd, panthor2kbase_ioctl_req_name(request), request, panthor_id);
      break;
   }

   pthread_mutex_unlock(&g_lock);
   panthor2kbase_log_ioctl_struct_out(request, arg, ret, errno);
   if (ret == 0)
      panthor_drm_capture_record_ioctl(fd, request, arg, ret);
   return ret;
}

void *panthor_kbase_adapter_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_mmap",
                           "addr=%p, len=%zu, prot=0x%x, flags=0x%x, fd=%d, off=0x%llx",
                           addr, length, prot, flags, fd, static_cast<unsigned long long>(offset));
   resolve_real_funcs();
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      LOG_DEBUG("adapter_mmap: non-virtual fd, passthrough");
      return real_mmap(addr, length, prot, flags, fd, offset);
   }

   pthread_mutex_lock(&g_lock);
   uint64_t uoff = static_cast<uint64_t>(offset);

   /* Case 1: Mapping DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET */
   if (uoff == DRM_PANTHOR_USER_MMIO_OFFSET_64BIT ||
       uoff == DRM_PANTHOR_USER_MMIO_OFFSET_32BIT ||
       (g_ctx.user_mmio_offset && uoff == g_ctx.user_mmio_offset)) {
      void *res = g_ctx.user_reg_page;
      if (!res) {
         res = real_mmap(addr, length, PROT_READ, MAP_SHARED,
                         g_ctx.mali_fd, static_cast<off_t>(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE));
         if (res != MAP_FAILED)
            g_ctx.user_reg_page = res;
      }
      LOG_INFO("adapter_mmap: USER_REG_PAGE fd=%d offset=0x%llx -> %p",
               fd, static_cast<unsigned long long>(offset), res);
      pthread_mutex_unlock(&g_lock);
      return (res && res != MAP_FAILED) ? res : MAP_FAILED;
   }

   /* Case 2: Mapping a GEM BO offset returned by DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET */
   if ((uoff & 0xffff00000000ULL) == 0x1000000000ULL) {
      uint32_t handle = static_cast<uint32_t>((uoff >> 12) & 0xfffffU);
      struct adapter_bo *bo = find_bo(handle);
      if (bo) {
         if (bo->cpu_ptr && (addr == nullptr || addr == bo->cpu_ptr)) {
            LOG_DEBUG("adapter_mmap: BO handle=%u returning cached cpu_ptr=%p",
                      bo->gem_handle, bo->cpu_ptr);
            void *ptr = bo->cpu_ptr;
            pthread_mutex_unlock(&g_lock);
            return ptr;
         }
         if (bo->dmabuf_fd >= 0) {
            void *ptr = real_mmap(addr, length, prot, flags, bo->dmabuf_fd, 0);
            if (ptr != MAP_FAILED && !bo->cpu_ptr) {
               bo->cpu_ptr = ptr;
               bo->cpu_size = length;
            }
            LOG_INFO("adapter_mmap: BO handle=%u dmabuf_fd=%d -> %p",
                     bo->gem_handle, bo->dmabuf_fd, ptr);
            pthread_mutex_unlock(&g_lock);
            return ptr;
         }
         if (bo->cpu_ptr) {
            void *ptr = bo->cpu_ptr;
            pthread_mutex_unlock(&g_lock);
            return ptr;
         }
      }
   }

   pthread_mutex_unlock(&g_lock);
   void *res = safe_real_mmap(addr, length, prot, flags, g_ctx.mali_fd, offset);
   LOG_DEBUG("adapter_mmap: mali passthrough -> %p", res);
   return res;
}

int panthor_kbase_adapter_munmap(void *addr, size_t length)
{
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_adapter_munmap", "addr=%p, len=%zu", addr, length);
   resolve_real_funcs();
   if (!addr)
      return real_munmap(addr, length);
   pthread_mutex_lock(&g_lock);
   if (g_ctx.active) {
      /* Guard shared context-wide mappings (especially g_ctx.user_reg_page @ 0x5ffffff000) so
       * closing a temporary VkInstance / pan_kmod_dev (e.g. Unity/DXVK swapchain re-enumeration
       * in DOG WITCH.exe & Machick 2.exe) NEVER unmaps USER_REG_PAGE while active VkDevices
       * dereference dev->kmod.dev->user_reg_mapping inside panthor_kmod_get_flush_id()! */
      if (addr == g_ctx.user_reg_page || addr == g_ctx.tracking_page ||
          addr == g_ctx.event_bo_cpu || addr == g_ctx.diag_bo_cpu ||
          addr == g_ctx.shadow_cs_bo_cpu) {
         g_ctx.munmap_guard_hits++;
         LOG_INFO("ADAPTER_MUNMAP_GUARD pinned context mapping addr=%p len=%zu (user_reg=%p)",
                  addr, length, g_ctx.user_reg_page);
         trace_log("ADAPTER_MUNMAP_GUARD pinned context mapping addr=%p len=%zu\n", addr, length);
         pthread_mutex_unlock(&g_lock);
         return 0;
      }
      for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
         if (g_ctx.groups[g].in_use) {
            for (uint32_t q = 0; q < g_ctx.groups[g].queue_count && q < ADAPTER_MAX_SUBQUEUES; q++) {
               struct adapter_subqueue *sq = &g_ctx.groups[g].subqueues[q];
               if (sq->bound && (sq->ring_cpu_ptr == addr || sq->user_io_ptr == addr)) {
                  g_ctx.munmap_guard_hits++;
                  LOG_INFO("ADAPTER_MUNMAP_GUARD pinned group=%u q=%u mapping addr=%p len=%zu",
                           g_ctx.groups[g].group_handle, q, addr, length);
                  pthread_mutex_unlock(&g_lock);
                  return 0;
               }
            }
         }
      }
   }
   size_t bo_lim = g_ctx.max_bo_slot_used ? g_ctx.max_bo_slot_used : ADAPTER_MAX_BOS;
   if (bo_lim > ADAPTER_MAX_BOS)
      bo_lim = ADAPTER_MAX_BOS;
   for (size_t i = 0; i < bo_lim; i++) {
      if (g_ctx.bos[i].in_use &&
          (g_ctx.bos[i].cpu_ptr == addr || g_ctx.bos[i].gpu_mapping == addr || g_ctx.bos[i].alias_ptr == addr)) {
         g_ctx.munmap_guard_hits++;
         LOG_INFO("ADAPTER_MUNMAP_GUARD pinned BO=%u addr=%p len=%zu",
                  g_ctx.bos[i].gem_handle, addr, length);
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
 * Synthetic DRM Device Enumeration & libdrm Interception Entrypoints
 * ============================================================================ */

static drmDevicePtr alloc_synthetic_panthor_drm_device()
{
   const char *node_path = "/dev/dri/renderD128";
   const char *env_path = getenv("PANVK_KBASE_NODE_PATH");
   bool has_emu = (dlsym(RTLD_DEFAULT, "mali_kbase_emu_get_stats") != nullptr);
   if (!has_emu) {
      if (env_path && env_path[0] && access(env_path, R_OK | W_OK) == 0)
         node_path = env_path;
      else if (access("/dev/mali0", R_OK | W_OK) == 0)
         node_path = "/dev/mali0";
   }

   drmDevicePtr dev = static_cast<drmDevicePtr>(calloc(1, sizeof(drmDevice)));
   if (!dev)
      return nullptr;

   dev->nodes = static_cast<char **>(calloc(DRM_NODE_MAX, sizeof(char *)));
   dev->nodes[DRM_NODE_PRIMARY] = strdup(node_path);
   dev->nodes[DRM_NODE_CONTROL] = strdup(node_path);
   dev->nodes[DRM_NODE_RENDER]  = strdup(node_path);
   dev->available_nodes = (1 << DRM_NODE_RENDER) | (1 << DRM_NODE_PRIMARY);
   dev->bustype = DRM_BUS_PLATFORM;

   dev->businfo.platform = static_cast<drmPlatformBusInfoPtr>(calloc(1, sizeof(drmPlatformBusInfo)));
   strncpy(dev->businfo.platform->fullname, "/soc/mali", DRM_PLATFORM_DEVICE_NAME_LEN - 1);

   dev->deviceinfo.platform = static_cast<drmPlatformDeviceInfoPtr>(calloc(1, sizeof(drmPlatformDeviceInfo)));
   dev->deviceinfo.platform->compatible = static_cast<char **>(calloc(2, sizeof(char *)));
   dev->deviceinfo.platform->compatible[0] = strdup("arm,mali-valhall-csf");
   dev->deviceinfo.platform->compatible[1] = nullptr;

   return dev;
}

static void free_synthetic_panthor_drm_device(drmDevicePtr dev)
{
   if (!dev)
      return;
   if (dev->nodes) {
      for (int i = 0; i < DRM_NODE_MAX; i++)
         free(dev->nodes[i]);
      free(dev->nodes);
   }
   if (dev->businfo.platform)
      free(dev->businfo.platform);
   if (dev->deviceinfo.platform) {
      if (dev->deviceinfo.platform->compatible) {
         for (int i = 0; dev->deviceinfo.platform->compatible[i]; i++)
            free(dev->deviceinfo.platform->compatible[i]);
         free(dev->deviceinfo.platform->compatible);
      }
      free(dev->deviceinfo.platform);
   }
   free(dev);
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetDevices2", "flags=0x%x, devices=%p, max=%d",
                           flags, static_cast<void *>(devices), max_devices);
   if (!devices)
      return 1;
   if (max_devices <= 0)
      return 0;
   drmDevicePtr dev = alloc_synthetic_panthor_drm_device();
   if (!dev)
      return -ENOMEM;
   devices[0] = dev;
   return 1;
}

int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetDevices", "devices=%p, max=%d",
                           static_cast<void *>(devices), max_devices);
   return drmGetDevices2(0, devices, max_devices);
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
   PANTHOR2KBASE_LOG_SCOPE("drmFreeDevices", "devices=%p, count=%d",
                           static_cast<void *>(devices), count);
   if (!devices)
      return;
   for (int i = 0; i < count; i++)
      free_synthetic_panthor_drm_device(devices[i]);
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetDevice2", "fd=%d, flags=0x%x", fd, flags);
   (void)fd;
   (void)flags;
   if (!device)
      return -EINVAL;
   *device = alloc_synthetic_panthor_drm_device();
   return *device ? 0 : -ENOMEM;
}

int drmGetDeviceFromDevId(dev_t find_rdev, uint32_t flags, drmDevicePtr *device)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetDeviceFromDevId", "rdev=0x%llx, flags=0x%x",
                           static_cast<unsigned long long>(find_rdev), flags);
   return drmGetDevice2(-1, flags, device);
}

void drmFreeDevice(drmDevicePtr *device)
{
   PANTHOR2KBASE_LOG_SCOPE("drmFreeDevice", "device=%p", static_cast<void *>(device));
   if (device && *device) {
      free_synthetic_panthor_drm_device(*device);
      *device = nullptr;
   }
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
   return (a != nullptr && b != nullptr) ? 1 : 0;
}

int drmGetNodeTypeFromFd(int fd)
{
   (void)fd;
   return DRM_NODE_RENDER;
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
   (void)fd;
   return strdup("/dev/dri/renderD128");
}

char *drmGetDeviceNameFromFd2(int fd)
{
   (void)fd;
   return strdup("/dev/dri/renderD128");
}

drmVersionPtr drmGetVersion(int fd)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetVersion", "fd=%d", fd);
   pthread_mutex_lock(&g_lock);
   if (!g_ctx.active && fd >= 0) {
      LOG_INFO("drmGetVersion: lazily initializing kbase context on fd=%d", fd);
      int priv_mali_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      if (priv_mali_fd < 0)
         priv_mali_fd = fd;
      memset(&g_ctx, 0, sizeof(g_ctx));
      for (size_t i = 0; i < ADAPTER_MAX_VIRTUAL_FDS; i++)
         g_ctx.virtual_fds[i] = -1;
      g_ctx.drm_fd = fd;
      g_ctx.mali_fd = priv_mali_fd;
      struct stat st;
      if (fstat(priv_mali_fd, &st) == 0) {
         g_ctx.mali_dev = st.st_dev;
         g_ctx.mali_ino = st.st_ino;
         g_ctx.mali_rdev = st.st_rdev;
      }
      if (init_kbase_context(&g_ctx, priv_mali_fd) == 0) {
         register_virtual_fd_locked(fd);
         if (priv_mali_fd != fd)
            register_virtual_fd_locked(priv_mali_fd);
      } else if (priv_mali_fd != fd) {
         real_close(priv_mali_fd);
      }
   }
   pthread_mutex_unlock(&g_lock);

   if (panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      drmVersionPtr v = static_cast<drmVersionPtr>(calloc(1, sizeof(*v)));
      v->version_major = 1;
      v->version_minor = 3;
      v->version_patchlevel = 0;
      v->name = strdup("panthor");
      v->name_len = static_cast<int>(strlen(v->name));
      v->date = strdup("20230801");
      v->date_len = static_cast<int>(strlen(v->date));
      v->desc = strdup("Panthor-to-Kbase UAPI Adapter");
      v->desc_len = static_cast<int>(strlen(v->desc));
      LOG_DEBUG("drmGetVersion fd=%d -> panthor 1.3.0", fd);
      return v;
   }
   typedef drmVersionPtr (*next_fn_t)(int);
   next_fn_t next_fn = reinterpret_cast<next_fn_t>(dlsym(RTLD_NEXT, "drmGetVersion"));
   return next_fn ? next_fn(fd) : nullptr;
}

void drmFreeVersion(drmVersionPtr ptr)
{
   PANTHOR2KBASE_LOG_SCOPE("drmFreeVersion", "ptr=%p", static_cast<void *>(ptr));
   if (!ptr)
      return;
   if (ptr->name && strcmp(ptr->name, "panthor") == 0) {
      free(ptr->name);
      free(ptr->date);
      free(ptr->desc);
      free(ptr);
      return;
   }
   typedef void (*next_fn_t)(drmVersionPtr);
   next_fn_t next_fn = reinterpret_cast<next_fn_t>(dlsym(RTLD_NEXT, "drmFreeVersion"));
   if (next_fn)
      next_fn(ptr);
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
   PANTHOR2KBASE_LOG_SCOPE("drmIoctl", "fd=%d, req=%s(0x%lx)",
                           fd, panthor2kbase_ioctl_req_name(request), request);
   return panthor_kbase_adapter_ioctl(fd, request, arg);
}

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjCreate", "fd=%d, flags=0x%x", fd, flags);
   struct drm_syncobj_create arg = { .handle = 0, .flags = flags };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   LOG_DEBUG("drmSyncobjCreate fd=%d -> handle=%u ret=%d", fd, handle ? *handle : 0, ret);
   return ret;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjDestroy", "fd=%d, handle=%u", fd, handle);
   struct drm_syncobj_destroy arg = { .handle = handle, .pad = 0 };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjReset", "fd=%d, count=%u", fd, handle_count);
   struct drm_syncobj_array arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .count_handles = handle_count,
      .pad = 0,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &arg);
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjWait", "fd=%d, num=%u, timeout=%lld",
                           fd, num_handles, static_cast<long long>(timeout_nsec));
   struct drm_syncobj_wait arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
      .first_signaled = 0,
      .pad = 0,
      .deadline_nsec = 0,
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
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjTimelineWait", "fd=%d, num=%u, timeout=%lld",
                           fd, num_handles, static_cast<long long>(timeout_nsec));
   struct drm_syncobj_timeline_wait arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .points = reinterpret_cast<uintptr_t>(points),
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
      .first_signaled = 0,
      .pad = 0,
      .deadline_nsec = 0,
   };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg);
   if (ret == 0 && first_signaled)
      *first_signaled = arg.first_signaled;
   return ret;
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjTransfer", "fd=%d, src=%u, dst=%u",
                           fd, src_handle, dst_handle);
   struct drm_syncobj_transfer arg = {
      .src_handle = src_handle,
      .dst_handle = dst_handle,
      .src_point = src_point,
      .dst_point = dst_point,
      .flags = flags,
      .pad = 0,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &arg);
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   PANTHOR2KBASE_LOG_SCOPE("drmPrimeHandleToFD", "fd=%d, handle=%u, flags=0x%x",
                           fd, handle, flags);
   struct drm_prime_handle arg = { .handle = handle, .flags = flags, .fd = -1 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
   if (ret == 0 && prime_fd)
      *prime_fd = arg.fd;
   return ret;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   PANTHOR2KBASE_LOG_SCOPE("drmPrimeFDToHandle", "fd=%d, prime_fd=%d", fd, prime_fd);
   struct drm_prime_handle arg = { .handle = 0, .flags = 0, .fd = prime_fd };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   return ret;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjExportSyncFile", "fd=%d, handle=%u", fd, handle);
   (void)handle;
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return -1;
   pthread_mutex_lock(&g_lock);
   if (g_ctx.has_kcpu_queue) {
      struct base_fence fence = { .basep = { .fd = -1, .stream_fd = g_ctx.stream_fd } };
      struct base_kcpu_command cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL;
      cmd.info.fence.fence = reinterpret_cast<uintptr_t>(&fence);
      struct kbase_ioctl_kcpu_queue_enqueue enq;
      memset(&enq, 0, sizeof(enq));
      enq.addr = reinterpret_cast<uintptr_t>(&cmd);
      enq.nr_commands = 1;
      enq.id = g_ctx.kcpu_queue_id;
      adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enq);
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
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjImportSyncFile", "fd=%d, handle=%u, sync_fd=%d",
                           fd, handle, sync_file_fd);
   (void)handle;
   if (!panthor_kbase_adapter_is_virtual_drm_fd(fd))
      return -1;
   pthread_mutex_lock(&g_lock);
   if (g_ctx.has_kcpu_queue && sync_file_fd >= 0) {
      struct base_fence fence = { .basep = { .fd = sync_file_fd, .stream_fd = g_ctx.stream_fd } };
      struct base_kcpu_command cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.type = BASE_KCPU_COMMAND_TYPE_FENCE_WAIT;
      cmd.info.fence.fence = reinterpret_cast<uintptr_t>(&fence);
      struct kbase_ioctl_kcpu_queue_enqueue enq;
      memset(&enq, 0, sizeof(enq));
      enq.addr = reinterpret_cast<uintptr_t>(&cmd);
      enq.nr_commands = 1;
      enq.id = g_ctx.kcpu_queue_id;
      adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enq);
   }
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   PANTHOR2KBASE_LOG_SCOPE("drmGetCap", "fd=%d, cap=%llu",
                           fd, static_cast<unsigned long long>(capability));
   struct { uint64_t capability; uint64_t value; } arg = { capability, 0 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_GET_CAP, &arg);
   if (ret == 0 && value)
      *value = arg.value;
   return ret;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSetClientCap", "fd=%d, cap=%llu, val=%llu",
                           fd, static_cast<unsigned long long>(capability),
                           static_cast<unsigned long long>(value));
   (void)fd;
   (void)capability;
   (void)value;
   return 0;
}

int drmCloseBufferHandle(int fd, uint32_t handle)
{
   PANTHOR2KBASE_LOG_SCOPE("drmCloseBufferHandle", "fd=%d, handle=%u", fd, handle);
   struct drm_gem_close arg = { .handle = handle, .pad = 0 };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}

int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
   unsigned long req = _IOC(_IOC_WRITE, 'd', DRM_COMMAND_BASE + drmCommandIndex, size);
   PANTHOR2KBASE_LOG_SCOPE("drmCommandWrite", "fd=%d, idx=%lu, req=%s",
                           fd, drmCommandIndex, panthor2kbase_ioctl_req_name(req));
   return panthor_kbase_adapter_ioctl(fd, req, data);
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjSignal", "fd=%d, count=%u", fd, handle_count);
   struct drm_syncobj_array arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .count_handles = handle_count,
      .pad = 0,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &arg);
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             uint64_t *points, uint32_t handle_count)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjTimelineSignal", "fd=%d, count=%u", fd, handle_count);
   struct drm_syncobj_timeline_array arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .points = reinterpret_cast<uintptr_t>(points),
      .count_handles = handle_count,
      .flags = 0,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &arg);
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjQuery", "fd=%d, count=%u", fd, handle_count);
   struct drm_syncobj_timeline_array arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .points = reinterpret_cast<uintptr_t>(points),
      .count_handles = handle_count,
      .flags = 0,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &arg);
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                     uint32_t handle_count, uint32_t flags)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjQuery2", "fd=%d, count=%u, flags=0x%x",
                           fd, handle_count, flags);
   struct drm_syncobj_timeline_array arg = {
      .handles = reinterpret_cast<uintptr_t>(handles),
      .points = reinterpret_cast<uintptr_t>(points),
      .count_handles = handle_count,
      .flags = flags,
   };
   return panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &arg);
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjHandleToFD", "fd=%d, handle=%u", fd, handle);
   struct drm_syncobj_handle arg = { .handle = handle, .flags = 0, .fd = -1, .pad = 0 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &arg);
   if (ret == 0 && obj_fd)
      *obj_fd = arg.fd;
   return ret;
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjFDToHandle", "fd=%d, obj_fd=%d", fd, obj_fd);
   struct drm_syncobj_handle arg = { .handle = 0, .flags = 0, .fd = obj_fd, .pad = 0 };
   int ret = panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &arg);
   if (ret == 0 && handle)
      *handle = arg.handle;
   return ret;
}

} /* extern "C" */
