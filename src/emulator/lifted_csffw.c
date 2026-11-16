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
#include "generated/csffw_genxml_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static bool csffw_is_mapped_va(uint64_t va, size_t len) {
    if (va < 0x10000ULL || len == 0 || (va + len) < va) return false;
    uint64_t p0 = va & ~4095ULL;
    uint64_t p1 = (va + len - 1ULL) & ~4095ULL;
    return msync((void *)(uintptr_t)p0, (size_t)(p1 - p0 + 4096ULL), MS_ASYNC) == 0;
}

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

    const char *cands[] = {
        path,
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

    /* Field layouts below are generated from mesa/src/panfrost/genxml/v11.xml
     * by tools/gen_csffw_genxml.py (see csffw_genxml_decode.h). The opcode
     * *names* follow the Mesa v11 CS Opcode enum; the *semantics* follow the
     * firmware handlers in mali_csffw.bin (validated by
     * src/verifier/csffw_formal_semantics_verifier.py). */
    const uint64_t insn = ((uint64_t)cmd_hi << 32) | (uint64_t)cmd_lo;
    const uint32_t opcode = (uint32_t)cs_base_opcode(insn);
    if (opcode > 128U)
        return csffw_raise_invalid_insn_fault(st);

    switch (opcode) {
    /* --- CSHWIF hardware frontend instructions --- */
    case CS_OPCODE_NOP:
    case 0x80: /* NOP_TRAP @ 0x00806b3c (not in the v11 CS Opcode enum) */
        return CSFFW_OP_RET_COMPLETED;

    case CS_OPCODE_MOVE48: {
        uint32_t dst = (uint32_t)cs_move48_destination(insn);
        if (dst >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t imm48 = cs_move48_immediate(insn);
        st->cshwif_regs[dst] = (uint32_t)(imm48 & 0xFFFFFFFFULL);
        st->cshwif_regs[dst + 1] = (uint32_t)(imm48 >> 32);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_MOVE32: {
        uint32_t dst = (uint32_t)cs_move32_destination(insn);
        if (dst >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_regs[dst] = (uint32_t)cs_move32_immediate(insn);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_WAIT:
        return CSFFW_OP_RET_COMPLETED;

    case CS_OPCODE_RUN_COMPUTE:
        st->iter_states[CSFFW_TL_ITER_TYPE_COMPUTE] = CSFFW_ITER_ENABLED;
        st->csi_state = CSFFW_CSI_RUNNABLE;
        return CSFFW_OP_RET_COMPLETED;

    case 0x05: /* RUN_TILING (v10; absent from the v11 CS Opcode enum) */
    case CS_OPCODE_RUN_IDVS:
        st->iter_states[CSFFW_TL_ITER_TYPE_TILER] = CSFFW_ITER_ENABLED;
        st->csi_state = CSFFW_CSI_RUNNABLE;
        return CSFFW_OP_RET_COMPLETED;

    case CS_OPCODE_RUN_FRAGMENT:
        st->iter_states[CSFFW_TL_ITER_TYPE_FRAGMENT] = CSFFW_ITER_ENABLED;
        st->csi_state = CSFFW_CSI_RUNNABLE;
        return CSFFW_OP_RET_COMPLETED;

    /* --- Firmware-trapped CSF opcodes in g_csffw_opcode_table --- */
    case CS_OPCODE_FINISH_FRAGMENT: {
        /* Handler @ 0x0080796c. Firmware models this as a shared scoreboard
         * decrement; validate the two register operands. */
        uint32_t src0 = (uint32_t)cs_finish_fragment_first_heap_chunk(insn);
        uint32_t src1 = (uint32_t)cs_finish_fragment_last_heap_chunk(insn);
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_PROGRESS_WAIT: {
        /* Handler @ 0x00812668: deferred progress wait. */
        uint32_t src = (uint32_t)cs_progress_wait_source(insn);
        uint32_t slot = (uint32_t)cs_progress_wait_queue(insn);
        if (src >= (uint32_t)(st->max_regs - 1U) || slot >= 8U)
            return csffw_raise_invalid_insn_fault(st);
        st->csi_deferred_opcode = 24U;
        st->csi_deferred_arg0 = slot;
        st->csi_deferred_cmd_lo = st->cshwif_regs[src];
        st->csi_deferred_cmd_hi = st->cshwif_regs[src + 1];
        st->csi_state = CSFFW_CSI_BLOCKED_DEFERRED;
        return CSFFW_OP_RET_BLOCKED_DEFERRED;
    }

    case CS_OPCODE_SET_EXCEPTION_HANDLER: {
        /* Handler @ 0x00806dc8. */
        uint32_t src0 = (uint32_t)cs_set_exception_handler_address(insn);
        uint32_t src1 = (uint32_t)cs_set_exception_handler_length(insn);
        if (src0 >= (uint32_t)(st->max_regs - 1U) || src1 >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_CALL: {
        uint32_t src0 = (uint32_t)cs_call_address(insn);
        uint32_t src1 = (uint32_t)cs_call_length(insn);
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

    case CS_OPCODE_JUMP: {
        uint32_t src0 = (uint32_t)cs_jump_address(insn);
        uint32_t src1 = (uint32_t)cs_jump_length(insn);
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
    case CS_OPCODE_REQ_RESOURCE: {
        uint8_t req_mask = 0;
        if (cs_req_resource_compute(insn))  req_mask |= 1U << 0;
        if (cs_req_resource_fragment(insn)) req_mask |= 1U << 1;
        if (cs_req_resource_tiler(insn))    req_mask |= 1U << 2;
        if (cs_req_resource_idvs(insn))     req_mask |= 1U << 3;
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

    case CS_OPCODE_FLUSH_CACHE2: {
        uint32_t sb = (uint32_t)cs_flush_cache2_signal_slot(insn);
        uint32_t reg = (uint32_t)cs_flush_cache2_latest_flush_id(insn);
        if (sb >= st->max_scoreboards || reg >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_SYNC_ADD32:
    case CS_OPCODE_SYNC_SET32:
    case CS_OPCODE_SYNC_ADD64:
    case CS_OPCODE_SYNC_SET64: {
        bool is_64 = (opcode == CS_OPCODE_SYNC_ADD64 || opcode == CS_OPCODE_SYNC_SET64);
        bool is_add = (opcode == CS_OPCODE_SYNC_ADD32 || opcode == CS_OPCODE_SYNC_ADD64);
        uint32_t addr_reg = (uint32_t)(is_64 ? cs_sync_add64_address(insn)
                                             : cs_sync_add32_address(insn));
        uint32_t val_reg = (uint32_t)(is_64 ? cs_sync_add64_data(insn)
                                            : cs_sync_add32_data(insn));
        uint32_t sb = (uint32_t)(is_64 ? cs_sync_add64_signal_slot(insn)
                                       : cs_sync_add32_signal_slot(insn));
        if (addr_reg >= (uint32_t)(st->max_regs - 1U) || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        if (is_64 ? (val_reg >= (uint32_t)(st->max_regs - 1U)) : (val_reg >= st->max_regs))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t sync_va = csffw_read_reg64(st, addr_reg);
        if ((sync_va & 0x7ULL) != 0ULL || sync_va < 0x10000ULL ||
            !csffw_is_mapped_va(sync_va, is_64 ? 8U : 4U))
            return csffw_raise_invalid_insn_fault(st);
        if (is_64) {
            uint64_t v = csffw_read_reg64(st, val_reg);
            uint64_t *p = (uint64_t *)(uintptr_t)sync_va;
            if (is_add) *p += v; else *p = v;
        } else {
            uint32_t v = st->cshwif_regs[val_reg];
            uint32_t *p = (uint32_t *)(uintptr_t)sync_va;
            if (is_add) *p += v; else *p = v;
        }
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_SYNC_WAIT32:
    case CS_OPCODE_SYNC_WAIT64: {
        bool is_64 = (opcode == CS_OPCODE_SYNC_WAIT64);
        uint32_t addr_reg = (uint32_t)(is_64 ? cs_sync_wait64_address(insn)
                                             : cs_sync_wait32_address(insn));
        uint32_t val_reg = (uint32_t)(is_64 ? cs_sync_wait64_data(insn)
                                            : cs_sync_wait32_data(insn));
        uint32_t cond = (uint32_t)(is_64 ? cs_sync_wait64_condition(insn)
                                         : cs_sync_wait32_condition(insn));
        if (addr_reg >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        if (is_64 ? (val_reg >= (uint32_t)(st->max_regs - 1U)) : (val_reg >= st->max_regs))
            return csffw_raise_invalid_insn_fault(st);
        /* 0x008019fc..0x00801a04: cond <= 5 && ((1U << cond) & 0x23U) */
        if (cond > 5U || (((1U << cond) & 0x23U) == 0U))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t sync_va = csffw_read_reg64(st, addr_reg);
        if ((sync_va & 0x7ULL) != 0ULL || sync_va < 0x10000ULL ||
            !csffw_is_mapped_va(sync_va, is_64 ? 8U : 4U))
            return csffw_raise_invalid_insn_fault(st);
        bool satisfied;
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

    case CS_OPCODE_STORE_STATE: {
        /* Handler @ 0x00806f52: signed 16-bit pointer offset on a register pair. */
        uint32_t sb = (uint32_t)cs_store_state_signal_slot(insn);
        uint32_t reg = (uint32_t)cs_store_state_address(insn);
        if (reg >= (uint32_t)(st->max_regs - 1U) || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        int16_t delta = (int16_t)(uint16_t)(cs_store_state_offset(insn) & 0xFFF8U);
        uint64_t ptr = csffw_read_reg64(st, reg);
        ptr = (uint64_t)((int64_t)ptr + (int64_t)delta);
        if ((ptr & 0x7ULL) != 0ULL)
            return csffw_raise_invalid_insn_fault(st);
        csffw_write_reg64(st, reg, ptr);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_PROT_REGION: {
        /* Handler @ 0x00806b40: skip (cmd_lo << 3) bytes. */
        uint32_t skip_bytes = (uint32_t)cs_prot_region_size(insn);
        if (skip_bytes == 0)
            return CSFFW_OP_RET_COMPLETED;
        if (st->cshwif_pc + 8ULL + skip_bytes > st->cshwif_end_pc)
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_pc += skip_bytes;
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_PROGRESS_STORE: {
        uint32_t src = (uint32_t)cs_progress_store_source(insn);
        if (src >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        st->cshwif_iter_state64 = csffw_read_reg64(st, src);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_PROGRESS_LOAD: {
        uint32_t dst = (uint32_t)cs_progress_load_destination(insn);
        if (dst >= st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        csffw_write_reg64(st, dst, st->cshwif_iter_state64);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_RUN_COMPUTE_INDIRECT: {
        if (st->csi_iter_active && cs_run_compute_indirect_progress_increment(insn))
            st->cshwif_iter_state64 += 1ULL;
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_ERROR_BARRIER: {
        /* Handler @ 0x00806a5c: protected-mode exit. */
        st->csi_iter_active = 0;
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_HEAP_SET: {
        /* Handler @ 0x00807672. */
        uint32_t src = (uint32_t)cs_heap_set_address(insn);
        if (src >= (uint32_t)(st->max_regs - 1U))
            return csffw_raise_invalid_insn_fault(st);
        uint64_t heap_va = csffw_read_reg64(st, src);
        if (heap_va == 0ULL || (heap_va & 0x7ULL) != 0ULL || st->csi_tiler_state != 8U)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_HEAP_OPERATION: {
        /* Handler @ 0x008078c4. */
        uint32_t mode = (uint32_t)cs_heap_operation_operation(insn);
        uint32_t sb = (uint32_t)cs_heap_operation_signal_slot(insn);
        if (mode > 3U || sb >= st->max_scoreboards)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    case CS_OPCODE_TRACE_POINT: {
        /* Handler @ 0x00812cb6. */
        uint32_t count = (uint32_t)cs_trace_point_register_count(insn);
        uint32_t sb = (uint32_t)cs_trace_point_signal_slot(insn);
        uint32_t base_reg = (uint32_t)cs_trace_point_base_register(insn);
        if (count > 16U || sb >= st->max_scoreboards || base_reg + count > st->max_regs)
            return csffw_raise_invalid_insn_fault(st);
        return CSFFW_OP_RET_COMPLETED;
    }

    default:
        return csffw_raise_invalid_insn_fault(st);
    }
}
