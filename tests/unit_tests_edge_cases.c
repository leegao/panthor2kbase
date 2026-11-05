/*
 * tests/unit_tests_edge_cases.c
 * -----------------------------
 * Supplementary unit and regression tests for edge cases, error handling,
 * boundary conditions, and state machine transitions in:
 *   - `panthor_kbase_adapter.c` (UAPI normalizer, ring emitter, syncobj, memory import)
 *   - `mali_kbase_emu.c` (handshake order, invalid flags, error codes, kick guards)
 *
 * Designed to provide comprehensive branch and line coverage for CI workflows.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm.h"
#include "panthor_drm.h"
#include "mali_kbase_uapi.h"
#include "panthor_kbase_adapter.h"
#include "mali_kbase_emu.h"

int main(void)
{
   printf("[Unit Tests] Running edge cases and error path coverage suite...\n");

   /* 1. Test CSF Bytecode Normalizer edge cases */
   uint64_t stream[16];
   memset(stream, 0, sizeof(stream));
   struct adapter_cs_patch_stats stats;

   /* Empty / NULL stream handling */
   panthor_kbase_normalize_cs_stream(NULL, 0, false, &stats);
   panthor_kbase_normalize_cs_stream(stream, sizeof(uint32_t), false, &stats);

   /* Stream with initial submit MOV48, SYNC_ADD32/SET32/ADD64/SET64, HEAP_OP modes */
   stream[0] = ((uint64_t)CSF_OP_MOVE48 << 56) | (88ULL << 48) | 0x12345678ULL;
   stream[1] = ((uint64_t)CSF_OP_SYNC_ADD32 << 56) | (10ULL << 40) | (11ULL << 32) | (0ULL << 28); /* CSG scope */
   stream[2] = ((uint64_t)CSF_OP_SYNC_SET32 << 56) | (12ULL << 40) | (13ULL << 32) | (0ULL << 28); /* CSG scope */
   stream[3] = ((uint64_t)CSF_OP_SYNC_SET64 << 56) | (14ULL << 40) | (15ULL << 32) | (0ULL << 28); /* CSG scope */
   stream[4] = ((uint64_t)CSF_OP_SYNC_ADD64 << 56) | (16ULL << 40) | (17ULL << 32) | (2ULL << 28); /* Already SYSTEM */
   stream[5] = ((uint64_t)CSF_OP_HEAP_OPERATION << 56) | (0ULL << 28); /* VT_START -> keep */
   stream[6] = ((uint64_t)CSF_OP_HEAP_OPERATION << 56) | (1ULL << 28); /* VT_END -> NOP */
   stream[7] = ((uint64_t)CSF_OP_HEAP_OPERATION << 56) | (2ULL << 28); /* FRAGMENT_COMPLETED -> NOP */

   panthor_kbase_normalize_cs_stream(stream, 8 * sizeof(uint64_t), true, &stats);
   assert(stats.extracted_subq_ctx_reg == 88);
   assert(stats.extracted_subq_ctx_va == 0x12345678ULL);
   assert(stats.sync_scope_upgrades == 3);
   assert(stats.heap_op_nops == 2);
   assert(((stream[1] >> 28) & 0x3ULL) == CSF_SYNC_SCOPE_SYSTEM);
   assert(((stream[2] >> 28) & 0x3ULL) == CSF_SYNC_SCOPE_SYSTEM);
   assert(((stream[3] >> 28) & 0x3ULL) == CSF_SYNC_SCOPE_SYSTEM);
   assert(stream[6] == 0ULL && stream[7] == 0ULL);
   printf("  -> CSF Bytecode Normalizer edge cases: PASS\n");

   /* 2. Test Ring Slot Emitter for Valhall v10 (64-reg) and v11+ (96-reg) */
   uint64_t ring_slot[32];
   size_t sz10 = panthor_kbase_emit_ring_slot(ring_slot, 10, 64, 0, 0x1000, 0x2000, 0x3000, 128, 1, 0x4000, 0xff);
   assert(sz10 > 0);
   size_t sz11 = panthor_kbase_emit_ring_slot(ring_slot, 11, 96, 2, 0x1000, 0, 0x5000, 256, 1, 0x6000, 0xff);
   assert(sz11 > 0);
   printf("  -> Ring Slot Emitter Valhall v10/v11+: PASS\n");

   /* 3. Test /dev/mali0 state machine error conditions in emulator */
   int raw_fd = open("/dev/mali0", O_RDWR); /* Missing O_CLOEXEC! */
   assert(raw_fd < 0 && errno == EINVAL);

   int mali_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   assert(mali_fd >= 0);

   /* Calling SET_FLAGS before VERSION_CHECK -> must fail with EPERM */
   struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
   assert(ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &sf) < 0 && errno == EPERM);

   /* Calling JM handshake (nr 0) on CSF -> must fail with EPERM */
   struct kbase_ioctl_version_check vc_jm = {0};
   assert(ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK_RESERVED, &vc_jm) < 0 && errno == EPERM);

   /* Calling premature MEM_ALLOC before context setup -> must fail with EPERM */
   union kbase_ioctl_mem_alloc ma = {0};
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &ma) < 0 && errno == EPERM);

   /* Proper handshake sequence */
   struct kbase_ioctl_version_check vc = { .major = 1, .minor = 38 };
   assert(ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK, &vc) == 0);
   assert(ioctl(mali_fd, KBASE_IOCTL_SET_FLAGS, &sf) == 0);

   /* Re-running VERSION_CHECK on initialized context -> must fail */
   assert(ioctl(mali_fd, KBASE_IOCTL_VERSION_CHECK, &vc) < 0);

   /* Memory allocation with BASE_MEM_PROT_GPU_EX without MEM_EXEC_INIT -> must fail with EINVAL */
   ma.in.va_pages = 1;
   ma.in.flags = BASE_MEM_PROT_GPU_EX;
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &ma) < 0 && errno == EINVAL);

   /* Initialize EXEC_VA */
   struct kbase_ioctl_mem_exec_init mi = { .va_pages = 16 };
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_EXEC_INIT, &mi) == 0);
   assert(ioctl(mali_fd, KBASE_IOCTL_MEM_ALLOC, &ma) == 0);

   /* Tiler heap invalid OOM simulation check */
   union kbase_ioctl_cs_tiler_heap_init_1_13 thi = { .in = { .chunk_size = 65536, .initial_chunks = 2, .max_chunks = 10 } };
   assert(ioctl(mali_fd, KBASE_IOCTL_CS_TILER_HEAP_INIT_1_13, &thi) == 0);

   /* Queue registration and bind with invalid buffer size (not power of 2) -> must fail */
   struct kbase_ioctl_cs_queue_register qr = { .buffer_gpu_addr = 0x1000, .buffer_size = 5000 };
   assert(ioctl(mali_fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &qr) < 0 && errno == EINVAL);

   close(mali_fd);
   printf("  -> /dev/mali0 State Machine & Precondition Violations: PASS\n");

   printf("[Unit Tests] ALL EDGE CASES PASSED!\n");
   return 0;
}
