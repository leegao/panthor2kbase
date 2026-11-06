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
 * Key Invariants Verified Against `mali_kbase.ko` (CSF v1.21, Mali-G615 MC2):
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
 *  6. RAII scoped indentation logging (`PANTHOR2KBASE_LOG_SCOPE`) on every interceptor entrypoint.
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
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "panthor_kbase_adapter.h"
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

   /*
    * Seamless Valhall v11 (Mali-G715 / Mali-G615 / Immortalis-G715) support:
    *  1. In upstream Mesa 26.3.0 `pan_model.c`, G715 (`PAN_PROD_ID(11, 8, 2)`) and
    *     G615 (`PAN_PROD_ID(11, 8, 3)`) require `gpu_variant == 4` (`core_features & 0xff == 4`),
    *     and `PAN_PROD_ID(11, 8, 0)` (Immortalis-G715) or `PAN_PROD_ID(11, 8, 0xa3)` (r1p3 G615)
    *     must map to `(11, 8, 2)` or `(11, 8, 3)` with `core_features = 4`.
    *  2. Upstream `panvk_physical_device.c` checks `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` on `arch == 11`.
    */
   if (ctx->arch_major == 11) {
      setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
      uint32_t prod_major = DRM_PANTHOR_PRODUCT_MAJOR(ctx->gpu_info.gpu_id);
      if (prod_major != 2 && prod_major != 3) {
         /* Map Immortalis-G715 (prod=0) to G715 (prod=2) and G615 variants to G615 (prod=3) */
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

   /* Ensure v11 is enabled without requiring external environment configuration */
   setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);

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
    * On 64-bit clients, kbase_region_tracker_init_jit carves CUSTOM_VA_ZONE out of
    * the top of SAME_VA_ZONE so KBASE_IOCTL_CS_TILER_HEAP_INIT can allocate heap chunks.
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
      /* In mali_kbase.ko (kbase_api_stream_create at 0x2a304), ioctl returns the fd > 0;
       * in user-space emulators that return 0 and write strm.out.fd, accept either. */
      ctx->stream_fd = (strm_ret > 0) ? strm_ret : strm.out.fd;
   }

   LOG_INFO("init_kbase_context: arch=%u kbase_ver=%u.%u cs_regs=%u sb_slots=%u has_kcpu=%d stream_fd=%d",
            ctx->arch_major, ctx->kbase_ver_major, ctx->kbase_ver_minor,
            ctx->csif_info.cs_reg_count, ctx->csif_info.scoreboard_slot_count,
            ctx->has_kcpu_queue, ctx->stream_fd);

   /*
    * Step 10: Allocate CSF_EVENT BO for CQS syncobjs & subqueue seqnos.
    * Verified in mali_kbase.ko:
    *  - BASE_MEM_CSF_EVENT (1<<19) forces BASE_MEM_SAME_VA (1<<13) | BASE_MEM_CACHED_CPU (1<<12) |
    *    BASE_MEM_COHERENT_SYSTEM (1<<10).
    *  - Do NOT set BASE_MEM_COHERENT_LOCAL (1<<11) with BASE_MEM_CSF_EVENT!
    */
   uint64_t ev_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                       BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                       BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT;
   uint64_t ev_out_flags = 0;
   uint64_t ev_cookie = 0;
   if (alloc_kbase_mem(mali_fd, 16, 16, 0, ev_flags, &ev_out_flags, &ev_cookie) == 0) {
      ctx->event_bo_size = 16 * 4096;
      ctx->event_bo_cpu = static_cast<uint8_t *>(
         real_mmap(nullptr, ctx->event_bo_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, mali_fd, static_cast<off_t>(ev_cookie)));
      if (ctx->event_bo_cpu != MAP_FAILED) {
         ctx->event_bo_gpu_va = reinterpret_cast<uintptr_t>(ctx->event_bo_cpu);
         memset(ctx->event_bo_cpu, 0, ctx->event_bo_size);
      } else {
         ctx->event_bo_cpu = nullptr;
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

static struct adapter_bo *find_bo(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
      if (g_ctx.bos[i].in_use && g_ctx.bos[i].gem_handle == handle)
         return &g_ctx.bos[i];
   }
   return nullptr;
}

static struct adapter_syncobj *find_syncobj(uint32_t handle)
{
   for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
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

} /* anonymous namespace */

extern "C" {

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

   for (uint32_t i = 0; i < count; i++) {
      uint64_t insn = stream_cpu[i];
      uint64_t opcode = (insn >> 56) & 0xffULL;

      if (is_initial_submit && i == 0 && opcode == CSF_OP_MOVE48) {
         uint8_t dest_reg = static_cast<uint8_t>((insn >> 48) & 0xffU);
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
            insn |= (1ULL << 24);
            stream_cpu[i] = insn;
            if (stats)
               stats->sync_scope_upgrades++;
         }
      } else if (opcode == CSF_OP_HEAP_OPERATION) {
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
 * kernel ring emission + kbase userspace queue preamble.
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
   PANTHOR2KBASE_LOG_SCOPE("panthor_kbase_emit_ring_slot",
                           "arch=%u, subq=%u, stream_addr=0x%llx, size=%u",
                           arch_major, subqueue_index,
                           static_cast<unsigned long long>(stream_addr), stream_size);
   size_t idx = 0;
   uint32_t scratch_reg = (cs_reg_count >= 64) ? (cs_reg_count - 4) : 60;
   uint32_t subq_ctx_reg = (cs_reg_count >= 96) ? 88 : 56;

   if (arch_major >= 11) {
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (0ULL << 40) | 0U;
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (1ULL << 40) | 1U;
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (2ULL << 40) | 2U;
      ring_slot[idx++] = (CSF_OP_SET_STATE << 56) | (3ULL << 40) | 3U;
   } else {
      ring_slot[idx++] = (CSF_OP_SET_SB_ENTRY << 56) | (0ULL << 40) | (2ULL << 32);
   }

   uint32_t res_mask = (subqueue_index == 0) ? (CSF_RES_IDVS | CSF_RES_TILER) :
                       (subqueue_index == 1) ? CSF_RES_FRAGMENT : CSF_RES_COMPUTE;
   ring_slot[idx++] = (CSF_OP_REQ_RESOURCE << 56) | static_cast<uint64_t>(res_mask);

   if (subq_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(subq_ctx_reg) << 48) |
                         (subq_ctx_va & 0xffffffffffffULL);
   }

   if (subqueue_index != 2 && tiler_heap_ctx_va != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(scratch_reg) << 48) |
                         (tiler_heap_ctx_va & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_HEAP_SET << 56) | (static_cast<uint64_t>(scratch_reg) << 40);
   }

   if (stream_size > 0 && stream_addr != 0) {
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | (static_cast<uint64_t>(scratch_reg + 2) << 48) |
                         static_cast<uint64_t>(latest_flush);
      ring_slot[idx++] = (CSF_OP_FLUSH_CACHE2 << 56) | (1ULL << 48) |
                         (static_cast<uint64_t>(scratch_reg + 2) << 40);
      ring_slot[idx++] = (CSF_OP_WAIT << 56) | (1ULL << 16);
      ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(scratch_reg) << 48) |
                         (stream_addr & 0xffffffffffffULL);
      ring_slot[idx++] = (CSF_OP_MOVE32 << 56) | (static_cast<uint64_t>(scratch_reg + 2) << 48) |
                         static_cast<uint64_t>(stream_size);
      ring_slot[idx++] = (CSF_OP_CALL << 56) | (static_cast<uint64_t>(scratch_reg) << 40) |
                         (static_cast<uint64_t>(scratch_reg + 2) << 32);
   }

   ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(scratch_reg) << 48) |
                      (seqno_gpu_va & 0xffffffffffffULL);
   ring_slot[idx++] = (CSF_OP_WAIT << 56) | (static_cast<uint64_t>(all_sb_mask ? all_sb_mask : 0xffffU) << 16);
   ring_slot[idx++] = (CSF_OP_MOVE48 << 56) | (static_cast<uint64_t>(scratch_reg + 2) << 48) | 1ULL;
   ring_slot[idx++] = (CSF_OP_SYNC_ADD64 << 56) | (1ULL << 52) |
                      (static_cast<uint64_t>(scratch_reg) << 40) |
                      (static_cast<uint64_t>(scratch_reg + 2) << 32) |
                      (CSF_SYNC_SCOPE_SYSTEM << 28) | (1ULL << 24);
   ring_slot[idx++] = (CSF_OP_ERROR_BARRIER << 56);

   while (idx < ADAPTER_RING_SLOT_QWORDS)
      ring_slot[idx++] = 0ULL;

   return idx * sizeof(uint64_t);
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

   /* Keep a dedicated private duplicate of mali_fd inside g_ctx so caller close() cannot invalidate g_ctx.mali_fd */
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

   pthread_mutex_unlock(&g_lock);
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
      /* Never close our internal g_ctx.mali_fd while external FDs remain */
      if (fd == g_ctx.mali_fd && g_ctx.open_refcount > 1) {
         LOG_DEBUG("adapter_close: keeping internal mali_fd=%d alive (refcnt=%d)",
                   fd, g_ctx.open_refcount);
         pthread_mutex_unlock(&g_lock);
         return 0;
      }
      unregister_virtual_fd_locked(fd);
      /* Only tear down the kbase context when no external virtual FDs remain */
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
   /* Check if `fd` was duplicated via unhooked character device dup on Android */
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

   LOG_INFO("ioctl intercepted: req=%s(0x%lx) fd=%d nr=%u type=%c",
            panthor2kbase_ioctl_req_name(request), request, fd, nr,
            type ? static_cast<char>(type) : '?');
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
      if (sh) {
         sh->fd = (g_ctx.stream_fd >= 0) ? dup(g_ctx.stream_fd) : dup(g_ctx.mali_fd);
      }
      LOG_INFO("DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD -> fd=%d", sh ? sh->fd : -1);
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
         void *unmap_ptr = (bo->cpu_ptr && bo->is_same_va) ? bo->cpu_ptr : nullptr;
         void *exec_cpu_ptr = (bo->cpu_ptr && !bo->is_same_va) ? bo->cpu_ptr : nullptr;
         size_t unmap_sz = bo->cpu_size;
         uint64_t free_va = (!bo->is_same_va && bo->gpu_va) ? bo->gpu_va : 0;
         uint32_t h = bo->gem_handle;
         uint64_t va = bo->gpu_va;
         if (bo->dmabuf_fd >= 0)
            real_close(bo->dmabuf_fd);
         memset(bo, 0, sizeof(*bo));
         pthread_mutex_unlock(&g_lock);
         if (alias_ptr && alias_sz > 0)
            real_munmap(alias_ptr, alias_sz);
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
                     BASE_MEM_IMPORT_SHARED | BASE_MEM_COHERENT_SYSTEM;
      imp.in.phandle = reinterpret_cast<uintptr_t>(&fd_copy);
      imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;

      int ret = adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT, &imp);
      if (ret < 0 && errno == ENOTTY) {
         union kbase_ioctl_mem_import_mtk imp_mtk;
         memset(&imp_mtk, 0, sizeof(imp_mtk));
         imp_mtk.in.flags = imp.in.flags;
         imp_mtk.in.phandle = reinterpret_cast<uintptr_t>(&fd_copy);
         imp_mtk.in.type = BASE_MEM_IMPORT_TYPE_UMM;
         ret = adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_MEM_IMPORT_MTK, &imp_mtk);
         if (ret == 0) {
            imp.out.gpu_va = imp_mtk.out.gpu_va;
            imp.out.va_pages = imp_mtk.out.va_pages;
            imp.out.flags = imp_mtk.out.flags;
         }
      }
      if (ret < 0) {
         close(fd_copy);
         pthread_mutex_unlock(&g_lock);
         return -1; // Propagate kernel errno to caller instead of creating a va=0 BO
      }

      /* Establish GPU VA mapping for SAME_VA / NEED_MMAP imported dma-bufs */
      uint64_t bo_sz = imp.out.va_pages ? (imp.out.va_pages * 4096ULL) : sz;
      uint64_t resolved_gpu_va = imp.out.gpu_va;
      void *imported_cpu_ptr = nullptr;
      if (imp.out.flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)) {
         void *gpu_map = real_mmap(nullptr, static_cast<size_t>(bo_sz),
                                   PROT_READ | PROT_WRITE, MAP_SHARED,
                                   g_ctx.mali_fd, static_cast<off_t>(imp.out.gpu_va));
         if (gpu_map != MAP_FAILED) {
            resolved_gpu_va = reinterpret_cast<uintptr_t>(gpu_map);
            imported_cpu_ptr = gpu_map;
         }
      }

      for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
         if (!g_ctx.bos[i].in_use) {
            struct adapter_bo *bo = &g_ctx.bos[i];
            memset(bo, 0, sizeof(*bo));
            bo->in_use = true;
            bo->gem_handle = g_ctx.next_gem_handle++;
            bo->size = bo_sz;
            bo->gpu_va = resolved_gpu_va;
            bo->cpu_ptr = imported_cpu_ptr;
            bo->cpu_size = static_cast<size_t>(bo_sz);
            bo->dmabuf_fd = fd_copy;
            bo->dmabuf_ino = have_stat ? static_cast<uint64_t>(st.st_ino) : 0;
            bo->is_imported = true;
            bo->is_same_va = (imported_cpu_ptr != nullptr);
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
      for (size_t i = 0; i < ADAPTER_MAX_SYNCOBJS; i++) {
         if (!g_ctx.syncobjs[i].in_use) {
            struct adapter_syncobj *so = &g_ctx.syncobjs[i];
            so->in_use = true;
            so->handle = g_ctx.next_syncobj_handle++;
            so->value = (sc->flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1ULL : 0ULL;
            if (g_ctx.event_bo_cpu) {
               size_t off = 1024 + i * 16;
               so->cqs_gpu_va = g_ctx.event_bo_gpu_va + off;
               so->cqs_cpu_ptr = reinterpret_cast<uint64_t *>(g_ctx.event_bo_cpu + off);
               *so->cqs_cpu_ptr = so->value;
            }
            sc->handle = so->handle;
            LOG_INFO("DRM_IOCTL_SYNCOBJ_CREATE -> handle=%u signaled=%d",
                     so->handle, (so->value != 0));
            pthread_mutex_unlock(&g_lock);
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
      LOG_INFO("DRM_IOCTL_SYNCOBJ_DESTROY handle=%u", sd ? sd->handle : 0);
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   case 0xc3: /* DRM_IOCTL_SYNCOBJ_WAIT */
   case 0xca: { /* DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT */
      LOG_DEBUG("DRM_IOCTL_SYNCOBJ_WAIT/TIMELINE_WAIT");
      if (g_ctx.has_kcpu_queue && g_ctx.event_bo_gpu_va) {
         struct base_cqs_wait_operation_info winfo;
         memset(&winfo, 0, sizeof(winfo));
         winfo.addr = g_ctx.event_bo_gpu_va;
         winfo.val = 0;
         winfo.operation = BASEP_CQS_WAIT_OPERATION_GT;
         winfo.data_type = BASEP_CQS_DATA_TYPE_U64;

         struct base_kcpu_command cmd;
         memset(&cmd, 0, sizeof(cmd));
         cmd.type = BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION;
         cmd.info.cqs_wait_operation.objs = reinterpret_cast<uintptr_t>(&winfo);
         cmd.info.cqs_wait_operation.nr_objs = 1;

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
   case 0xc4: /* DRM_IOCTL_SYNCOBJ_RESET */
   case 0xc5: /* DRM_IOCTL_SYNCOBJ_SIGNAL */
   case 0xcb: /* DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL */
   case 0xcc: /* DRM_IOCTL_SYNCOBJ_TRANSFER */
   case 0xcd: /* DRM_IOCTL_SYNCOBJ_QUERY */
      LOG_DEBUG("DRM_IOCTL_SYNCOBJ_* nr=0x%x (no-op)", nr);
      pthread_mutex_unlock(&g_lock);
      return 0;
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
      uint32_t kmod_flags = g_tls_next_bo_kmod_flags;
      g_tls_next_bo_kmod_flags = 0;

      uint64_t aligned_sz = (bc->size + 4095ULL) & ~4095ULL;
      if (aligned_sz == 0)
         aligned_sz = 4096ULL;
      uint64_t pages = aligned_sz >> 12;

      /*
       * Verified against mali_kbase.ko (kbase_mem_alloc at 0x1367c):
       *  - Regular BOs use BASE_MEM_COHERENT_LOCAL (1<<11) | BASE_MEM_SAME_VA (1<<13).
       *  - Executable BOs (PAN_KMOD_BO_FLAG_EXECUTABLE) use BASE_MEM_PROT_GPU_EX (1<<4)
       *    in EXEC_VA_ZONE, which MUST NOT set BASE_MEM_PROT_GPU_WR (W^X) or BASE_MEM_SAME_VA.
       *  - NEVER set BASE_MEM_CSF_EVENT on general BOs.
       */
      bool is_exec = (kmod_flags & ADAPTER_KMOD_BO_FLAG_EXECUTABLE) != 0;
      uint64_t kflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_COHERENT_LOCAL;
      if (is_exec) {
         kflags |= BASE_MEM_PROT_GPU_EX;
      } else {
         kflags |= BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
      }
      if ((bc->flags & DRM_PANTHOR_BO_WB_MMAP) || (kmod_flags & ADAPTER_KMOD_BO_FLAG_WB_MMAP))
         kflags |= BASE_MEM_CACHED_CPU;
      if (kmod_flags & ADAPTER_KMOD_BO_FLAG_GPU_UNCACHED)
         kflags |= BASE_MEM_UNCACHED_GPU;

      uint64_t out_flags = 0;
      uint64_t out_cookie_or_va = 0;
      if (alloc_kbase_mem(g_ctx.mali_fd, pages, pages, 0, kflags, &out_flags, &out_cookie_or_va) < 0) {
         LOG_ERROR("DRM_PANTHOR_BO_CREATE: alloc_kbase_mem(pages=%llu, flags=0x%llx) failed: %s",
                   static_cast<unsigned long long>(pages),
                   static_cast<unsigned long long>(kflags), strerror(errno));
         ret = -1;
         break;
      }

      /* Map BO in kbase immediately so SAME_VA GPU VA == CPU VA (or EXEC_VA CPU mapping) is materialized */
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
      memset(cpu, 0, static_cast<size_t>(aligned_sz));

      struct adapter_bo *bo = nullptr;
      for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
         if (!g_ctx.bos[i].in_use) {
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
      bo->gpu_va = bo->is_same_va ? reinterpret_cast<uintptr_t>(cpu) : out_cookie_or_va;
      bo->cpu_ptr = cpu;
      bo->cpu_size = static_cast<size_t>(aligned_sz);
      bo->dmabuf_fd = -1;

      bc->handle = bo->gem_handle;
      bc->size = aligned_sz;
      LOG_INFO("DRM_PANTHOR_BO_CREATE size=%llu -> handle=%u va=0x%llx cpu=%p flags=0x%llx exec=%d",
               static_cast<unsigned long long>(aligned_sz), bo->gem_handle,
               static_cast<unsigned long long>(bo->gpu_va), cpu,
               static_cast<unsigned long long>(bo->kbase_flags), is_exec);
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
               alias_req.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_CPU_RD;
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
               if (!(ops[i].flags & DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC))
                  bo->kbase_flags |= BASE_MEM_PROT_GPU_EX;
               /* Always report the kbase-assigned GPU VA back to panthor_kmod / panvk */
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
      for (uint32_t i = 0; i < bs->ops.count; i++) {
         struct adapter_bo *bo = find_bo(ops[i].handle);
         if (bo && bo->cpu_ptr) {
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
      union kbase_ioctl_cs_tiler_heap_init_1_13 hi13;
      memset(&hi13, 0, sizeof(hi13));
      hi13.in.chunk_size = hc->chunk_size ? hc->chunk_size : 1048576U;
      hi13.in.initial_chunks = hc->initial_chunk_count ? hc->initial_chunk_count : 5U;
      hi13.in.max_chunks = hc->max_chunks ? hc->max_chunks : 200U;
      hi13.in.target_in_flight = static_cast<uint16_t>(hc->target_in_flight ? hc->target_in_flight : 65535U);
      hi13.in.group_id = 0;

      if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13, &hi13) < 0) {
         union kbase_ioctl_cs_tiler_heap_init hi24;
         memset(&hi24, 0, sizeof(hi24));
         hi24.in.chunk_size = hi13.in.chunk_size;
         hi24.in.initial_chunks = hi13.in.initial_chunks;
         hi24.in.max_chunks = hi13.in.max_chunks;
         hi24.in.target_in_flight = hi13.in.target_in_flight;
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

      bool stored = false;
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
            for (size_t g = 0; g < ADAPTER_MAX_GROUPS; g++) {
               if (g_ctx.groups[g].in_use && g_ctx.groups[g].vm_id == hc->vm_id)
                  g_ctx.groups[g].active_tiler_heap_ctx_va = hi13.out.gpu_heap_va;
            }
            LOG_INFO("DRM_PANTHOR_TILER_HEAP_CREATE -> KBASE_IOCTL_CS_TILER_HEAP_INIT heap_va=0x%llx first_chunk=0x%llx handle=%u",
                     static_cast<unsigned long long>(hc->tiler_heap_ctx_gpu_va),
                     static_cast<unsigned long long>(hc->first_heap_chunk_gpu_va), hc->handle);
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

      LOG_INFO("DRM_PANTHOR_GROUP_CREATE vm_id=%u queues=%u priority=%u",
               gc->vm_id, qcnt, gc->priority <= 3 ? gc->priority : 1);

      union kbase_ioctl_cs_queue_group_create_1_6 csg;
      memset(&csg, 0, sizeof(csg));
      csg.in.tiler_mask = gc->tiler_core_mask ? gc->tiler_core_mask : 1ULL;
      csg.in.fragment_mask = gc->fragment_core_mask ? gc->fragment_core_mask : ~0ULL;
      csg.in.compute_mask = gc->compute_core_mask ? gc->compute_core_mask : ~0ULL;
      csg.in.cs_min = static_cast<uint8_t>(qcnt ? qcnt : 3);
      csg.in.priority = static_cast<uint8_t>(gc->priority <= 3 ? gc->priority : 1);
      csg.in.tiler_max = gc->max_tiler_cores ? gc->max_tiler_cores : 1;
      csg.in.fragment_max = gc->max_fragment_cores ? gc->max_fragment_cores : 64;
      csg.in.compute_max = gc->max_compute_cores ? gc->max_compute_cores : 64;

      if (adapter_ioctl(g_ctx.mali_fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &csg) < 0) {
         LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 failed");
         ret = -1;
         break;
      }

      struct adapter_group *grp = nullptr;
      for (size_t i = 0; i < ADAPTER_MAX_GROUPS; i++) {
         if (!g_ctx.groups[i].in_use) {
            grp = &g_ctx.groups[i];
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
      grp->kbase_group_handle = csg.out.group_handle;
      grp->vm_id = gc->vm_id;
      grp->queue_count = qcnt;
      for (size_t h = 0; h < ADAPTER_MAX_HEAPS; h++) {
         if (g_ctx.heaps[h].in_use)
            grp->active_tiler_heap_ctx_va = g_ctx.heaps[h].gpu_heap_va;
      }

      /* Allocate SAME_VA ring buffer BO, register queue, bind queue, and mmap USER_IO for each subqueue */
      for (uint32_t q = 0; q < qcnt; q++) {
         struct adapter_subqueue *sq = &grp->subqueues[q];
         sq->csi_index = q;
         sq->ringbuf_size = ADAPTER_RINGBUF_SIZE;

         uint64_t rflags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                           BASE_MEM_PROT_GPU_RD | BASE_MEM_SAME_VA | BASE_MEM_COHERENT_LOCAL;
         uint64_t rout_flags = 0;
         uint64_t rcookie = 0;
         if (alloc_kbase_mem(g_ctx.mali_fd, ADAPTER_RINGBUF_SIZE >> 12,
                             ADAPTER_RINGBUF_SIZE >> 12, 0, rflags, &rout_flags, &rcookie) < 0) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: ring alloc failed for queue %u", q);
            ret = -1;
            break;
         }
         void *rcpu = real_mmap(nullptr, ADAPTER_RINGBUF_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, g_ctx.mali_fd, static_cast<off_t>(rcookie));
         if (rcpu == MAP_FAILED) {
            LOG_ERROR("DRM_PANTHOR_GROUP_CREATE: ring mmap failed for queue %u", q);
            ret = -1;
            break;
         }
         memset(rcpu, 0, ADAPTER_RINGBUF_SIZE);
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

         union kbase_ioctl_cs_queue_bind qbind;
         memset(&qbind, 0, sizeof(qbind));
         qbind.in.buffer_gpu_addr = sq->ring_gpu_va;
         qbind.in.group_handle = grp->kbase_group_handle;
         qbind.in.csi_index = static_cast<uint8_t>(q);
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

         if (g_ctx.event_bo_cpu) {
            size_t seq_off = q * 64;
            sq->seqno_gpu_va = g_ctx.event_bo_gpu_va + seq_off;
            sq->seqno_cpu_ptr = reinterpret_cast<volatile uint64_t *>(g_ctx.event_bo_cpu + seq_off);
            *sq->seqno_cpu_ptr = 0;
         }
      }

      gc->group_handle = grp->group_handle;
      LOG_INFO("DRM_PANTHOR_GROUP_CREATE -> KBASE_IOCTL_CS_QUEUE_GROUP_CREATE handle=%u kbase_group=%u queues=%u",
               grp->group_handle, grp->kbase_group_handle, qcnt);
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
                  real_munmap(sq->ring_cpu_ptr, sq->ringbuf_size);
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
      gs->state = 0;
      gs->fatal_queues = 0;
      LOG_DEBUG("DRM_PANTHOR_GROUP_GET_STATE -> state=0 fatal_queues=0");
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

      LOG_INFO("DRM_PANTHOR_GROUP_SUBMIT handle=%u queue_submits=%u",
               grp->group_handle, gs->queue_submits.count);

      for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
         struct drm_panthor_queue_submit *qs = &qsubs[i];
         if (qs->queue_index >= grp->queue_count)
            continue;
         struct adapter_subqueue *sq = &grp->subqueues[qs->queue_index];
         bool is_initial = (sq->emitted_jobs == 0);

         /* 1. Normalize the submitted CSF command stream in-place */
         struct adapter_cs_patch_stats stats;
         memset(&stats, 0, sizeof(stats));
         if (qs->stream_addr && qs->stream_size >= 8) {
            auto *stream_cpu = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(qs->stream_addr));
            panthor_kbase_normalize_cs_stream(stream_cpu, qs->stream_size, is_initial, &stats);
            if (is_initial && stats.extracted_subq_ctx_va) {
               sq->cached_subq_ctx_va = stats.extracted_subq_ctx_va;
               sq->cached_subq_ctx_reg = stats.extracted_subq_ctx_reg;
            }
         }

         /* 2. Emit the CSF ring slot into the kbase userspace ring buffer */
         uint32_t ring_off = static_cast<uint32_t>(sq->insert_offset % sq->ringbuf_size);
         if (ring_off + ADAPTER_RING_SLOT_BYTES > sq->ringbuf_size) {
            memset(reinterpret_cast<uint8_t *>(sq->ring_cpu_ptr) + ring_off, 0,
                   sq->ringbuf_size - ring_off);
            sq->insert_offset += (sq->ringbuf_size - ring_off);
            ring_off = 0;
         }
         auto *slot_ptr = reinterpret_cast<uint64_t *>(
            reinterpret_cast<uint8_t *>(sq->ring_cpu_ptr) + ring_off);
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
            static_cast<uint16_t>(g_ctx.csif_info.scoreboard_slot_count
               ? ((1U << g_ctx.csif_info.scoreboard_slot_count) - 1U)
               : 0x00ffU));

         sq->insert_offset += slot_bytes;
         sq->emitted_jobs++;

         /* 3. Publish CS_INSERT in USER_IO Input Page (+0x1000), ring Doorbell (+0x0000), and kick */
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

         /* 4. Update signaled drm_syncobjs */
         auto *sops = reinterpret_cast<struct drm_panthor_sync_op *>(
            static_cast<uintptr_t>(qs->syncs.array));
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

         LOG_INFO("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u -> KBASE_IOCTL_CS_QUEUE_KICK insert=%llu jobs=%llu",
                  qs->queue_index, static_cast<unsigned long long>(qs->stream_addr),
                  qs->stream_size, stats.sync_scope_upgrades,
                  stats.heap_op_nops, static_cast<unsigned long long>(sq->insert_offset),
                  static_cast<unsigned long long>(sq->emitted_jobs));
         trace_log("DRM_PANTHOR_GROUP_SUBMIT q=%u stream=0x%llx sz=%u sync_upgrades=%u heap_nops=%u -> KBASE_IOCTL_CS_QUEUE_KICK insert=%llu\n",
                   qs->queue_index, static_cast<unsigned long long>(qs->stream_addr),
                   qs->stream_size, stats.sync_scope_upgrades,
                   stats.heap_op_nops, static_cast<unsigned long long>(sq->insert_offset));
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
      if (!res)
         res = real_mmap(addr, length, PROT_READ, MAP_SHARED,
                         g_ctx.mali_fd, static_cast<off_t>(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE));
      LOG_INFO("adapter_mmap: USER_REG_PAGE fd=%d offset=0x%llx -> %p",
               fd, static_cast<unsigned long long>(offset), res);
      pthread_mutex_unlock(&g_lock);
      return res ? res : MAP_FAILED;
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
   pthread_mutex_lock(&g_lock);
   /*
    * Guard against the kbase SAME_VA munmap trap (mali_kbase_mem_linux.c:2805):
    * Calling munmap() on a SAME_VA BO tears down the GPU MMU page tables in kbase!
    * Keep the SAME_VA mapping pinned until DRM_IOCTL_GEM_CLOSE.
    */
   for (size_t i = 0; i < ADAPTER_MAX_BOS; i++) {
      if (g_ctx.bos[i].in_use &&
          (g_ctx.bos[i].cpu_ptr == addr || g_ctx.bos[i].alias_ptr == addr)) {
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
   /*
    * If create_kmod_dev() in panvk_physical_device.c opened `/dev/mali0` directly via
    * libc `open()` before calling `drmGetVersion(fd)`, lazily initialize `g_ctx` on `fd`!
    */
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
   PANTHOR2KBASE_LOG_SCOPE("drmSyncobjQuery2", "fd=%d, count=%u, flags=0x%x",
                           fd, handle_count, flags);
   (void)flags;
   return drmSyncobjQuery(fd, handles, points, handle_count);
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
