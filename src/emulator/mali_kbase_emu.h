/*
 * SPDX-License-Identifier: MIT
 *
 * User-Space /dev/mali0 Kbase CSF + Valhall ISA Emulator (`libmali_kbase_emu.so`)
 *
 * Emulates ARM's mali_kbase kernel driver (/dev/mali0) on x86_64 / arm64:
 *  1. Routes every /dev/mali0 file_operations UAPI (open, release/close, ioctl,
 *     mmap, poll) straight into the statically lifted AArch64 kernel module binary
 *     (`mali_kbase.ko` r44p1 or `mali_kbase_husky.ko` r54p1) in `lifted_kbase_ko.ll`.
 *  2. Supports multiple concurrent /dev/mali0 file descriptors within a single
 *     process (per-FD `struct emu_kfile` isolation + dup/dup2/fcntl refcounting).
 *  3. Implements an asynchronous Client/Server CSF architecture where
 *     `KBASE_IOCTL_CS_QUEUE_KICK` and `USER_IO` doorbell writes enqueue kick
 *     events onto a lock-free atomic ringbuffer serviced by a dedicated
 *     background CSF server thread (`csf_server_thread_fn`).
 *  4. Redirects lower-level OS kernel primitives, GPU VM/MMU backing, CSF
 *     hardware doorbell/ringbuffer execution, Valhall 64-bit ISA-to-SPIR-V /
 *     Lavapipe Vulkan pipeline execution, and companion Android kernel UAPIs
 *     (/dev/dma_heap/system, dma-buf, sync_file, ashmem) through host shims.
 */
#ifndef MALI_KBASE_EMU_H
#define MALI_KBASE_EMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "mali_kbase_uapi.h"
#include "lifted_kbase_ko.h"

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
enum mali_kbase_emu_profile {
   MALI_EMU_PROFILE_MT6878_R44 = 0, /* mali_kbase.ko (MT6878 G615 MC2, r44p1, kbase v1.21) */
   MALI_EMU_PROFILE_HUSKY_R54  = 1, /* mali_kbase_husky.ko (Pixel 8 Pro G715 MC7, r54p1/r54p3, kbase v1.38) */
};

/*
 * Explicit Host & Hardware Redirector ABI (`struct kbase_ko_host_abi`) between
 * the translated `.ko` binary (`lifted_r44p1_*` / `lifted_husky_r54p1_*`) and
 * concrete user-space implementations of OS kernel primitives, GPU MMU/VM
 * backing, CSF firmware/scheduler, and Valhall ISA-to-SPIR-V pipelines.
 */
struct kbase_ko_host_abi {
   /* Domain 1: Linux Kernel OS Primitives (User-Space Kernel Shim) */
   uint64_t (*os_copy_from_user)(void *dst, const void *user_src, size_t n);
   uint64_t (*os_copy_to_user)(void *user_dst, const void *src, size_t n);
   void    *(*os_kmalloc)(LiftedKbaseCPU *cpu, size_t size, uint32_t gfp_flags);
   void     (*os_kfree)(LiftedKbaseCPU *cpu, void *ptr);

   /* Domain 2: /dev/mali0 Device, Context & GPU Virtual Memory / MMU Backend */
   void    *(*mmu_device_get_list)(LiftedKbaseCPU *cpu);
   void    *(*mmu_create_context)(LiftedKbaseCPU *cpu, void *kbdev, bool is_compat,
                                  uint32_t flags, uint64_t api_version, void *kfile);
   void     (*mmu_destroy_context)(LiftedKbaseCPU *cpu, void *kctx);
   int      (*mmu_region_tracker_init_exec)(LiftedKbaseCPU *cpu, void *kctx, uint64_t va_pages);
   int      (*mmu_region_tracker_init_jit)(LiftedKbaseCPU *cpu, void *kctx, const void *jit_info);
   void    *(*mmu_mem_alloc)(LiftedKbaseCPU *cpu, void *kctx, uint64_t va_pages,
                             uint64_t commit_pages, uint64_t extension,
                             uint64_t *io_flags, uint64_t *io_gpu_va);
   int      (*mmu_mem_query)(LiftedKbaseCPU *cpu, void *kctx, uint64_t gpu_addr,
                             uint64_t query, uint64_t *out_val);
   int      (*mmu_mem_free)(LiftedKbaseCPU *cpu, void *kctx, uint64_t gpu_addr);
   int      (*mmu_mem_commit)(LiftedKbaseCPU *cpu, void *kctx, uint64_t gpu_addr, uint64_t pages);
   uint64_t (*mmu_mem_alias)(LiftedKbaseCPU *cpu, void *kctx, uint64_t *io_flags,
                             uint64_t stride, uint64_t nents,
                             const struct base_mem_aliasing_info *ai, uint64_t *out_num_pages);
   int      (*mmu_mem_import)(LiftedKbaseCPU *cpu, void *kctx, uint32_t type,
                              uint64_t phandle, uint32_t padding,
                              uint64_t *out_gpu_va, uint64_t *out_va_pages, uint64_t *io_flags);
   int      (*mmu_context_mmap)(LiftedKbaseCPU *cpu, void *kctx, void *vma_desc);

   /* Domain 3: CSF Firmware & Hardware Scheduler Backend */
   uint32_t (*csf_firmware_get_glb_iface)(LiftedKbaseCPU *cpu, void *kbdev,
                                          struct basep_cs_group_control *group_data,
                                          uint32_t max_group_num,
                                          struct basep_cs_stream_control *stream_data,
                                          uint32_t max_total_stream_num,
                                          uint32_t *out_glb_version, uint32_t *out_features,
                                          uint32_t *out_group_num, uint32_t *out_prfcnt_size,
                                          uint32_t *out_instr_features);
   int      (*csf_queue_register_internal)(LiftedKbaseCPU *cpu, void *kctx,
                                           const struct kbase_ioctl_cs_queue_register *reg,
                                           const struct kbase_ioctl_cs_queue_register_ex *reg_ex);
   int      (*csf_queue_bind)(LiftedKbaseCPU *cpu, void *kctx,
                              union kbase_ioctl_cs_queue_bind *bind);
   int      (*csf_queue_kick)(LiftedKbaseCPU *cpu, void *kctx,
                              const struct kbase_ioctl_cs_queue_kick *kick);
   int      (*csf_queue_terminate)(LiftedKbaseCPU *cpu, void *kctx,
                                   const struct kbase_ioctl_cs_queue_terminate *term);
   int      (*csf_create_queue_group)(LiftedKbaseCPU *cpu, void *kctx, void *create_union);
   int      (*csf_queue_group_terminate)(LiftedKbaseCPU *cpu, void *kctx, uint8_t group_handle);
   int      (*csf_tiler_heap_init)(LiftedKbaseCPU *cpu, void *kctx,
                                   uint32_t chunk_size, uint32_t initial_chunks,
                                   uint32_t max_chunks, uint16_t target_in_flight,
                                   uint64_t buf_desc_va,
                                   uint64_t *out_gpu_heap_va, uint64_t *out_first_chunk_va);
   int      (*csf_tiler_heap_term)(LiftedKbaseCPU *cpu, void *kctx, uint64_t gpu_heap_va);
   int      (*csf_kcpu_queue_new)(LiftedKbaseCPU *cpu, void *kctx,
                                  struct kbase_ioctl_kcpu_queue_new *newq);
   int      (*csf_kcpu_queue_delete)(LiftedKbaseCPU *cpu, void *kctx, uint8_t id);
   int      (*csf_kcpu_queue_enqueue)(LiftedKbaseCPU *cpu, void *kctx,
                                      const struct kbase_ioctl_kcpu_queue_enqueue *enq);

   /* Domain 4: Valhall 64-bit ISA -> SPIR-V Lifter & Lavapipe Vulkan Pipeline Executor */
   void     (*isa2spirv_execute_shader)(const struct valhall_shader_desc *spd);
};

const struct kbase_ko_host_abi *mali_kbase_emu_get_host_abi(void);

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
   uint32_t unwaited_deferred_sync_errors;
   uint32_t csf_jumps_followed;
   uint32_t lifted_ko_calls_executed;
   uint64_t lifted_ko_insns_executed;
   uint32_t active_profile;
   uint16_t kbase_ver_major;
   uint16_t kbase_ver_minor;
   uint32_t active_kfile_count;
   uint32_t total_kfiles_opened;
   uint32_t csf_server_kicks_processed;
};

void mali_kbase_emu_get_stats(struct kbase_emu_stats *out);
size_t mali_kbase_emu_get_stats_sized(struct kbase_emu_stats *out, size_t out_size);
int mali_kbase_emu_export_trace_json(const char *path);
void mali_kbase_emu_reset_stats(void);
bool mali_kbase_emu_init_lavapipe_bridge(void);
void mali_kbase_emu_set_profile(enum mali_kbase_emu_profile profile);
enum mali_kbase_emu_profile mali_kbase_emu_get_profile(void);
uint32_t mali_kbase_emu_get_open_fd_count(void);
int mali_kbase_emu_wait_csf_idle(uint64_t timeout_ns);
void mali_kbase_emu_dump_csf_state(void);

/* Direct C API for AOT-translated libGLES_mali.so and r54p3_vulkan_icd */
typedef void (*mali_kbase_emu_csf_job_hook_fn)(uint8_t cs_opcode, uint64_t insn, const uint64_t *cs_regs, void *user_data);
void mali_kbase_emu_register_csf_job_hook(mali_kbase_emu_csf_job_hook_fn fn, void *user_data);

int mali_kbase_emu_open_direct(const char *pathname, int flags);
int mali_kbase_emu_close_direct(int fd);
int mali_kbase_emu_ioctl_direct(int fd, unsigned long request, void *arg);
void *mali_kbase_emu_mmap_direct(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int mali_kbase_emu_munmap_direct(void *addr, size_t length);
ssize_t mali_kbase_emu_read_direct(int fd, void *buf, size_t count);
int mali_kbase_emu_poll_direct(void *fds, unsigned long nfds, int timeout);
int mali_kbase_emu_stat_direct(const char *pathname, struct stat *statbuf);
int mali_kbase_emu_fstat_direct(int fd, struct stat *statbuf);
int mali_kbase_emu_access_direct(const char *pathname, int mode);
bool mali_kbase_emu_is_emulated_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif /* MALI_KBASE_EMU_H */
