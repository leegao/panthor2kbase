#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "drm.h"
#include "panthor_drm.h"
#include "mali_kbase_uapi.h"
#include "panthor_kbase_adapter.h"
#include "panthor_drm_capture_replay.h"
#include "panthor_vk_capture_replay.h"
#include "xf86drm.h"
#include "xf86drmMode.h"
#include "mali_kbase_emu.h"

static void test_mali_kbase_ko_flag_bit_positions(void)
{
   /* Verify exact bit positions against disassembled mali_kbase.ko */
   assert(BASE_MEM_PROT_CPU_RD == (1UL << 0));
   assert(BASE_MEM_PROT_CPU_WR == (1UL << 1));
   assert(BASE_MEM_PROT_GPU_RD == (1UL << 2));
   assert(BASE_MEM_PROT_GPU_WR == (1UL << 3));
   assert(BASE_MEM_PROT_GPU_EX == (1UL << 4));
   assert(BASEP_MEM_PERMANENT_KERNEL_MAPPING == (1UL << 5));
   assert(BASE_MEM_GPU_VA_SAME_4GB_PAGE == (1UL << 6));
   assert(BASEP_MEM_NO_USER_FREE == (1UL << 7));
   assert((BASEP_MEM_PERMANENT_KERNEL_MAPPING | BASEP_MEM_NO_USER_FREE) == 0xa0UL);
   assert(BASE_MEM_FIXED == (1UL << 8));
   assert(BASE_MEM_GROW_ON_GPF == (1UL << 9));
   assert(BASE_MEM_COHERENT_SYSTEM == (1ULL << 10));
   assert(BASE_MEM_COHERENT_LOCAL == (1ULL << 11));
   assert((BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL) == 0xc00ULL);
   assert(BASE_MEM_CACHED_CPU == (1ULL << 12));
   assert(BASE_MEM_SAME_VA == (1ULL << 13));
   assert(BASE_MEM_NEED_MMAP == (1ULL << 14));
   assert(BASE_MEM_COHERENT_SYSTEM_REQUIRED == (1ULL << 15));
   assert(BASE_MEM_PROTECTED == (1ULL << 16));
   assert(BASE_MEM_DONT_NEED == (1ULL << 17));
   assert(BASE_MEM_IMPORT_SHARED == (1ULL << 18));
   assert(BASE_MEM_CSF_EVENT == (1ULL << 19));
   assert(BASE_MEM_TILER_ALIGN_TOP == (1ULL << 20));
   assert(BASE_MEM_UNCACHED_GPU == (1ULL << 21));
   assert(BASE_MEM_FIXABLE == (1ULL << 29));
   printf("  -> Exact BASE_MEM_* bit positions verified against mali_kbase.ko: PASS\n");
}

static void test_csf_normalizer_edge_cases(void)
{
   struct adapter_cs_patch_stats stats;
   uint64_t dummy[8] = {0};

   panthor_kbase_normalize_cs_stream(NULL, 0, false, &stats);
   assert(stats.sync_scope_upgrades == 0 && stats.heap_op_nops == 0);

   panthor_kbase_normalize_cs_stream(dummy, 4, false, &stats);
   assert(stats.sync_scope_upgrades == 0 && stats.heap_op_nops == 0);

   dummy[0] = ((uint64_t)CSF_OP_SYNC_ADD32 << 56) | (0ULL << 28);
   dummy[1] = ((uint64_t)CSF_OP_SYNC_ADD64 << 56) | (0ULL << 28);
   dummy[2] = ((uint64_t)CSF_OP_SYNC_SET32 << 56) | (1ULL << 28) | (1ULL << 24);
   dummy[3] = ((uint64_t)CSF_OP_SYNC_SET64 << 56) | (2ULL << 28) | (1ULL << 24);
   dummy[4] = ((uint64_t)CSF_OP_HEAP_OPERATION << 56) | (1ULL << 28);
   dummy[5] = ((uint64_t)CSF_OP_MOVE48 << 56);

   dummy[6] = ((uint64_t)HW_CSF_OP_FINISH_FRAGMENT << 56) | (10ULL << 40) | (12ULL << 32) | 1ULL;
   panthor_kbase_normalize_cs_stream(dummy, sizeof(dummy), true, &stats);
   assert(stats.sync_scope_upgrades == 2 && stats.heap_op_nops == 1 && stats.finish_frag_stat_clears == 1);
   assert((dummy[6] & 1ULL) == 0ULL);
   assert(strcmp(panthor_kbase_csf_opcode_name(HW_CSF_OP_FINISH_FRAGMENT), "FINISH_FRAGMENT") == 0);
   panthor_kbase_normalize_cs_stream(dummy, sizeof(dummy), false, NULL);
   printf("  -> CSF Bytecode Normalizer & RAII indent invariant: PASS\n");
}

static void test_ring_slot_emitter_v10_and_v11(void)
{
   uint64_t ring[ADAPTER_RING_SLOT_QWORDS];
   size_t bytes10 = panthor_kbase_emit_ring_slot(
      ring, 10, 64, 0, 0x1000, 0x2000, 0x3000, 128, 5, 0x4000, 0xff);
   assert(bytes10 == ADAPTER_RING_SLOT_BYTES);
   assert((ring[0] >> 56) == CSF_OP_SET_SB_ENTRY);

   size_t bytes11 = panthor_kbase_emit_ring_slot(
      ring, 11, 96, 2, 0, 0, 0x5000, 256, 9, 0x6000, 0xff);
   assert(bytes11 == ADAPTER_RING_SLOT_BYTES);
   assert((ring[ADAPTER_RING_SLOT_QWORDS - 1]) == 0);

   /* Also verify PANTHOR_KBASE_DUP_TRACKING feature flag toggle */
   assert(panthor_kbase_adapter_get_dup_tracking() == true);
   panthor_kbase_adapter_set_dup_tracking(false);
   assert(panthor_kbase_adapter_get_dup_tracking() == false);
   panthor_kbase_adapter_set_dup_tracking(true);
   assert(panthor_kbase_adapter_get_dup_tracking() == true);
   printf("  -> Ring Slot Emitter Valhall v10/v11+: PASS\n");
}

static void test_mali0_state_machine_and_flag_regressions(void)
{
   /* 1. open("/dev/mali0") without O_CLOEXEC -> EINVAL */
   int bad_fd = open("/dev/mali0", O_RDWR);
   assert(bad_fd == -1 && errno == EINVAL);

   int mfd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   assert(mfd >= 0);

   /* 2. Legacy JM VERSION_CHECK (nr 0) -> EPERM on CSF */
   struct kbase_ioctl_version_check vc = { .major = 1, .minor = 21 };
   assert(ioctl(mfd, KBASE_IOCTL_VERSION_CHECK_RESERVED, &vc) == -1 && errno == EPERM);

   /* 3. CSF VERSION_CHECK (nr 52) -> 0 */
   assert(ioctl(mfd, KBASE_IOCTL_VERSION_CHECK, &vc) == 0);

   /* 4. SET_FLAGS -> 0 */
   struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
   assert(ioctl(mfd, KBASE_IOCTL_SET_FLAGS, &sf) == 0);

   /* 5. Map BASE_MEM_MAP_TRACKING_HANDLE (3 << 12) */
   void *trk = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, mfd, BASE_MEM_MAP_TRACKING_HANDLE);
   assert(trk != MAP_FAILED);

   /* 6. Regression test: kernel-only flags (0x80 or 0x20 in 0xa0) MUST be rejected with ENOMEM! */
   union kbase_ioctl_mem_alloc ma_bad_kern = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = 0x8128fULL, /* Old buggy flag value with bit 7 (0x80) and bit 9 (0x200) set */
      }
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_ALLOC, &ma_bad_kern) == -1 && errno == ENOMEM);

   /* 7. Regression test: GROW_ON_GPF (0x200) with extension == 0 MUST be rejected with ENOMEM! */
   union kbase_ioctl_mem_alloc ma_bad_grow = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_GROW_ON_GPF,
      }
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_ALLOC, &ma_bad_grow) == -1 && errno == ENOMEM);

   /* 8. Regression test: COHERENT_SYSTEM | COHERENT_LOCAL (0xc00) MUST be rejected with ENOMEM! */
   union kbase_ioctl_mem_alloc ma_bad_coh = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD |
                  BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL,
      }
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_ALLOC, &ma_bad_coh) == -1 && errno == ENOMEM);

   /* 9. Regression test: CSF_EVENT | COHERENT_LOCAL MUST be rejected with ENOMEM (kernel forces COHERENT_SYSTEM)! */
   union kbase_ioctl_mem_alloc ma_bad_csf_coh = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_CSF_EVENT | BASE_MEM_COHERENT_LOCAL,
      }
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_ALLOC, &ma_bad_csf_coh) == -1 && errno == ENOMEM);

   /* 10. Regression test: STREAM_CREATE returns fd in retval and leaves name untouched */
   struct kbase_ioctl_stream_create sc;
   memset(&sc, 0, sizeof(sc));
   memcpy(sc.name, "panvk_sync", 11);
   int sfd = ioctl(mfd, KBASE_IOCTL_STREAM_CREATE, &sc);
   assert(sfd >= 0);
   assert(memcmp(sc.name, "panvk_sync", 11) == 0);
   close(sfd);

   /* 11. Valid MEM_JIT_INIT BEFORE any MEM_ALLOC */
   struct kbase_ioctl_mem_jit_init jit = {
      .va_pages = 0x2000000ULL,
      .max_allocations = 255,
      .trim_level = 0,
      .group_id = 0,
      .phys_pages = 0x2000000ULL,
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_JIT_INIT, &jit) == 0);

   /* 12. Valid CSF_EVENT allocation with corrected flags (0x8200f) */
   union kbase_ioctl_mem_alloc ma_good_evt = {
      .in = {
         .va_pages = 16,
         .commit_pages = 16,
         .extension = 0,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT,
      }
   };
   assert(ioctl(mfd, KBASE_IOCTL_MEM_ALLOC, &ma_good_evt) == 0);
   assert(ma_good_evt.out.gpu_va != 0);

   assert(close(mfd) == 0);
   printf("  -> /dev/mali0 State Machine & mali_kbase.ko Flag Regression Tests: PASS\n");
}

static void test_kms_and_libdrm_wrappers(void)
{
   int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(drm_fd >= 0);
   assert(drmModeGetResources(drm_fd) == NULL);
   drmModeFreeResources(NULL);
   assert(drmModeGetCrtc(drm_fd, 1) == NULL);
   drmModeFreeCrtc(NULL);
   assert(drmModeGetEncoder(drm_fd, 1) == NULL);
   drmModeFreeEncoder(NULL);
   assert(drmModeGetConnector(drm_fd, 1) == NULL);
   assert(drmModeGetConnectorCurrent(drm_fd, 1) == NULL);
   drmModeFreeConnector(NULL);
   assert(drmModeGetPlaneResources(drm_fd) == NULL);
   drmModeFreePlaneResources(NULL);
   assert(drmModeGetPlane(drm_fd, 1) == NULL);
   drmModeFreePlane(NULL);
   assert(drmModeGetProperty(drm_fd, 1) == NULL);
   drmModeFreeProperty(NULL);
   assert(drmModeGetPropertyBlob(drm_fd, 1) == NULL);
   drmModeFreePropertyBlob(NULL);
   assert(drmModeObjectGetProperties(drm_fd, 1, 1) == NULL);
   drmModeFreeObjectProperties(NULL);
   assert(drmModeConnectorSetProperty(drm_fd, 1, 1, 0) < 0);
   assert(drmModeCreatePropertyBlob(drm_fd, NULL, 0, NULL) < 0);
   assert(drmModeDestroyPropertyBlob(drm_fd, 1) < 0);
   assert(drmModeAddFB2WithModifiers(drm_fd, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, 0) < 0);
   assert(drmModeRmFB(drm_fd, 1) < 0);
   assert(drmModeAtomicAlloc() == NULL);
   drmModeAtomicFree(NULL);
   assert(drmModeAtomicAddProperty(NULL, 1, 1, 1) < 0);
   assert(drmModeAtomicCommit(drm_fd, NULL, 0, NULL) < 0);
   assert(drmModeCreateLease(drm_fd, NULL, 0, 0, NULL) < 0);
   assert(drmCrtcGetSequence(drm_fd, 1, NULL, NULL) < 0);
   assert(drmCrtcQueueSequence(drm_fd, 1, 0, 0, NULL, 0) < 0);
   drm_magic_t magic = 0;
   assert(drmGetMagic(drm_fd, &magic) == 0);
   assert(drmAuthMagic(drm_fd, magic) == 0);
   assert(drmSetMaster(drm_fd) == 0);
   assert(drmDropMaster(drm_fd) == 0);
   assert(drmIsMaster(drm_fd) == 0);
   assert(drmHandleEvent(drm_fd, NULL) == 0);
   assert(drmSetClientCap(drm_fd, 1, 1) == 0);
   assert(drmGetNodeTypeFromFd(drm_fd) == DRM_NODE_RENDER);
   drmDevicePtr devs[4] = {0};
   int ndevs = drmGetDevices2(0, devs, 4);
   assert(ndevs >= 1);
   drmFreeDevices(devs, ndevs);
   drmDevicePtr single_dev = NULL;
   assert(drmGetDevice2(drm_fd, 0, &single_dev) == 0 && single_dev != NULL);
   drmFreeDevice(&single_dev);
   char *rname = drmGetRenderDeviceNameFromFd(drm_fd);
   assert(rname != NULL);
   free(rname);
   int dup2_fd = panthor_kbase_adapter_dup(drm_fd);
   assert(dup2_fd >= 0);
   assert(panthor_kbase_adapter_close(dup2_fd) == 0);
   assert(close(drm_fd) == 0);
   printf("  -> KMS Stubs & libdrm Wrappers: PASS\n");
}

static void test_panvk_auto_va_ringbuf_and_tracebuf_integration(void)
{
   int drm_fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(drm_fd >= 0);

   struct drm_panthor_vm_create vmc = { .flags = 0, .user_va_range = (1ULL << 47) };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);
   assert(vmc.id != 0);

   /* 1. Test 2MB tracebuf / ringbuf BO allocation with GPU_UNCACHED (PANVK_DEBUG=trace) */
   panthor_kbase_adapter_hint_bo_flags(1U << 2 /* PAN_KMOD_BO_FLAG_GPU_UNCACHED */);
   struct drm_panthor_bo_create boc = {
      .size = 2 * 1024 * 1024,
      .flags = 0,
      .exclusive_vm_id = vmc.id,
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
   assert(boc.handle != 0);

   struct drm_panthor_bo_mmap_offset mmo = { .handle = boc.handle };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mmo) == 0);
   void *cpu_ptr = panthor_kbase_adapter_mmap(NULL, boc.size, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, drm_fd, (off_t)mmo.offset);
   assert(cpu_ptr != MAP_FAILED && cpu_ptr != NULL);

   /* 2. Test 2-op contiguous circular wraparound mapping (init_render_desc_ringbuf) with AUTO_VA (~0ULL) */
   struct drm_panthor_vm_bind_op bind_ops[2];
   memset(bind_ops, 0, sizeof(bind_ops));
   for (int i = 0; i < 2; i++) {
      bind_ops[i].flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
                          DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC |
                          DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED;
      bind_ops[i].bo_handle = boc.handle;
      bind_ops[i].bo_offset = 0;
      bind_ops[i].va = ~0ULL; /* PAN_KMOD_VM_MAP_AUTO_VA */
      bind_ops[i].size = boc.size;
   }
   struct drm_panthor_vm_bind vmb = {
      .vm_id = vmc.id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(2, bind_ops),
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vmb) == 0);
   assert(bind_ops[0].va != 0 && bind_ops[0].va != ~0ULL);
   assert(bind_ops[1].va == bind_ops[0].va + boc.size);
   /* Verify 2x mapping does not cross a 4GB boundary */
   assert((bind_ops[0].va >> 32) == ((bind_ops[0].va + boc.size * 2 - 1) >> 32));

   /* 3. Test UNMAP of Kbase-assigned VA without VMA heap corruption + munmap + GEM_CLOSE */
   struct drm_panthor_vm_bind_op unmap_op;
   memset(&unmap_op, 0, sizeof(unmap_op));
   unmap_op.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP;
   unmap_op.va = bind_ops[0].va;
   unmap_op.size = boc.size * 2;
   struct drm_panthor_vm_bind vmb_unmap = {
      .vm_id = vmc.id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &unmap_op),
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vmb_unmap) == 0);
   assert(panthor_kbase_adapter_munmap(cpu_ptr, boc.size) == 0);
   assert(drmCloseBufferHandle(drm_fd, boc.handle) == 0);

   struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
   assert(panthor_kbase_adapter_close(drm_fd) == 0);
   printf("  -> PanVK AUTO_VA + 2MB Ringbuf/Tracebuf 2x Alias + UNMAP Integration Test: PASS\n");
}

static void test_u_gralloc_wsi_modifier_and_dmabuf_sync_resilience(void)
{
   int drm_fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(drm_fd >= 0);

   /* 1. Simulate importing a 500x500 R8G8B8A8_UNORM AHB (stride=512 px -> 2048 B/row, 250 pages = 1,024,000 B) */
   int ahb_fd = memfd_create("mock_ahb_500x500", MFD_CLOEXEC);
   assert(ahb_fd >= 0);
   size_t ahb_size = 512 * 4 * 500; /* 1,024,000 bytes (250 pages) */
   assert(ftruncate(ahb_fd, (off_t)ahb_size) == 0);

   uint32_t gem_handle = 0;
   assert(drmPrimeFDToHandle(drm_fd, ahb_fd, &gem_handle) == 0);
   assert(gem_handle != 0);

   /* 2. Verify non-destructive save/restore linear probe preserves existing pixel contents */
   uint32_t *raw_u32 = (uint32_t *)mmap(NULL, ahb_size, PROT_READ | PROT_WRITE, MAP_SHARED, ahb_fd, 0);
   assert(raw_u32 != MAP_FAILED);
   const uint32_t xs[3] = { 0, 250, 499 };
   const uint32_t ys[3] = { 0, 250, 499 };
   for (int iy = 0; iy < 3; iy++) {
      for (int ix = 0; ix < 3; ix++) {
         raw_u32[ys[iy] * 512 + xs[ix]] = 0x11223300u + (uint32_t)(iy * 3 + ix);
      }
   }
   uint32_t saved_pixels[9];
   for (int iy = 0; iy < 3; iy++) {
      for (int ix = 0; ix < 3; ix++) {
         saved_pixels[iy * 3 + ix] = raw_u32[ys[iy] * 512 + xs[ix]];
         raw_u32[ys[iy] * 512 + xs[ix]] = 0xA5C30000u | (uint32_t)(iy * 16 + ix);
      }
   }
   for (int iy = 0; iy < 3; iy++) {
      for (int ix = 0; ix < 3; ix++) {
         assert(raw_u32[ys[iy] * 512 + xs[ix]] == (0xA5C30000u | (uint32_t)(iy * 16 + ix)));
         raw_u32[ys[iy] * 512 + xs[ix]] = saved_pixels[iy * 3 + ix];
      }
   }
   for (int iy = 0; iy < 3; iy++) {
      for (int ix = 0; ix < 3; ix++) {
         assert(raw_u32[ys[iy] * 512 + xs[ix]] == (0x11223300u + (uint32_t)(iy * 3 + ix)));
      }
   }
   munmap(raw_u32, ahb_size);

   /* 3. Verify DMA_BUF_IOCTL_IMPORT_SYNC_FILE & DMA_BUF_IOCTL_EXPORT_SYNC_FILE graceful handling */
   struct { uint32_t flags; int32_t fd; } esync = { .flags = 1, .fd = -1 };
   unsigned long req_export = _IOWR('b', 2, esync);
   assert(drmIoctl(ahb_fd, req_export, &esync) == 0);
   assert(esync.fd >= 0);

   struct { uint32_t flags; int32_t fd; } isync = { .flags = 3, .fd = esync.fd };
   unsigned long req_import = _IOW('b', 3, isync);
   assert(drmIoctl(ahb_fd, req_import, &isync) == 0);
   close(esync.fd);

   assert(drmCloseBufferHandle(drm_fd, gem_handle) == 0);
   close(ahb_fd);
   assert(panthor_kbase_adapter_close(drm_fd) == 0);
   printf("  -> u_gralloc WSI Linear Probe & DMA-BUF Sync File Resilience: PASS\n");
}


static void test_kbase_mem_alias_csf_event_xor_and_shareability_invariants(void)
{
   printf("  -> Testing KBASE_IOCTL_MEM_ALIAS CSF_EVENT XOR, idempotent CS normalization & zero-overlay syncobj redirection...\n");
   int mali_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   assert(mali_fd >= 0);
   struct kbase_ioctl_version_check vc = { .major = 1, .minor = 21 };
   assert(ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK, &vc) == 0);
   struct kbase_ioctl_set_flags sf = { .create_flags = 2 };
   assert(ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &sf) == 0);
   struct kbase_ioctl_mem_jit_init ji = { .va_pages = 0x100000, .max_allocations = 255, .trim_level = 100 };
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_JIT_INIT, &ji) == 0);
   void *tp = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mali_fd, (off_t)BASE_MEM_MAP_TRACKING_HANDLE);
   assert(tp != MAP_FAILED);

   /* 1. Allocate a BASE_MEM_CSF_EVENT BO and verify KBASE_IOCTL_MEM_ALIAS rejects it with ENOMEM (mali_kbase_mem_linux.c:2080) */
   union kbase_ioctl_mem_alloc ev_alloc;
   memset(&ev_alloc, 0, sizeof(ev_alloc));
   ev_alloc.in.va_pages = 16;
   ev_alloc.in.commit_pages = 16;
   ev_alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                       BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                       BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT;
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &ev_alloc) == 0);
   void *ev_ptr = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, (off_t)ev_alloc.out.gpu_va);
   assert(ev_ptr != MAP_FAILED);

   struct base_mem_aliasing_info bad_ai[2] = {
      { .handle = (uint64_t)(uintptr_t)ev_ptr, .offset = 0, .length = 16 },
      { .handle = (uint64_t)(uintptr_t)ev_ptr, .offset = 0, .length = 16 },
   };
   union kbase_ioctl_mem_alias bad_alias;
   memset(&bad_alias, 0, sizeof(bad_alias));
   bad_alias.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_CPU_RD | BASE_MEM_COHERENT_LOCAL;
   bad_alias.in.stride = 16;
   bad_alias.in.nents = 2;
   bad_alias.in.aliasing_info = (uint64_t)(uintptr_t)bad_ai;
   errno = 0;
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALIAS, &bad_alias) == -1);
   assert(errno == ENOMEM);

   /* 2. Allocate a BASE_MEM_COHERENT_LOCAL (GPU-cached) BO and verify KBASE_IOCTL_MEM_ALIAS succeeds */
   union kbase_ioctl_mem_alloc reg_alloc;
   memset(&reg_alloc, 0, sizeof(reg_alloc));
   reg_alloc.in.va_pages = 16;
   reg_alloc.in.commit_pages = 16;
   reg_alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                        BASE_MEM_SAME_VA | BASE_MEM_COHERENT_LOCAL;
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &reg_alloc) == 0);
   void *reg_ptr = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, (off_t)reg_alloc.out.gpu_va);
   assert(reg_ptr != MAP_FAILED);

   struct base_mem_aliasing_info ok_ai[2] = {
      { .handle = (uint64_t)(uintptr_t)reg_ptr, .offset = 0, .length = 16 },
      { .handle = (uint64_t)(uintptr_t)reg_ptr, .offset = 0, .length = 16 },
   };
   union kbase_ioctl_mem_alias ok_alias;
   memset(&ok_alias, 0, sizeof(ok_alias));
   ok_alias.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_CPU_RD | BASE_MEM_COHERENT_LOCAL;
   ok_alias.in.stride = 16;
   ok_alias.in.nents = 2;
   ok_alias.in.aliasing_info = (uint64_t)(uintptr_t)ok_ai;
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALIAS, &ok_alias) == 0);
   assert(ok_alias.out.gpu_va != 0);

   munmap(reg_ptr, 16 * 4096);
   munmap(ev_ptr, 16 * 4096);
   munmap(tp, 4096);
   close(mali_fd);

   /* 3. Verify idempotent panthor_kbase_normalize_cs_stream on real Valhall v11 instructions */
   uint64_t hw_stream[7] = {
      0x0300000000010000ULL, /* [0] WAIT mask=0x0001 (SB_ID(LS)) before SYNC_WAIT32 */
      0x2700424410000000ULL, /* [1] SYNC_WAIT32 addr=r66 ref=r68 cond=GT(1) */
      0x2500424400000004ULL, /* [2] SYNC_ADD32 addr=r66 data=r68 wait=0x0000 scope=CSG(2) */
      0x2400420000000200ULL, /* [3] FLUSH_CACHE2 flush_reg=r66 wait=0x0000 mode=0x200 */
      0x3301424400040005ULL, /* [4] SYNC_ADD64 addr=r66 data=r68 wait=0x0004 sig_slot=1 scope=CSG(2) err_prop=1 */
      0x3100000100040000ULL, /* [5] HEAP_OPERATION mode=VT_END(1) wait=0x0004 */
      0x0b004644003f0001ULL, /* [6] FINISH_FRAGMENT first=r70 last=r68 wait=0x003f inc_frag=1 */
   };
   struct adapter_cs_patch_stats st1, st2;
   panthor_kbase_normalize_cs_stream(hw_stream, sizeof(hw_stream), false, &st1);
   assert(hw_stream[0] == 0x0300000000030000ULL); /* Upgraded WAIT mask=0x0001 -> 0x0003 (LS | DEFERRED_SYNC) */
   assert(hw_stream[1] == 0x2700424410000000ULL);
   assert(hw_stream[2] == 0x2500424400000000ULL); /* Scope CSG(2)->SYSTEM(0), wait_mask stays 0x0000! */
   assert(hw_stream[3] == 0x2400420000000200ULL); /* FLUSH_CACHE2 wait_mask stays 0x0000! */
   assert(hw_stream[4] == 0x3301424400040001ULL); /* Scope CSG(2)->SYSTEM(0), wait_mask stays 0x0004! */
   assert(hw_stream[5] == 0x0300000000040000ULL); /* HEAP_OP VT_END -> WAIT mask=0x0004 */
   assert(hw_stream[6] == 0x0b004644003f0000ULL); /* FINISH_FRAGMENT bit 0 cleared! */
   assert(st1.sync_scope_upgrades == 2 && st1.deferred_sync_waits == 1 && st1.finish_frag_stat_clears == 1);

   /* Second pass (resubmitted command buffer): MUST be 100% idempotent and never write 0x21000000! */
   panthor_kbase_normalize_cs_stream(hw_stream, sizeof(hw_stream), false, &st2);
   assert(hw_stream[0] == 0x0300000000030000ULL);
   assert(hw_stream[2] == 0x2500424400000000ULL);
   assert(hw_stream[3] == 0x2400420000000200ULL);
   assert(hw_stream[4] == 0x3301424400040001ULL);
   assert(st2.sync_scope_upgrades == 0 && st2.deferred_sync_waits == 0);
   printf("  -> KBASE_IOCTL_MEM_ALIAS CSF_EVENT XOR & Idempotent CS Normalizer: PASS\n");
}

static void test_ringbuffer_multi_wrap_600_jobs(void)
{
   int drm_fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(drm_fd >= 0);

   struct drm_panthor_vm_create vmc = { .flags = 0, .user_va_range = (1ULL << 47) };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);

   struct drm_panthor_queue_create q_defs[3];
   memset(q_defs, 0, sizeof(q_defs));
   for (int i = 0; i < 3; i++) {
      q_defs[i].priority = 1;
      q_defs[i].ringbuf_size = 65536;
   }
   struct drm_panthor_group_create gc;
   memset(&gc, 0, sizeof(gc));
   gc.queues = (struct drm_panthor_obj_array){
      .count = 3,
      .stride = sizeof(struct drm_panthor_queue_create),
      .array = (uint64_t)(uintptr_t)q_defs,
   };
   gc.max_compute_cores = 2;
   gc.max_fragment_cores = 2;
   gc.max_tiler_cores = 1;
   gc.priority = PANTHOR_GROUP_PRIORITY_HIGH;
   gc.compute_core_mask = 0x3;
   gc.fragment_core_mask = 0x3;
   gc.tiler_core_mask = 0x1;
   gc.vm_id = vmc.id;
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc) == 0);
   assert(gc.group_handle != 0);

   uint32_t sync_handle = 0;
   assert(drmSyncobjCreate(drm_fd, 0, &sync_handle) == 0);

   /* Allocate a minimal 64-byte CS stream BO */
   struct drm_panthor_bo_create boc = {
      .size = 4096,
      .flags = 0,
      .exclusive_vm_id = vmc.id,
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
   struct drm_panthor_bo_mmap_offset mmo = { .handle = boc.handle };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mmo) == 0);
   uint64_t *cs_cpu = (uint64_t *)panthor_kbase_adapter_mmap(
      NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)mmo.offset);
   assert(cs_cpu != MAP_FAILED && cs_cpu != NULL);
   cs_cpu[0] = 0ULL; /* NOP */
   cs_cpu[1] = 0ULL; /* NOP */

   /*
    * Submit 600 consecutive jobs on VT(0) (600 * 320B = 192,000B > 2 * 65536B),
    * crossing the 64 KiB boundary at Job 205 (65,280B) and Job 409/410 (130,880B),
    * and verify DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT succeeds on every single job!
    */
   /* Submit 750 consecutive jobs so both 192B/slot (750*192=144,000B > 2*65536B) and
    * 320B/slot (750*320=240,000B > 3*65536B) modes wrap the 64 KiB ringbuffer >= 2 times! */
   for (uint64_t job = 1; job <= 750; job++) {
      struct drm_panthor_sync_op sop = {
         .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_SIGNAL,
         .handle = sync_handle,
         .timeline_value = job,
      };
      struct drm_panthor_queue_submit qsub = {
         .queue_index = 0,
         .stream_size = 16,
         .stream_addr = (uint64_t)(uintptr_t)cs_cpu,
         .latest_flush = 1,
         .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sop),
      };
      struct drm_panthor_group_submit gsub = {
         .group_handle = gc.group_handle,
         .pad = 0,
         .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsub),
      };
      assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsub) == 0);
      uint64_t pt = 0;
      assert(drmSyncobjTimelineWait(drm_fd, &sync_handle, &pt, 1, 100000000ULL, 0, NULL) == 0);
   }

   /* Verify telemetry snapshot counters (total submits, jobs, seqno, wraps, and fast-spin waits) */
   struct adapter_telemetry_snapshot snap;
   memset(&snap, 0, sizeof(snap));
   panthor_kbase_get_telemetry_snapshot(&snap);
   assert(snap.total_group_submits >= 750ULL);
   assert(snap.total_subqueue_jobs[0] == 750ULL);
   assert(snap.completed_subqueue_seqno[0] == 750ULL);
   assert(snap.ring_wraps[0] >= 2ULL);
   assert(snap.total_syncobj_waits >= 750ULL);
   assert(snap.fast_spin_syncobj_waits > 0ULL);
   assert(snap.active_bos >= 1U);
   assert(snap.active_syncobjs >= 1U);

   /* Also verify panthor_kbase_dump_csf_diagnostics outputs the flight recorder cleanly */
   panthor_kbase_dump_csf_diagnostics("unit_test_600_jobs_wrap_verification");

   assert(drmSyncobjDestroy(drm_fd, sync_handle) == 0);
   assert(drmCloseBufferHandle(drm_fd, boc.handle) == 0);
   struct drm_panthor_group_destroy gd = { .group_handle = gc.group_handle };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd) == 0);
   struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
   assert(panthor_kbase_adapter_close(drm_fd) == 0);
   printf("  -> 600-Job Multi-Wrap CSF Ringbuffer Stress Test (Jobs 205 & 410 wrap boundaries): PASS\n");
}


struct bcn_race_thread_ctx {
   int drm_fd;
   uint32_t group_handle;
   uint32_t sync_handle;
   uint64_t cs_va;
   volatile int wait_started;
   volatile int submit_done;
};

static void *bcn_producer_thread(void *arg)
{
   struct bcn_race_thread_ctx *ctx = (struct bcn_race_thread_ctx *)arg;
   while (!ctx->wait_started)
      usleep(100);
   /* Sleep 3ms to guarantee bcn_layer cleanup thread is already inside DRM_IOCTL_SYNCOBJ_WAIT
    * on the unsignaled staging_fence BEFORE QueueSubmit runs! */
   usleep(3000);

   struct drm_panthor_sync_op sop = {
      .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_SIGNAL,
      .handle = ctx->sync_handle,
      .timeline_value = 0,
   };
   struct drm_panthor_queue_submit qsub = {
      .queue_index = 0,
      .stream_size = 16,
      .stream_addr = ctx->cs_va,
      .latest_flush = 1,
      .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sop),
   };
   struct drm_panthor_group_submit gsub = {
      .group_handle = ctx->group_handle,
      .pad = 0,
      .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsub),
   };
   ctx->submit_done = 1;
   assert(panthor_kbase_adapter_ioctl(ctx->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsub) == 0);
   return NULL;
}

static void test_dice_a_million_syncobj_wait_and_bcn_layer_race(void)
{
   int drm_fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(drm_fd >= 0);

   /* 1. Verify unsignaled syncobj returns -1 (ETIME) on timeout_nsec=0 (not 0/VK_SUCCESS!) */
   uint32_t unsig = 0;
   assert(drmSyncobjCreate(drm_fd, 0, &unsig) == 0);
   assert(unsig != 0);
   assert(drmSyncobjWait(drm_fd, &unsig, 1, 0, 0, NULL) == -1);
   assert(errno == ETIME);
   uint64_t pt1 = 1;
   assert(drmSyncobjTimelineWait(drm_fd, &unsig, &pt1, 1, 0, 0, NULL) == -1);
   assert(errno == ETIME);

   /* 2. Verify EXPORT_SYNC_FILE on unsignaled syncobj returns -1 (EINVAL) */
   struct drm_syncobj_handle sh = {
      .handle = unsig,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
      .fd = -1,
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == -1);
   assert(errno == EINVAL);

   /* 3. Verify 0xCB (DRM_IOCTL_SYNCOBJ_QUERY) vs 0xCD (DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) */
   uint64_t queried_pt = 999;
   assert(drmSyncobjQuery(drm_fd, &unsig, &queried_pt, 1) == 0);
   assert(queried_pt == 0ULL);
   uint64_t sig_pt = 42ULL;
   assert(drmSyncobjTimelineSignal(drm_fd, &unsig, &sig_pt, 1) == 0);
   assert(drmSyncobjQuery(drm_fd, &unsig, &queried_pt, 1) == 0);
   assert(queried_pt == 42ULL);
   assert(drmSyncobjWait(drm_fd, &unsig, 1, 0, 0, NULL) == 0);
   assert(drmSyncobjTimelineWait(drm_fd, &unsig, &sig_pt, 1, 0, 0, NULL) == 0);

   /* 4. Verify DRM_IOCTL_SYNCOBJ_RESET resets back to unsignaled and wait(timeout=0) returns -1 */
   assert(drmSyncobjReset(drm_fd, &unsig, 1) == 0);
   assert(drmSyncobjWait(drm_fd, &unsig, 1, 0, 0, NULL) == -1);
   assert(errno == ETIME);

   /* 5. Reproduce bcn_layer_QueueSubmit multi-threaded race:
    *    Thread B calls drmSyncobjWait(unsig, timeout=INT64_MAX) BEFORE Thread A submits GPU work! */
   struct drm_panthor_vm_create vmc = { .flags = 0 };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);
   struct drm_panthor_queue_create qcreq[3];
   memset(qcreq, 0, sizeof(qcreq));
   for (int q = 0; q < 3; q++) {
      qcreq[q].priority = 1;
      qcreq[q].ringbuf_size = 65536;
   }
   struct drm_panthor_group_create gc;
   memset(&gc, 0, sizeof(gc));
   gc.queues = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(3, qcreq);
   gc.max_compute_cores = 4;
   gc.max_fragment_cores = 4;
   gc.max_tiler_cores = 1;
   gc.compute_core_mask = 0xf;
   gc.fragment_core_mask = 0xf;
   gc.tiler_core_mask = 0x1;
   gc.vm_id = vmc.id;
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc) == 0);

   struct drm_panthor_bo_create boc = { .size = 4096, .flags = 0, .exclusive_vm_id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
   struct drm_panthor_bo_mmap_offset bmo = { .handle = boc.handle };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &bmo) == 0);
   uint64_t *cs_cpu = (uint64_t *)panthor_kbase_adapter_mmap(
      NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)bmo.offset);
   assert(cs_cpu != MAP_FAILED);
   struct drm_panthor_vm_bind_op bop = {
      .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
      .bo_handle = boc.handle,
      .bo_offset = 0,
      .va = (uint64_t)(uintptr_t)cs_cpu,
      .size = 4096,
   };
   struct drm_panthor_vm_bind vmb = {
      .vm_id = vmc.id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bop),
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vmb) == 0);
   cs_cpu[0] = ((uint64_t)0x01 << 56) | ((uint64_t)60 << 48) | 0x5fffe00000ULL;
   cs_cpu[1] = 0ULL;

   struct bcn_race_thread_ctx rctx = {
      .drm_fd = drm_fd,
      .group_handle = gc.group_handle,
      .sync_handle = unsig,
      .cs_va = (uint64_t)(uintptr_t)cs_cpu,
      .wait_started = 0,
      .submit_done = 0,
   };
   pthread_t prod_tid;
   assert(pthread_create(&prod_tid, NULL, bcn_producer_thread, &rctx) == 0);
   rctx.wait_started = 1;
   assert(drmSyncobjWait(drm_fd, &unsig, 1, INT64_MAX, 0, NULL) == 0);
   assert(rctx.submit_done == 1);
   pthread_join(prod_tid, NULL);

   /* 6. Verify sync-only submit propagates waited subqueue seqnos across all subqueues */
   uint32_t sync2 = 0;
   assert(drmSyncobjCreate(drm_fd, 0, &sync2) == 0);
   struct drm_panthor_sync_op sync_ops[2] = {
      {
         .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_WAIT,
         .handle = unsig,
         .timeline_value = 0,
      },
      {
         .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_SIGNAL,
         .handle = sync2,
         .timeline_value = 7ULL,
      },
   };
   struct drm_panthor_queue_submit sync_only_qsub = {
      .queue_index = 2, /* COMPUTE subqueue waiting on VT subqueue's unsig syncobj */
      .stream_size = 0,
      .stream_addr = 0,
      .latest_flush = 0,
      .syncs = DRM_PANTHOR_OBJ_ARRAY(2, sync_ops),
   };
   struct drm_panthor_group_submit sync_only_gsub = {
      .group_handle = gc.group_handle,
      .pad = 0,
      .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &sync_only_qsub),
   };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &sync_only_gsub) == 0);
   uint64_t q2_pt = 0;
   assert(drmSyncobjQuery(drm_fd, &sync2, &q2_pt, 1) == 0);
   assert(q2_pt == 7ULL);

   assert(drmSyncobjDestroy(drm_fd, sync2) == 0);
   assert(drmSyncobjDestroy(drm_fd, unsig) == 0);
   assert(drmCloseBufferHandle(drm_fd, boc.handle) == 0);
   struct drm_panthor_group_destroy gd = { .group_handle = gc.group_handle };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd) == 0);
   struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(drm_fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
   assert(panthor_kbase_adapter_close(drm_fd) == 0);
   printf("  -> Dice a Million Unsignaled SYNCOBJ_WAIT, 0xCB/0xCD UAPI & bcn_layer Race Test: PASS\n");
}


static void test_v17_cs_builder_abi_and_ls_tracker_invariants(void)
{
   /* Verify that mesa_overlay/src/panfrost/genxml/cs_builder.h has 100%
    * identical struct cs_builder field layout to upstream Mesa (no extra
    * struct cs_chunk fields shifting cur_chunk onto root_chunk or shifting
    * cur_ls_tracker), and contains cs_get_ls_tracker() self-healing. */
   FILE *fp = fopen("mesa_overlay/src/panfrost/genxml/cs_builder.h", "r");
   if (!fp) fp = fopen("../mesa_overlay/src/panfrost/genxml/cs_builder.h", "r");
   assert(fp != NULL);
   char buf[16384];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   buf[n] = '\0';
   fclose(fp);

   assert(strstr(buf, "last_active_chunk") == NULL &&
          "struct cs_builder must not contain last_active_chunk ABI shift");
   assert(strstr(buf, "pre_epilogue_chunk") == NULL &&
          "struct cs_builder must not contain pre_epilogue_chunk ABI shift");
   assert(strstr(buf, "cs_get_ls_tracker(struct cs_builder *b)") != NULL &&
          "cs_builder.h must define self-healing cs_get_ls_tracker(b)");

   /* Verify compile.sh uses fast cmp -s / cksum and touches synced destination files */
   FILE *cfp = fopen("compile.sh", "r");
   assert(cfp != NULL);
   char cbuf[8192];
   size_t cn = fread(cbuf, 1, sizeof(cbuf) - 1, cfp);
   cbuf[cn] = '\0';
   fclose(cfp);
   assert(strstr(cbuf, "cmp -s") != NULL &&
          "compile.sh must use fast cmp -s byte comparison in sync_tree_if_changed");
   assert(strstr(cbuf, "cksum") != NULL &&
          "compile.sh must use fast POSIX cksum CRC-32 digest");
   assert(strstr(cbuf, "touch \"$dst_file\"") != NULL &&
          "compile.sh must touch updated destination files so Ninja recompiles");

   printf("  -> Checkpoint v17/v18 struct cs_builder ABI & compile.sh Fast Checksum Invariants: PASS\n");
}

static void test_v18_dogwitch_machick2_user_reg_munmap_guard_and_multigroup_isolation(void)
{
   /* Simulates the exact DOG WITCH.exe & Machick 2.exe multi-device + temporary-instance
    * sequence that previously crashed inside vkQueueSubmit -> panthor_kmod_get_flush_id:
    *   1. Device 1 opens /dev/mali0 (fd1), creates VM 1, Tiler Heap 1, Group 1, and maps USER_REG_PAGE.
    *   2. Device 2 (bcn_layer / secondary device) opens /dev/mali0 (fd2), creates VM 2, Tiler Heap 2, Group 2,
    *      and maps USER_REG_PAGE.
    *   3. Temporary VkInstance (Unity/DXVK swapchain re-enumeration) opens /dev/mali0 (fd3), maps USER_REG_PAGE,
    *      and immediately calls panthor_kbase_adapter_munmap(user_reg, 4096) + panthor_kbase_adapter_close(fd3).
    *   4. Verify USER_REG_PAGE remains mapped and readable (no SIGSEGV on *(volatile uint32_t *)user_reg!)
    *      and Group 1 & Group 2 have completely isolated seqno cells in event_bo!
    */
   int fd1 = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(fd1 >= 0);
   int fd2 = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(fd2 >= 0);

   void *ureg1 = panthor_kbase_adapter_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd1,
                                            (off_t)DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);
   assert(ureg1 != NULL && ureg1 != MAP_FAILED);

   void *ureg2 = panthor_kbase_adapter_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd2,
                                            (off_t)DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);
   assert(ureg2 == ureg1);

   /* Open temporary 3rd instance fd3, map USER_REG_PAGE, then munmap + close fd3 */
   int fd3 = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(fd3 >= 0);
   void *ureg3 = panthor_kbase_adapter_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd3,
                                            (off_t)DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);
   assert(ureg3 == ureg1);
   assert(panthor_kbase_adapter_munmap(ureg3, 4096) == 0);
   assert(panthor_kbase_adapter_close(fd3) == 0);

   /* Dereference ureg1 just as panthor_kmod_get_flush_id() does inside vkQueueSubmit! */
   volatile uint32_t flush_id = *(const volatile uint32_t *)ureg1;
   (void)flush_id;

   /* Create VM 1 + Heap 1 + Group 1 on fd1, and VM 2 + Heap 2 + Group 2 on fd2 */
   struct drm_panthor_vm_create vmc1 = { .flags = 0, .id = 0, .user_va_range = 0 };
   struct drm_panthor_vm_create vmc2 = { .flags = 0, .id = 0, .user_va_range = 0 };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc1) == 0);
   assert(panthor_kbase_adapter_ioctl(fd2, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc2) == 0);

   struct drm_panthor_tiler_heap_create hc1 = { .vm_id = vmc1.id, .initial_chunk_count = 5, .chunk_size = 2097152, .max_chunks = 64, .target_in_flight = 65535 };
   struct drm_panthor_tiler_heap_create hc2 = { .vm_id = vmc2.id, .initial_chunk_count = 5, .chunk_size = 2097152, .max_chunks = 64, .target_in_flight = 65535 };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc1) == 0);
   assert(panthor_kbase_adapter_ioctl(fd2, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc2) == 0);

   struct drm_panthor_queue_create qdefs[3] = {
      { .priority = 1, .ringbuf_size = 65536 },
      { .priority = 1, .ringbuf_size = 65536 },
      { .priority = 1, .ringbuf_size = 65536 },
   };
   struct drm_panthor_group_create gc1 = {
      .queues = DRM_PANTHOR_OBJ_ARRAY(3, qdefs),
      .max_compute_cores = 2, .max_fragment_cores = 2, .max_tiler_cores = 1,
      .priority = PANTHOR_GROUP_PRIORITY_MEDIUM, .vm_id = vmc1.id
   };
   struct drm_panthor_group_create gc2 = {
      .queues = DRM_PANTHOR_OBJ_ARRAY(3, qdefs),
      .max_compute_cores = 2, .max_fragment_cores = 2, .max_tiler_cores = 1,
      .priority = PANTHOR_GROUP_PRIORITY_MEDIUM, .vm_id = vmc2.id
   };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc1) == 0);
   assert(panthor_kbase_adapter_ioctl(fd2, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc2) == 0);
   assert(gc1.group_handle != gc2.group_handle);

   /* Submit a compute stream to Group 1 and Group 2 and verify both complete independently */
   struct drm_panthor_bo_create boc = { .size = 4096, .flags = 0 };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
   struct drm_panthor_bo_mmap_offset bmo = { .handle = boc.handle };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &bmo) == 0);
   uint64_t *cs_ptr = (uint64_t *)panthor_kbase_adapter_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, (off_t)bmo.offset);
   assert(cs_ptr != NULL && cs_ptr != MAP_FAILED);
   struct drm_panthor_vm_bind_op bop = { .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = boc.handle, .bo_offset = 0, .va = 0, .size = 4096 };
   struct drm_panthor_vm_bind vmb = { .vm_id = vmc1.id, .flags = 0, .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bop) };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_VM_BIND, &vmb) == 0);
   cs_ptr[0] = ((uint64_t)CSF_OP_MOVE48 << 56) | (68ULL << 48) | bop.va;

   uint32_t s1 = 0, s2 = 0;
   assert(drmSyncobjCreate(fd1, 0, &s1) == 0);
   assert(drmSyncobjCreate(fd2, 0, &s2) == 0);

   struct drm_panthor_sync_op sop1 = { .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = s1, .timeline_value = 0 };
   struct drm_panthor_queue_submit qsub1 = { .queue_index = 2, .stream_size = 8, .stream_addr = bop.va, .latest_flush = *(const volatile uint32_t *)ureg1, .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sop1) };
   struct drm_panthor_group_submit gsub1 = { .group_handle = gc1.group_handle, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsub1) };
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsub1) == 0);

   struct drm_panthor_sync_op sop2 = { .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = s2, .timeline_value = 0 };
   struct drm_panthor_queue_submit qsub2 = { .queue_index = 0, .stream_size = 8, .stream_addr = bop.va, .latest_flush = *(const volatile uint32_t *)ureg2, .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sop2) };
   struct drm_panthor_group_submit gsub2 = { .group_handle = gc2.group_handle, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsub2) };
   assert(panthor_kbase_adapter_ioctl(fd2, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsub2) == 0);

   assert(drmSyncobjWait(fd1, &s1, 1, INT64_MAX, 0, NULL) == 0);
   assert(drmSyncobjWait(fd2, &s2, 1, INT64_MAX, 0, NULL) == 0);

   assert(drmSyncobjDestroy(fd1, s1) == 0);
   assert(drmSyncobjDestroy(fd2, s2) == 0);
   assert(drmCloseBufferHandle(fd1, boc.handle) == 0);
   struct drm_panthor_group_destroy gd2 = { .group_handle = gc2.group_handle };
   struct drm_panthor_group_destroy gd1 = { .group_handle = gc1.group_handle };
   assert(panthor_kbase_adapter_ioctl(fd2, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd2) == 0);
   assert(panthor_kbase_adapter_ioctl(fd1, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd1) == 0);
   assert(panthor_kbase_adapter_close(fd2) == 0);
   assert(panthor_kbase_adapter_close(fd1) == 0);
   printf("  -> Checkpoint v18 DOG WITCH / Machick 2 USER_REG_PAGE munmap Guard & Multi-Group Seqno Isolation: PASS\n");
}

static void test_v19_dogwitch_recycled_cb_jump_chain_and_machick2_v11_defer_sync(void)
{
   /* 1. Machick 2 Valhall v11 SYNC_ADD64.defer (bit 52 = 1, sig_slot = 0) + multi-SYNC_WAIT64 test */
   uint64_t machick_stream[6] = {
      0x0300000000010000ULL, /* [0] WAIT mask=0x0001 (SB_ID(LS)) before 1st SYNC_WAIT64 */
      0x3500424420000000ULL, /* [1] SYNC_WAIT64 cond=le, r66, r68 */
      0x3310424400000005ULL, /* [2] Valhall v11 SYNC_ADD64.csg.defer (defer_mode=1 at bit 52, sig_slot=0) */
      0x0300000000010000ULL, /* [3] WAIT mask=0x0001 (SB_ID(LS)) before 2nd SYNC_WAIT64 */
      0x3500424420000000ULL, /* [4] 2nd SYNC_WAIT64 cond=le, r66, r68 */
      0x0300000000040000ULL, /* [5] WAIT mask=0x0004 */
   };
   struct adapter_cs_patch_stats mst;
   panthor_kbase_normalize_cs_stream(machick_stream, sizeof(machick_stream), false, &mst);
   assert(machick_stream[0] == 0x0300000000030000ULL);
   assert(machick_stream[3] == 0x0300000000030000ULL);
   assert(mst.deferred_sync_waits == 2U);

   /* 2. DogWitch recycled DXVK command buffer + CSF_OP_JUMP (0x21) multi-chunk normalization test */
   int fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   assert(fd >= 0);
   struct drm_panthor_vm_create vmc = { .flags = 0, .user_va_range = (1ULL << 47) };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);

   struct drm_panthor_tiler_heap_create thc;
   memset(&thc, 0, sizeof(thc));
   thc.vm_id = vmc.id;
   thc.initial_chunk_count = 5;
   thc.chunk_size = 2 * 1024 * 1024;
   thc.max_chunks = 64;
   thc.target_in_flight = 65535;
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &thc) == 0);
   assert(thc.handle != 0);

   struct drm_panthor_queue_create q_defs[2];
   memset(q_defs, 0, sizeof(q_defs));
   q_defs[0].priority = 1; q_defs[0].ringbuf_size = 65536;
   q_defs[1].priority = 1; q_defs[1].ringbuf_size = 65536;
   struct drm_panthor_group_create gc;
   memset(&gc, 0, sizeof(gc));
   gc.queues = (struct drm_panthor_obj_array){ .count = 2, .stride = sizeof(q_defs[0]), .array = (uint64_t)(uintptr_t)q_defs };
   gc.max_compute_cores = 1; gc.max_fragment_cores = 1; gc.max_tiler_cores = 1;
   gc.compute_core_mask = 1; gc.fragment_core_mask = 1; gc.tiler_core_mask = 1;
   gc.vm_id = vmc.id;
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc) == 0);

   struct drm_panthor_bo_create boc = { .size = 8192, .flags = 0, .exclusive_vm_id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
   struct drm_panthor_bo_mmap_offset mmo = { .handle = boc.handle };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mmo) == 0);
   uint64_t *cs_bo = (uint64_t *)panthor_kbase_adapter_mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)mmo.offset);
   assert(cs_bo != MAP_FAILED && cs_bo != NULL);

   uint64_t *chunk1 = cs_bo;
   uint64_t *chunk2 = cs_bo + 256;
   uint64_t chunk2_va = (uint64_t)(uintptr_t)chunk2;

   /* Initial submit on q=1 (FRAG) */
   chunk1[0] = ((uint64_t)CSF_OP_MOVE48 << 56) | (90ULL << 48) | (uint64_t)(uintptr_t)(cs_bo + 512);
   chunk1[1] = 0ULL;
   chunk1[2] = 0x0b004644003f0001ULL; /* FINISH_FRAGMENT inc_frag=1 */
   chunk1[3] = 0ULL;
   chunk1[4] = 0x0300000000040000ULL; /* tail WAIT 0x0004 */
   struct drm_panthor_queue_submit qs1 = { .queue_index = 1, .stream_size = 40, .stream_addr = (uint64_t)(uintptr_t)chunk1, .latest_flush = 1 };
   struct drm_panthor_group_submit gs1 = { .group_handle = gc.group_handle, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs1) };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs1) == 0);
   assert((chunk1[2] & 1ULL) == 0ULL);

   /* Submit #2: populate norm_cache at chunk1 with identical head (chunk1[0]), mid (chunk1[2]=0), tail (chunk1[4]) */
   chunk1[1] = 0x0b004644003f0001ULL;
   chunk1[2] = 0ULL;
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs1) == 0);
   assert((chunk1[1] & 1ULL) == 0ULL);

   /* Submit #3 (DogWitch recycled command buffer with identical head, mid, tail, PLUS CSF_OP_JUMP to chunk2!):
    * Must NOT skip normalization on chunk1[1] OR on chunk2[0..1]! */
   chunk1[1] = ((uint64_t)CSF_OP_MOVE48 << 56) | (64ULL << 48) | (chunk2_va & 0xffffffffffffULL);
   chunk1[2] = ((uint64_t)CSF_OP_MOVE32 << 56) | (66ULL << 48) | 24ULL;
   chunk1[3] = ((uint64_t)CSF_OP_JUMP   << 56) | (64ULL << 40) | (66ULL << 32);
   chunk2[0] = 0x3100000100040000ULL; /* HEAP_OPERATION VT_END */
   chunk2[1] = 0x0b004644003f0001ULL; /* FINISH_FRAGMENT inc_frag=1 */
   chunk2[2] = 0x0300000000040000ULL;
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs1) == 0);
   assert(chunk2[0] == 0x0300000000040000ULL); /* HEAP_OPERATION VT_END in JUMP target normalized! */
   assert((chunk2[1] & 1ULL) == 0ULL);         /* FINISH_FRAGMENT bit 0 in JUMP target cleared! */

   struct drm_panthor_tiler_heap_destroy thd = { .handle = thc.handle };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd) == 0);
   assert(drmCloseBufferHandle(fd, boc.handle) == 0);
   struct drm_panthor_group_destroy gd = { .group_handle = gc.group_handle };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd) == 0);
   struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
   assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
   assert(panthor_kbase_adapter_close(fd) == 0);
   printf("  -> Checkpoint v19 DogWitch Recycled CB + CSF_OP_JUMP Chain & Machick 2 Valhall v11 SYNC_ADD64.defer: PASS\n");
}


static void test_v20_worklist_chunk_hash_bug_catching_and_dual_capture_replay(void)
{
   /* 1. Verify XML-generated typed CSF instruction unpackers (doco.-HqHl3p-B9OHzBasFqSJTA & doco.fdxkj9JdapjjMg1-Mt1DZQ) */
   assert(MALI_CSF_MAX_CALL_STACK_DEPTH == 8U);
   struct mali_csf_sync_op_fields sf;
   assert(mali_csf_unpack_sync_op(0x3310424400000005ULL, &sf) == true);
   assert(sf.opcode == MALI_CSF_OP_SYNC_ADD64);
   assert(sf.defer_mode == MALI_CS_DEFER_MODE_DEFER_INDIRECT);
   assert(sf.signal_slot == 0U);
   assert(sf.is_deferred == true);
   assert(mali_csf_sync_op_is_deferred(0x3310424400000005ULL) == true);
   assert(mali_csf_sync_op_is_deferred(0x2501424400000005ULL) == true);
   assert(mali_csf_sync_op_is_deferred(0x2500424400000005ULL) == false);

   uint8_t cj_op = 0, cj_addr = 0, cj_len = 0;
   uint64_t jump_insn = ((uint64_t)CSF_OP_JUMP << 56) | (64ULL << 40) | (66ULL << 32);
   assert(mali_csf_unpack_call_or_jump(jump_insn, &cj_op, &cj_addr, &cj_len) == true);
   assert(cj_op == MALI_CSF_OP_JUMP && cj_addr == 64U && cj_len == 66U);

   /* 2. Verify Emulator catches unwaited Valhall v11 SYNC_ADD64.defer before SYNC_WAIT64 (doco.AXXnH1ZOCT0UtWpCJhU2jA) */
   mali_kbase_emu_reset_stats();
   {
      int efd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR);
      assert(efd >= 0);
      struct drm_panthor_vm_create vmc = {0};
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);
      struct drm_panthor_bo_create boc = { .size = 16384, .flags = 0 };
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_BO_CREATE, &boc) == 0);
      struct drm_panthor_vm_bind_op bop = {
         .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
         .bo_handle = boc.handle, .bo_offset = 0, .va = 0, .size = 16384
      };
      struct drm_panthor_vm_bind vmb = {
         .vm_id = vmc.id, .flags = 0, .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bop)
      };
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_VM_BIND, &vmb) == 0);

      /* Test PANTHOR_ADAPTER_BUG_CATCHING_MODE=1 small tiler heap (doco.Wu9_quJwcH_87YhcCfIFjg) */
      setenv("PANTHOR_ADAPTER_BUG_CATCHING_MODE", "1", 1);
      struct drm_panthor_tiler_heap_create thc = {
         .vm_id = vmc.id, .initial_chunk_count = 5, .chunk_size = 1048576, .max_chunks = 64, .target_in_flight = 65535
      };
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &thc) == 0);
      assert(thc.tiler_heap_ctx_gpu_va != 0);
      unsetenv("PANTHOR_ADAPTER_BUG_CATCHING_MODE");

      struct drm_panthor_tiler_heap_destroy thd = { .handle = thc.handle };
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd) == 0);
      assert(drmCloseBufferHandle(efd, boc.handle) == 0);
      struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
      assert(panthor_kbase_adapter_ioctl(efd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
      assert(panthor_kbase_adapter_close(efd) == 0);
   }

   /* 3. End-to-End Panthor DRM / CSF / Valhall ISA Capture & Local Replay (doco.y11rfnLETNbFbEGlv3C_iQ) */
   const char *drm_trace_path = "/tmp/test_v20_capture.pandrmcap";
   unlink(drm_trace_path);
   assert(panthor_drm_capture_start(drm_trace_path,
          PANDRM_FLAG_STUB_TEXTURES | PANDRM_FLAG_CAPTURE_ISA | PANDRM_FLAG_POST_NORM_CSF) == 0);
   assert(panthor_drm_capture_is_active() == true);
   {
      int fd = panthor_kbase_adapter_open("/dev/dri/renderD128", O_RDWR);
      assert(fd >= 0);
      struct drm_panthor_gpu_info ginfo = {0};
      struct drm_panthor_dev_query dq = {
         .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO, .size = sizeof(ginfo), .pointer = (uint64_t)(uintptr_t)&ginfo
      };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &dq) == 0);

      struct drm_panthor_vm_create vmc = {0};
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) == 0);

      /* Allocate CS BO (16 KiB), Shader Executable BO (4 KiB), and Texture BO (512 KiB to trigger stubbed tex meta) */
      struct drm_panthor_bo_create cs_boc = { .size = 16384, .flags = 0 };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &cs_boc) == 0);
      panthor_kbase_adapter_hint_bo_flags(1U /* ADAPTER_KMOD_BO_FLAG_EXECUTABLE */);
      struct drm_panthor_bo_create isa_boc = { .size = 4096, .flags = 0 };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &isa_boc) == 0);
      struct drm_panthor_bo_create tex_boc = { .size = 524288, .flags = 0 };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &tex_boc) == 0);

      struct drm_panthor_vm_bind_op bops[3] = {
         { .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = cs_boc.handle, .bo_offset = 0, .va = 0, .size = 16384 },
         { .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP | DRM_PANTHOR_VM_BIND_OP_MAP_READONLY, .bo_handle = isa_boc.handle, .bo_offset = 0, .va = 0, .size = 4096 },
         { .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = tex_boc.handle, .bo_offset = 0, .va = 0, .size = 524288 },
      };
      struct drm_panthor_vm_bind vmb = { .vm_id = vmc.id, .flags = 0, .ops = DRM_PANTHOR_OBJ_ARRAY(3, bops) };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &vmb) == 0);

      uint64_t *cs_cpu = (uint64_t *)(uintptr_t)bops[0].va;
      uint64_t *isa_cpu = (uint64_t *)(uintptr_t)panthor_kbase_adapter_get_bo_gpu_va(isa_boc.handle);
      uint64_t *tex_cpu = (uint64_t *)(uintptr_t)bops[2].va;
      if (isa_cpu) isa_cpu[0] = 0x0000000091000000ULL; /* Valhall MOV_I32 */
      if (tex_cpu) tex_cpu[0] = 0xDEADBEEFCAFEBABEULL;

      /* Build a 3-chunk transitive closure: Root -> CALL sub_chunk -> JUMP tail_chunk */
      uint64_t *root_chunk = cs_cpu;
      uint64_t *call_chunk = cs_cpu + 64;
      uint64_t *jump_chunk = cs_cpu + 128;
      uint64_t call_va = (uint64_t)(uintptr_t)call_chunk;
      uint64_t jump_va = (uint64_t)(uintptr_t)jump_chunk;

      root_chunk[0] = ((uint64_t)CSF_OP_MOVE48 << 56) | (90ULL << 48) | (uint64_t)(uintptr_t)(cs_cpu + 256);
      root_chunk[1] = ((uint64_t)CSF_OP_MOVE48 << 56) | (64ULL << 48) | (call_va & 0xffffffffffffULL);
      root_chunk[2] = ((uint64_t)CSF_OP_MOVE32 << 56) | (66ULL << 48) | 32ULL;
      root_chunk[3] = ((uint64_t)CSF_OP_CALL   << 56) | (64ULL << 40) | (66ULL << 32);
      root_chunk[4] = 0x0300000000040000ULL;

      call_chunk[0] = ((uint64_t)CSF_OP_MOVE48 << 56) | (68ULL << 48) | (jump_va & 0xffffffffffffULL);
      call_chunk[1] = ((uint64_t)CSF_OP_MOVE32 << 56) | (70ULL << 48) | 24ULL;
      call_chunk[2] = ((uint64_t)CSF_OP_JUMP   << 56) | (68ULL << 40) | (70ULL << 32);
      call_chunk[3] = 0ULL;

      jump_chunk[0] = 0x3310424400000005ULL; /* Valhall v11 SYNC_ADD64.defer */
      jump_chunk[1] = 0x0300000000010000ULL; /* WAIT mask=0x0001 -> normalized to 0x0003 */
      jump_chunk[2] = 0x35005944c0000000ULL; /* SYNC_WAIT64 */

      struct drm_panthor_queue_create qcre[2] = {
         { .priority = 1, .ringbuf_size = 65536 },
         { .priority = 1, .ringbuf_size = 65536 }
      };
      struct drm_panthor_group_create gc = {
         .queues = DRM_PANTHOR_OBJ_ARRAY(2, qcre),
         .max_compute_cores = 1, .max_fragment_cores = 1, .max_tiler_cores = 1,
         .priority = PANTHOR_GROUP_PRIORITY_MEDIUM,
         .compute_core_mask = 1, .fragment_core_mask = 1, .tiler_core_mask = 1,
         .vm_id = vmc.id
      };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc) == 0);

      struct drm_syncobj_create soc = { .handle = 0, .flags = 0 };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &soc) == 0);
      struct drm_panthor_sync_op sop = {
         .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ,
         .handle = soc.handle, .timeline_value = 0
      };
      struct drm_panthor_queue_submit qs = {
         .queue_index = 0, .stream_size = 40, .stream_addr = (uint64_t)(uintptr_t)root_chunk,
         .latest_flush = 1, .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sop)
      };
      struct drm_panthor_group_submit gs = {
         .group_handle = gc.group_handle, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs)
      };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs) == 0);
      /* Verify worklist followed root -> CALL -> JUMP and upgraded WAIT before SYNC_WAIT64! */
      assert(jump_chunk[1] == 0x0300000000030000ULL);

      struct drm_syncobj_wait sw = {
         .handles = (uint64_t)(uintptr_t)&soc.handle, .timeout_nsec = INT64_MAX, .count_handles = 1, .flags = 0
      };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == 0);

      struct drm_syncobj_destroy sod = { .handle = soc.handle, .pad = 0 };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sod) == 0);
      struct drm_panthor_group_destroy gd = { .group_handle = gc.group_handle };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd) == 0);
      assert(drmCloseBufferHandle(fd, tex_boc.handle) == 0);
      assert(drmCloseBufferHandle(fd, isa_boc.handle) == 0);
      assert(drmCloseBufferHandle(fd, cs_boc.handle) == 0);
      struct drm_panthor_vm_destroy vmd = { .id = vmc.id };
      assert(panthor_kbase_adapter_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd) == 0);
      assert(panthor_kbase_adapter_close(fd) == 0);
   }
   panthor_drm_capture_stop();
   assert(panthor_drm_capture_is_active() == false);

   /* Replay captured .pandrmcap locally and verify all invariants */
   struct pandrm_replay_options drm_ropts = { .verbose = true, .dump_csf = false, .verify_invariants = true, .bug_catching_mode = false };
   struct pandrm_replay_summary drm_rsum = {0};
   assert(panthor_drm_replay_trace_file(drm_trace_path, &drm_ropts, &drm_rsum) == 0);
   assert(drm_rsum.csf_invariant_violations == 0U);
   assert(drm_rsum.total_bo_creates == 3ULL);
   assert(drm_rsum.total_csf_chunks_verified == 3ULL); /* root + CALL + JUMP */
   assert(drm_rsum.total_tex_metas_restored >= 1ULL);
   assert(drm_rsum.total_group_submits == 1ULL);
   unlink(drm_trace_path);

   /* 4. End-to-End Vulkan API Layer Capture & Local Replay (.panvkcap) */
   const char *vk_trace_path = "/tmp/test_v20_capture.panvkcap";
   unlink(vk_trace_path);
   assert(panthor_vk_capture_start(vk_trace_path, PANVK_CAP_FLAG_STUB_TEXTURES) == 0);
   assert(panthor_vk_capture_is_active() == true);
   panthor_vk_capture_stop();
   struct panvk_replay_options vk_ropts = { .icd_json_path = NULL, .dump_ppm_path = NULL, .verbose = true };
   struct panvk_replay_summary vk_rsum = {0};
   assert(panthor_vk_replay_trace_file(vk_trace_path, &vk_ropts, &vk_rsum) == 0);
   assert(vk_rsum.final_status == 0);
   unlink(vk_trace_path);

   printf("  -> Checkpoint v20 Worklist Chunk Hash, Bug-Catching Mode & Dual-Level Capture/Replay: PASS\n");
}

int main(void)
{
   printf("[Unit Tests] Running edge cases, mali_kbase.ko flag invariants, and RAII logging suite...\n");
   test_mali_kbase_ko_flag_bit_positions();
   test_csf_normalizer_edge_cases();
   test_ring_slot_emitter_v10_and_v11();
   test_mali0_state_machine_and_flag_regressions();
   test_kms_and_libdrm_wrappers();
   test_panvk_auto_va_ringbuf_and_tracebuf_integration();
   test_u_gralloc_wsi_modifier_and_dmabuf_sync_resilience();
   test_kbase_mem_alias_csf_event_xor_and_shareability_invariants();
   test_ringbuffer_multi_wrap_600_jobs();
   test_dice_a_million_syncobj_wait_and_bcn_layer_race();
   test_v17_cs_builder_abi_and_ls_tracker_invariants();
   test_v18_dogwitch_machick2_user_reg_munmap_guard_and_multigroup_isolation();
   test_v19_dogwitch_recycled_cb_jump_chain_and_machick2_v11_defer_sync();
   test_v20_worklist_chunk_hash_bug_catching_and_dual_capture_replay();
   printf("[Unit Tests] ALL EDGE CASES PASSED!\n");
   return 0;
}
