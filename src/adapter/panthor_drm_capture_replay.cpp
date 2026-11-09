/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor DRM / CSF / Valhall ISA Capture & Local/On-Device Replay Engine
 * (src/adapter/panthor_drm_capture_replay.cpp)
 *
 * Implements:
 *   1. Binary trace recording (.pandrmcap) inside libpanthor_kbase_adapter
 *      for all DRM_IOCTL_PANTHOR_*, DRM_IOCTL_SYNCOBJ_*, GEM_CLOSE, BO memory
 *      snapshots, transitive CSF chunk closures (CALL + JUMP), Valhall ISA
 *      shader blobs, and stubbed/full texture metadata.
 *   2. Deterministic local & on-device replay (panthor_drm_replay_trace_file)
 *      with handle remapping, SAME_VA GPU pointer relocation across CS streams
 *      and subqueue contexts, and CSF invariant verification.
 */
#include "panthor_drm_capture_replay.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>

namespace {

static pthread_mutex_t g_cap_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_cap_fp = nullptr;
static bool g_cap_active = false;
static bool g_cap_env_checked = false;
static uint32_t g_cap_flags = PANDRM_FLAG_CAPTURE_ISA | PANDRM_FLAG_POST_NORM_CSF;
static uint64_t g_cap_seq_id = 0;

struct CapBoInfo {
   uint32_t gem_handle;
   uint32_t is_executable;
   uint64_t gpu_va;
   uint64_t size;
   uint64_t last_snapshot_fnv1a64;
};

static std::unordered_map<uint32_t, CapBoInfo> g_cap_bos;

static uint64_t cap_monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(ts.tv_nsec);
}

static void write_record_locked(enum pandrm_record_type rtype,
                                uint32_t fd_tag,
                                int32_t ioctl_ret,
                                const void *payload,
                                uint32_t payload_bytes)
{
   if (!g_cap_fp || !g_cap_active)
      return;

   struct pandrm_record_header hdr;
   memset(&hdr, 0, sizeof(hdr));
   hdr.record_type = static_cast<uint32_t>(rtype);
   hdr.payload_bytes = payload_bytes;
   hdr.seq_id = ++g_cap_seq_id;
   hdr.fnv1a64_checksum = (payload && payload_bytes > 0)
                             ? panthor_fnv1a64_bytes(payload, payload_bytes)
                             : UINT64_C(0xcbf29ce484222325);
   hdr.fd_tag = fd_tag;
   hdr.ioctl_ret = ioctl_ret;

   fwrite(&hdr, sizeof(hdr), 1, g_cap_fp);
   if (payload && payload_bytes > 0)
      fwrite(payload, 1, payload_bytes, g_cap_fp);
   fflush(g_cap_fp);
}

} /* anonymous namespace */

extern "C" {

bool panthor_drm_capture_is_active(void)
{
   return __atomic_load_n(&g_cap_active, __ATOMIC_ACQUIRE);
}

int panthor_drm_capture_start(const char *filepath, uint32_t flags)
{
   if (!filepath || !filepath[0])
      return -1;

   pthread_mutex_lock(&g_cap_mutex);
   if (g_cap_fp) {
      fclose(g_cap_fp);
      g_cap_fp = nullptr;
   }
   g_cap_fp = fopen(filepath, "wb");
   if (!g_cap_fp) {
      pthread_mutex_unlock(&g_cap_mutex);
      return -1;
   }

   g_cap_flags = flags ? flags : (PANDRM_FLAG_CAPTURE_ISA | PANDRM_FLAG_POST_NORM_CSF);
   const char *stub_env = getenv("PANTHOR_DRM_CAPTURE_STUB_TEXTURES");
   if (stub_env && stub_env[0] && atoi(stub_env) != 0)
      g_cap_flags |= PANDRM_FLAG_STUB_TEXTURES;

   g_cap_seq_id = 0;
   g_cap_bos.clear();

   struct pandrm_trace_file_header fhdr;
   memset(&fhdr, 0, sizeof(fhdr));
   memcpy(fhdr.magic, PANDRM_TRACE_MAGIC, 8);
   fhdr.version = PANDRM_TRACE_VERSION;
   fhdr.flags = g_cap_flags;
   fhdr.gpu_id = 0xa8670000U;
   fhdr.arch_major = 11U;
   fhdr.cs_reg_count = 96U;
   fhdr.scoreboard_slot_count = 8U;
   fhdr.capture_timestamp_ns = cap_monotonic_ns();

   fwrite(&fhdr, sizeof(fhdr), 1, g_cap_fp);
   fflush(g_cap_fp);
   __atomic_store_n(&g_cap_active, true, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&g_cap_mutex);
   LOG_INFO("[DRM_CAPTURE] Started capturing Panthor DRM/CSF trace to '%s' (flags=0x%x)",
            filepath, g_cap_flags);
   return 0;
}

void panthor_drm_capture_stop(void)
{
   pthread_mutex_lock(&g_cap_mutex);
   __atomic_store_n(&g_cap_active, false, __ATOMIC_RELEASE);
   if (g_cap_fp) {
      fflush(g_cap_fp);
      fclose(g_cap_fp);
      g_cap_fp = nullptr;
   }
   g_cap_bos.clear();
   pthread_mutex_unlock(&g_cap_mutex);
}

void panthor_drm_capture_init_from_env(uint32_t gpu_id, uint32_t arch_major,
                                       uint32_t cs_reg_count, uint32_t sb_slot_count)
{
   pthread_mutex_lock(&g_cap_mutex);
   if (g_cap_env_checked) {
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }
   g_cap_env_checked = true;
   const char *path = getenv("PANTHOR_DRM_CAPTURE_PATH");
   pthread_mutex_unlock(&g_cap_mutex);

   if (path && path[0]) {
      uint32_t flags = PANDRM_FLAG_CAPTURE_ISA | PANDRM_FLAG_POST_NORM_CSF;
      const char *stub_env = getenv("PANTHOR_DRM_CAPTURE_STUB_TEXTURES");
      if (stub_env && stub_env[0] && atoi(stub_env) != 0)
         flags |= PANDRM_FLAG_STUB_TEXTURES;
      if (panthor_drm_capture_start(path, flags) == 0) {
         pthread_mutex_lock(&g_cap_mutex);
         if (g_cap_fp) {
            struct pandrm_trace_file_header fhdr;
            memset(&fhdr, 0, sizeof(fhdr));
            memcpy(fhdr.magic, PANDRM_TRACE_MAGIC, 8);
            fhdr.version = PANDRM_TRACE_VERSION;
            fhdr.flags = g_cap_flags;
            fhdr.gpu_id = gpu_id ? gpu_id : 0xa8670000U;
            fhdr.arch_major = arch_major ? arch_major : 11U;
            fhdr.cs_reg_count = cs_reg_count ? cs_reg_count : 96U;
            fhdr.scoreboard_slot_count = sb_slot_count ? sb_slot_count : 8U;
            fhdr.capture_timestamp_ns = cap_monotonic_ns();
            fseek(g_cap_fp, 0, SEEK_SET);
            fwrite(&fhdr, sizeof(fhdr), 1, g_cap_fp);
            fseek(g_cap_fp, 0, SEEK_END);
            fflush(g_cap_fp);
         }
         pthread_mutex_unlock(&g_cap_mutex);
      }
   }
}

void panthor_drm_capture_record_csf_chunk(uint32_t group_handle,
                                          uint32_t queue_index,
                                          uint32_t chunk_index,
                                          uint16_t call_depth,
                                          uint16_t is_jump_target,
                                          uint64_t gpu_va,
                                          uint32_t size_bytes,
                                          uint64_t pre_norm_hash,
                                          uint64_t post_norm_hash,
                                          const uint64_t *qwords)
{
   if (!panthor_drm_capture_is_active() || !qwords || size_bytes == 0)
      return;

   pthread_mutex_lock(&g_cap_mutex);
   struct pandrm_rec_csf_stream_closure rec;
   memset(&rec, 0, sizeof(rec));
   rec.group_handle = group_handle;
   rec.queue_index = queue_index;
   rec.chunk_index = chunk_index;
   rec.call_depth = call_depth;
   rec.is_jump_target = is_jump_target;
   rec.gpu_va = gpu_va;
   rec.size_bytes = size_bytes;
   rec.pre_norm_fnv1a64 = pre_norm_hash;
   rec.post_norm_fnv1a64 = post_norm_hash;

   std::vector<uint8_t> buf(sizeof(rec) + size_bytes);
   memcpy(buf.data(), &rec, sizeof(rec));
   memcpy(buf.data() + sizeof(rec), qwords, size_bytes);
   write_record_locked(PANDRM_REC_CSF_STREAM_CLOSURE, 0, 0,
                       buf.data(), static_cast<uint32_t>(buf.size()));
   pthread_mutex_unlock(&g_cap_mutex);
}

void panthor_drm_capture_record_pre_submit(uint32_t group_handle,
                                           const struct drm_panthor_queue_submit *qsubs,
                                           uint32_t qsub_count)
{
   (void)group_handle;
   (void)qsubs;
   (void)qsub_count;
   if (!panthor_drm_capture_is_active())
      return;

   pthread_mutex_lock(&g_cap_mutex);
   /* Snapshot changed BOs (up to 256 KiB per BO, or stubbed metadata for larger texture BOs) */
   for (auto &kv : g_cap_bos) {
      CapBoInfo &bi = kv.second;
      if (bi.gpu_va < 0x10000ULL || bi.size == 0)
         continue;
      const uint8_t *cpu_ptr = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(bi.gpu_va));
      /* If BO is > 256 KiB and STUB_TEXTURES is enabled, emit a compact PANDRM_REC_TEX_META
       * + snapshot only the first 16 KiB descriptor/header region */
      if (bi.size > (256U * 1024U) && (g_cap_flags & PANDRM_FLAG_STUB_TEXTURES) != 0 && !bi.is_executable) {
         uint64_t hdr_hash = panthor_fnv1a64_bytes(cpu_ptr, 16384U);
         if (hdr_hash != bi.last_snapshot_fnv1a64) {
            bi.last_snapshot_fnv1a64 = hdr_hash;
            struct pandrm_rec_tex_meta tmeta;
            memset(&tmeta, 0, sizeof(tmeta));
            tmeta.gem_handle = bi.gem_handle;
            tmeta.width = 256U;
            tmeta.height = 256U;
            tmeta.depth = 1U;
            tmeta.format = 37U; /* VK_FORMAT_R8G8B8A8_UNORM */
            tmeta.stride = 1024U;
            tmeta.is_stubbed = 1U;
            tmeta.payload_bytes = 0U;
            tmeta.tex_gpu_va = bi.gpu_va;
            tmeta.tex_fnv1a64 = hdr_hash;
            write_record_locked(PANDRM_REC_TEX_META, 0, 0, &tmeta, sizeof(tmeta));

            /* Also snapshot the first 16 KiB in case descriptors/ring headers live at the start of the slab */
            uint32_t snap_sz = 16384U;
            struct pandrm_rec_bo_data_snapshot snap;
            memset(&snap, 0, sizeof(snap));
            snap.gem_handle = bi.gem_handle;
            snap.is_executable = bi.is_executable;
            snap.gpu_va = bi.gpu_va;
            snap.offset = 0;
            snap.byte_size = snap_sz;
            snap.content_fnv1a64 = hdr_hash;
            std::vector<uint8_t> buf(sizeof(snap) + snap_sz);
            memcpy(buf.data(), &snap, sizeof(snap));
            memcpy(buf.data() + sizeof(snap), cpu_ptr, snap_sz);
            write_record_locked(PANDRM_REC_BO_DATA_SNAPSHOT, 0, 0,
                                buf.data(), static_cast<uint32_t>(buf.size()));
         }
         continue;
      }

      uint32_t snap_sz = (bi.size <= 262144ULL) ? static_cast<uint32_t>(bi.size) : 65536U;
      uint64_t cur_hash = panthor_fnv1a64_bytes(cpu_ptr, snap_sz);
      if (cur_hash == bi.last_snapshot_fnv1a64)
         continue;
      bi.last_snapshot_fnv1a64 = cur_hash;

      if (bi.is_executable && (g_cap_flags & PANDRM_FLAG_CAPTURE_ISA) != 0) {
         struct pandrm_rec_isa_shader_blob isa;
         memset(&isa, 0, sizeof(isa));
         isa.gem_handle = bi.gem_handle;
         isa.shader_size = snap_sz;
         isa.shader_gpu_va = bi.gpu_va;
         isa.isa_fnv1a64 = cur_hash;
         std::vector<uint8_t> buf(sizeof(isa) + snap_sz);
         memcpy(buf.data(), &isa, sizeof(isa));
         memcpy(buf.data() + sizeof(isa), cpu_ptr, snap_sz);
         write_record_locked(PANDRM_REC_ISA_SHADER_BLOB, 0, 0,
                             buf.data(), static_cast<uint32_t>(buf.size()));
      }

      struct pandrm_rec_bo_data_snapshot snap;
      memset(&snap, 0, sizeof(snap));
      snap.gem_handle = bi.gem_handle;
      snap.is_executable = bi.is_executable;
      snap.gpu_va = bi.gpu_va;
      snap.offset = 0;
      snap.byte_size = snap_sz;
      snap.content_fnv1a64 = cur_hash;
      std::vector<uint8_t> buf(sizeof(snap) + snap_sz);
      memcpy(buf.data(), &snap, sizeof(snap));
      memcpy(buf.data() + sizeof(snap), cpu_ptr, snap_sz);
      write_record_locked(PANDRM_REC_BO_DATA_SNAPSHOT, 0, 0,
                          buf.data(), static_cast<uint32_t>(buf.size()));
   }
   pthread_mutex_unlock(&g_cap_mutex);
}

void panthor_drm_capture_record_ioctl(int fd, unsigned long request, void *arg, int ret)
{
   if (!panthor_drm_capture_is_active() || !arg)
      return;

   unsigned int nr = _IOC_NR(request);
   unsigned int type = _IOC_TYPE(request);
   if (type != 'd')
      return;

   pthread_mutex_lock(&g_cap_mutex);
   uint32_t fd_tag = static_cast<uint32_t>(fd);

   /* Standard DRM GEM / SYNCOBJ ioctls */
   if (nr == 0x09) { /* DRM_IOCTL_GEM_CLOSE */
      auto *cl = static_cast<struct drm_gem_close *>(arg);
      struct pandrm_rec_gem_close rec = { .gem_handle = cl->handle, .pad = 0 };
      g_cap_bos.erase(cl->handle);
      write_record_locked(PANDRM_REC_GEM_CLOSE, fd_tag, ret, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }
   if (nr == 0xbf) { /* DRM_IOCTL_SYNCOBJ_CREATE */
      auto *sc = static_cast<struct drm_syncobj_create *>(arg);
      struct pandrm_rec_syncobj_create rec = { .handle = sc->handle, .flags = sc->flags };
      write_record_locked(PANDRM_REC_SYNCOBJ_CREATE, fd_tag, ret, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }
   if (nr == 0xc0) { /* DRM_IOCTL_SYNCOBJ_DESTROY */
      auto *sd = static_cast<struct drm_syncobj_destroy *>(arg);
      struct pandrm_rec_syncobj_destroy rec = { .handle = sd->handle, .pad = 0 };
      write_record_locked(PANDRM_REC_SYNCOBJ_DESTROY, fd_tag, ret, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }
   if (nr == 0xc3 || nr == 0xca || nr == 0xc4 || nr == 0xc5 || nr == 0xcb || nr == 0xcc || nr == 0xcd) {
      struct pandrm_rec_syncobj_op rec;
      memset(&rec, 0, sizeof(rec));
      if (nr == 0xc3) {
         auto *sw = static_cast<struct drm_syncobj_wait *>(arg);
         rec.op_kind = PANDRM_SYNCOBJ_WAIT;
         rec.count_handles = (sw->count_handles <= 16U) ? sw->count_handles : 16U;
         rec.flags = sw->flags;
         const auto *h = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(sw->handles));
         if (h) {
            for (uint32_t i = 0; i < rec.count_handles; i++)
               rec.handles[i] = h[i];
         }
      } else if (nr == 0xca) {
         auto *tw = static_cast<struct drm_syncobj_timeline_wait *>(arg);
         rec.op_kind = PANDRM_SYNCOBJ_TIMELINE_WAIT;
         rec.count_handles = (tw->count_handles <= 16U) ? tw->count_handles : 16U;
         rec.flags = tw->flags;
         const auto *h = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(tw->handles));
         const auto *p = reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(tw->points));
         for (uint32_t i = 0; i < rec.count_handles; i++) {
            if (h) rec.handles[i] = h[i];
            if (p) rec.points[i] = p[i];
         }
      } else if (nr == 0xc4 || nr == 0xc5) {
         auto *sa = static_cast<struct drm_syncobj_array *>(arg);
         rec.op_kind = (nr == 0xc4) ? PANDRM_SYNCOBJ_RESET : PANDRM_SYNCOBJ_SIGNAL;
         rec.count_handles = (sa->count_handles <= 16U) ? sa->count_handles : 16U;
         const auto *h = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(sa->handles));
         if (h) {
            for (uint32_t i = 0; i < rec.count_handles; i++)
               rec.handles[i] = h[i];
         }
      } else if (nr == 0xcb || nr == 0xcd) {
         auto *ta = static_cast<struct drm_syncobj_timeline_array *>(arg);
         rec.op_kind = (nr == 0xcb) ? PANDRM_SYNCOBJ_QUERY : PANDRM_SYNCOBJ_TIMELINE_SIGNAL;
         rec.count_handles = (ta->count_handles <= 16U) ? ta->count_handles : 16U;
         rec.flags = ta->flags;
         const auto *h = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(ta->handles));
         const auto *p = reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(ta->points));
         for (uint32_t i = 0; i < rec.count_handles; i++) {
            if (h) rec.handles[i] = h[i];
            if (p) rec.points[i] = p[i];
         }
      } else if (nr == 0xcc) {
         auto *tr = static_cast<struct drm_syncobj_transfer *>(arg);
         rec.op_kind = PANDRM_SYNCOBJ_TRANSFER;
         rec.count_handles = 1U;
         rec.handles[0] = tr->src_handle;
         rec.points[0] = tr->src_point;
         rec.dst_handle = tr->dst_handle;
         rec.dst_point = tr->dst_point;
         rec.flags = tr->flags;
      }
      write_record_locked(PANDRM_REC_SYNCOBJ_OP, fd_tag, ret, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }

   if (nr < DRM_COMMAND_BASE || nr >= DRM_COMMAND_BASE + 0x10) {
      pthread_mutex_unlock(&g_cap_mutex);
      return;
   }

   unsigned int panthor_id = nr - DRM_COMMAND_BASE;
   switch (panthor_id) {
   case DRM_PANTHOR_DEV_QUERY: {
      auto *q = static_cast<struct drm_panthor_dev_query *>(arg);
      uint32_t qsz = (q->pointer && q->size <= 1024U) ? q->size : 0U;
      struct pandrm_rec_dev_query hdr = { .query_type = q->type, .query_size = qsz };
      std::vector<uint8_t> buf(sizeof(hdr) + qsz);
      memcpy(buf.data(), &hdr, sizeof(hdr));
      if (qsz > 0)
         memcpy(buf.data() + sizeof(hdr), reinterpret_cast<const void *>(static_cast<uintptr_t>(q->pointer)), qsz);
      write_record_locked(PANDRM_REC_DEV_QUERY, fd_tag, ret,
                          buf.data(), static_cast<uint32_t>(buf.size()));
      break;
   }
   case DRM_PANTHOR_VM_CREATE: {
      auto *vc = static_cast<struct drm_panthor_vm_create *>(arg);
      struct pandrm_rec_vm_create rec = {
         .vm_id = vc->id,
         .flags = vc->flags,
         .user_va_range = vc->user_va_range
      };
      write_record_locked(PANDRM_REC_VM_CREATE, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_VM_DESTROY: {
      auto *vd = static_cast<struct drm_panthor_vm_destroy *>(arg);
      struct pandrm_rec_vm_destroy rec = { .vm_id = vd->id, .pad = 0 };
      write_record_locked(PANDRM_REC_VM_DESTROY, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_BO_CREATE: {
      auto *bc = static_cast<struct drm_panthor_bo_create *>(arg);
      uint64_t gpu_va = panthor_kbase_adapter_get_bo_gpu_va(bc->handle);
      struct pandrm_rec_bo_create rec;
      memset(&rec, 0, sizeof(rec));
      rec.gem_handle = bc->handle;
      rec.panthor_flags = bc->flags;
      rec.size = bc->size;
      rec.gpu_va = gpu_va;
      g_cap_bos[bc->handle] = CapBoInfo{
         .gem_handle = bc->handle,
         .is_executable = 0U,
         .gpu_va = gpu_va,
         .size = bc->size,
         .last_snapshot_fnv1a64 = 0ULL
      };
      write_record_locked(PANDRM_REC_BO_CREATE, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_VM_BIND: {
      auto *vb = static_cast<struct drm_panthor_vm_bind *>(arg);
      uint32_t cnt = (vb->ops.count <= 64U) ? vb->ops.count : 64U;
      const auto *ops = reinterpret_cast<const struct drm_panthor_vm_bind_op *>(
         static_cast<uintptr_t>(vb->ops.array));
      struct pandrm_rec_vm_bind_header hdr = {
         .vm_id = vb->vm_id,
         .flags = vb->flags,
         .op_count = ops ? cnt : 0U,
         .pad = 0U
      };
      std::vector<uint8_t> buf(sizeof(hdr) + hdr.op_count * sizeof(struct drm_panthor_vm_bind_op));
      memcpy(buf.data(), &hdr, sizeof(hdr));
      if (ops && hdr.op_count > 0) {
         memcpy(buf.data() + sizeof(hdr), ops, hdr.op_count * sizeof(struct drm_panthor_vm_bind_op));
         for (uint32_t i = 0; i < hdr.op_count; i++) {
            auto it = g_cap_bos.find(ops[i].bo_handle);
            if (it != g_cap_bos.end()) {
               if (ops[i].va != 0)
                  it->second.gpu_va = ops[i].va - ops[i].bo_offset;
               if ((ops[i].flags & (DRM_PANTHOR_VM_BIND_OP_MAP_READONLY | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC)) ==
                   DRM_PANTHOR_VM_BIND_OP_MAP_READONLY) {
                  it->second.is_executable = 1U;
               }
            }
         }
      }
      write_record_locked(PANDRM_REC_VM_BIND, fd_tag, ret,
                          buf.data(), static_cast<uint32_t>(buf.size()));
      break;
   }
   case DRM_PANTHOR_TILER_HEAP_CREATE: {
      auto *hc = static_cast<struct drm_panthor_tiler_heap_create *>(arg);
      struct pandrm_rec_tiler_heap_create rec = {
         .vm_id = hc->vm_id,
         .initial_chunk_count = hc->initial_chunk_count,
         .chunk_size = hc->chunk_size,
         .max_chunks = hc->max_chunks,
         .target_in_flight = hc->target_in_flight,
         .handle = hc->handle,
         .tiler_heap_ctx_gpu_va = hc->tiler_heap_ctx_gpu_va,
         .first_heap_chunk_gpu_va = hc->first_heap_chunk_gpu_va
      };
      write_record_locked(PANDRM_REC_TILER_HEAP_CREATE, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_TILER_HEAP_DESTROY: {
      auto *hd = static_cast<struct drm_panthor_tiler_heap_destroy *>(arg);
      struct pandrm_rec_tiler_heap_destroy rec = { .handle = hd->handle, .pad = 0 };
      write_record_locked(PANDRM_REC_TILER_HEAP_DESTROY, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_GROUP_CREATE: {
      auto *gc = static_cast<struct drm_panthor_group_create *>(arg);
      struct pandrm_rec_group_create rec;
      memset(&rec, 0, sizeof(rec));
      rec.vm_id = gc->vm_id;
      rec.queues_count = (gc->queues.count <= ADAPTER_MAX_SUBQUEUES) ? gc->queues.count : ADAPTER_MAX_SUBQUEUES;
      rec.max_compute_cores = gc->max_compute_cores;
      rec.max_fragment_cores = gc->max_fragment_cores;
      rec.max_tiler_cores = gc->max_tiler_cores;
      rec.priority = gc->priority;
      rec.group_handle = gc->group_handle;
      rec.compute_core_mask = gc->compute_core_mask;
      rec.fragment_core_mask = gc->fragment_core_mask;
      rec.tiler_core_mask = gc->tiler_core_mask;
      const auto *qs = reinterpret_cast<const struct drm_panthor_queue_create *>(
         static_cast<uintptr_t>(gc->queues.array));
      if (qs && rec.queues_count > 0)
         memcpy(rec.queues, qs, rec.queues_count * sizeof(struct drm_panthor_queue_create));
      write_record_locked(PANDRM_REC_GROUP_CREATE, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_GROUP_DESTROY: {
      auto *gd = static_cast<struct drm_panthor_group_destroy *>(arg);
      struct pandrm_rec_group_destroy rec = { .group_handle = gd->group_handle, .pad = 0 };
      write_record_locked(PANDRM_REC_GROUP_DESTROY, fd_tag, ret, &rec, sizeof(rec));
      break;
   }
   case DRM_PANTHOR_GROUP_SUBMIT: {
      auto *gs = static_cast<struct drm_panthor_group_submit *>(arg);
      uint32_t qcnt = (gs->queue_submits.count <= 16U) ? gs->queue_submits.count : 16U;
      const auto *qsubs = reinterpret_cast<const struct drm_panthor_queue_submit *>(
         static_cast<uintptr_t>(gs->queue_submits.array));
      struct pandrm_rec_group_submit hdr = {
         .group_handle = gs->group_handle,
         .queue_submit_count = qsubs ? qcnt : 0U
      };
      std::vector<uint8_t> buf(sizeof(hdr) + hdr.queue_submit_count * sizeof(struct pandrm_rec_queue_submit_entry));
      memcpy(buf.data(), &hdr, sizeof(hdr));
      auto *entries = reinterpret_cast<struct pandrm_rec_queue_submit_entry *>(buf.data() + sizeof(hdr));
      for (uint32_t i = 0; i < hdr.queue_submit_count; i++) {
         memset(&entries[i], 0, sizeof(entries[i]));
         entries[i].queue_index = qsubs[i].queue_index;
         entries[i].stream_size = qsubs[i].stream_size;
         entries[i].stream_addr = qsubs[i].stream_addr;
         entries[i].latest_flush = qsubs[i].latest_flush;
         uint32_t scnt = (qsubs[i].syncs.count <= 16U) ? qsubs[i].syncs.count : 16U;
         entries[i].sync_count = scnt;
         const auto *sops = reinterpret_cast<const struct drm_panthor_sync_op *>(
            static_cast<uintptr_t>(qsubs[i].syncs.array));
         if (sops && scnt > 0)
            memcpy(entries[i].syncs, sops, scnt * sizeof(struct drm_panthor_sync_op));
      }
      write_record_locked(PANDRM_REC_GROUP_SUBMIT, fd_tag, ret,
                          buf.data(), static_cast<uint32_t>(buf.size()));
      break;
   }
   default:
      break;
   }
   pthread_mutex_unlock(&g_cap_mutex);
}

/* ==========================================================================
 * Local & On-Device Replay Engine (panthor_drm_replay_trace_file)
 * ========================================================================== */
struct ReplayBoMapping {
   uint32_t cap_handle;
   uint32_t replay_handle;
   uint64_t cap_va;
   uint64_t replay_va;
   uint64_t size;
   void    *cpu_ptr;
};

static uint64_t relocate_gpu_va(uint64_t addr,
                                const std::vector<ReplayBoMapping> &mappings,
                                uint64_t *reloc_counter)
{
   if (addr < 0x10000ULL)
      return addr;
   for (const auto &m : mappings) {
      if (m.cap_va != 0 && addr >= m.cap_va && addr < m.cap_va + m.size) {
         uint64_t off = addr - m.cap_va;
         uint64_t new_va = m.replay_va + off;
         if (new_va != addr && reloc_counter)
            (*reloc_counter)++;
         return new_va;
      }
   }
   return addr;
}

/* Scan CSF instruction stream and relocate any MOVE48 immediate pointing into a captured BO */
static void relocate_csf_stream_vas(uint64_t *qwords,
                                    uint32_t n_qw,
                                    const std::vector<ReplayBoMapping> &mappings,
                                    uint64_t *reloc_counter)
{
   for (uint32_t i = 0; i < n_qw; i++) {
      uint64_t insn = qwords[i];
      uint8_t op = static_cast<uint8_t>((insn >> 56) & 0xffULL);
      if (op == MALI_CSF_OP_MOVE48) {
         uint64_t imm48 = insn & MALI_CSF_INS_MOVE48_IMMEDIATE_MASK;
         uint64_t reloc = relocate_gpu_va(imm48, mappings, reloc_counter);
         if (reloc != imm48) {
            qwords[i] = (insn & ~MALI_CSF_INS_MOVE48_IMMEDIATE_MASK) |
                        (reloc & MALI_CSF_INS_MOVE48_IMMEDIATE_MASK);
         }
      }
   }
}

/* Verify CSF stream invariants (Valhall v10/v11 deferred sync waits, FINISH_FRAGMENT bit 0, MOVE48 before FLUSH_CACHE2) */
static uint32_t audit_csf_chunk_invariants(const uint64_t *qwords,
                                           uint32_t n_qw,
                                           bool dump_csf,
                                           uint32_t q_idx,
                                           uint32_t chunk_idx)
{
   uint32_t violations = 0;
   const uint64_t def_wait_bit = static_cast<uint64_t>(MALI_PANVK_SB_MASK_DEFERRED_SYNC)
                                 << MALI_CSF_INS_WAIT_WAIT_MASK_SHIFT;
   for (uint32_t i = 0; i < n_qw; i++) {
      uint64_t insn = qwords[i];
      uint8_t op = static_cast<uint8_t>((insn >> 56) & 0xffULL);
      if (dump_csf && insn != 0ULL) {
         printf("    [CSF q=%u chunk=%u +%03u] 0x%016llx (%s)\n",
                q_idx, chunk_idx, i * 8U,
                static_cast<unsigned long long>(insn),
                panthor_kbase_csf_opcode_name(op));
      }
      /* Invariant 1: FINISH_FRAGMENT (0x0b) must have bit 0 (increment_frag_completed) == 0 */
      if (op == MALI_CSF_OP_FINISH_FRAGMENT &&
          (insn & MALI_CSF_INS_FINISH_FRAGMENT_INCREMENT_FRAGMENT_COMPLETED_MASK) != 0ULL &&
          ((insn >> 1) & 0x7fffULL) == 0ULL) {
         violations++;
      }
      /* Invariant 2: SYNC_WAIT32/64 preceded by WAIT must wait on SB_ID(DEFERRED_SYNC) (slot 1) */
      if ((op == MALI_CSF_OP_SYNC_WAIT32 || op == MALI_CSF_OP_SYNC_WAIT64) &&
          ((insn >> 32) & 0xffffULL) != 0ULL && i > 0U) {
         uint32_t min_j = (i >= 4U) ? (i - 4U) : 0U;
         for (uint32_t j = i; j > min_j; j--) {
            uint64_t prev = qwords[j - 1U];
            if (((prev >> 56) & 0xffULL) == MALI_CSF_OP_WAIT) {
               if ((prev & def_wait_bit) == 0ULL)
                  violations++;
               break;
            }
         }
      }
   }
   return violations;
}

int panthor_drm_replay_trace_file(const char *filepath,
                                  const struct pandrm_replay_options *opts,
                                  struct pandrm_replay_summary *out_summary)
{
   struct pandrm_replay_summary sum;
   memset(&sum, 0, sizeof(sum));
   sum.final_status = -1;

   if (!filepath || !filepath[0])
      return -1;

   FILE *fp = fopen(filepath, "rb");
   if (!fp)
      return -1;

   struct pandrm_trace_file_header fhdr;
   if (fread(&fhdr, sizeof(fhdr), 1, fp) != 1 ||
       memcmp(fhdr.magic, PANDRM_TRACE_MAGIC, 8) != 0 ||
       fhdr.version != PANDRM_TRACE_VERSION) {
      fclose(fp);
      return -1;
   }

   bool verbose = opts ? opts->verbose : false;
   bool dump_csf = opts ? opts->dump_csf : false;
   bool verify_inv = opts ? opts->verify_invariants : true;

   /* Pause capture while replaying so replay doesn't recursively record itself */
   bool was_capturing = panthor_drm_capture_is_active();
   if (was_capturing)
      __atomic_store_n(&g_cap_active, false, __ATOMIC_RELEASE);

   if (opts && opts->bug_catching_mode)
      setenv("PANTHOR_ADAPTER_BUG_CATCHING_MODE", "1", 1);

   int drm_fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR);
   if (drm_fd < 0) {
      fclose(fp);
      if (was_capturing)
         __atomic_store_n(&g_cap_active, true, __ATOMIC_RELEASE);
      return -1;
   }

   std::unordered_map<uint32_t, uint32_t> vm_map;
   std::unordered_map<uint32_t, ReplayBoMapping> bo_map;
   std::vector<ReplayBoMapping> bo_list;
   std::unordered_map<uint32_t, uint32_t> heap_map;
   std::unordered_map<uint32_t, uint32_t> group_map;
   std::unordered_map<uint32_t, uint32_t> syncobj_map;

   struct pandrm_record_header rhdr;
   while (fread(&rhdr, sizeof(rhdr), 1, fp) == 1) {
      std::vector<uint8_t> payload(rhdr.payload_bytes);
      if (rhdr.payload_bytes > 0) {
         if (fread(payload.data(), 1, rhdr.payload_bytes, fp) != rhdr.payload_bytes)
            break;
         uint64_t calc_hash = panthor_fnv1a64_bytes(payload.data(), rhdr.payload_bytes);
         if (calc_hash != rhdr.fnv1a64_checksum) {
            LOG_ERROR("[DRM_REPLAY] Checksum mismatch at record seq=%llu (expected 0x%llx, got 0x%llx)",
                      static_cast<unsigned long long>(rhdr.seq_id),
                      static_cast<unsigned long long>(rhdr.fnv1a64_checksum),
                      static_cast<unsigned long long>(calc_hash));
            break;
         }
      }

      sum.total_records_replayed++;
      switch (static_cast<enum pandrm_record_type>(rhdr.record_type)) {
      case PANDRM_REC_DEV_QUERY: {
         if (payload.size() >= sizeof(struct pandrm_rec_dev_query)) {
            auto *rq = reinterpret_cast<const struct pandrm_rec_dev_query *>(payload.data());
            std::vector<uint8_t> qbuf(rq->query_size ? rq->query_size : 128U);
            struct drm_panthor_dev_query dq = {
               .type = rq->query_type,
               .size = rq->query_size,
               .pointer = rq->query_size ? reinterpret_cast<uintptr_t>(qbuf.data()) : 0ULL
            };
            panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &dq);
         }
         break;
      }
      case PANDRM_REC_VM_CREATE: {
         if (payload.size() >= sizeof(struct pandrm_rec_vm_create)) {
            auto *rv = reinterpret_cast<const struct pandrm_rec_vm_create *>(payload.data());
            struct drm_panthor_vm_create vc = {
               .flags = rv->flags,
               .id = 0,
               .user_va_range = rv->user_va_range
            };
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vc) == 0)
               vm_map[rv->vm_id] = vc.id;
         }
         break;
      }
      case PANDRM_REC_VM_DESTROY: {
         if (payload.size() >= sizeof(struct pandrm_rec_vm_destroy)) {
            auto *rv = reinterpret_cast<const struct pandrm_rec_vm_destroy *>(payload.data());
            uint32_t mapped_vm = vm_map.count(rv->vm_id) ? vm_map[rv->vm_id] : rv->vm_id;
            struct drm_panthor_vm_destroy vd = { .id = mapped_vm, .pad = 0 };
            panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vd);
         }
         break;
      }
      case PANDRM_REC_BO_CREATE: {
         if (payload.size() >= sizeof(struct pandrm_rec_bo_create)) {
            auto *rb = reinterpret_cast<const struct pandrm_rec_bo_create *>(payload.data());
            struct drm_panthor_bo_create bc;
            memset(&bc, 0, sizeof(bc));
            bc.size = rb->size;
            bc.flags = rb->panthor_flags;
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bc) == 0) {
               uint64_t r_va = panthor_kbase_adapter_get_bo_gpu_va(bc.handle);
               ReplayBoMapping m = {
                  .cap_handle = rb->gem_handle,
                  .replay_handle = bc.handle,
                  .cap_va = rb->gpu_va,
                  .replay_va = r_va,
                  .size = bc.size,
                  .cpu_ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(r_va))
               };
               bo_map[rb->gem_handle] = m;
               bo_list.push_back(m);
               sum.total_bo_creates++;
            }
         }
         break;
      }
      case PANDRM_REC_VM_BIND: {
         if (payload.size() >= sizeof(struct pandrm_rec_vm_bind_header)) {
            auto *vh = reinterpret_cast<const struct pandrm_rec_vm_bind_header *>(payload.data());
            const auto *in_ops = reinterpret_cast<const struct drm_panthor_vm_bind_op *>(
               payload.data() + sizeof(*vh));
            std::vector<struct drm_panthor_vm_bind_op> ops(vh->op_count);
            for (uint32_t i = 0; i < vh->op_count; i++) {
               ops[i] = in_ops[i];
               if (bo_map.count(ops[i].bo_handle))
                  ops[i].bo_handle = bo_map[ops[i].bo_handle].replay_handle;
            }
            struct drm_panthor_vm_bind vb;
            memset(&vb, 0, sizeof(vb));
            vb.vm_id = vm_map.count(vh->vm_id) ? vm_map[vh->vm_id] : vh->vm_id;
            vb.flags = vh->flags;
            vb.ops.count = vh->op_count;
            vb.ops.array = reinterpret_cast<uintptr_t>(ops.data());
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb) == 0) {
               for (uint32_t i = 0; i < vh->op_count; i++) {
                  uint32_t cap_h = in_ops[i].bo_handle;
                  if (bo_map.count(cap_h) && ops[i].va != 0) {
                     bo_map[cap_h].replay_va = ops[i].va - ops[i].bo_offset;
                     if (bo_map[cap_h].cpu_ptr == nullptr) {
                        bo_map[cap_h].cpu_ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(bo_map[cap_h].replay_va));
                     }
                     for (auto &bm : bo_list) {
                        if (bm.cap_handle == cap_h) {
                           bm.replay_va = bo_map[cap_h].replay_va;
                           bm.cpu_ptr = bo_map[cap_h].cpu_ptr;
                        }
                     }
                  }
               }
            }
         }
         break;
      }
      case PANDRM_REC_TILER_HEAP_CREATE: {
         if (payload.size() >= sizeof(struct pandrm_rec_tiler_heap_create)) {
            auto *rh = reinterpret_cast<const struct pandrm_rec_tiler_heap_create *>(payload.data());
            struct drm_panthor_tiler_heap_create hc;
            memset(&hc, 0, sizeof(hc));
            hc.vm_id = vm_map.count(rh->vm_id) ? vm_map[rh->vm_id] : rh->vm_id;
            hc.initial_chunk_count = rh->initial_chunk_count;
            hc.chunk_size = rh->chunk_size;
            hc.max_chunks = rh->max_chunks;
            hc.target_in_flight = rh->target_in_flight;
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc) == 0)
               heap_map[rh->handle] = hc.handle;
         }
         break;
      }
      case PANDRM_REC_TILER_HEAP_DESTROY: {
         if (payload.size() >= sizeof(struct pandrm_rec_tiler_heap_destroy)) {
            auto *rh = reinterpret_cast<const struct pandrm_rec_tiler_heap_destroy *>(payload.data());
            struct drm_panthor_tiler_heap_destroy hd = {
               .handle = heap_map.count(rh->handle) ? heap_map[rh->handle] : rh->handle,
               .pad = 0
            };
            panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd);
         }
         break;
      }
      case PANDRM_REC_GROUP_CREATE: {
         if (payload.size() >= sizeof(struct pandrm_rec_group_create)) {
            auto *rg = reinterpret_cast<const struct pandrm_rec_group_create *>(payload.data());
            struct drm_panthor_queue_create qcopy[ADAPTER_MAX_SUBQUEUES];
            memcpy(qcopy, rg->queues, sizeof(qcopy));
            struct drm_panthor_group_create gc;
            memset(&gc, 0, sizeof(gc));
            gc.vm_id = vm_map.count(rg->vm_id) ? vm_map[rg->vm_id] : rg->vm_id;
            gc.queues.count = rg->queues_count;
            gc.queues.array = reinterpret_cast<uintptr_t>(qcopy);
            gc.max_compute_cores = rg->max_compute_cores;
            gc.max_fragment_cores = rg->max_fragment_cores;
            gc.max_tiler_cores = rg->max_tiler_cores;
            gc.priority = rg->priority;
            gc.compute_core_mask = rg->compute_core_mask;
            gc.fragment_core_mask = rg->fragment_core_mask;
            gc.tiler_core_mask = rg->tiler_core_mask;
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc) == 0)
               group_map[rg->group_handle] = gc.group_handle;
         }
         break;
      }
      case PANDRM_REC_GROUP_DESTROY: {
         if (payload.size() >= sizeof(struct pandrm_rec_group_destroy)) {
            auto *rg = reinterpret_cast<const struct pandrm_rec_group_destroy *>(payload.data());
            struct drm_panthor_group_destroy gd = {
               .group_handle = group_map.count(rg->group_handle) ? group_map[rg->group_handle] : rg->group_handle,
               .pad = 0
            };
            panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd);
         }
         break;
      }
      case PANDRM_REC_SYNCOBJ_CREATE: {
         if (payload.size() >= sizeof(struct pandrm_rec_syncobj_create)) {
            auto *rs = reinterpret_cast<const struct pandrm_rec_syncobj_create *>(payload.data());
            struct drm_syncobj_create sc = { .handle = 0, .flags = rs->flags };
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) == 0)
               syncobj_map[rs->handle] = sc.handle;
         }
         break;
      }
      case PANDRM_REC_SYNCOBJ_DESTROY: {
         if (payload.size() >= sizeof(struct pandrm_rec_syncobj_destroy)) {
            auto *rs = reinterpret_cast<const struct pandrm_rec_syncobj_destroy *>(payload.data());
            struct drm_syncobj_destroy sd = {
               .handle = syncobj_map.count(rs->handle) ? syncobj_map[rs->handle] : rs->handle,
               .pad = 0
            };
            panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
         }
         break;
      }
      case PANDRM_REC_SYNCOBJ_OP: {
         if (payload.size() >= sizeof(struct pandrm_rec_syncobj_op)) {
            auto *ro = reinterpret_cast<const struct pandrm_rec_syncobj_op *>(payload.data());
            uint32_t mapped_h[16] = {0};
            uint64_t pts[16] = {0};
            for (uint32_t i = 0; i < ro->count_handles && i < 16U; i++) {
               mapped_h[i] = syncobj_map.count(ro->handles[i]) ? syncobj_map[ro->handles[i]] : ro->handles[i];
               pts[i] = ro->points[i];
            }
            if (ro->op_kind == PANDRM_SYNCOBJ_WAIT) {
               struct drm_syncobj_wait sw;
               memset(&sw, 0, sizeof(sw));
               sw.handles = reinterpret_cast<uintptr_t>(mapped_h);
               sw.timeout_nsec = INT64_MAX;
               sw.count_handles = ro->count_handles;
               sw.flags = ro->flags;
               panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
               sum.total_syncobj_waits++;
            } else if (ro->op_kind == PANDRM_SYNCOBJ_TIMELINE_WAIT) {
               struct drm_syncobj_timeline_wait tw;
               memset(&tw, 0, sizeof(tw));
               tw.handles = reinterpret_cast<uintptr_t>(mapped_h);
               tw.points = reinterpret_cast<uintptr_t>(pts);
               tw.timeout_nsec = INT64_MAX;
               tw.count_handles = ro->count_handles;
               tw.flags = ro->flags;
               panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw);
               sum.total_syncobj_waits++;
            } else if (ro->op_kind == PANDRM_SYNCOBJ_RESET || ro->op_kind == PANDRM_SYNCOBJ_SIGNAL) {
               struct drm_syncobj_array sa = {
                  .handles = reinterpret_cast<uintptr_t>(mapped_h),
                  .count_handles = ro->count_handles,
                  .pad = 0
               };
               panthor_kbase_adapter_ioctl(
                  drm_fd,
                  (ro->op_kind == PANDRM_SYNCOBJ_RESET) ? DRM_IOCTL_SYNCOBJ_RESET : DRM_IOCTL_SYNCOBJ_SIGNAL,
                  &sa);
            } else if (ro->op_kind == PANDRM_SYNCOBJ_TIMELINE_SIGNAL || ro->op_kind == PANDRM_SYNCOBJ_QUERY) {
               struct drm_syncobj_timeline_array ta = {
                  .handles = reinterpret_cast<uintptr_t>(mapped_h),
                  .points = reinterpret_cast<uintptr_t>(pts),
                  .count_handles = ro->count_handles,
                  .flags = ro->flags
               };
               panthor_kbase_adapter_ioctl(
                  drm_fd,
                  (ro->op_kind == PANDRM_SYNCOBJ_QUERY) ? DRM_IOCTL_SYNCOBJ_QUERY : DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL,
                  &ta);
            }
         }
         break;
      }
      case PANDRM_REC_BO_DATA_SNAPSHOT: {
         if (payload.size() >= sizeof(struct pandrm_rec_bo_data_snapshot)) {
            auto *rs = reinterpret_cast<const struct pandrm_rec_bo_data_snapshot *>(payload.data());
            const uint8_t *raw = payload.data() + sizeof(*rs);
            if (payload.size() >= sizeof(*rs) + rs->byte_size && bo_map.count(rs->gem_handle)) {
               ReplayBoMapping &bm = bo_map[rs->gem_handle];
               if (bm.cpu_ptr && rs->offset + rs->byte_size <= bm.size) {
                  uint8_t *dst = static_cast<uint8_t *>(bm.cpu_ptr) + rs->offset;
                  memcpy(dst, raw, static_cast<size_t>(rs->byte_size));
                  /* Also relocate any 64-bit GPU VA words inside non-executable BOs (e.g. subq_ctx, descriptor tables) */
                  if (!rs->is_executable && (rs->byte_size % sizeof(uint64_t)) == 0) {
                     auto *u64p = reinterpret_cast<uint64_t *>(dst);
                     uint32_t n_u64 = static_cast<uint32_t>(rs->byte_size / sizeof(uint64_t));
                     relocate_csf_stream_vas(u64p, n_u64, bo_list, &sum.total_va_relocations_applied);
                     for (uint32_t w = 0; w < n_u64; w++) {
                        uint64_t v = u64p[w];
                        if ((v >> 56) == 0ULL && v >= 0x10000ULL) {
                           u64p[w] = relocate_gpu_va(v, bo_list, &sum.total_va_relocations_applied);
                        }
                     }
                  }
                  sum.total_bo_snapshots_restored++;
               }
            }
         }
         break;
      }
      case PANDRM_REC_CSF_STREAM_CLOSURE: {
         if (payload.size() >= sizeof(struct pandrm_rec_csf_stream_closure)) {
            auto *rc = reinterpret_cast<const struct pandrm_rec_csf_stream_closure *>(payload.data());
            const auto *qwords = reinterpret_cast<const uint64_t *>(payload.data() + sizeof(*rc));
            uint32_t n_qw = rc->size_bytes / sizeof(uint64_t);
            if (payload.size() >= sizeof(*rc) + rc->size_bytes) {
               /* Restore CSF chunk into its mapped replay BO as well so CALL/JUMP targets are guaranteed populated */
               uint64_t dst_va = relocate_gpu_va(rc->gpu_va, bo_list, &sum.total_va_relocations_applied);
               if (dst_va >= 0x10000ULL) {
                  auto *dst_qw = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(dst_va));
                  memcpy(dst_qw, qwords, rc->size_bytes);
                  relocate_csf_stream_vas(dst_qw, n_qw, bo_list, &sum.total_va_relocations_applied);
               }
               if (verify_inv) {
                  sum.csf_invariant_violations += audit_csf_chunk_invariants(
                     qwords, n_qw, dump_csf, rc->queue_index, rc->chunk_index);
               }
               sum.total_csf_chunks_verified++;
            }
         }
         break;
      }
      case PANDRM_REC_ISA_SHADER_BLOB: {
         if (payload.size() >= sizeof(struct pandrm_rec_isa_shader_blob)) {
            auto *ri = reinterpret_cast<const struct pandrm_rec_isa_shader_blob *>(payload.data());
            if (payload.size() >= sizeof(*ri) + ri->shader_size) {
               sum.total_isa_blobs_restored++;
            }
         }
         break;
      }
      case PANDRM_REC_TEX_META: {
         if (payload.size() >= sizeof(struct pandrm_rec_tex_meta)) {
            auto *rt = reinterpret_cast<const struct pandrm_rec_tex_meta *>(payload.data());
            if (bo_map.count(rt->gem_handle)) {
               ReplayBoMapping &bm = bo_map[rt->gem_handle];
               if (rt->is_stubbed && bm.cpu_ptr && bm.size >= 4096U) {
                  /* Synthesize deterministic diagnostic checkerboard pattern for stubbed textures */
                  auto *px = static_cast<uint32_t *>(bm.cpu_ptr);
                  size_t max_px = (bm.size / 4U < 65536U) ? (bm.size / 4U) : 65536U;
                  for (size_t p = 0; p < max_px; p++) {
                     uint32_t x = static_cast<uint32_t>(p & 255U);
                     uint32_t y = static_cast<uint32_t>((p >> 8) & 255U);
                     px[p] = (((x >> 4) ^ (y >> 4)) & 1U) ? 0xFF7E771DU : 0xFF868686U;
                  }
               }
               sum.total_tex_metas_restored++;
            }
         }
         break;
      }
      case PANDRM_REC_GROUP_SUBMIT: {
         if (payload.size() >= sizeof(struct pandrm_rec_group_submit)) {
            auto *rgs = reinterpret_cast<const struct pandrm_rec_group_submit *>(payload.data());
            const auto *entries = reinterpret_cast<const struct pandrm_rec_queue_submit_entry *>(
               payload.data() + sizeof(*rgs));
            uint32_t qcnt = rgs->queue_submit_count;
            std::vector<struct drm_panthor_queue_submit> qsubs(qcnt);
            std::vector<std::vector<struct drm_panthor_sync_op>> sync_vecs(qcnt);
            for (uint32_t i = 0; i < qcnt; i++) {
               memset(&qsubs[i], 0, sizeof(qsubs[i]));
               qsubs[i].queue_index = entries[i].queue_index;
               qsubs[i].stream_size = entries[i].stream_size;
               qsubs[i].stream_addr = relocate_gpu_va(
                  entries[i].stream_addr, bo_list, &sum.total_va_relocations_applied);
               qsubs[i].latest_flush = entries[i].latest_flush;
               uint32_t scnt = entries[i].sync_count;
               sync_vecs[i].resize(scnt);
               for (uint32_t s = 0; s < scnt; s++) {
                  sync_vecs[i][s] = entries[i].syncs[s];
                  uint32_t cap_sh = sync_vecs[i][s].handle;
                  if (syncobj_map.count(cap_sh))
                     sync_vecs[i][s].handle = syncobj_map[cap_sh];
               }
               qsubs[i].syncs.count = scnt;
               qsubs[i].syncs.array = scnt ? reinterpret_cast<uintptr_t>(sync_vecs[i].data()) : 0ULL;
            }
            struct drm_panthor_group_submit gs;
            memset(&gs, 0, sizeof(gs));
            gs.group_handle = group_map.count(rgs->group_handle) ? group_map[rgs->group_handle] : rgs->group_handle;
            gs.queue_submits.count = qcnt;
            gs.queue_submits.array = qcnt ? reinterpret_cast<uintptr_t>(qsubs.data()) : 0ULL;
            if (panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs) == 0)
               sum.total_group_submits++;
         }
         break;
      }
      case PANDRM_REC_GEM_CLOSE: {
         if (payload.size() >= sizeof(struct pandrm_rec_gem_close)) {
            auto *rc = reinterpret_cast<const struct pandrm_rec_gem_close *>(payload.data());
            if (bo_map.count(rc->gem_handle)) {
               struct drm_gem_close cl = { .handle = bo_map[rc->gem_handle].replay_handle, .pad = 0 };
               panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
               bo_map.erase(rc->gem_handle);
            }
         }
         break;
      }
      default:
         break;
      }
   }

   panthor_kbase_adapter_close(drm_fd);
   fclose(fp);
   if (was_capturing)
      __atomic_store_n(&g_cap_active, true, __ATOMIC_RELEASE);

   sum.final_status = (sum.csf_invariant_violations == 0) ? 0 : -2;
   if (verbose) {
      printf("[DRM_REPLAY] Completed replay of '%s': records=%llu bos=%llu snapshots=%llu relocs=%llu "
             "csf_chunks=%llu isa_blobs=%llu tex_metas=%llu submits=%llu waits=%llu violations=%u\n",
             filepath,
             static_cast<unsigned long long>(sum.total_records_replayed),
             static_cast<unsigned long long>(sum.total_bo_creates),
             static_cast<unsigned long long>(sum.total_bo_snapshots_restored),
             static_cast<unsigned long long>(sum.total_va_relocations_applied),
             static_cast<unsigned long long>(sum.total_csf_chunks_verified),
             static_cast<unsigned long long>(sum.total_isa_blobs_restored),
             static_cast<unsigned long long>(sum.total_tex_metas_restored),
             static_cast<unsigned long long>(sum.total_group_submits),
             static_cast<unsigned long long>(sum.total_syncobj_waits),
             sum.csf_invariant_violations);
   }
   if (out_summary)
      *out_summary = sum;
   return sum.final_status;
}

} /* extern "C" */
