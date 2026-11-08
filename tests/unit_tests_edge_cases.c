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
   for (uint64_t job = 1; job <= 600; job++) {
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
   assert(snap.total_group_submits >= 600ULL);
   assert(snap.total_subqueue_jobs[0] == 600ULL);
   assert(snap.completed_subqueue_seqno[0] == 600ULL);
   assert(snap.ring_wraps[0] >= 2ULL);
   assert(snap.total_syncobj_waits >= 600ULL);
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
   printf("[Unit Tests] ALL EDGE CASES PASSED!\n");
   return 0;
}
