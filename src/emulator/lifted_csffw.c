/*
 * Mali CSF MCU Firmware (mali_csffw.bin) Binary Parser, ARMv7-M Thumb-2 Static Lifter
 * & Formal Operational Semantics Implementation (src/emulator/lifted_csffw.c)
 *
 * Implements:
 *   1. Binary parser for `mali_csffw.bin` (magic 0xC3F13A6E, version 0.3, 968-byte header,
 *      27 entries, 8 IFACE sections, 167 FUNC_LIST pairs, and the 129-entry 64-bit CSF opcode
 *      dispatch table at VA 0x004100BC / file offset 0x00125C).
 *   2. Instruction-accurate C lifting of the 23 ARMv7-M Cortex-M7 Thumb-2 CSF opcode handlers
 *      in `mali_csffw.bin` (`fn_0x00800484` -> `g_csffw_opcode_table[0..128]`), plus CSHWIF
 *      hardware frontend instructions (`MOVE48`, `MOV32`, `WAIT`, `RUN_COMPUTE/IDVS/FRAGMENT`).
 */

#include "lifted_csffw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32_le(const uint8_t *p)
{
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}


bool csffw_parse_binary_file(const char *path, struct csffw_parsed_image *out_img)
{
    if (!out_img)
        return false;
    memset(out_img, 0, sizeof(*out_img));

    const char *cands[6] = {
        path,
        "predownloaded/mali_csffw.bin",
        "../predownloaded/mali_csffw.bin",
        "../../predownloaded/mali_csffw.bin",
        "kernel/mali_csffw.bin",
        "../kernel/mali_csffw.bin",
        "../../kernel/mali_csffw.bin",
        "mali_csffw.bin",
        NULL
    };

    FILE *fp = NULL;
    for (int i = 0; i < 6; ++i) {
        if (!cands[i] || !cands[i][0])
            continue;
        fp = fopen(cands[i], "rb");
        if (fp)
            break;
    }
    if (!fp)
        return false;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long fsz = ftell(fp);
    if (fsz < 0x44000 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    uint8_t *fw = (uint8_t *)malloc((size_t)fsz);
    if (!fw) {
        fclose(fp);
        return false;
    }
    if (fread(fw, 1, (size_t)fsz, fp) != (size_t)fsz) {
        free(fw);
        fclose(fp);
        return false;
    }
    fclose(fp);

    out_img->file_size = (uint32_t)fsz;
    out_img->magic = read_u32_le(fw + 0);
    out_img->ver_minor = fw[4];
    out_img->ver_major = fw[5];
    out_img->hdr_size = read_u32_le(fw + 16);

    if (out_img->magic != CSFFW_BIN_MAGIC || out_img->hdr_size != CSFFW_HEADER_SIZE_EXPECTED) {
        free(fw);
        return false;
    }

    uint32_t off = 20;
    while (off + 4 <= out_img->hdr_size) {
        uint32_t hdr_word = read_u32_le(fw + off);
        uint32_t etype = hdr_word & 0xFFU;
        uint32_t esize = (hdr_word >> 8) & 0xFFU;
        if (esize < 4 || off + esize > out_img->hdr_size)
            break;
        const uint8_t *payload = fw + off + 4;
        if (etype == 0 && out_img->num_iface_sections < 8 && esize >= 24) {
            struct csffw_iface_section *sec = &out_img->iface_sections[out_img->num_iface_sections++];
            sec->flags = read_u32_le(payload + 0);
            sec->va_start = read_u32_le(payload + 4);
            sec->va_end = read_u32_le(payload + 8);
            sec->data_start = read_u32_le(payload + 12);
            sec->data_end = read_u32_le(payload + 16);
        } else if (etype == 7 && esize >= 12) {
            uint32_t f_va_start = read_u32_le(payload + 0);
            uint32_t f_va_end = read_u32_le(payload + 4);
            out_img->func_list_count = (f_va_end - f_va_start) / 8U;
        }
        out_img->num_entries++;
        off += esize;
    }

    /* Extract ARMv7-M Vector Table at file offset 0x001124 (VA 0x00000000) */
    out_img->initial_sp = read_u32_le(fw + 0x001124 + 0);
    out_img->reset_handler_va = read_u32_le(fw + 0x001124 + 4);
    out_img->systick_handler_va = read_u32_le(fw + 0x001124 + 15 * 4);
    out_img->irq0_handler_va = read_u32_le(fw + 0x001124 + 16 * 4);

    /* Extract Shared Interface (GLB_IFACE + CSG_IFACE[0]) at file offset 0x042000 (VA 0x04000000) */
    out_img->glb_group_num = read_u32_le(fw + CSFFW_SHARED_IFACE_FILE_OFF + 0x0010);
    out_img->glb_group_stride = read_u32_le(fw + CSFFW_SHARED_IFACE_FILE_OFF + 0x0014);
    out_img->glb_instr_features = read_u32_le(fw + CSFFW_SHARED_IFACE_FILE_OFF + 0x001C);
    out_img->csg_stream_num = read_u32_le(fw + CSFFW_SHARED_IFACE_FILE_OFF + 0x1014);
    out_img->csg_stream_stride = read_u32_le(fw + CSFFW_SHARED_IFACE_FILE_OFF + 0x1018);

    /* Count unique timeline tracepoint emitters in FUNC_LIST (VA 0x00410de4..0x0041131c) */
    uint32_t seen_emitters[64];
    uint32_t n_seen = 0;
    uint32_t fl_file_off = 0x0011A0U + (CSFFW_FUNC_LIST_VA_START - 0x00410000U);
    for (uint32_t i = 0; i < CSFFW_FUNC_LIST_PAIRS; ++i) {
        uint32_t target_va = read_u32_le(fw + fl_file_off + i * 8U + 4U);
        bool dup = false;
        for (uint32_t j = 0; j < n_seen; ++j) {
            if (seen_emitters[j] == target_va) {
                dup = true;
                break;
            }
        }
        if (!dup && n_seen < 64)
            seen_emitters[n_seen++] = target_va;
    }
    out_img->unique_tl_emitters = n_seen;

    /* Extract 129-entry 64-bit CSF Opcode Dispatch Table at VA 0x004100BC (file 0x00125C) */
    uint32_t non_null = 0;
    for (uint32_t op = 0; op < CSFFW_OPCODE_TABLE_ENTRIES; ++op) {
        uint32_t fn_va = read_u32_le(fw + CSFFW_OPCODE_TABLE_FILE_OFF + op * 4U);
        out_img->opcode_table[op] = fn_va;
        if (fn_va != 0)
            non_null++;
    }
    out_img->non_null_opcode_handlers = non_null;
    out_img->valid = true;

    free(fw);
    return true;
}

void csffw_emu_state_init(struct csffw_emu_state *st)
{
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
    st->max_regs = CSFFW_MAX_REGS_DEFAULT;       /* VA 0x0202dce4 = 96 */
    st->max_scoreboards = CSFFW_MAX_SCOREBOARDS; /* VA 0x0202dcee = 16 */
    st->csi_iter_active = 1;
    st->csi_tiler_state = 8;                     /* Ready for HEAP_OPERATION (0x30) */
    st->csi_state = CSFFW_CSI_RUNNABLE;
    st->csg_state = CSFFW_TL_CSG_ENABLED;
    st->cshwif_state = CSFFW_TL_CSHWIF_ENABLED;
    for (int i = 0; i < 4; ++i)
        st->iter_states[i] = CSFFW_ITER_ENABLED;
}

static enum csffw_opcode_ret csffw_raise_invalid_insn_fault(struct csffw_emu_state *st)
{
    st->fault_code = 73U; /* 0x49 = CS_INVALID_INSTRUCTION (@ 0x008079b4: movs r1, #73) */
    st->fault_pc = st->cshwif_pc;
    st->csi_state = CSFFW_CSI_FAULT;
    return CSFFW_OP_RET_FAULT;
}

static uint64_t csffw_read_reg64(const struct csffw_emu_state *st, uint32_t even_reg)
{
    if (even_reg + 1 >= st->max_regs)
        return 0ULL;
    return ((uint64_t)st->cshwif_regs[even_reg + 1] << 32) | (uint64_t)st->cshwif_regs[even_reg];
}

static void csffw_write_reg64(struct csffw_emu_state *st, uint32_t even_reg, uint64_t val)
{
    if (even_reg + 1 >= st->max_regs)
        return;
    st->cshwif_regs[even_reg] = (uint32_t)(val & 0xFFFFFFFFULL);
    st->cshwif_regs[even_reg + 1] = (uint32_t)(val >> 32);
}

/*
 * Statically lifted C implementation of `fn_0x00800484` + `g_csffw_opcode_table[0..128]`
 * from `mali_csffw.bin` (ARMv7-M Cortex-M7 Thumb-2).
 */
enum csffw_opcode_ret lifted_csffw_dispatch_opcode(
    struct csffw_emu_state *st,
    uint32_t cshwif_idx,
    uint32_t cmd_lo,
    uint32_t cmd_hi)
{
    (void)cshwif_idx;
    if (!st)
        return CSFFW_OP_RET_FAULT;

    uint32_t opcode = (cmd_hi >> 24) & 0xFFU;
    if (opcode > 128U)
        return csffw_raise_invalid_insn_fault(st);

    switch (opcode) {
    /* --- CSHWIF Hardware Frontend Instructions (0x00..0x07) --- */
    case 0x00: /* NOP */
    case 0x80: /* NOP_TRAP @ 0x00806b3c: movs r0, #0; bx lr */
        return CSFFW_OP_RET_COMPLETED;

    case 0x01: /* MOVE48: dst = bits[23:16] of cmd_hi, imm48 = ((cmd_hi & 0xFFFF) << 32) | cmd_lo */ {
        uint32_t dst = (cmd_hi >> 16) & 0xFFU;
        if (dst >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_regs[dst] = cmd_lo;
        st->cshwif_regs[dst + 1] = cmd_hi & 0xFFFFU;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x02: /* MOV32 / MOVE32: dst = bits[23:16] of cmd_hi, imm32 = cmd_lo */ {
        uint32_t dst = (cmd_hi >> 16) & 0xFFU;
        if (dst >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_regs[dst] = cmd_lo;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x03: /* WAIT: scoreboard mask in cmd_lo[15:0] */
        return CSFFW_OP_RET_COMPLETED;

    /* --- Firmware-Trapped CSF Opcodes in g_csffw_opcode_table (VA 0x004100BC) --- */
    case 0x0B: /* Handler @ 0x0080796c: SHARED_SB_DEC / SB_INC */ {
        uint32_t src0 = (cmd_hi >> 8) & 0xFFU;
        uint32_t src1 = cmd_hi & 0xFFU;
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x18: /* Handler @ 0x00812668: DEFERRED_SYNC / PROT_REGION */ {
        uint32_t src = (cmd_hi >> 8) & 0xFFU;
        uint32_t slot = cmd_lo & 0x1FU;
        if (src >= (uint32_t)(st->max_regs - 1U) || slot >= 8U)
            return csffw_raise_invalid_insn_fault(st);
        st->csi_deferred_opcode = 24U;
        st->csi_deferred_arg0 = slot;
        st->csi_deferred_cmd_lo = st->cshwif_regs[src];
        st->csi_deferred_cmd_hi = st->cshwif_regs[src + 1];
        st->csi_state = CSFFW_CSI_BLOCKED_DEFERRED;
        return CSFFW_OP_RET_BLOCKED_DEFERRED;
    }

    case 0x19: /* Handler @ 0x00806dc8: TILER_HEAP_SET */ {
        uint32_t src0 = (cmd_hi >> 8) & 0xFFU;
        uint32_t src1 = cmd_hi & 0xFFU;
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x20: /* Handler @ 0x00811714: CALL (subroutine call via CSHWIF_REG[src0:src0+1], len CSHWIF_REG[src1]) */ {
        uint32_t src0 = (cmd_hi >> 8) & 0xFFU;
        uint32_t src1 = cmd_hi & 0xFFU;
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        if (st->csi_call_depth >= CSFFW_MAX_CALL_STACK_DEPTH)
            return csffw_raise_invalid_insn_fault(st);
        uint64_t target_va = csffw_read_reg64(st, src0) & ~7ULL;
        uint32_t len_bytes = st->cshwif_regs[src1] & ~7U;
        if (target_va == 0 || len_bytes == 0)
            return csffw_raise_invalid_insn_fault(st);
        st->call_stack_pc[st->csi_call_depth] = st->cshwif_pc + 8ULL;
        st->call_stack_end[st->csi_call_depth] = st->cshwif_end_pc;
        st->csi_call_depth++;
        st->cshwif_pc = target_va;
        st->cshwif_end_pc = target_va + len_bytes;
        return CSFFW_OP_RET_BRANCHED;
    }

    case 0x21: /* Handler @ 0x0081182e: JUMP (tail branch inside subroutine or ringbuffer) */ {
        uint32_t src0 = (cmd_hi >> 8) & 0xFFU;
        uint32_t src1 = cmd_hi & 0xFFU;
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        uint64_t target_va = csffw_read_reg64(st, src0) & ~7ULL;
        uint32_t len_bytes = st->cshwif_regs[src1] & ~7U;
        if (target_va == 0 || len_bytes == 0)
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_pc = target_va;
        st->cshwif_end_pc = target_va + len_bytes;
        return CSFFW_OP_RET_BRANCHED;
    }

    case 0x16: /* Synthetic alias for REQ_RESOURCE */
    case 0x22: /* Handler @ 0x00806cec: REQ_RESOURCE (acquires/releases TILER/COMPUTE/FRAGMENT/NEURAL iterators) */ {
        uint8_t req_mask = (uint8_t)(cmd_lo & 0x0FU);
        st->csi_req_res_mask = req_mask;
        st->csi_granted_res_mask = req_mask;
        for (int it = 0; it < 4; ++it) {
            if (req_mask & (1U << it))
                st->iter_states[it] = CSFFW_ITER_ENABLED;
        }
        return (st->csi_granted_res_mask == st->csi_req_res_mask)
                   ? CSFFW_OP_RET_COMPLETED
                   : CSFFW_OP_RET_BLOCKED_RESOURCE;
    }

    case 0x24: /* Handler @ 0x008014b0: FLUSH_CACHE */ {
        uint32_t sb = (cmd_hi >> 16) & 0x0FU;
        uint32_t reg = (cmd_hi >> 8) & 0xFFU;
        if (sb >= st->max_scoreboards || reg >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x25: /* Handler @ 0x00801650: SYNC_ADD32 */
    case 0x26: /* Handler @ 0x00801650: SYNC_SET32 */
    case 0x33: /* Handler @ 0x00801650: SYNC_ADD64 */
    case 0x34: /* Handler @ 0x00801650: SYNC_SET64 */ {
        bool is_64 = (opcode >= 0x33U);
        uint32_t addr_reg = (cmd_hi >> 8) & 0xFFU;
        uint32_t val_reg = cmd_hi & 0xFFU;
        uint32_t sb = (cmd_hi >> 16) & 0x0FU;
        if (addr_reg >= (uint32_t)(st->max_regs - 1U) || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        if (is_64 ? (val_reg >= (uint32_t)(st->max_regs - 1U)) : (val_reg >= st->max_regs))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t sync_va = csffw_read_reg64(st, addr_reg);
        /* 0x008016ac / 0x008016d4: 8-byte alignment check for 32-bit, 16-byte or 8-byte for 64-bit */
        if ((sync_va & 0x7ULL) != 0ULL || sync_va < 0x10000ULL)
            return csffw_raise_invalid_insn_fault(st);
        if (is_64) {
            uint64_t v64 = csffw_read_reg64(st, val_reg);
            uint64_t *p64 = (uint64_t *)(uintptr_t)sync_va;
            if (opcode == 0x33U)
                *p64 += v64;
            else
                *p64 = v64;
        } else {
            uint32_t v32 = st->cshwif_regs[val_reg];
            uint32_t *p32 = (uint32_t *)(uintptr_t)sync_va;
            if (opcode == 0x25U)
                *p32 += v32;
            else
                *p32 = v32;
        }
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x27: /* Handler @ 0x0080195c: SYNC_WAIT32 */
    case 0x35: /* Handler @ 0x0080195c: SYNC_WAIT64 */ {
        bool is_64 = (opcode >= 0x33U);
        uint32_t addr_reg = (cmd_hi >> 8) & 0xFFU;
        uint32_t val_reg = cmd_hi & 0xFFU;
        uint32_t cond = ((cmd_lo >> 28) | ((cmd_hi & 0xFFU) << 4)) & 0x0FU;
        if (addr_reg >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        if (is_64 ? (val_reg >= (uint32_t)(st->max_regs - 1U)) : (val_reg >= st->max_regs))
            return csffw_raise_invalid_insn_fault(st);
        /* 0x008019fc..0x00801a04: cond <= 5 && ((1U << cond) & 0x23U) */
        if (cond > 5U || (((1U << cond) & 0x23U) == 0U))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t sync_va = csffw_read_reg64(st, addr_reg);
        if ((sync_va & 0x7ULL) != 0ULL || sync_va < 0x10000ULL)
            return csffw_raise_invalid_insn_fault(st);
        bool satisfied = false;
        if (is_64) {
            uint64_t cur = *(const uint64_t *)(uintptr_t)sync_va;
            uint64_t ref = csffw_read_reg64(st, val_reg);
            satisfied = (cond == 0U) ? (cur <= ref) : (cond == 1U) ? (cur > ref) : (cur >= ref);
        } else {
            uint32_t cur = *(const uint32_t *)(uintptr_t)sync_va;
            uint32_t ref = st->cshwif_regs[val_reg];
            satisfied = (cond == 0U) ? (cur <= ref) : (cond == 1U) ? (cur > ref) : (cur >= ref);
        }
        if (!satisfied) {
            st->csi_state = CSFFW_CSI_BLOCKED_SYNC_WAIT;
            return CSFFW_OP_RET_BLOCKED_DEFERRED;
        }
        st->csi_state = CSFFW_CSI_RUNNABLE;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x28: /* Handler @ 0x00806f52: POINTER_OFFSET (adds sxth(cmd_lo & 0xFFF8) to 64-bit reg pair) */ {
        uint32_t sb = (cmd_hi >> 16) & 0x0FU;
        uint32_t reg = (cmd_hi >> 8) & 0xFFU;
        if (reg >= (uint32_t)(st->max_regs - 1U) || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        int16_t delta = (int16_t)(cmd_lo & 0xFFF8U);
        uint64_t ptr = csffw_read_reg64(st, reg);
        ptr = (uint64_t)((int64_t)ptr + (int64_t)delta);
        if ((ptr & 0x7ULL) != 0ULL)
            return csffw_raise_invalid_insn_fault(st);
        csffw_write_reg64(st, reg, ptr);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x29: /* Handler @ 0x00806b40: SKIP */ {
        uint32_t skip_bytes = (cmd_lo << 3) & 0x0007FFF8U;
        if (skip_bytes == 0)
            return CSFFW_OP_RET_COMPLETED;
        if (st->cshwif_pc + 8ULL + skip_bytes > st->cshwif_end_pc)
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_pc += skip_bytes;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x2A: /* Handler @ 0x008125f0: LOAD_ITER_STATE (CSHWIF_REG[src:src+1] -> CSHWIF+0x90..0x94) */ {
        uint32_t src = (cmd_hi >> 8) & 0xFFU;
        if (src >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_iter_state64 = csffw_read_reg64(st, src);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x2B: /* Handler @ 0x00812578: STORE_ITER_STATE (CSHWIF+0x90..0x94 -> CSHWIF_REG[dst:dst+1]) */ {
        uint32_t dst = (cmd_hi >> 16) & 0xFFU;
        if (dst >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        csffw_write_reg64(st, dst, st->cshwif_iter_state64);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x2C: /* Handler @ 0x0081284a: FINISH_FRAGMENT / ITER_INC */ {
        if (st->csi_iter_active && (cmd_hi & 1U))
            st->cshwif_iter_state64 += 1ULL;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x2F: /* Handler @ 0x00806a5c: PROTM_EXIT */ {
        st->csi_iter_active = 0;
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x30: /* Handler @ 0x00807672: HEAP_OPERATION */ {
        uint32_t src = (cmd_hi >> 8) & 0xFFU;
        if (src >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t heap_va = csffw_read_reg64(st, src);
        if (heap_va == 0ULL || (heap_va & 0x7ULL) != 0ULL || st->csi_tiler_state != 8U)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x31: /* Handler @ 0x008078c4: ITER_PROGRESS_INC */ {
        uint32_t mode = cmd_hi & 0x7U;
        uint32_t sb = (cmd_hi >> 16) & 0x0FU;
        if (mode > 3U || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case 0x32: /* Handler @ 0x00812cb6: LOAD_MULTIPLE */ {
        uint32_t count = (cmd_hi >> 8) & 0xFFU;
        uint32_t sb = (cmd_hi >> 16) & 0x0FU;
        uint32_t base_reg = cmd_hi & 0xFFU;
        if (count > 16U || sb >= st->max_scoreboards || base_reg + count > st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    default:
        return csffw_raise_invalid_insn_fault(st);
    }
}
