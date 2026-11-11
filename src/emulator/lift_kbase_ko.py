#!/usr/bin/env python3
"""
lift_kbase_ko.py — Direct AArch64-to-C Static Lifter for mali_kbase.ko (MT6878 r44p1)
and mali_kbase_husky.ko (Pixel 8 Pro Tensor G3 r54p1/r54p3).

Uses libLLVM (LLVMMCDisassembler from src/verifier/aarch64_mc_lifter.py) and ELF64
.symtab / .text / .rodata / SHT_RELA parsing to lift the top-level /dev/mali0
file_operations entrypoints (kbase_open, kbase_release, kbase_ioctl, kbase_kfile_ioctl,
kbase_mmap, kbase_poll) and all UAPI validation, capability negotiation, memory,
GPUPROPS, and CSF wrapper functions of both kernel modules directly into host C code
(src/emulator/lifted_kbase_ko.h and src/emulator/lifted_kbase_ko.c) while keeping
lower-level OS kernel primitives, CSF command-stream execution, and Valhall ISA-to-SPIR-V
pipeline execution cleanly separated behind a well-defined Host/Hardware Redirector ABI.
"""

import os
import re
import struct
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
sys.path.insert(0, os.path.join(REPO_ROOT, "src/verifier"))
from aarch64_mc_lifter import LLVMMCDisassembler

TARGET_FUNCTIONS = [
    "kbase_check_flags",
    "mali_kbase_supports_cap",
    "kbase_check_alloc_flags",
    "kbase_check_import_flags",
    "kbase_check_alloc_sizes",
    "kbase_gpuprops_parse_gpu_id",
    "kbase_gpuprops_get_curr_config_props",
    "kbase_gpuprops_populate_user_buffer",
    "kbase_file_create_kctx",
    "kbase_file_delete",
    "kbase_api_mem_alloc_ex",
    "kbase_api_mem_alloc",
    "kbase_api_mem_alias",
    "kbase_api_mem_import",
    "kbase_api_get_cpu_gpu_timeinfo",
    "kbase_ioctl_cs_get_glb_iface",
    "kbase_ioctl_read_user_page",
    "kbase_mem_flags_change",
    "kbase_csf_queue_register",
    "kbase_csf_queue_register_ex",
    "kbase_csf_alloc_command_stream_user_pages",
    "kbase_csf_queue_bind",
    "kbase_csf_queue_group_create",
    "kbasep_cs_queue_group_create",
    "kbasep_cs_queue_group_create_1_6",
    "kbasep_cs_queue_group_create_1_18",
    "kbasep_cs_queue_group_create_1_35",
    "kbasep_cs_tiler_heap_init",
    "kbasep_cs_tiler_heap_init_1_13",
    "kbase_open",
    "kbase_release",
    "kbase_mmap",
    "kbase_poll",
    "kbase_kfile_ioctl",
    "kbase_ioctl",
]

DIRECT_LIFTED_CALLEES = {
    "mali_kbase_supports_cap",
    "kbase_file_create_kctx",
    "kbase_file_delete",
    "kbase_api_mem_alloc",
    "kbase_api_mem_alloc_ex",
    "kbase_api_mem_alias",
    "kbase_api_mem_import",
    "kbase_api_get_cpu_gpu_timeinfo",
    "kbase_ioctl_cs_get_glb_iface",
    "kbase_ioctl_read_user_page",
    "kbasep_cs_queue_group_create",
    "kbasep_cs_queue_group_create_1_6",
    "kbasep_cs_queue_group_create_1_18",
    "kbasep_cs_queue_group_create_1_35",
    "kbase_csf_queue_group_create",
    "kbasep_cs_tiler_heap_init",
    "kbasep_cs_tiler_heap_init_1_13",
    "kbase_csf_queue_register",
    "kbase_csf_queue_register_ex",
    "kbase_kfile_ioctl",
}


def parse_ko(path):
    with open(path, "rb") as f:
        data = f.read()
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)
    shdrs = [
        struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
        for i in range(e_shnum)
    ]
    shstr = data[shdrs[e_shstrndx][4] : shdrs[e_shstrndx][4] + shdrs[e_shstrndx][5]]
    sec_by_name = {}
    for idx, s in enumerate(shdrs):
        name = shstr[s[0] :].split(b"\x00", 1)[0].decode("latin1")
        sec_by_name[name] = (idx, s)
    _, sym_sh = sec_by_name[".symtab"]
    str_sh = shdrs[sym_sh[6]]
    strtab = data[str_sh[4] : str_sh[4] + str_sh[5]]
    text_idx, text_sh = sec_by_name[".text"]
    ro_idx, ro_sh = sec_by_name[".rodata"]
    syms = []
    sym_by_name = {}
    sym_by_addr = {}
    for off in range(sym_sh[4], sym_sh[4] + sym_sh[5], sym_sh[9]):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, off
        )
        sname = strtab[st_name:].split(b"\x00", 1)[0].decode("latin1")
        syms.append((sname, st_info, st_shndx, st_value, st_size))
        if sname:
            sym_by_name[sname] = (st_info, st_shndx, st_value, st_size)
            if st_shndx == text_idx and not sname.startswith("$") and not sname.endswith(".cfi_jt"):
                sym_by_addr[st_value] = sname
    relas = {}
    for r_sh in [s for s in shdrs if s[1] == 4 and s[7] == text_idx]:
        for off in range(r_sh[4], r_sh[4] + r_sh[5], r_sh[9]):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, off)
            r_sym = r_info >> 32
            r_type = r_info & 0xFFFFFFFF
            relas[r_offset] = (
                r_type,
                syms[r_sym][0],
                syms[r_sym][2],
                syms[r_sym][3],
                r_addend,
            )
    text = data[text_sh[4] : text_sh[4] + text_sh[5]]
    rodata = data[ro_sh[4] : ro_sh[4] + ro_sh[5]]
    return {
        "text": text,
        "rodata": rodata,
        "ro_idx": ro_idx,
        "text_idx": text_idx,
        "sym_by_name": sym_by_name,
        "sym_by_addr": sym_by_addr,
        "relas": relas,
    }


def reg_expr(r):
    r = r.strip()
    if r == "xzr":
        return "0ULL"
    if r == "wzr":
        return "0U"
    if r == "sp":
        return "cpu->sp"
    if r == "wsp":
        return "((uint32_t)cpu->sp)"
    if r.startswith("x"):
        idx = int(r[1:])
        return f"cpu->x[{idx}]"
    if r.startswith("w"):
        idx = int(r[1:])
        return f"((uint32_t)cpu->x[{idx}])"
    raise ValueError(f"Unknown reg: {r}")


def set_reg(r, val_expr):
    r = r.strip()
    if r in ("xzr", "wzr"):
        return f"(void)({val_expr});"
    if r == "sp":
        return f"cpu->sp = (uint64_t)({val_expr});"
    if r == "wsp":
        return f"cpu->sp = (uint64_t)(uint32_t)({val_expr});"
    if r.startswith("x"):
        idx = int(r[1:])
        return f"cpu->x[{idx}] = (uint64_t)({val_expr});"
    if r.startswith("w"):
        idx = int(r[1:])
        return f"cpu->x[{idx}] = (uint64_t)(uint32_t)({val_expr});"
    raise ValueError(f"Unknown dst reg: {r}")


def parse_imm(s):
    s = s.strip()
    if s.startswith("#"):
        s = s[1:]
    return int(s, 0)


def cond_expr(cc):
    cc = cc.strip().lower()
    mapping = {
        "eq": "(cpu->z == 1)",
        "ne": "(cpu->z == 0)",
        "hs": "(cpu->c == 1)",
        "cs": "(cpu->c == 1)",
        "lo": "(cpu->c == 0)",
        "cc": "(cpu->c == 0)",
        "mi": "(cpu->n == 1)",
        "pl": "(cpu->n == 0)",
        "vs": "(cpu->v == 1)",
        "vc": "(cpu->v == 0)",
        "hi": "(cpu->c == 1 && cpu->z == 0)",
        "ls": "(!(cpu->c == 1 && cpu->z == 0))",
        "ge": "(cpu->n == cpu->v)",
        "lt": "(cpu->n != cpu->v)",
        "gt": "(cpu->z == 0 && cpu->n == cpu->v)",
        "le": "(!(cpu->z == 0 && cpu->n == cpu->v))",
    }
    return mapping[cc]


def split_operands(ops_str):
    out = []
    cur = []
    depth = 0
    for ch in ops_str:
        if ch == "[":
            depth += 1
            cur.append(ch)
        elif ch == "]":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur).strip())
    return out


def parse_shifted_op(tokens, is_64):
    base_t = tokens[0]
    if base_t.startswith("#"):
        val = parse_imm(base_t)
        if len(tokens) > 1 and tokens[1].startswith("lsl"):
            sh = parse_imm(tokens[1].split()[1])
            val = val << sh
        if is_64:
            uval = val & 0xFFFFFFFFFFFFFFFF
            # Translate 39-bit Android kernel TASK_SIZE_64 (1ULL << 39 = 0x8000000000)
            # access_ok() bounds to 48-bit host user-space VA bounds (1ULL << 48).
            if 0x7FFFFF0000 <= uval <= 0x8000000000:
                uval = (1 << 48) - (0x8000000000 - uval)
            return f"((uint64_t)0x{uval:x}ULL)"
        return f"((uint32_t){val & 0xFFFFFFFF}U)"
    base_e = reg_expr(base_t)
    if len(tokens) == 1:
        return base_e
    mod = tokens[1]
    parts = mod.split()
    kind = parts[0]
    sh = parse_imm(parts[1]) if len(parts) > 1 else 0
    if kind == "lsl":
        return f"({base_e} << {sh})"
    if kind == "lsr":
        return f"({base_e} >> {sh})"
    if kind == "asr":
        stype = "int64_t" if is_64 else "int32_t"
        utype = "uint64_t" if is_64 else "uint32_t"
        return f"(({utype})((({stype}){base_e}) >> {sh}))"
    if kind == "uxtw":
        return f"(((uint64_t)(uint32_t)({base_e})) << {sh})"
    if kind == "sxtw":
        return f"(((uint64_t)(int64_t)(int32_t)({base_e})) << {sh})"
    raise ValueError(f"Unsupported shift modifier: {mod}")


def parse_mem_operand(mem_str):
    """Returns (addr_expr, pre_wb_stmt, post_wb_stmt)."""
    mem_str = mem_str.strip()
    if mem_str.endswith("!"):
        inner = mem_str[1:-2].strip()
        parts = [p.strip() for p in inner.split(",")]
        base_r = parts[0]
        off = parse_imm(parts[1]) if len(parts) > 1 else 0
        wb = set_reg(base_r, f"{reg_expr(base_r)} + (int64_t)({off}LL)")
        addr_e = reg_expr(base_r)
        return addr_e, wb, ""
    if "]," in mem_str:
        bpart, opart = mem_str.split("],", 1)
        base_r = bpart[1:].strip()
        off = parse_imm(opart.strip())
        addr_e = reg_expr(base_r)
        wb = set_reg(base_r, f"{reg_expr(base_r)} + (int64_t)({off}LL)")
        return addr_e, "", wb
    inner = mem_str[1:-1].strip()
    parts = [p.strip() for p in inner.split(",")]
    base_r = parts[0]
    if len(parts) == 1:
        return reg_expr(base_r), "", ""
    if parts[1].startswith("#"):
        off = parse_imm(parts[1])
        return f"({reg_expr(base_r)} + (int64_t)({off}LL))", "", ""
    idx_r = parts[1]
    idx_e = reg_expr(idx_r)
    if len(parts) == 3:
        mod = parts[2]
        if mod.startswith("lsl"):
            sh = parse_imm(mod.split()[1])
            idx_e = f"((uint64_t)({idx_e}) << {sh})"
        elif mod.startswith("uxtw"):
            sh = parse_imm(mod.split()[1]) if len(mod.split()) > 1 else 0
            idx_e = f"(((uint64_t)(uint32_t)({idx_e})) << {sh})"
        elif mod.startswith("sxtw"):
            sh = parse_imm(mod.split()[1]) if len(mod.split()) > 1 else 0
            idx_e = f"(((uint64_t)(int64_t)(int32_t)({idx_e})) << {sh})"
    return f"({reg_expr(base_r)} + (uint64_t)({idx_e}))", "", ""


RODATA_TABLE_MAP = {
    ("r44p1", 67800): "g_lifted_r44p1_kbase_caps_table",
    ("r44p1", 65408): "g_lifted_r44p1_gpu_property_mapping",
    ("r44p1", 61524): "g_lifted_r44p1_gpuprops_jtab",
    ("r44p1", 62284): "g_lifted_r44p1_ioctl_jtab1",
    ("r44p1", 62112): "g_lifted_r44p1_ioctl_jtab2",
    ("r44p1", 61968): "g_lifted_r44p1_ioctl_jtab3",
    ("husky_r54p1", 89160): "g_lifted_husky_r54p1_kbase_caps_table",
    ("husky_r54p1", 78896): "g_lifted_husky_r54p1_gpu_property_mapping",
    ("husky_r54p1", 78776): "g_lifted_husky_r54p1_gpuprops_jtab",
    ("husky_r54p1", 88900): "g_lifted_husky_r54p1_ioctl_jtab1",
    ("husky_r54p1", 88584): "g_lifted_husky_r54p1_ioctl_jtab2",
    ("husky_r54p1", 88728): "g_lifted_husky_r54p1_ioctl_jtab3",
    ("husky_r54p1", 88456): "g_lifted_husky_r54p1_ioctl_jtab4",
}


def lift_instruction(label, fn_name, ins, relas, sym_by_addr, ro_idx, text_idx):
    pc = ins["addr"]
    mnem = ins["mnemonic"]
    ops_str = ins["operands"]
    ops = split_operands(ops_str) if ops_str else []
    next_pc = pc + 4
    rel = relas.get(pc)

    if mnem in ("hint", "nop", "prfm", "isb", "msr", "dmb", "dsb"):
        return f"pc = 0x{next_pc:06x}; break;"
    if mnem in ("brk", "ret"):
        return "return (int64_t)(int32_t)cpu->x[0];"
    if mnem == ".word":
        w = int(ins["hex"], 16)
        if w == 0xB82A013F:
            # stadd w10, [x9]
            return f"*(uint32_t *)(uintptr_t)cpu->x[9] += (uint32_t)cpu->x[10]; pc = 0x{next_pc:06x}; break;"
        if w == 0xF829011F:
            # stadd x9, [x8]
            return f"*(uint64_t *)(uintptr_t)cpu->x[8] += cpu->x[9]; pc = 0x{next_pc:06x}; break;"
        if w == 0xB829311F:
            # stset w9, [x8]
            return f"*(uint32_t *)(uintptr_t)cpu->x[8] |= (uint32_t)cpu->x[9]; pc = 0x{next_pc:06x}; break;"
        if w == 0x88AA7D02:
            # casal w10, w2, [x8]
            return (
                "{ uint32_t _cur = *(uint32_t *)(uintptr_t)cpu->x[8]; "
                "if (_cur == (uint32_t)cpu->x[10]) *(uint32_t *)(uintptr_t)cpu->x[8] = (uint32_t)cpu->x[2]; "
                f"cpu->x[10] = _cur; }} pc = 0x{next_pc:06x}; break;"
            )
        if w == 0x88A97EA2:
            # casal w9, w2, [x21]
            return (
                "{ uint32_t _cur = *(uint32_t *)(uintptr_t)cpu->x[21]; "
                "if (_cur == (uint32_t)cpu->x[9]) *(uint32_t *)(uintptr_t)cpu->x[21] = (uint32_t)cpu->x[2]; "
                f"cpu->x[9] = _cur; }} pc = 0x{next_pc:06x}; break;"
            )
        if w == 0x88E8FC02:
            # casal w8, w2, [x0]
            return (
                "{ uint32_t _cur = *(uint32_t *)(uintptr_t)cpu->x[0]; "
                "if (_cur == (uint32_t)cpu->x[8]) *(uint32_t *)(uintptr_t)cpu->x[0] = (uint32_t)cpu->x[2]; "
                f"cpu->x[8] = _cur; }} pc = 0x{next_pc:06x}; break;"
            )
        if w == 0x88E9FC02:
            # casal w9, w2, [x0]
            return (
                "{ uint32_t _cur = *(uint32_t *)(uintptr_t)cpu->x[0]; "
                "if (_cur == (uint32_t)cpu->x[9]) *(uint32_t *)(uintptr_t)cpu->x[0] = (uint32_t)cpu->x[2]; "
                f"cpu->x[9] = _cur; }} pc = 0x{next_pc:06x}; break;"
            )
        return f"pc = 0x{next_pc:06x}; break;"

    if mnem == "mrs":
        dst, sysreg = ops[0], ops[1].upper()
        if sysreg == "SP_EL0":
            return f"{set_reg(dst, 'cpu->sp_el0')} pc = 0x{next_pc:06x}; break;"
        if sysreg == "DAIF":
            return f"{set_reg(dst, 'cpu->daif')} pc = 0x{next_pc:06x}; break;"
        if sysreg == "TTBR1_EL1":
            return f"{set_reg(dst, 'cpu->ttbr1_el1')} pc = 0x{next_pc:06x}; break;"
        raise ValueError(f"Unhandled mrs {sysreg}")

    if mnem == "adrp":
        dst = ops[0]
        if rel:
            rtype, sname, shndx, sval, addend = rel
            if shndx == ro_idx:
                off_ro = sval + addend
                tbl = RODATA_TABLE_MAP.get((label, off_ro))
                if tbl:
                    return f"{set_reg(dst, f'(uintptr_t){tbl}')} pc = 0x{next_pc:06x}; break;"
                return f"{set_reg(dst, '(uintptr_t)&cpu->dummy_global')} pc = 0x{next_pc:06x}; break;"
            if sname == "_totalram_pages":
                return f"{set_reg(dst, '(uintptr_t)&cpu->totalram_pages')} pc = 0x{next_pc:06x}; break;"
            if sname == "kmalloc_caches":
                return f"{set_reg(dst, '(uintptr_t)&cpu->kmalloc_cache_dummy')} pc = 0x{next_pc:06x}; break;"
            if sname == "cpu_hwcaps":
                return f"{set_reg(dst, '(uintptr_t)&cpu->cpu_hwcaps')} pc = 0x{next_pc:06x}; break;"
            if label == "r44p1" and (sval + addend) == 0xBCA0:
                return f"{set_reg(dst, '(uintptr_t)&cpu->dev_list_head')} pc = 0x{next_pc:06x}; break;"
            return f"{set_reg(dst, '(uintptr_t)&cpu->dummy_global')} pc = 0x{next_pc:06x}; break;"
        return f"{set_reg(dst, '0ULL')} pc = 0x{next_pc:06x}; break;"

    if mnem == "adr":
        dst = ops[0]
        off = parse_imm(ops[1])
        return f"{set_reg(dst, f'0x{pc + off:06x}ULL')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("add", "adds", "sub", "subs", "and", "ands", "orr", "eor", "bic", "bics"):
        if mnem == "add" and rel and rel[0] == 277:
            dst, src1 = ops[0], ops[1]
            return f"{set_reg(dst, reg_expr(src1))} pc = 0x{next_pc:06x}; break;"
        dst, src1 = ops[0], ops[1]
        is_64 = dst.startswith("x") or dst == "sp"
        op1 = reg_expr(src1)
        op2 = parse_shifted_op(ops[2:], is_64)
        utype = "uint64_t" if is_64 else "uint32_t"
        if mnem == "add":
            return f"{set_reg(dst, f'({utype})({op1}) + ({utype})({op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "adds":
            fn = "lifted_add64_flags" if is_64 else "lifted_add32_flags"
            return f"{set_reg(dst, f'{fn}(cpu, {op1}, {op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "sub":
            return f"{set_reg(dst, f'({utype})({op1}) - ({utype})({op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "subs":
            fn = "lifted_sub64_flags" if is_64 else "lifted_sub32_flags"
            return f"{set_reg(dst, f'{fn}(cpu, {op1}, {op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "and":
            return f"{set_reg(dst, f'({utype})({op1}) & ({utype})({op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "ands":
            fn = "lifted_and64_flags" if is_64 else "lifted_and32_flags"
            return f"{set_reg(dst, f'{fn}(cpu, {op1}, {op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "bic":
            return f"{set_reg(dst, f'({utype})({op1}) & ~(({utype})({op2}))')} pc = 0x{next_pc:06x}; break;"
        if mnem == "bics":
            fn = "lifted_and64_flags" if is_64 else "lifted_and32_flags"
            return f"{set_reg(dst, f'{fn}(cpu, {op1}, ~(({utype})({op2})))')} pc = 0x{next_pc:06x}; break;"
        if mnem == "orr":
            return f"{set_reg(dst, f'({utype})({op1}) | ({utype})({op2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "eor":
            return f"{set_reg(dst, f'({utype})({op1}) ^ ({utype})({op2})')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("cmp", "cmn", "tst"):
        src1 = ops[0]
        is_64 = src1.startswith("x") or src1 == "sp"
        op1 = reg_expr(src1)
        op2 = parse_shifted_op(ops[1:], is_64)
        if mnem == "cmp":
            fn = "lifted_sub64_flags" if is_64 else "lifted_sub32_flags"
            return f"(void){fn}(cpu, {op1}, {op2}); pc = 0x{next_pc:06x}; break;"
        if mnem == "cmn":
            fn = "lifted_add64_flags" if is_64 else "lifted_add32_flags"
            return f"(void){fn}(cpu, {op1}, {op2}); pc = 0x{next_pc:06x}; break;"
        if mnem == "tst":
            fn = "lifted_and64_flags" if is_64 else "lifted_and32_flags"
            return f"(void){fn}(cpu, {op1}, {op2}); pc = 0x{next_pc:06x}; break;"

    if mnem == "ccmp":
        src1, op2_t, nzcv_t, cc = ops[0], ops[1], ops[2], ops[3]
        is_64 = src1.startswith("x")
        op1 = reg_expr(src1)
        op2 = parse_shifted_op([op2_t], is_64)
        nzcv_imm = parse_imm(nzcv_t)
        fn = "lifted_sub64_flags" if is_64 else "lifted_sub32_flags"
        return (
            f"if {cond_expr(cc)} {{ (void){fn}(cpu, {op1}, {op2}); }} "
            f"else {{ lifted_set_nzcv_imm(cpu, {nzcv_imm}); }} "
            f"pc = 0x{next_pc:06x}; break;"
        )

    if mnem == "mov":
        dst, src = ops[0], ops[1]
        is_64 = dst.startswith("x") or dst == "sp"
        val_e = parse_shifted_op([src], is_64)
        return f"{set_reg(dst, val_e)} pc = 0x{next_pc:06x}; break;"

    if mnem == "mvn":
        dst, src = ops[0], ops[1]
        is_64 = dst.startswith("x")
        val_e = parse_shifted_op(ops[1:], is_64)
        return f"{set_reg(dst, f'~({val_e})')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("sxtw", "sxth", "sxtb"):
        dst, src = ops[0], ops[1]
        if mnem == "sxtw":
            return f"{set_reg(dst, f'(uint64_t)(int64_t)(int32_t)({reg_expr(src)})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "sxth":
            return f"{set_reg(dst, f'(uint64_t)(int64_t)(int16_t)({reg_expr(src)})')} pc = 0x{next_pc:06x}; break;"
        return f"{set_reg(dst, f'(uint64_t)(int64_t)(int8_t)({reg_expr(src)})')} pc = 0x{next_pc:06x}; break;"

    if mnem == "movk":
        dst = ops[0]
        imm = parse_imm(ops[1]) & 0xFFFF
        sh = parse_imm(ops[2].split()[1]) if len(ops) > 2 else 0
        mask = ~(0xFFFF << sh) & 0xFFFFFFFFFFFFFFFF
        if dst.startswith("w"):
            idx = int(dst[1:])
            return f"cpu->x[{idx}] = (uint64_t)(uint32_t)(((uint32_t)cpu->x[{idx}] & 0x{mask & 0xFFFFFFFF:08x}U) | (0x{imm:x}U << {sh})); pc = 0x{next_pc:06x}; break;"
        idx = int(dst[1:])
        return f"cpu->x[{idx}] = (cpu->x[{idx}] & 0x{mask:016x}ULL) | ((uint64_t)0x{imm:x}ULL << {sh}); pc = 0x{next_pc:06x}; break;"

    if mnem in ("lsl", "lsr", "mul", "umulh", "udiv"):
        dst, s1, s2 = ops[0], ops[1], ops[2]
        is_64 = dst.startswith("x")
        e1 = reg_expr(s1)
        e2 = parse_shifted_op([s2], is_64)
        if mnem == "lsl":
            mask = 63 if is_64 else 31
            return f"{set_reg(dst, f'({e1}) << (({e2}) & {mask})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "lsr":
            mask = 63 if is_64 else 31
            return f"{set_reg(dst, f'({e1}) >> (({e2}) & {mask})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "mul":
            return f"{set_reg(dst, f'({e1}) * ({e2})')} pc = 0x{next_pc:06x}; break;"
        if mnem == "umulh":
            return f"{set_reg(dst, f'(uint64_t)(((__uint128_t)({e1}) * (__uint128_t)({e2})) >> 64)')} pc = 0x{next_pc:06x}; break;"
        if mnem == "udiv":
            return f"{set_reg(dst, f'(({e2}) != 0) ? (({e1}) / ({e2})) : 0ULL')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("msub", "madd"):
        dst, s1, s2, s3 = ops[0], ops[1], ops[2], ops[3]
        e1, e2, e3 = reg_expr(s1), reg_expr(s2), reg_expr(s3)
        op_ch = "-" if mnem == "msub" else "+"
        return f"{set_reg(dst, f'({e3}) {op_ch} (({e1}) * ({e2}))')} pc = 0x{next_pc:06x}; break;"

    if mnem == "ubfx":
        dst, src, lsb_s, wid_s = ops[0], ops[1], ops[2], ops[3]
        lsb, wid = parse_imm(lsb_s), parse_imm(wid_s)
        mask = (1 << wid) - 1
        return f"{set_reg(dst, f'(({reg_expr(src)}) >> {lsb}) & 0x{mask:x}ULL')} pc = 0x{next_pc:06x}; break;"

    if mnem == "ubfiz":
        dst, src, lsb_s, wid_s = ops[0], ops[1], ops[2], ops[3]
        lsb, wid = parse_imm(lsb_s), parse_imm(wid_s)
        mask = (1 << wid) - 1
        return f"{set_reg(dst, f'(((uint64_t)({reg_expr(src)}) & 0x{mask:x}ULL) << {lsb})')} pc = 0x{next_pc:06x}; break;"

    if mnem == "bfi":
        dst, src, lsb_s, wid_s = ops[0], ops[1], ops[2], ops[3]
        lsb, wid = parse_imm(lsb_s), parse_imm(wid_s)
        smask = (1 << wid) - 1
        dmask = ~(smask << lsb) & 0xFFFFFFFFFFFFFFFF
        return f"{set_reg(dst, f'(({reg_expr(dst)}) & 0x{dmask:016x}ULL) | (((uint64_t)({reg_expr(src)}) & 0x{smask:x}ULL) << {lsb})')} pc = 0x{next_pc:06x}; break;"

    if mnem == "bfxil":
        dst, src, lsb_s, wid_s = ops[0], ops[1], ops[2], ops[3]
        lsb, wid = parse_imm(lsb_s), parse_imm(wid_s)
        smask = (1 << wid) - 1
        dmask = ~smask & 0xFFFFFFFFFFFFFFFF
        return f"{set_reg(dst, f'(({reg_expr(dst)}) & 0x{dmask:016x}ULL) | ((({reg_expr(src)}) >> {lsb}) & 0x{smask:x}ULL)')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("csel", "csinv", "csinc", "csneg"):
        dst, s1, s2, cc = ops[0], ops[1], ops[2], ops[3]
        e1, e2 = reg_expr(s1), reg_expr(s2)
        if mnem == "csel":
            alt = e2
        elif mnem == "csinv":
            alt = f"~({e2})"
        elif mnem == "csinc":
            alt = f"(({e2}) + 1)"
        else:
            alt = f"(-({e2}))"
        return f"{set_reg(dst, f'{cond_expr(cc)} ? ({e1}) : ({alt})')} pc = 0x{next_pc:06x}; break;"

    if mnem == "cset":
        dst, cc = ops[0], ops[1]
        return f"{set_reg(dst, f'{cond_expr(cc)} ? 1ULL : 0ULL')} pc = 0x{next_pc:06x}; break;"

    if mnem in ("ldr", "ldur", "ldar", "ldxr", "ldrb", "ldrh", "ldurh", "ldrsb", "ldrsw"):
        dst = ops[0]
        addr_e, pre_wb, post_wb = parse_mem_operand(", ".join(ops[1:]))
        if mnem in ("ldr", "ldur", "ldar", "ldxr"):
            ctype = "uint64_t" if dst.startswith("x") else "uint32_t"
            load_e = f"*({ctype} *)(uintptr_t)({addr_e})"
        elif mnem == "ldrb":
            load_e = f"(uint32_t)(*(uint8_t *)(uintptr_t)({addr_e}))"
        elif mnem in ("ldrh", "ldurh"):
            load_e = f"(uint32_t)(*(uint16_t *)(uintptr_t)({addr_e}))"
        elif mnem == "ldrsb":
            ctype = "uint64_t" if dst.startswith("x") else "uint32_t"
            load_e = f"({ctype})(int64_t)(*(int8_t *)(uintptr_t)({addr_e}))"
        elif mnem == "ldrsw":
            load_e = f"(uint64_t)(int64_t)(*(int32_t *)(uintptr_t)({addr_e}))"
        return f"{{ {pre_wb} {set_reg(dst, load_e)} {post_wb} }} pc = 0x{next_pc:06x}; break;"

    if mnem in ("str", "stur", "strb", "strh", "sturh"):
        src = ops[0]
        addr_e, pre_wb, post_wb = parse_mem_operand(", ".join(ops[1:]))
        if mnem in ("str", "stur"):
            ctype = "uint64_t" if (src.startswith("x") or src == "xzr") else "uint32_t"
        elif mnem == "strb":
            ctype = "uint8_t"
        elif mnem in ("strh", "sturh"):
            ctype = "uint16_t"
        return f"{{ {pre_wb} *({ctype} *)(uintptr_t)({addr_e}) = ({ctype})({reg_expr(src)}); {post_wb} }} pc = 0x{next_pc:06x}; break;"

    if mnem in ("stxr", "stlxr"):
        status_r, src_r = ops[0], ops[1]
        addr_e, pre_wb, post_wb = parse_mem_operand(", ".join(ops[2:]))
        ctype = "uint64_t" if (src_r.startswith("x") or src_r == "xzr") else "uint32_t"
        return f"{{ {pre_wb} *({ctype} *)(uintptr_t)({addr_e}) = ({ctype})({reg_expr(src_r)}); {set_reg(status_r, '0U')} {post_wb} }} pc = 0x{next_pc:06x}; break;"

    if mnem == "ldp":
        r1, r2 = ops[0], ops[1]
        addr_e, pre_wb, post_wb = parse_mem_operand(", ".join(ops[2:]))
        is_64 = r1.startswith("x")
        ctype = "uint64_t" if is_64 else "uint32_t"
        step = 8 if is_64 else 4
        return (
            f"{{ {pre_wb} uintptr_t _a = (uintptr_t)({addr_e}); "
            f"{ctype} _v1 = *({ctype} *)_a; {ctype} _v2 = *({ctype} *)(_a + {step}); "
            f"{set_reg(r1, '_v1')} {set_reg(r2, '_v2')} {post_wb} }} pc = 0x{next_pc:06x}; break;"
        )

    if mnem == "stp":
        r1, r2 = ops[0], ops[1]
        addr_e, pre_wb, post_wb = parse_mem_operand(", ".join(ops[2:]))
        is_64 = r1.startswith("x") or r1 == "xzr"
        ctype = "uint64_t" if is_64 else "uint32_t"
        step = 8 if is_64 else 4
        return (
            f"{{ {pre_wb} uintptr_t _a = (uintptr_t)({addr_e}); "
            f"*({ctype} *)_a = ({ctype})({reg_expr(r1)}); "
            f"*({ctype} *)(_a + {step}) = ({ctype})({reg_expr(r2)}); "
            f"{post_wb} }} pc = 0x{next_pc:06x}; break;"
        )

    if mnem == "b":
        off = parse_imm(ops[0])
        return f"pc = 0x{pc + off:06x}; break;"

    if mnem.startswith("b."):
        cc = mnem.split(".")[1]
        off = parse_imm(ops[0])
        return f"pc = {cond_expr(cc)} ? 0x{pc + off:06x} : 0x{next_pc:06x}; break;"

    if mnem in ("cbz", "cbnz"):
        r, off_s = ops[0], ops[1]
        off = parse_imm(off_s)
        op = "==" if mnem == "cbz" else "!="
        return f"pc = ({reg_expr(r)} {op} 0) ? 0x{pc + off:06x} : 0x{next_pc:06x}; break;"

    if mnem in ("tbz", "tbnz"):
        r, bit_s, off_s = ops[0], ops[1], ops[2]
        bit, off = parse_imm(bit_s), parse_imm(off_s)
        op = "==" if mnem == "tbz" else "!="
        return f"pc = (((({reg_expr(r)}) >> {bit}) & 1ULL) {op} 0) ? 0x{pc + off:06x} : 0x{next_pc:06x}; break;"

    if mnem == "br":
        r = ops[0]
        return f"pc = (uint32_t)({reg_expr(r)}); break;"

    if mnem == "blr":
        return f"cpu->calls_executed++; lifted_kernel_shim_dispatch(cpu, \"{label}\", \"__indirect_blr\"); pc = 0x{next_pc:06x}; break;"

    if mnem == "bl":
        if rel:
            rtype, sname, shndx, sval, addend = rel
            target = sname if sname else sym_by_addr.get(sval + addend, f"sub_{sval + addend:x}")
        else:
            off = parse_imm(ops[0])
            taddr = pc + off
            target = sym_by_addr.get(taddr, f"sub_{taddr:x}")
        if target in DIRECT_LIFTED_CALLEES:
            return f"cpu->calls_executed++; (void)lifted_{label}_{target}(cpu); pc = 0x{next_pc:06x}; break;"
        return f"cpu->calls_executed++; lifted_kernel_shim_dispatch(cpu, \"{label}\", \"{target}\"); pc = 0x{next_pc:06x}; break;"

    raise ValueError(f"Unhandled mnemonic {mnem} ({ins['raw']}) in {fn_name}")


def main():
    for cand in (
        os.path.join(REPO_ROOT, "build-x86-64/aot/horizon2llvm"),
        os.path.join(REPO_ROOT, "src/aot/bin/horizon2llvm"),
    ):
        if os.path.exists(cand) and os.access(cand, os.X_OK):
            r44_ko = os.path.join(REPO_ROOT, "mali_kbase.ko")
            husky_ko = (
                os.path.join(REPO_ROOT, "kernel/mali_kbase_husky.ko")
                if os.path.exists(os.path.join(REPO_ROOT, "kernel/mali_kbase_husky.ko"))
                else os.path.join(REPO_ROOT, "../bundle/kernel/mali_kbase_husky.ko")
            )
            hdr_path = os.path.join(REPO_ROOT, "src/emulator/lifted_kbase_ko.h")
            src_path = os.path.join(REPO_ROOT, "src/emulator/lifted_kbase_ko.c")
            ir_path = os.path.join(REPO_ROOT, "artifacts/unified_horizon2llvm_kbase_ko.ll")
            os.makedirs(os.path.dirname(ir_path), exist_ok=True)
            rc = os.system(f'"{cand}" --lift-ko "{r44_ko}" "{husky_ko}" "{hdr_path}" "{src_path}" --out-ir "{ir_path}"')
            if rc == 0:
                return
    dis = LLVMMCDisassembler()
    mods = {
        "r44p1": parse_ko(os.path.join(REPO_ROOT, "mali_kbase.ko")),
        "husky_r54p1": parse_ko(os.path.join(REPO_ROOT, "kernel/mali_kbase_husky.ko")),
    }

    hdr_path = os.path.join(REPO_ROOT, "src/emulator/lifted_kbase_ko.h")
    src_path = os.path.join(REPO_ROOT, "src/emulator/lifted_kbase_ko.c")

    with open(hdr_path, "w") as hf:
        hf.write("""/* Auto-generated by src/emulator/lift_kbase_ko.py — DO NOT EDIT MANUALLY */
#ifndef LIFTED_KBASE_KO_H
#define LIFTED_KBASE_KO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LiftedKbaseCPU LiftedKbaseCPU;
typedef void (*LiftedKbaseShimHook)(LiftedKbaseCPU *cpu, const char *ko_profile, const char *callee);

struct LiftedKbaseCPU {
    uint64_t x[31];
    uint64_t sp;
    uint64_t x18;
    uint8_t n, z, c, v;
    uint64_t sp_el0;
    uint64_t daif;
    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t totalram_pages;
    uint64_t kmalloc_cache_dummy;
    uint64_t cpu_hwcaps;
    uint64_t dummy_global;
    uint64_t dev_list_head[2];
    uint64_t insns_executed;
    uint64_t calls_executed;
    void *user_ctx;
    LiftedKbaseShimHook shim_hook;
    uint32_t heap_off;
    uint64_t scs_stack[128];
    uint8_t stack[65536] __attribute__((aligned(16)));
    uint8_t task_struct[4096] __attribute__((aligned(16)));
    uint8_t heap_arena[65536] __attribute__((aligned(16)));
};

void lifted_kbase_cpu_init(LiftedKbaseCPU *cpu, void *user_ctx, LiftedKbaseShimHook shim_hook);

""")
        for label in ("r44p1", "husky_r54p1"):
            sym_by_name = mods[label]["sym_by_name"]
            for fn in TARGET_FUNCTIONS:
                if fn in sym_by_name:
                    hf.write(f"int64_t lifted_{label}_{fn}(LiftedKbaseCPU *cpu);\n")
        hf.write("""
#ifdef __cplusplus
}
#endif

#endif /* LIFTED_KBASE_KO_H */
""")

    with open(src_path, "w") as cf:
        cf.write("""/* Auto-generated by src/emulator/lift_kbase_ko.py from mali_kbase.ko (MT6878 r44p1)
 * and kernel/mali_kbase_husky.ko (Pixel 8 Pro Tensor G3 r54p1/r54p3).
 * Every function below is statically lifted instruction-for-instruction from the
 * kernel module .text section with relocated .rodata tables embedded.
 */
#include "lifted_kbase_ko.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t lifted_sub32_flags(LiftedKbaseCPU *cpu, uint32_t a, uint32_t b) {
    uint32_t res = a - b;
    cpu->n = (res >> 31) & 1U;
    cpu->z = (res == 0U) ? 1U : 0U;
    cpu->c = (a >= b) ? 1U : 0U;
    cpu->v = (((a ^ b) & (a ^ res)) >> 31) & 1U;
    return res;
}

static inline uint64_t lifted_sub64_flags(LiftedKbaseCPU *cpu, uint64_t a, uint64_t b) {
    uint64_t res = a - b;
    cpu->n = (res >> 63) & 1U;
    cpu->z = (res == 0ULL) ? 1U : 0U;
    cpu->c = (a >= b) ? 1U : 0U;
    cpu->v = (((a ^ b) & (a ^ res)) >> 63) & 1U;
    return res;
}

static inline uint32_t lifted_add32_flags(LiftedKbaseCPU *cpu, uint32_t a, uint32_t b) {
    uint64_t wide = (uint64_t)a + (uint64_t)b;
    uint32_t res = (uint32_t)wide;
    cpu->n = (res >> 31) & 1U;
    cpu->z = (res == 0U) ? 1U : 0U;
    cpu->c = (wide >> 32) ? 1U : 0U;
    cpu->v = ((~(a ^ b) & (a ^ res)) >> 31) & 1U;
    return res;
}

static inline uint64_t lifted_add64_flags(LiftedKbaseCPU *cpu, uint64_t a, uint64_t b) {
    __uint128_t wide = (__uint128_t)a + (__uint128_t)b;
    uint64_t res = (uint64_t)wide;
    cpu->n = (res >> 63) & 1U;
    cpu->z = (res == 0ULL) ? 1U : 0U;
    cpu->c = (wide >> 64) ? 1U : 0U;
    cpu->v = ((~(a ^ b) & (a ^ res)) >> 63) & 1U;
    return res;
}

static inline uint32_t lifted_and32_flags(LiftedKbaseCPU *cpu, uint32_t a, uint32_t b) {
    uint32_t res = a & b;
    cpu->n = (res >> 31) & 1U;
    cpu->z = (res == 0U) ? 1U : 0U;
    cpu->c = 0U;
    cpu->v = 0U;
    return res;
}

static inline uint64_t lifted_and64_flags(LiftedKbaseCPU *cpu, uint64_t a, uint64_t b) {
    uint64_t res = a & b;
    cpu->n = (res >> 63) & 1U;
    cpu->z = (res == 0ULL) ? 1U : 0U;
    cpu->c = 0U;
    cpu->v = 0U;
    return res;
}

static inline void lifted_set_nzcv_imm(LiftedKbaseCPU *cpu, uint32_t imm4) {
    cpu->n = (imm4 >> 3) & 1U;
    cpu->z = (imm4 >> 2) & 1U;
    cpu->c = (imm4 >> 1) & 1U;
    cpu->v = imm4 & 1U;
}

void lifted_kbase_cpu_init(LiftedKbaseCPU *cpu, void *user_ctx, LiftedKbaseShimHook shim_hook) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->sp = (uint64_t)(uintptr_t)(cpu->stack + sizeof(cpu->stack) - 256);
    cpu->x18 = (uint64_t)(uintptr_t)&cpu->scs_stack[16];
    cpu->x[18] = cpu->x18;
    cpu->sp_el0 = (uint64_t)(uintptr_t)&cpu->task_struct[0];
    /* Populate stack canary and current->mm pointers in simulated task_struct */
    *(uint64_t *)(cpu->task_struct + 1488) = 0xdecafbad12345678ULL;
    *(uint64_t *)(cpu->task_struct + 1592) = 0xdecafbad12345678ULL;
    *(uint64_t *)(cpu->task_struct + 1304) = (uint64_t)(uintptr_t)(cpu->task_struct + 2048);
    *(uint64_t *)(cpu->task_struct + 1440) = (uint64_t)(uintptr_t)(cpu->task_struct + 2048);
    *(uint64_t *)(cpu->task_struct + 1984) = (uint64_t)(uintptr_t)(cpu->task_struct + 2048);
    cpu->totalram_pages = (12ULL * 1024ULL * 1024ULL * 1024ULL) >> 12;
    cpu->dev_list_head[0] = (uint64_t)(uintptr_t)&cpu->dev_list_head[0];
    cpu->dev_list_head[1] = (uint64_t)(uintptr_t)&cpu->dev_list_head[0];
    cpu->user_ctx = user_ctx;
    cpu->shim_hook = shim_hook;
}

static void lifted_kernel_shim_dispatch(LiftedKbaseCPU *cpu, const char *ko_profile, const char *callee) {
    if (strcmp(callee, "memset") == 0) {
        memset((void *)(uintptr_t)cpu->x[0], (int)(cpu->x[1] & 0xff), (size_t)cpu->x[2]);
        return;
    }
    if (strcmp(callee, "kmalloc_trace") == 0 || strcmp(callee, "kmem_cache_alloc_trace") == 0 ||
        strcmp(callee, "__kmalloc") == 0 || strcmp(callee, "vmalloc") == 0) {
        size_t sz = (strcmp(callee, "vmalloc") == 0 || strcmp(callee, "__kmalloc") == 0)
            ? (size_t)cpu->x[0]
            : (size_t)cpu->x[2];
        if (sz == 0 || sz > 32768U) sz = 4096U;
        size_t aligned = (sz + 15U) & ~15U;
        if (cpu->heap_off + aligned > sizeof(cpu->heap_arena))
            cpu->heap_off = 0U;
        void *ptr = &cpu->heap_arena[cpu->heap_off];
        memset(ptr, 0, aligned);
        cpu->heap_off += (uint32_t)aligned;
        cpu->x[0] = (uint64_t)(uintptr_t)ptr;
        return;
    }
    if (strcmp(callee, "kfree") == 0 || strcmp(callee, "vfree") == 0) {
        cpu->x[0] = 0;
        return;
    }
    if (strcmp(callee, "__arch_copy_from_user") == 0 || strncmp(callee, "_copy_from_user", 15) == 0) {
        if (cpu->x[0] && cpu->x[1] && cpu->x[2]) {
            memcpy((void *)(uintptr_t)cpu->x[0], (const void *)(uintptr_t)cpu->x[1], (size_t)cpu->x[2]);
            cpu->x[0] = 0;
        } else {
            cpu->x[0] = cpu->x[2];
        }
        return;
    }
    if (strcmp(callee, "__arch_copy_to_user") == 0 || strncmp(callee, "_copy_to_user", 13) == 0) {
        if (cpu->x[0] && cpu->x[1] && cpu->x[2]) {
            memcpy((void *)(uintptr_t)cpu->x[0], (const void *)(uintptr_t)cpu->x[1], (size_t)cpu->x[2]);
            cpu->x[0] = 0;
        } else {
            cpu->x[0] = cpu->x[2];
        }
        return;
    }
    if (strcmp(callee, "__check_object_size") == 0) {
        cpu->x[0] = 0;
        return;
    }
    if (strcmp(callee, "__sw_hweight64") == 0) {
        cpu->x[0] = (uint64_t)__builtin_popcountll(cpu->x[0]);
        return;
    }
    if (strcmp(callee, "kbase_has_exec_va_zone") == 0) {
        uint64_t v = *(uint64_t *)((uintptr_t)cpu->x[0] + 37072 + 192);
        cpu->x[0] = (v != ~0ULL && v != 0ULL) ? 1ULL : 0ULL;
        return;
    }
    if (strcmp(callee, "scnprintf") == 0) {
        cpu->x[0] = 16ULL;
        return;
    }
    if (cpu->shim_hook) {
        cpu->shim_hook(cpu, ko_profile, callee);
        return;
    }
    cpu->x[0] = 0;
}
""")
        ro_slices = [
            ("g_lifted_r44p1_kbase_caps_table", mods["r44p1"]["rodata"][67800 : 67800 + 16]),
            ("g_lifted_r44p1_gpu_property_mapping", mods["r44p1"]["rodata"][65408 : 65408 + 1992]),
            ("g_lifted_r44p1_gpuprops_jtab", mods["r44p1"]["rodata"][61524 : 61524 + 32]),
            ("g_lifted_r44p1_ioctl_jtab1", mods["r44p1"]["rodata"][62284 : 62284 + 168]),
            ("g_lifted_r44p1_ioctl_jtab2", mods["r44p1"]["rodata"][62112 : 62112 + 176]),
            ("g_lifted_r44p1_ioctl_jtab3", mods["r44p1"]["rodata"][61968 : 61968 + 148]),
            ("g_lifted_husky_r54p1_kbase_caps_table", mods["husky_r54p1"]["rodata"][89160 : 89160 + 72]),
            ("g_lifted_husky_r54p1_gpu_property_mapping", mods["husky_r54p1"]["rodata"][78896 : 78896 + 2040]),
            ("g_lifted_husky_r54p1_gpuprops_jtab", mods["husky_r54p1"]["rodata"][78776 : 78776 + 32]),
            ("g_lifted_husky_r54p1_ioctl_jtab1", mods["husky_r54p1"]["rodata"][88900 : 88900 + 200]),
            ("g_lifted_husky_r54p1_ioctl_jtab2", mods["husky_r54p1"]["rodata"][88584 : 88584 + 148]),
            ("g_lifted_husky_r54p1_ioctl_jtab3", mods["husky_r54p1"]["rodata"][88728 : 88728 + 176]),
            ("g_lifted_husky_r54p1_ioctl_jtab4", mods["husky_r54p1"]["rodata"][88456 : 88456 + 132]),
        ]
        for arr_name, blob in ro_slices:
            cf.write(f"\nstatic const uint8_t {arr_name}[{len(blob)}] __attribute__((aligned(16))) = {{\n")
            for i in range(0, len(blob), 16):
                chunk = blob[i : i + 16]
                cf.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            cf.write("};\n")

        for label in ("r44p1", "husky_r54p1"):
            m = mods[label]
            text = m["text"]
            sym_by_name = m["sym_by_name"]
            sym_by_addr = m["sym_by_addr"]
            relas = m["relas"]
            ro_idx = m["ro_idx"]
            text_idx = m["text_idx"]
            for fn in TARGET_FUNCTIONS:
                if fn not in sym_by_name:
                    continue
                _, _, val, sz = sym_by_name[fn]
                insns = dis.disasm_range(text, val, val + sz)
                cf.write(f"\n/* Lifted from {label}: {fn} (0x{val:06x}..0x{val+sz:06x}, {len(insns)} insns) */\n")
                cf.write(f"int64_t lifted_{label}_{fn}(LiftedKbaseCPU *cpu) {{\n")
                cf.write(f"    uint32_t pc = 0x{val:06x}U;\n")
                cf.write("    for (;;) {\n")
                cf.write("        cpu->insns_executed++;\n")
                cf.write("        switch (pc) {\n")
                for ins in insns:
                    pc = ins["addr"]
                    stmt = lift_instruction(label, fn, ins, relas, sym_by_addr, ro_idx, text_idx)
                    cf.write(f"        case 0x{pc:06x}U: /* {ins['raw']} */\n")
                    cf.write(f"            {stmt}\n")
                cf.write("        default:\n")
                cf.write("            return (int64_t)(int32_t)cpu->x[0];\n")
                cf.write("        }\n")
                cf.write("    }\n")
                cf.write("}\n")

    print(f"Successfully generated {hdr_path} and {src_path}")


if __name__ == "__main__":
    main()
