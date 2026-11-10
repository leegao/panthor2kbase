/*
 * Mali CSF MCU Firmware (mali_csffw.bin) Reverse-Engineered Header, ARMv7-M Thumb-2
 * Static Lifter & Formal Operational Semantics Model (src/emulator/lifted_csffw.h)
 *
 * Kept strictly separate from AArch64 `horizon2llvm` per doco.1txF5t3FgEjpV6xYe0pcEg:
 * `mali_csffw.bin` is a 32-bit ARMv7-M (Cortex-M7 Thumb-2) microcontroller firmware image
 * (magic 0xC3F13A6E, version 0.3, 27 header entries, 278,528 bytes), whereas `horizon2llvm`
 * targets 64-bit AArch64 user-space (.so) and Linux kernel (.ko) ELF64 binaries.
 */

#ifndef LIFTED_CSFFW_H
#define LIFTED_CSFFW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSFFW_BIN_MAGIC              0xC3F13A6EU
#define CSFFW_HEADER_SIZE_EXPECTED   968U
#define CSFFW_OPCODE_TABLE_VA        0x004100BCU
#define CSFFW_OPCODE_TABLE_FILE_OFF  0x0000125CU
#define CSFFW_OPCODE_TABLE_ENTRIES   129U
#define CSFFW_FUNC_LIST_VA_START     0x00410DE4U
#define CSFFW_FUNC_LIST_VA_END       0x0041131CU
#define CSFFW_FUNC_LIST_PAIRS        167U
#define CSFFW_SHARED_IFACE_VA        0x04000000U
#define CSFFW_SHARED_IFACE_FILE_OFF  0x00042000U
#define CSFFW_CSHWIF_MMIO_BASE       0x40034000U
#define CSFFW_CSHWIF_REGFILE_BASE    0x40038000U
#define CSFFW_MAX_REGS_DEFAULT       96U
#define CSFFW_MAX_SCOREBOARDS        16U
#define CSFFW_MAX_CSG_SLOTS          8U
#define CSFFW_STREAMS_PER_CSG        8U
#define CSFFW_MAX_CSI_SLOTS          64U
#define CSFFW_MAX_CALL_STACK_DEPTH   8U

/* Return codes from g_csffw_opcode_table[op](csi_slot, cshwif_idx, cmd_lo, cmd_hi)
 * switched via `tbb [pc, r6]` at 0x00800584 in fn_0x00800484: */
enum csffw_opcode_ret {
    CSFFW_OP_RET_COMPLETED        = 0, /* Advance CSHWIF PC to next 64-bit instruction */
    CSFFW_OP_RET_BLOCKED_DEFERRED = 1, /* Blocked on deferred sync/cache/heap operation */
    CSFFW_OP_RET_BLOCKED_RESOURCE = 2, /* Blocked waiting for iterator/resource grant */
    CSFFW_OP_RET_BRANCHED         = 3, /* CALL/JUMP updated CSHWIF ringbuffer PC */
    CSFFW_OP_RET_PROTM_PEND       = 4, /* Blocked waiting for protected mode entry */
    CSFFW_OP_RET_FAULT            = 5  /* Invalid register/alignment/bounds -> CS_FAULT */
};

/* 14 Command Stream Interface (CSI) States from timeline_header (+0x0618..+0x08a2) */
enum csffw_csi_state {
    CSFFW_CSI_STOPPED               = 0x0,
    CSFFW_CSI_STOPPING              = 0x1,
    CSFFW_CSI_SUSPENDING            = 0x2,
    CSFFW_CSI_TERMINATING           = 0x3,
    CSFFW_CSI_BLOCKED_PROGRESS      = 0x4,
    CSFFW_CSI_BLOCKED_DEFERRED      = 0x5,
    CSFFW_CSI_BLOCKED_RESOURCE      = 0x6,
    CSFFW_CSI_BLOCKED_PROTM_PEND    = 0x7,
    CSFFW_CSI_BLOCKED_SHARED_SB_DEC = 0x8,
    CSFFW_CSI_FAULT                 = 0x9,
    CSFFW_CSI_SUSPENDING_FAULT      = 0xA,
    CSFFW_CSI_RUNNABLE              = 0xB,
    CSFFW_CSI_BLOCKED_SYNC_WAIT     = 0xC,
    CSFFW_CSI_EMPTY                 = 0xD
};

/* 7 Command Stream Group (CSG) Internal States from timeline_header (+0x08d8..+0x0a4d) */
enum csffw_csg_state {
    CSFFW_TL_CSG_DISABLING        = 0,
    CSFFW_TL_CSG_ENABLING         = 1,
    CSFFW_TL_CSG_SUSPENDING       = 2,
    CSFFW_TL_CSG_TERMINATING      = 3,
    CSFFW_TL_CSG_PROTM_SUSPENDING = 4,
    CSFFW_TL_CSG_ENABLED          = 5,
    CSFFW_TL_CSG_DISABLED         = 6
};

/* 6 CSHWIF Hardware Slot States from timeline_header (+0x0a83..+0x0b9d) */
enum csffw_cshwif_state {
    CSFFW_TL_CSHWIF_DISABLED  = 0,
    CSFFW_TL_CSHWIF_ENABLED   = 1,
    CSFFW_TL_CSHWIF_HALTED    = 2,
    CSFFW_TL_CSHWIF_DISABLING = 3,
    CSFFW_TL_CSHWIF_PAUSING   = 4,
    CSFFW_TL_CSHWIF_PAUSED    = 5
};

/* 7 Hardware Iterator States from timeline_header (+0x0bd0..+0x0ceb) */
enum csffw_iter_state {
    CSFFW_ITER_DISABLED  = 0,
    CSFFW_ITER_ENABLED   = 1,
    CSFFW_ITER_ENABLING  = 2,
    CSFFW_ITER_PAUSING   = 3,
    CSFFW_ITER_HALTING   = 4,
    CSFFW_ITER_HALTED    = 5,
    CSFFW_ITER_DISABLING = 6
};

/* 4 Hardware Iterator Types from timeline_header (+0x04a1..+0x0548) */
enum csffw_iter_type {
    CSFFW_TL_ITER_TYPE_TILER    = 0,
    CSFFW_TL_ITER_TYPE_COMPUTE  = 1,
    CSFFW_TL_ITER_TYPE_FRAGMENT = 2,
    CSFFW_TL_ITER_TYPE_NEURAL   = 3
};

struct csffw_iface_section {
    uint32_t flags;
    uint32_t va_start;
    uint32_t va_end;
    uint32_t data_start;
    uint32_t data_end;
};

struct csffw_parsed_image {
    bool valid;
    uint32_t magic;
    uint8_t ver_major;
    uint8_t ver_minor;
    uint32_t hdr_size;
    uint32_t file_size;
    uint32_t num_entries;
    uint32_t num_iface_sections;
    struct csffw_iface_section iface_sections[8];
    uint32_t initial_sp;
    uint32_t reset_handler_va;
    uint32_t systick_handler_va;
    uint32_t irq0_handler_va;
    uint32_t glb_group_num;
    uint32_t glb_group_stride;
    uint32_t glb_instr_features;
    uint32_t csg_stream_num;
    uint32_t csg_stream_stride;
    uint32_t func_list_count;
    uint32_t unique_tl_emitters;
    uint32_t non_null_opcode_handlers;
    uint32_t opcode_table[CSFFW_OPCODE_TABLE_ENTRIES];
};

/* Emulated Cortex-M7 CSF Firmware + CSHWIF Hardware Slot State */
struct csffw_emu_state {
    uint8_t max_regs;                   /* VA 0x0202dce4 = 96 */
    uint8_t max_scoreboards;            /* VA 0x0202dcee = 16 */
    uint8_t protm_active;               /* VA 0x0202bc6c */
    uint8_t csi_slot_idx;               /* csi_slot + 0 */
    uint8_t csg_slot_idx;               /* csi_slot + 1 */
    uint8_t csi_iter_active;            /* csi_slot + 170 (0xaa) */
    uint8_t csi_call_depth;             /* csi_slot + 648 (0x288), max 8 */
    uint8_t csi_tiler_state;            /* csi_slot + 2232 (0x8b8) */
    uint8_t csi_req_res_mask;           /* csi_slot + 2233 (0x8b9): bit0=COMP, bit1=FRAG, bit2=TILER, bit3=NEURAL */
    uint8_t csi_granted_res_mask;       /* csi_slot + 124  (0x7c) */
    uint8_t csi_deferred_opcode;        /* csi_slot + 2240 (0x8c0) */
    uint32_t csi_deferred_arg0;         /* csi_slot + 2248 (0x8c8) */
    uint32_t csi_deferred_cmd_lo;       /* csi_slot + 2256 (0x8d0) */
    uint32_t csi_deferred_cmd_hi;       /* csi_slot + 2260 (0x8d4) */
    enum csffw_csi_state csi_state;
    enum csffw_csg_state csg_state;
    enum csffw_cshwif_state cshwif_state;
    enum csffw_iter_state iter_states[4];
    uint32_t cshwif_regs[CSFFW_MAX_REGS_DEFAULT]; /* VA 0x40038000 + reg * 4 */
    uint64_t cshwif_iter_state64;       /* VA 0x40034000 + 0x90..0x94 */
    uint64_t cshwif_pc;                 /* VA 0x40034000 + 0x00..0x04 */
    uint64_t cshwif_end_pc;             /* VA 0x40034000 + 0x08..0x0c */
    uint64_t call_stack_pc[CSFFW_MAX_CALL_STACK_DEPTH];
    uint64_t call_stack_end[CSFFW_MAX_CALL_STACK_DEPTH];
    uint64_t csg_heap_va[8];            /* csg_slot + 560 + i * 16 */
    uint8_t  csg_heap_valid[8];
    uint32_t fault_code;                /* VA 0x0202b738 (73 = 0x49 = CS_INVALID_INSTRUCTION) */
    uint64_t fault_pc;                  /* VA 0x0202b740 */
};

/* Parse and validate `mali_csffw.bin` from disk */
bool csffw_parse_binary_file(const char *path, struct csffw_parsed_image *out_img);

/* Initialize a clean Cortex-M7 + CSHWIF state matching Reset_Handler + csffw_main_init */
void csffw_emu_state_init(struct csffw_emu_state *st);

/* Execute a single 64-bit CSF instruction using the lifted ARMv7-M Thumb-2 semantics of mali_csffw.bin */
enum csffw_opcode_ret lifted_csffw_dispatch_opcode(
    struct csffw_emu_state *st,
    uint32_t cshwif_idx,
    uint32_t cmd_lo,
    uint32_t cmd_hi);

#ifdef __cplusplus
}
#endif

#endif /* LIFTED_CSFFW_H */
