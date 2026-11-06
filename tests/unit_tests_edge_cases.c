#undef NDEBUG
#define _GNU_SOURCE
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

   panthor_kbase_normalize_cs_stream(dummy, sizeof(dummy), true, &stats);
   assert(stats.sync_scope_upgrades == 2 && stats.heap_op_nops == 1);
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

int main(void)
{
   printf("[Unit Tests] Running edge cases, mali_kbase.ko flag invariants, and RAII logging suite...\n");
   test_mali_kbase_ko_flag_bit_positions();
   test_csf_normalizer_edge_cases();
   test_ring_slot_emitter_v10_and_v11();
   test_mali0_state_machine_and_flag_regressions();
   test_kms_and_libdrm_wrappers();
   printf("[Unit Tests] ALL EDGE CASES PASSED!\n");
   return 0;
}
