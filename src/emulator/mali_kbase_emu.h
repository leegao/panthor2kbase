/*
 * SPDX-License-Identifier: MIT
 *
 * User-Space /dev/mali0 Kbase CSF + Valhall ISA Emulator (`libmali_kbase_emu.so`)
 *
 * Emulates ARM's mali_kbase kernel driver (/dev/mali0) on x86_64 / arm64:
 *  1. Enforces the exact mali_kbase_core_linux.c state machine (O_CLOEXEC,
 *     VERSION_CHECK -> SET_FLAGS -> MEM_EXEC_INIT, USER_IO mmap before KICK,
 *     SAME_VA munmap free_on_close=1 semantics, and tiler OOM vt_end/frag_end check).
 *  2. Executes CSF ring buffers & command streams (SET_STATE, REQ_RESOURCE,
 *     MOVE48/32, CALL, FLUSH_CACHE2, RUN_IDVS, RUN_FRAGMENT, RUN_COMPUTE,
 *     SYNC_ADD/SET/WAIT, HEAP_OPERATION, ERROR_BARRIER).
 *  3. Disassembles and interprets 64-bit Valhall ISA shader binaries AND bridges
 *     compute/graphics draws to headless Vulkan (Lavapipe `libvulkan_lvp.so`).
 */
#ifndef MALI_KBASE_EMU_H
#define MALI_KBASE_EMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mali_kbase_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Valhall 64-bit ISA Toy/Reference Opcodes for Shader Execution & Formal Verification */
#define VALHALL_OP_NOP          0x00ULL
#define VALHALL_OP_MOV_I32      0x91ULL
#define VALHALL_OP_IADD_IMM_I32 0x98ULL
#define VALHALL_OP_IMUL_I32     0xa0ULL
#define VALHALL_OP_SHL_I32      0xa4ULL
#define VALHALL_OP_OR_I32       0xa8ULL
#define VALHALL_OP_FMA_F32      0xb0ULL
#define VALHALL_OP_LD_ATTR_IMM  0x60ULL
#define VALHALL_OP_LD_BUFFER_I32 0x68ULL
#define VALHALL_OP_ST_BUFFER_I32 0x69ULL
#define VALHALL_OP_ST_CVT       0x70ULL
#define VALHALL_OP_BLEND        0x7fULL
#define VALHALL_OP_BRANCHZ      0x1fULL

/* Shader Program Descriptor (SPD) in GPU memory (simplified 64-byte Valhall descriptor) */
struct valhall_shader_desc {
   uint64_t shader_va;
   uint32_t shader_size;
   uint32_t nr_registers;
   uint64_t fau_va;
   uint64_t resources_va;
   uint64_t output_va;
   uint32_t workgroup_x;
   uint32_t workgroup_y;
   uint32_t workgroup_z;
   uint32_t flags;
};

/* Emulator telemetry counters for testing & verification */
struct kbase_emu_stats {
   uint32_t ioctl_count;
   uint32_t mem_alloc_count;
   uint32_t queue_kick_count;
   uint32_t csf_instructions_executed;
   uint32_t csf_calls_followed;
   uint32_t valhall_shaders_executed;
   uint32_t valhall_instructions_executed;
   uint32_t compute_jobs_run;
   uint32_t idvs_jobs_run;
   uint32_t fragment_jobs_run;
   uint32_t lvp_bridges_run;
   uint32_t csg_scope_warnings;
   uint32_t oom_heap_stat_errors;
   uint32_t same_va_munmap_destroys;
};

void mali_kbase_emu_get_stats(struct kbase_emu_stats *out);
void mali_kbase_emu_reset_stats(void);
bool mali_kbase_emu_init_lavapipe_bridge(void);

#ifdef __cplusplus
}
#endif

#endif /* MALI_KBASE_EMU_H */
