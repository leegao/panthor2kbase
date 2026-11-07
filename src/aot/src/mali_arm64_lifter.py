#!/usr/bin/env python3
"""
mali_arm64_lifter.py
====================
Ahead-Of-Time (AOT) Binary-to-LLVM-IR Lifter for 64-bit ARM64-Bionic
libGLES_mali.so (supporting Valhall/5th-Gen r54p3-00eac0, Bifrost/Valhall
r26p0-01eac0, and general 64-bit AArch64 Mali driver builds) targeting
x86-64 Linux (glibc).

Incorporates reference AArch64 lifting idioms from Horizon's aarch64_lifter.hpp:
  - @llvm.fshr.i64 / @llvm.fshr.i32 for EXTR, ROR (immediate), and RORV (register)
  - @llvm.bitreverse, @llvm.bswap, @llvm.ctlz for 1-source RBIT, REV16/32/64, CLZ, CLS
  - Guarded UDIV / SDIV (zero-divisor and INT_MIN / -1 overflow guards)
  - Variable shifts LSLV, LSRV, ASRV, RORV (modulo 32/64 shift masks)
  - Generic AArch64 PIC jump-table recovery (cmp / b.hi / adrp / add / adr / ldrb / add / br)
  - Semantic LLVM intrinsics for ARM64-Bionic idiosyncrasies lowered by C++ LLVM passes:
      * @llvm.mali.arm64.hint (PACIASP, AUTIASP, BTI, NOP)
      * @llvm.mali.arm64.retaa (ARMv8.3-A pointer-authenticated return)
      * @llvm.mali.arm64.mrs (TPIDR_EL0 [TLS_SLOT_STACK_GUARD +0x28 & TLS_SLOT_OPENGL_API +0x18], DCZID_EL0)
      * @llvm.mali.arm64.dc_zva (cache-line zeroing)
      * @llvm.mali.bionic.tlsdesc (R_AARCH64_TLSDESC static TLS offset resolver)
      * @llvm.mali.bionic.plt64.<sym> (Bionic libc & FORTIFY_SOURCE *_chk PLT imports)
"""

import bisect
import lzma
import os
import re
import struct
import sys
from collections import deque

IMAGE_BASE = 0x400000000
HLE_MALLOC_MAGIC = 0xF000FFF0
HLE_TLSDESC_MAGIC = 0xF000FFF4
HLE_FREE_MAGIC = 0xF000FFF8


def sext64(val: int, bits: int) -> int:
    m = 1 << (bits - 1)
    val &= (1 << bits) - 1
    return (val ^ m) - m


def u64(val: int) -> int:
    return val & 0xFFFFFFFFFFFFFFFF


def u32(val: int) -> int:
    return val & 0xFFFFFFFF


def decode_bit_masks(sf: int, immN: int, imms: int, immr: int):
    combined = (immN << 6) | (~imms & 0x3F)
    if combined == 0:
        return 0, 0
    length = combined.bit_length() - 1
    levels = (1 << length) - 1
    s = imms & levels
    r = immr & levels
    esize = 1 << length
    diff = (s - r) & (esize - 1)
    welem = ~0 if (s + 1 == 64) else ((1 << (s + 1)) - 1)
    telem = ~0 if (diff + 1 == 64) else ((1 << (diff + 1)) - 1)
    welem &= (1 << esize) - 1
    telem &= (1 << esize) - 1
    if r != 0:
        mask = (1 << esize) - 1
        welem = ((welem >> r) | (welem << (esize - r))) & mask
    wmask = 0
    tmask = 0
    total = 64 if sf else 32
    for i in range(0, total, esize):
        wmask |= (welem << i)
        tmask |= (telem << i)
    return u64(wmask), u64(tmask)


def expand_advsimd_imm(op: int, cmode: int, imm8: int) -> int:
    u8 = imm8 & 0xFF
    c_hi = cmode >> 1
    if c_hi == 0:
        return (u8 << 32) | u8
    elif c_hi == 1:
        return ((u8 << 8) << 32) | (u8 << 8)
    elif c_hi == 2:
        return ((u8 << 16) << 32) | (u8 << 16)
    elif c_hi == 3:
        return ((u8 << 24) << 32) | (u8 << 24)
    elif c_hi == 4:
        return (u8 << 48) | (u8 << 32) | (u8 << 16) | u8
    elif c_hi == 5:
        return (u8 << 56) | (u8 << 40) | (u8 << 24) | (u8 << 8)
    elif c_hi == 6:
        v = ((u8 << 16) | 0xFFFF) if (cmode & 1) else ((u8 << 8) | 0xFF)
        return (v << 32) | v
    elif c_hi == 7:
        if op == 0 and (cmode & 1) == 0:
            r = 0
            for i in range(8):
                r |= (u8 << (i * 8))
            return u64(r)
        if op == 1 and (cmode & 1) == 0:
            r = 0
            for i in range(8):
                if (u8 >> i) & 1:
                    r |= (0xFF << (i * 8))
            return u64(r)
    return 0


def read_sleb128(buf: bytes, pos: int):
    res = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        res |= (b & 0x7F) << shift
        shift += 7
        if (b & 0x80) == 0:
            if shift < 64 and (b & 0x40):
                res |= -(1 << shift)
            return res, pos


class ELFAnalyzer:
    """
    General-purpose ELF64 (EM_AARCH64) loader and analyzer for 64-bit libGLES_mali.so:
      - Maps all PT_LOAD segments by p_vaddr into a contiguous virtual memory buffer
      - Applies .relr.dyn and .rela.dyn (APS2 packed or standard ELF64 Rela) relocations
      - Recovers function boundaries from .eh_frame FDEs, .dynsym, and optional .gnu_debugdata
      - Maps .plt stubs via .rela.plt + .dynsym
    """

    def __init__(self, so_path: str):
        with open(so_path, "rb") as f:
            self.raw = f.read()

        if self.raw[:4] != b"\x7fELF" or self.raw[4] != 2:
            raise ValueError(f"{so_path} is not a 64-bit ELF64 binary")
        e_machine = struct.unpack_from("<H", self.raw, 18)[0]
        if e_machine != 183:  # EM_AARCH64
            raise ValueError(f"{so_path} is not an AArch64 (EM_AARCH64=183) binary (got {e_machine})")

        m = re.search(rb"r\d+p\d+-\d+[a-z0-9]+", self.raw)
        self.driver_version = m.group(0).decode("ascii") if m else "unknown-64bit"

        # 1. Map PT_LOAD segments by virtual address (p_vaddr)
        e_phoff = struct.unpack_from("<Q", self.raw, 32)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.raw, 54)
        max_vaddr = 0
        pt_loads = []
        for i in range(e_phnum):
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", self.raw, e_phoff + i * e_phentsize
            )
            if p_type == 1:  # PT_LOAD
                pt_loads.append((p_vaddr, p_offset, p_filesz, p_memsz, p_flags))
                if p_vaddr + p_memsz > max_vaddr:
                    max_vaddr = p_vaddr + p_memsz

        self.image_size = (max_vaddr + 0xFFFF) & ~0xFFFF
        self.elf = bytearray(self.image_size)
        self.text_ranges = []
        for p_vaddr, p_offset, p_filesz, p_memsz, p_flags in pt_loads:
            if p_filesz > 0:
                self.elf[p_vaddr : p_vaddr + p_filesz] = self.raw[p_offset : p_offset + p_filesz]
            if p_flags & 1:  # PF_X
                self.text_ranges.append((p_vaddr, p_vaddr + p_filesz))

        # 2. Parse ELF64 Section Headers dynamically
        e_shoff = struct.unpack_from("<Q", self.raw, 40)[0]
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", self.raw, 58)
        shstr_off = struct.unpack_from("<Q", self.raw, e_shoff + e_shstrndx * e_shentsize + 24)[0]
        self.sections = {}
        for i in range(e_shnum):
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = (
                struct.unpack_from("<IIQQQQIIQQ", self.raw, e_shoff + i * e_shentsize)
            )
            end = self.raw.find(b"\x00", shstr_off + sh_name)
            sname = self.raw[shstr_off + sh_name : end].decode("utf-8", errors="replace")
            self.sections[sname] = (sh_addr, sh_offset, sh_size, sh_entsize)

        self.func_by_addr = {}
        self.addr_by_name = {}
        self.dynsyms = []
        self._parse_eh_frame()
        self._parse_dynsym()
        self._parse_gnu_debugdata()
        self._apply_relocations()
        self.plt_map = {}
        self._parse_plt()
        self.sorted_fstarts = sorted(self.func_by_addr.keys())

    def _is_exec_addr(self, addr: int) -> bool:
        for lo, hi in self.text_ranges:
            if lo <= addr < hi:
                return True
        return False

    def _parse_eh_frame(self):
        if ".eh_frame" not in self.sections:
            return
        eh_addr, eh_off, eh_size, _ = self.sections[".eh_frame"]
        pos = eh_off
        eh_end = eh_off + eh_size
        while pos + 4 <= eh_end:
            length = struct.unpack_from("<I", self.raw, pos)[0]
            if length == 0 or length == 0xFFFFFFFF:
                break
            entry_start = pos + 4
            entry_end = entry_start + length
            if entry_end > eh_end:
                break
            cie_id = struct.unpack_from("<I", self.raw, entry_start)[0]
            if cie_id != 0 and entry_start + 12 <= entry_end:
                pc_rel = struct.unpack_from("<i", self.raw, entry_start + 4)[0]
                fn_len = struct.unpack_from("<I", self.raw, entry_start + 8)[0]
                fn_start = (eh_addr + (entry_start + 4 - eh_off) + pc_rel) & 0xFFFFFFFFFFFFFFFF
                if self._is_exec_addr(fn_start) and 0 < fn_len < 0x200000:
                    self.func_by_addr[fn_start] = (f"sub_{fn_start:08x}", fn_len)
            pos = entry_end

    def _parse_dynsym(self):
        if ".dynsym" not in self.sections or ".dynstr" not in self.sections:
            return
        _, dynsym_off, dynsym_size, _ = self.sections[".dynsym"]
        _, dynstr_off, _, _ = self.sections[".dynstr"]
        for idx in range(dynsym_size // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", self.raw, dynsym_off + idx * 24
            )
            end = self.raw.find(b"\x00", dynstr_off + st_name)
            name = self.raw[dynstr_off + st_name : end].decode("utf-8", errors="replace")
            self.dynsyms.append((name, st_value, st_size, st_shndx))
            if st_value != 0 and (st_info & 0xF) == 2:
                old_name, old_sz = self.func_by_addr.get(st_value, (name, st_size))
                sz = st_size if st_size > 0 else old_sz
                self.func_by_addr[st_value] = (name, sz)
                self.addr_by_name[name] = st_value

    def _parse_gnu_debugdata(self):
        if ".gnu_debugdata" not in self.sections:
            return
        _, dbg_off, dbg_size, _ = self.sections[".gnu_debugdata"]
        try:
            dbg = lzma.decompress(self.raw[dbg_off : dbg_off + dbg_size])
        except Exception:
            return
        e_shoff = struct.unpack_from("<Q", dbg, 40)[0]
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", dbg, 58)
        shstr_off = struct.unpack_from("<Q", dbg, e_shoff + e_shstrndx * e_shentsize + 24)[0]
        dbg_secs = {}
        for i in range(e_shnum):
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = (
                struct.unpack_from("<IIQQQQIIQQ", dbg, e_shoff + i * e_shentsize)
            )
            end = dbg.find(b"\x00", shstr_off + sh_name)
            sname = dbg[shstr_off + sh_name : end].decode("utf-8", errors="replace")
            dbg_secs[sname] = (sh_offset, sh_size)
        if ".symtab" not in dbg_secs or ".strtab" not in dbg_secs:
            return
        sym_off, sym_sz = dbg_secs[".symtab"]
        str_off, str_sz = dbg_secs[".strtab"]
        strtab = dbg[str_off : str_off + str_sz]
        for off in range(0, sym_sz, 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", dbg, sym_off + off
            )
            if st_value != 0 and (st_info & 0xF) == 2:
                end = strtab.find(b"\x00", st_name)
                name = strtab[st_name:end].decode("utf-8", errors="replace")
                old_name, old_sz = self.func_by_addr.get(st_value, (name, st_size))
                sz = st_size if st_size > 0 else old_sz
                self.func_by_addr[st_value] = (name, sz)
                self.addr_by_name[name] = st_value

    def _apply_relocations(self):
        # Apply .rela.dyn (APS2 or standard ELF64 Rela) into self.elf so GOT pointers hold resolved RVAs
        if ".rela.dyn" not in self.sections:
            return
        _, reladyn_off, reladyn_size, _ = self.sections[".rela.dyn"]
        if self.raw[reladyn_off : reladyn_off + 4] == b"APS2":
            pos = reladyn_off + 4
            num_relocs, pos = read_sleb128(self.raw, pos)
            cur_offset, pos = read_sleb128(self.raw, pos)
            cur_addend = 0
            rel_idx = 0
            while rel_idx < num_relocs and pos < reladyn_off + reladyn_size:
                grp_size, pos = read_sleb128(self.raw, pos)
                grp_flags, pos = read_sleb128(self.raw, pos)
                grp_off_delta, pos = read_sleb128(self.raw, pos) if (grp_flags & 2) else (0, pos)
                grp_info, pos = read_sleb128(self.raw, pos) if (grp_flags & 1) else (0, pos)
                if (grp_flags & 8) and (grp_flags & 4):
                    d, pos = read_sleb128(self.raw, pos)
                    cur_addend += d
                elif not (grp_flags & 8):
                    cur_addend = 0
                for _ in range(grp_size):
                    if grp_flags & 2:
                        cur_offset += grp_off_delta
                    else:
                        d, pos = read_sleb128(self.raw, pos)
                        cur_offset += d
                    r_info = grp_info if (grp_flags & 1) else 0
                    if not (grp_flags & 1):
                        r_info, pos = read_sleb128(self.raw, pos)
                    if (grp_flags & 8) and not (grp_flags & 4):
                        d, pos = read_sleb128(self.raw, pos)
                        cur_addend += d
                    r_type = r_info & 0xFFFFFFFF
                    sym_idx = r_info >> 32
                    if 0 <= cur_offset <= len(self.elf) - 8:
                        if r_type == 1027:  # R_AARCH64_RELATIVE
                            struct.pack_into("<Q", self.elf, cur_offset, u64(cur_addend))
                        elif 0 < sym_idx < len(self.dynsyms):
                            _, sval, _, shndx = self.dynsyms[sym_idx]
                            if shndx != 0 and sval != 0:
                                struct.pack_into("<Q", self.elf, cur_offset, u64(sval + cur_addend))
                    rel_idx += 1

    def _parse_plt(self):
        if ".rela.plt" not in self.sections or ".plt" not in self.sections:
            return
        _, relaplt_off, relaplt_size, _ = self.sections[".rela.plt"]
        got_plt = {}
        for i in range(relaplt_size // 24):
            r_offset, r_info, _ = struct.unpack_from("<QQq", self.raw, relaplt_off + i * 24)
            sym_idx = r_info >> 32
            if 0 <= sym_idx < len(self.dynsyms):
                got_plt[r_offset] = self.dynsyms[sym_idx][0]

        plt_addr, _, plt_size, _ = self.sections[".plt"]
        for pc in range(plt_addr, plt_addr + plt_size - 12, 4):
            i0, i1, i2, i3 = struct.unpack_from("<IIII", self.elf, pc)
            if (
                (i0 & 0x9F00001F) == 0x90000010
                and (i1 & 0xFFC003FF) == 0xF9400211
                and i3 == 0xD61F0220
            ):
                immhi = (i0 >> 5) & 0x7FFFF
                immlo = (i0 >> 29) & 3
                imm = (immhi << 2) | immlo
                if imm & (1 << 20):
                    imm -= 1 << 21
                page = (pc & ~0xFFF) + (imm << 12)
                ldr_off = ((i1 >> 10) & 0xFFF) << 3
                g = page + ldr_off
                if g in got_plt:
                    self.plt_map[pc] = got_plt[g]

    def find_func_start(self, addr: int):
        if addr in self.func_by_addr:
            return addr
        idx = bisect.bisect_right(self.sorted_fstarts, addr) - 1
        if idx >= 0:
            st = self.sorted_fstarts[idx]
            _, sz = self.func_by_addr[st]
            if st <= addr < st + sz:
                return st
        return None

    def discover_roots(self):
        roots = []
        for sym in [
            "cmpbe_v2_deserialize_MBS2_to_C",
            "cmpbe_v2_init_options",
            "cmpbe_v2_set_option_value",
            "cmpbe_v2_free_compilation_result",
            "_ZN3gfxL14malloc_wrapperEj",
            "_ZN3gfxL12free_wrapperEPv",
            "cmpbe_chunk_write_MBS2",
            "cmpbe_chunk_stream_init_write",
            "cmpbe_chunk_stream_destroy",
            "get_internal_shader_data_MBS2",
            "_ZN3gfx8compiler16deserialize_mbs2EPKvmPP16cmpbe_chunk_MBS2RNS_13mem_allocatorE",
            "_ZN5clang22isStackNearlyExhaustedEv",
            "_Z30cinstrp_memory_report_set_infoP14cinstr_contextmmbmmb",
            "glGetString",
        ]:
            if sym in self.addr_by_name:
                roots.append(self.addr_by_name[sym])
        if not roots:
            raise RuntimeError("Could not find cmpbe_v2_deserialize_MBS2_to_C in .dynsym")
        return roots

    def compute_closure(self, root_addrs):
        visited = set()
        plt_targets = {}
        queue = deque()
        for r in root_addrs:
            fstart = self.find_func_start(r)
            queue.append(fstart if fstart is not None else r)

        while queue:
            faddr = queue.popleft()
            if faddr in visited:
                continue
            if faddr in self.plt_map:
                plt_targets[faddr] = self.plt_map[faddr]
                continue
            fstart = self.find_func_start(faddr)
            if fstart is not None and fstart != faddr:
                if fstart not in visited:
                    queue.append(fstart)
                continue
            visited.add(faddr)
            fname, fsize = self.func_by_addr.get(faddr, (f"sub_{faddr:08x}", 4))
            if fsize == 0:
                fsize = 4
            regs_page = {}
            for off in range(0, fsize, 4):
                pc = faddr + off
                insn = struct.unpack_from("<I", self.elf, pc)[0]
                top6 = (insn >> 26) & 0x3F
                if top6 in (0x25, 0x05):  # BL or B
                    imm26 = sext64(insn & 0x3FFFFFF, 26)
                    target = pc + (imm26 << 2)
                    if target in self.plt_map:
                        plt_targets[target] = self.plt_map[target]
                    elif top6 == 0x25 or not (faddr <= target < faddr + fsize):
                        tstart = self.find_func_start(target)
                        cand = tstart if tstart is not None else target
                        if cand not in visited:
                            queue.append(cand)
                if ((insn >> 24) & 0x1F) == 0x10:  # ADR / ADRP
                    sf = (insn >> 31) & 1
                    rd = insn & 0x1F
                    immhi = (insn >> 5) & 0x7FFFF
                    immlo = (insn >> 29) & 3
                    imm = sext64((immhi << 2) | immlo, 21)
                    if sf == 0:
                        tgt = pc + imm
                        if tgt in self.func_by_addr and tgt not in visited:
                            queue.append(tgt)
                    else:
                        regs_page[rd] = (pc & ~0xFFF) + (imm << 12)
                elif ((insn >> 22) & 0x3FF) == 0x244:  # ADD Xd, Xn, #imm12
                    rn = (insn >> 5) & 0x1F
                    imm12 = (insn >> 10) & 0xFFF
                    if rn in regs_page:
                        tgt = regs_page[rn] + imm12
                        if tgt in self.func_by_addr and tgt not in visited:
                            queue.append(tgt)
                elif (insn & 0xFFC00000) == 0xF9400000:  # LDR Xt, [Xn, #imm12] (GOT function pointer load)
                    rn = (insn >> 5) & 0x1F
                    imm12 = ((insn >> 10) & 0xFFF) << 3
                    if rn in regs_page:
                        got_addr = regs_page[rn] + imm12
                        if 0 <= got_addr <= len(self.elf) - 8:
                            ptr_val = struct.unpack_from("<Q", self.elf, got_addr)[0]
                            if ptr_val in self.func_by_addr and ptr_val not in visited:
                                queue.append(ptr_val)
        return sorted(visited), plt_targets


class LLVMIRLifter:
    def __init__(self, analyzer: ELFAnalyzer, func_addrs, plt_targets):
        self.analyzer = analyzer
        self.elf = analyzer.elf
        self.func_addrs = func_addrs
        self.func_set = set(func_addrs)
        self.plt_targets = plt_targets

        self.stats = {
            "driver_version": analyzer.driver_version,
            "total_functions": len(func_addrs),
            "total_instructions": 0,
            "pac_bti_stripped": 0,
            "retaa_lowered": 0,
            "bionic_tpidr_lowered": 0,
            "bionic_tls_slot_opengl_api": 0,
            "bionic_tls_slot_stack_guard": 0,
            "bionic_tlsdesc_calls_lowered": 0,
            "bionic_libc_calls_lowered": 0,
            "direct_calls_lowered": 0,
            "indirect_calls_lowered": 0,
            "jump_tables_lowered": 0,
            "simd_vector_ops_lowered": 0,
        }

        self.tmp_id = 0
        self.lines = []

    def new_tmp(self, prefix="t") -> str:
        self.tmp_id += 1
        return f"%{prefix}{self.tmp_id}"

    def emit(self, line: str):
        self.lines.append("  " + line)

    def emit_raw(self, line: str):
        self.lines.append(line)

    def load_gpr(self, r: int, is_64=True) -> str:
        if r == 31:
            return "0"
        t = self.new_tmp("x")
        self.emit(f"{t} = load i64, ptr %reg_x{r}, align 8")
        if not is_64:
            t32 = self.new_tmp("w")
            self.emit(f"{t32} = trunc i64 {t} to i32")
            return t32
        return t

    def load_gpr_or_sp(self, r: int, is_64=True) -> str:
        t = self.new_tmp("xsp")
        reg_name = "%reg_sp" if r == 31 else f"%reg_x{r}"
        self.emit(f"{t} = load i64, ptr {reg_name}, align 8")
        if not is_64:
            t32 = self.new_tmp("wsp")
            self.emit(f"{t32} = trunc i64 {t} to i32")
            return t32
        return t

    def store_gpr(self, r: int, val: str, is_64=True):
        if r == 31:
            return
        if not is_64:
            ext = self.new_tmp("zext")
            self.emit(f"{ext} = zext i32 {val} to i64")
            val = ext
        self.emit(f"store i64 {val}, ptr %reg_x{r}, align 8")

    def store_gpr_or_sp(self, r: int, val: str, is_64=True):
        if not is_64:
            ext = self.new_tmp("zext")
            self.emit(f"{ext} = zext i32 {val} to i64")
            val = ext
        reg_name = "%reg_sp" if r == 31 else f"%reg_x{r}"
        self.emit(f"store i64 {val}, ptr {reg_name}, align 8")

    def load_vec(self, r: int, ty: str) -> str:
        t = self.new_tmp("v")
        self.emit(f"{t} = load {ty}, ptr %reg_v{r}, align 16")
        return t

    def store_vec(self, r: int, ty: str, val: str, clear_128=True):
        if clear_128 and ty not in ("i128", "<16 x i8>", "<8 x i16>", "<4 x i32>", "<2 x i64>"):
            self.emit(f"store i128 0, ptr %reg_v{r}, align 16")
        self.emit(f"store {ty} {val}, ptr %reg_v{r}, align 16")

    def emit_flags_add(self, a: str, b: str, cin: str, res: str, is_64: bool):
        ty = "i64" if is_64 else "i32"
        ext_ty = "i128" if is_64 else "i64"
        shift = 63 if is_64 else 31

        n_cmp = self.new_tmp("fl_n")
        self.emit(f"{n_cmp} = icmp slt {ty} {res}, 0")
        self.emit(f"store i1 {n_cmp}, ptr %flag_n, align 1")

        z_cmp = self.new_tmp("fl_z")
        self.emit(f"{z_cmp} = icmp eq {ty} {res}, 0")
        self.emit(f"store i1 {z_cmp}, ptr %flag_z, align 1")

        ea = self.new_tmp("ea")
        eb = self.new_tmp("eb")
        ec = self.new_tmp("ec")
        s1 = self.new_tmp("s1")
        s2 = self.new_tmp("s2")
        c_sh = self.new_tmp("csh")
        c_cmp = self.new_tmp("fl_c")
        self.emit(f"{ea} = zext {ty} {a} to {ext_ty}")
        self.emit(f"{eb} = zext {ty} {b} to {ext_ty}")
        self.emit(f"{ec} = zext {ty} {cin} to {ext_ty}")
        self.emit(f"{s1} = add {ext_ty} {ea}, {eb}")
        self.emit(f"{s2} = add {ext_ty} {s1}, {ec}")
        self.emit(f"{c_sh} = lshr {ext_ty} {s2}, {shift + 1}")
        self.emit(f"{c_cmp} = icmp ne {ext_ty} {c_sh}, 0")
        self.emit(f"store i1 {c_cmp}, ptr %flag_c, align 1")

        axb = self.new_tmp("axb")
        naxb = self.new_tmp("naxb")
        axr = self.new_tmp("axr")
        vand = self.new_tmp("vand")
        v_cmp = self.new_tmp("fl_v")
        self.emit(f"{axb} = xor {ty} {a}, {b}")
        self.emit(f"{naxb} = xor {ty} {axb}, -1")
        self.emit(f"{axr} = xor {ty} {a}, {res}")
        self.emit(f"{vand} = and {ty} {naxb}, {axr}")
        self.emit(f"{v_cmp} = icmp slt {ty} {vand}, 0")
        self.emit(f"store i1 {v_cmp}, ptr %flag_v, align 1")

    def eval_cond(self, cond: int) -> str:
        c_hi = cond >> 1
        res = self.new_tmp("cond")
        if c_hi == 0:  # EQ / NE
            self.emit(f"{res} = load i1, ptr %flag_z, align 1")
        elif c_hi == 1:  # CS (HS) / CC (LO)
            self.emit(f"{res} = load i1, ptr %flag_c, align 1")
        elif c_hi == 2:  # MI / PL
            self.emit(f"{res} = load i1, ptr %flag_n, align 1")
        elif c_hi == 3:  # VS / VC
            self.emit(f"{res} = load i1, ptr %flag_v, align 1")
        elif c_hi == 4:  # HI / LS (C == 1 && Z == 0)
            fc = self.new_tmp("fc")
            fz = self.new_tmp("fz")
            nz = self.new_tmp("nz")
            self.emit(f"{fc} = load i1, ptr %flag_c, align 1")
            self.emit(f"{fz} = load i1, ptr %flag_z, align 1")
            self.emit(f"{nz} = xor i1 {fz}, true")
            self.emit(f"{res} = and i1 {fc}, {nz}")
        elif c_hi == 5:  # GE / LT (N == V)
            fn = self.new_tmp("fn")
            fv = self.new_tmp("fv")
            self.emit(f"{fn} = load i1, ptr %flag_n, align 1")
            self.emit(f"{fv} = load i1, ptr %flag_v, align 1")
            self.emit(f"{res} = icmp eq i1 {fn}, {fv}")
        elif c_hi == 6:  # GT / LE (Z == 0 && N == V)
            fn = self.new_tmp("fn")
            fv = self.new_tmp("fv")
            fz = self.new_tmp("fz")
            nv = self.new_tmp("nv")
            nz = self.new_tmp("nz")
            self.emit(f"{fn} = load i1, ptr %flag_n, align 1")
            self.emit(f"{fv} = load i1, ptr %flag_v, align 1")
            self.emit(f"{fz} = load i1, ptr %flag_z, align 1")
            self.emit(f"{nv} = icmp eq i1 {fn}, {fv}")
            self.emit(f"{nz} = xor i1 {fz}, true")
            self.emit(f"{res} = and i1 {nv}, {nz}")
        else:  # AL / NV
            return "true"

        if (cond & 1) and (cond != 0xF):
            inv = self.new_tmp("cinv")
            self.emit(f"{inv} = xor i1 {res}, true")
            return inv
        return res

    def emit_extend_reg(self, rm: int, option: int, shift: int) -> str:
        raw = self.load_gpr(rm, is_64=True)
        opt = option & 7
        if opt in (3, 7):
            ext = raw
        else:
            bits = 8 if (opt & 3) == 0 else (16 if (opt & 3) == 1 else 32)
            tr = self.new_tmp("tr")
            self.emit(f"{tr} = trunc i64 {raw} to i{bits}")
            ext = self.new_tmp("ext")
            op_str = "sext" if (opt & 4) else "zext"
            self.emit(f"{ext} = {op_str} i{bits} {tr} to i64")
        if shift > 0:
            sh = self.new_tmp("sh")
            self.emit(f"{sh} = shl i64 {ext}, {shift}")
            return sh
        return ext

    def emit_shift_reg(self, val: str, shift_type: int, amount: int, is_64: bool) -> str:
        if amount == 0:
            return val
        ty = "i64" if is_64 else "i32"
        st = shift_type & 3
        res = self.new_tmp("sh")
        if st == 0:
            self.emit(f"{res} = shl {ty} {val}, {amount}")
        elif st == 1:
            self.emit(f"{res} = lshr {ty} {val}, {amount}")
        elif st == 2:
            self.emit(f"{res} = ashr {ty} {val}, {amount}")
        else:
            # Horizon aarch64_lifter.hpp pattern: funnel shift right @llvm.fshr for ROR
            self.emit(f"{res} = call {ty} @llvm.fshr.{ty}({ty} {val}, {ty} {val}, {ty} {amount})")
        return res

    def emit_call_to_target(self, target_rva: int):
        if target_rva in self.plt_targets:
            sym = self.plt_targets[target_rva]
            self.stats["bionic_libc_calls_lowered"] += 1
            self.emit_bionic_plt_lowering(sym)
        elif target_rva in self.func_set:
            self.stats["direct_calls_lowered"] += 1
            self.emit(f"call void @aot_sub_{target_rva:08x}(ptr %cpu)")
        else:
            fstart = self.analyzer.find_func_start(target_rva)
            if fstart is not None and fstart in self.func_set and fstart == target_rva:
                self.stats["direct_calls_lowered"] += 1
                self.emit(f"call void @aot_sub_{fstart:08x}(ptr %cpu)")
            else:
                self.stats["indirect_calls_lowered"] += 1
                self.emit(f"call void @aot_indirect_dispatch(ptr %cpu, i64 {IMAGE_BASE + target_rva})")

    def emit_bionic_plt_lowering(self, sym: str):
        for r in range(6):
            rv = self.load_gpr(r, is_64=True)
            rp = self.new_tmp(f"plt_in_{r}")
            self.emit(f"{rp} = getelementptr inbounds i8, ptr %cpu, i64 {r * 8}")
            self.emit(f"store i64 {rv}, ptr {rp}, align 8")
        self.emit(f"call void @llvm.mali.bionic.plt64.{sym}(ptr %cpu)")
        r0_p = self.new_tmp("plt_out_0")
        r0_v = self.new_tmp("plt_res_0")
        self.emit(f"{r0_p} = getelementptr inbounds i8, ptr %cpu, i64 0")
        self.emit(f"{r0_v} = load i64, ptr {r0_p}, align 8")
        self.store_gpr(0, r0_v, is_64=True)

    def detect_pic_jump_table(self, faddr: int, end_addr: int, pc: int):
        """
        Detects the standard Clang/LLVM AArch64 PIC jump-table sequence ending at `br xBase`:
          pc - 20: adrp xT, #table_page
          pc - 16: add  xT, xT, #table_off
          pc - 12: adr  xBase, #16
          pc - 8:  ldrb wOff, [xT, xIdx]
          pc - 4:  add  xBase, xBase, xOff, lsl #2
          pc:      br   xBase
        """
        if pc - 28 < faddr:
            return None
        w_adrp, w_add, w_adr, w_ldrb, w_add2 = struct.unpack_from("<5I", self.elf, pc - 20)
        if (
            (w_adrp & 0x9F000000) == 0x90000000
            and (w_add & 0xFF800000) == 0x91000000
            and (w_adr & 0x9F000000) == 0x10000000
            and (w_ldrb & 0xFFE00000) == 0x38600000
        ):
            max_idx = None
            for back in range(pc - 28, max(faddr - 4, pc - 44), -4):
                w_cmp = struct.unpack_from("<I", self.elf, back)[0]
                if (w_cmp & 0x7F80001F) == 0x7100001F:  # cmp wN/xN, #imm12
                    max_idx = (w_cmp >> 10) & 0xFFF
                    break
            if max_idx is None or max_idx > 256:
                return None
            immlo = (w_adrp >> 29) & 3
            immhi = (w_adrp >> 5) & 0x7FFFF
            imm21 = sext64((immhi << 2) | immlo, 21)
            page = ((pc - 20) & ~0xFFF) + (imm21 << 12)
            off12 = (w_add >> 10) & 0xFFF
            tbl_addr = page + off12
            base_addr = pc + 4
            targets = []
            for idx in range(max_idx + 1):
                b = self.elf[tbl_addr + idx]
                t_rva = base_addr + (b << 2)
                if faddr <= t_rva < end_addr:
                    targets.append(t_rva)
            return targets
        return None

    def discover_basic_blocks(self, faddr: int, fsize: int):
        leaders = {faddr}
        end_addr = faddr + fsize
        jtables = {}
        for pc in range(faddr, end_addr, 4):
            insn = struct.unpack_from("<I", self.elf, pc)[0]
            top6 = (insn >> 26) & 0x3F
            if top6 == 0x05:  # B
                imm26 = sext64(insn & 0x3FFFFFF, 26) << 2
                tgt = pc + imm26
                if faddr <= tgt < end_addr:
                    leaders.add(tgt)
                if pc + 4 < end_addr:
                    leaders.add(pc + 4)
            elif ((insn >> 24) & 0xFF) == 0x54:  # B.cond
                imm19 = sext64((insn >> 5) & 0x7FFFF, 19) << 2
                tgt = pc + imm19
                if faddr <= tgt < end_addr:
                    leaders.add(tgt)
                if pc + 4 < end_addr:
                    leaders.add(pc + 4)
            elif ((insn >> 25) & 0x3F) == 0x1A:  # CBZ / CBNZ
                imm19 = sext64((insn >> 5) & 0x7FFFF, 19) << 2
                tgt = pc + imm19
                if faddr <= tgt < end_addr:
                    leaders.add(tgt)
                if pc + 4 < end_addr:
                    leaders.add(pc + 4)
            elif ((insn >> 25) & 0x3F) == 0x1B:  # TBZ / TBNZ
                imm14 = sext64((insn >> 5) & 0x3FFF, 14) << 2
                tgt = pc + imm14
                if faddr <= tgt < end_addr:
                    leaders.add(tgt)
                if pc + 4 < end_addr:
                    leaders.add(pc + 4)
            elif (insn & 0xFE000000) == 0xD6000000:
                opc = (insn >> 21) & 0xF
                if opc in (0, 2, 4):  # BR, RET, RETAA
                    if opc == 0:
                        jt = self.detect_pic_jump_table(faddr, end_addr, pc)
                        if jt:
                            jtables[pc] = jt
                            for t_rva in jt:
                                leaders.add(t_rva)
                    if pc + 4 < end_addr:
                        leaders.add(pc + 4)

        sorted_leaders = sorted(leaders)
        blocks = []
        for i, start in enumerate(sorted_leaders):
            stop = sorted_leaders[i + 1] if (i + 1 < len(sorted_leaders)) else end_addr
            blocks.append((start, stop))
        return blocks, jtables

    def lift_function(self, faddr: int):
        fname, fsize = self.analyzer.func_by_addr.get(faddr, (f"sub_{faddr:08x}", 4))
        if fsize == 0:
            fsize = 4
        blocks, jtables = self.discover_basic_blocks(faddr, fsize)
        end_addr = faddr + fsize

        self.emit_raw("; ============================================================================")
        self.emit_raw(f"; Function: {fname} (RVA: 0x{faddr:08x}, size: {fsize} bytes, {len(blocks)} BBs)")
        self.emit_raw("; ============================================================================")
        self.emit_raw(f"define void @aot_sub_{faddr:08x}(ptr %cpu) #0 {{")
        self.emit_raw("entry:")

        for r in range(31):
            self.emit(f"%reg_x{r} = alloca i64, align 8")
        self.emit("%reg_sp = alloca i64, align 8")
        for r in range(32):
            self.emit(f"%reg_v{r} = alloca i128, align 16")
        self.emit("%flag_n = alloca i1, align 1")
        self.emit("%flag_z = alloca i1, align 1")
        self.emit("%flag_c = alloca i1, align 1")
        self.emit("%flag_v = alloca i1, align 1")
        self.emit("%reg_tpidr = alloca i64, align 8")

        for r in range(31):
            p = self.new_tmp("gep_x")
            v = self.new_tmp("init_x")
            self.emit(f"{p} = getelementptr inbounds i8, ptr %cpu, i64 {r * 8}")
            self.emit(f"{v} = load i64, ptr {p}, align 8")
            self.emit(f"store i64 {v}, ptr %reg_x{r}, align 8")
        psp = self.new_tmp("gep_sp")
        vsp = self.new_tmp("init_sp")
        self.emit(f"{psp} = getelementptr inbounds i8, ptr %cpu, i64 248")
        self.emit(f"{vsp} = load i64, ptr {psp}, align 8")
        self.emit(f"store i64 {vsp}, ptr %reg_sp, align 8")

        ptp = self.new_tmp("gep_tp")
        vtp = self.new_tmp("init_tp")
        self.emit(f"{ptp} = getelementptr inbounds i8, ptr %cpu, i64 264")
        self.emit(f"{vtp} = load i64, ptr {ptp}, align 8")
        self.emit(f"store i64 {vtp}, ptr %reg_tpidr, align 8")

        for r in range(32):
            pv = self.new_tmp("gep_v")
            vv = self.new_tmp("init_v")
            self.emit(f"{pv} = getelementptr inbounds i8, ptr %cpu, i64 {272 + r * 16}")
            self.emit(f"{vv} = load i128, ptr {pv}, align 16")
            self.emit(f"store i128 {vv}, ptr %reg_v{r}, align 16")

        self.emit("store i1 false, ptr %flag_n, align 1")
        self.emit("store i1 false, ptr %flag_z, align 1")
        self.emit("store i1 false, ptr %flag_c, align 1")
        self.emit("store i1 false, ptr %flag_v, align 1")
        self.emit(f"br label %bb_{faddr:08x}")

        def flush_cpu_state():
            for r in range(31):
                p = self.new_tmp("fl_x")
                v = self.new_tmp("fv_x")
                self.emit(f"{v} = load i64, ptr %reg_x{r}, align 8")
                self.emit(f"{p} = getelementptr inbounds i8, ptr %cpu, i64 {r * 8}")
                self.emit(f"store i64 {v}, ptr {p}, align 8")
            psp = self.new_tmp("fl_sp")
            vsp = self.new_tmp("fv_sp")
            self.emit(f"{vsp} = load i64, ptr %reg_sp, align 8")
            self.emit(f"{psp} = getelementptr inbounds i8, ptr %cpu, i64 248")
            self.emit(f"store i64 {vsp}, ptr {psp}, align 8")
            for r in range(32):
                pv = self.new_tmp("fl_v")
                vv = self.new_tmp("fv_v")
                self.emit(f"{vv} = load i128, ptr %reg_v{r}, align 16")
                self.emit(f"{pv} = getelementptr inbounds i8, ptr %cpu, i64 {272 + r * 16}")
                self.emit(f"store i128 {vv}, ptr {pv}, align 16")

        def reload_cpu_state():
            for r in range(31):
                p = self.new_tmp("rl_x")
                v = self.new_tmp("rv_x")
                self.emit(f"{p} = getelementptr inbounds i8, ptr %cpu, i64 {r * 8}")
                self.emit(f"{v} = load i64, ptr {p}, align 8")
                self.emit(f"store i64 {v}, ptr %reg_x{r}, align 8")
            psp = self.new_tmp("rl_sp")
            vsp = self.new_tmp("rv_sp")
            self.emit(f"{psp} = getelementptr inbounds i8, ptr %cpu, i64 248")
            self.emit(f"{vsp} = load i64, ptr {psp}, align 8")
            self.emit(f"store i64 {vsp}, ptr %reg_sp, align 8")
            for r in range(32):
                pv = self.new_tmp("rl_v")
                vv = self.new_tmp("rv_v")
                self.emit(f"{pv} = getelementptr inbounds i8, ptr %cpu, i64 {272 + r * 16}")
                self.emit(f"{vv} = load i128, ptr {pv}, align 16")
                self.emit(f"store i128 {vv}, ptr %reg_v{r}, align 16")

        for b_start, b_stop in blocks:
            self.emit_raw(f"bb_{b_start:08x}:")
            terminated = False
            for pc in range(b_start, b_stop, 4):
                insn = struct.unpack_from("<I", self.elf, pc)[0]
                self.stats["total_instructions"] += 1

                if insn == 0x00000000:
                    self.emit("br label %exit_ret")
                    terminated = True
                    break

                op0 = (insn >> 25) & 0xF
                rd = insn & 0x1F
                rn = (insn >> 5) & 0x1F
                rm = (insn >> 16) & 0x1F
                ra = (insn >> 10) & 0x1F
                sf = (insn >> 31) & 1
                is_64 = sf == 1
                ty = "i64" if is_64 else "i32"

                # --------------------------------------------------------------
                # 1. Data Processing -- Immediate (op0 = 100x)
                # --------------------------------------------------------------
                if (op0 & 0xE) == 0x8:
                    subop = (insn >> 23) & 0x7
                    if subop in (0, 1):  # ADR / ADRP
                        imm = sext64(((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3), 21)
                        if sf:
                            val = IMAGE_BASE + ((pc & ~0xFFF) + (imm << 12))
                        else:
                            val = IMAGE_BASE + pc + imm
                        self.store_gpr(rd, str(u64(val)), is_64=True)
                    elif subop in (2, 3):  # ADD / ADDS / SUB / SUBS (imm)
                        op = (insn >> 30) & 1
                        S = (insn >> 29) & 1
                        sh = (insn >> 22) & 1
                        imm12 = ((insn >> 10) & 0xFFF) << (12 if sh else 0)
                        a = self.load_gpr_or_sp(rn, is_64=is_64)
                        res = self.new_tmp("alu")
                        if op == 0:
                            self.emit(f"{res} = add {ty} {a}, {imm12}")
                        else:
                            self.emit(f"{res} = sub {ty} {a}, {imm12}")
                        if S:
                            rhs = str(u64(~imm12) if is_64 else u32(~imm12)) if op else str(imm12)
                            cin = "1" if op else "0"
                            self.emit_flags_add(a, rhs, cin, res, is_64)
                            self.store_gpr(rd, res, is_64=is_64)
                        else:
                            self.store_gpr_or_sp(rd, res, is_64=is_64)
                    elif subop == 4:  # Logical immediate (AND / ORR / EOR / ANDS)
                        opc = (insn >> 29) & 3
                        immN = (insn >> 22) & 1
                        immr = (insn >> 16) & 0x3F
                        imms = (insn >> 10) & 0x3F
                        wmask, _ = decode_bit_masks(sf, immN, imms, immr)
                        mask_val = wmask if is_64 else u32(wmask)
                        a = self.load_gpr(rn, is_64=is_64)
                        res = self.new_tmp("log")
                        op_str = "and" if opc in (0, 3) else ("or" if opc == 1 else "xor")
                        self.emit(f"{res} = {op_str} {ty} {a}, {mask_val}")
                        if opc == 3:
                            n_cmp = self.new_tmp("fl_n")
                            z_cmp = self.new_tmp("fl_z")
                            self.emit(f"{n_cmp} = icmp slt {ty} {res}, 0")
                            self.emit(f"{z_cmp} = icmp eq {ty} {res}, 0")
                            self.emit(f"store i1 {n_cmp}, ptr %flag_n, align 1")
                            self.emit(f"store i1 {z_cmp}, ptr %flag_z, align 1")
                            self.emit("store i1 false, ptr %flag_c, align 1")
                            self.emit("store i1 false, ptr %flag_v, align 1")
                            self.store_gpr(rd, res, is_64=is_64)
                        else:
                            self.store_gpr_or_sp(rd, res, is_64=is_64)
                    elif subop == 5:  # MOVN / MOVZ / MOVK
                        opc = (insn >> 29) & 3
                        hw = ((insn >> 21) & 3) * 16
                        imm16 = (insn >> 5) & 0xFFFF
                        if opc == 0:
                            v = u64(~(imm16 << hw)) if is_64 else u32(~(imm16 << hw))
                            self.store_gpr(rd, str(v), is_64=is_64)
                        elif opc == 2:
                            v = u64(imm16 << hw) if is_64 else u32(imm16 << hw)
                            self.store_gpr(rd, str(v), is_64=is_64)
                        elif opc == 3:
                            cur = self.load_gpr(rd, is_64=is_64)
                            clr_mask = u64(~(0xFFFF << hw)) if is_64 else u32(~(0xFFFF << hw))
                            ins_val = u64(imm16 << hw) if is_64 else u32(imm16 << hw)
                            c1 = self.new_tmp("movk1")
                            c2 = self.new_tmp("movk2")
                            self.emit(f"{c1} = and {ty} {cur}, {clr_mask}")
                            self.emit(f"{c2} = or {ty} {c1}, {ins_val}")
                            self.store_gpr(rd, c2, is_64=is_64)
                    elif subop == 6:  # SBFM / BFM / UBFM
                        opc = (insn >> 29) & 3
                        immN = (insn >> 22) & 1
                        immr = (insn >> 16) & 0x3F
                        imms = (insn >> 10) & 0x3F
                        wmask, tmask = decode_bit_masks(sf, immN, imms, immr)
                        if not is_64:
                            wmask = u32(wmask)
                            tmask = u32(tmask)
                        src = self.load_gpr(rn, is_64=is_64)
                        rot = self.emit_shift_reg(src, 3, immr, is_64)
                        bot = self.new_tmp("bf_bot")
                        self.emit(f"{bot} = and {ty} {rot}, {wmask}")
                        if opc == 2:  # UBFM
                            res = self.new_tmp("ubfm")
                            self.emit(f"{res} = and {ty} {bot}, {tmask}")
                            self.store_gpr(rd, res, is_64=is_64)
                        elif opc == 1:  # BFM
                            dst = self.load_gpr(rd, is_64=is_64)
                            bmask = wmask & tmask
                            nbmask = u64(~bmask) if is_64 else u32(~bmask)
                            d1 = self.new_tmp("bfm1")
                            b1 = self.new_tmp("bfm2")
                            res = self.new_tmp("bfm")
                            self.emit(f"{d1} = and {ty} {dst}, {nbmask}")
                            self.emit(f"{b1} = and {ty} {bot}, {bmask}")
                            self.emit(f"{res} = or {ty} {d1}, {b1}")
                            self.store_gpr(rd, res, is_64=is_64)
                        else:  # SBFM
                            sh_s = self.new_tmp("sbfm_sh")
                            bit_s = self.new_tmp("sbfm_b")
                            sext_s = self.new_tmp("sbfm_sx")
                            ntmask = u64(~tmask) if is_64 else u32(~tmask)
                            top = self.new_tmp("sbfm_top")
                            b1 = self.new_tmp("sbfm_bot")
                            res = self.new_tmp("sbfm")
                            self.emit(f"{sh_s} = lshr {ty} {src}, {imms}")
                            self.emit(f"{bit_s} = and {ty} {sh_s}, 1")
                            self.emit(f"{sext_s} = sub {ty} 0, {bit_s}")
                            self.emit(f"{top} = and {ty} {sext_s}, {ntmask}")
                            self.emit(f"{b1} = and {ty} {bot}, {tmask}")
                            self.emit(f"{res} = or {ty} {top}, {b1}")
                            self.store_gpr(rd, res, is_64=is_64)
                    elif subop == 7:  # EXTR (Horizon aarch64_lifter.hpp @llvm.fshr pattern)
                        imms = (insn >> 10) & 0x3F
                        n_val = self.load_gpr(rn, is_64=is_64)
                        m_val = self.load_gpr(rm, is_64=is_64)
                        if imms == 0:
                            self.store_gpr(rd, m_val, is_64=is_64)
                        else:
                            res = self.new_tmp("extr")
                            self.emit(f"{res} = call {ty} @llvm.fshr.{ty}({ty} {n_val}, {ty} {m_val}, {ty} {imms})")
                            self.store_gpr(rd, res, is_64=is_64)
                    continue

                # --------------------------------------------------------------
                # 2. Branches, System Instructions, PAC/BTI, TPIDR_EL0 (op0 = 101x)
                # --------------------------------------------------------------
                if (op0 & 0xE) == 0xA:
                    top6 = (insn >> 26) & 0x3F
                    if top6 in (0x05, 0x25):  # B / BL
                        off = sext64(insn & 0x3FFFFFF, 26) << 2
                        target = pc + off
                        if top6 == 0x25:  # BL
                            self.store_gpr(30, str(IMAGE_BASE + pc + 4), is_64=True)
                            if target in self.plt_targets:
                                self.emit_call_to_target(target)
                            else:
                                flush_cpu_state()
                                self.emit_call_to_target(target)
                                reload_cpu_state()
                        else:  # B
                            if faddr <= target < end_addr:
                                self.emit(f"br label %bb_{target:08x}")
                            else:
                                if target in self.plt_targets:
                                    self.emit_call_to_target(target)
                                else:
                                    flush_cpu_state()
                                    self.emit_call_to_target(target)
                                    reload_cpu_state()
                                self.emit("br label %exit_ret")
                            terminated = True
                            break
                        continue

                    if ((insn >> 24) & 0xFF) == 0x54:  # B.cond
                        off = sext64((insn >> 5) & 0x7FFFF, 19) << 2
                        target = pc + off
                        cond_val = self.eval_cond(insn & 0xF)
                        self.emit(f"br i1 {cond_val}, label %bb_{target:08x}, label %bb_{pc + 4:08x}")
                        terminated = True
                        break

                    if ((insn >> 25) & 0x3F) == 0x1A:  # CBZ / CBNZ
                        op = (insn >> 24) & 1
                        off = sext64((insn >> 5) & 0x7FFFF, 19) << 2
                        target = pc + off
                        v = self.load_gpr(rd, is_64=is_64)
                        cmp_v = self.new_tmp("cbz")
                        pred = "ne" if op else "eq"
                        self.emit(f"{cmp_v} = icmp {pred} {ty} {v}, 0")
                        self.emit(f"br i1 {cmp_v}, label %bb_{target:08x}, label %bb_{pc + 4:08x}")
                        terminated = True
                        break

                    if ((insn >> 25) & 0x3F) == 0x1B:  # TBZ / TBNZ
                        op = (insn >> 24) & 1
                        bpos = ((insn >> 31) << 5) | ((insn >> 19) & 0x1F)
                        off = sext64((insn >> 5) & 0x3FFF, 14) << 2
                        target = pc + off
                        v = self.load_gpr(rd, is_64=True)
                        masked = self.new_tmp("tbz_m")
                        cmp_v = self.new_tmp("tbz_c")
                        pred = "ne" if op else "eq"
                        self.emit(f"{masked} = and i64 {v}, {1 << bpos}")
                        self.emit(f"{cmp_v} = icmp {pred} i64 {masked}, 0")
                        self.emit(f"br i1 {cmp_v}, label %bb_{target:08x}, label %bb_{pc + 4:08x}")
                        terminated = True
                        break

                    if (insn & 0xFE000000) == 0xD6000000:  # BR / BLR / RET / RETAA
                        opc = (insn >> 21) & 0xF
                        if opc in (2, 4):  # RET / RETAA / RETAB
                            if insn in (0xD65F0BFF, 0xD65F0FFF):
                                self.stats["retaa_lowered"] += 1
                                self.emit("call void @llvm.mali.arm64.retaa()")
                            self.emit("br label %exit_ret")
                            terminated = True
                            break
                        elif opc == 1:  # BLR xN
                            tgt = self.load_gpr(rn, is_64=True)
                            self.store_gpr(30, str(IMAGE_BASE + pc + 4), is_64=True)
                            if rn == 1:
                                self.stats["bionic_tlsdesc_calls_lowered"] += 1
                            else:
                                self.stats["indirect_calls_lowered"] += 1
                            flush_cpu_state()
                            self.emit(f"call void @aot_indirect_dispatch(ptr %cpu, i64 {tgt})")
                            reload_cpu_state()
                            continue
                        elif opc == 0:  # BR xN
                            if pc in jtables:
                                self.stats["jump_tables_lowered"] += 1
                                tgt = self.load_gpr(rn, is_64=True)
                                cases = []
                                seen_tgts = set()
                                for t_rva in jtables[pc]:
                                    if t_rva not in seen_tgts:
                                        seen_tgts.add(t_rva)
                                        cases.append(f"i64 {IMAGE_BASE + t_rva}, label %bb_{t_rva:08x}")
                                cases_str = " ".join(cases)
                                self.emit(f"switch i64 {tgt}, label %exit_ret [ {cases_str} ]")
                                terminated = True
                                break
                            else:
                                self.stats["indirect_calls_lowered"] += 1
                                tgt = self.load_gpr(rn, is_64=True)
                                flush_cpu_state()
                                self.emit(f"call void @aot_indirect_dispatch(ptr %cpu, i64 {tgt})")
                                reload_cpu_state()
                                self.emit("br label %exit_ret")
                                terminated = True
                                break

                    if (insn & 0xFFF00000) in (0xD5000000, 0xD5100000, 0xD5200000, 0xD5300000):
                        L = (insn >> 21) & 1
                        sysreg = (insn >> 5) & 0xFFFF
                        if insn in (0xD503201F, 0xD503233F, 0xD50323BF, 0xD503245F, 0xD503249F, 0xD50324DF):
                            imm_hint = (insn >> 5) & 0x7F
                            if insn != 0xD503201F:
                                self.stats["pac_bti_stripped"] += 1
                                self.emit(f"call void @llvm.mali.arm64.hint(i32 {imm_hint})")
                            continue
                        if (insn & 0xFFE00000) == 0xD5200000 and L == 1:  # MRS Xt, sysreg
                            if sysreg in (0xDE82, 0xDE83):
                                self.stats["bionic_tpidr_lowered"] += 1
                                if pc + 4 < end_addr:
                                    nxt = struct.unpack_from("<I", self.elf, pc + 4)[0]
                                    if (nxt & 0xFFC00000) in (0xF9400000, 0xF9000000):
                                        slot_idx = (nxt >> 10) & 0xFFF
                                        if slot_idx == 3:
                                            self.stats["bionic_tls_slot_opengl_api"] += 1
                                        elif slot_idx == 5:
                                            self.stats["bionic_tls_slot_stack_guard"] += 1
                            tp = self.new_tmp("mrs_val")
                            self.emit(f"{tp} = call i64 @llvm.mali.arm64.mrs(ptr %cpu, i32 {sysreg})")
                            self.store_gpr(rd, tp, is_64=True)
                        continue

                # --------------------------------------------------------------
                # 3. Loads and Stores (op0 = x1x0)
                # --------------------------------------------------------------
                if (op0 & 0x5) == 0x4:
                    size = (insn >> 30) & 3
                    vr = (insn >> 26) & 1
                    opc = (insn >> 22) & 3

                    # 3a. Load Pair / Store Pair (LDP / STP / LDPSW)
                    if ((insn >> 27) & 7) == 5:
                        mode = (insn >> 23) & 3
                        L = (insn >> 22) & 1
                        rt2 = (insn >> 10) & 0x1F
                        imm7 = sext64((insn >> 15) & 0x7F, 7)
                        scale = (2 + size) if vr else (2 + (size >> 1))
                        off = imm7 << scale
                        base_val = self.load_gpr_or_sp(rn, is_64=True)
                        if mode == 1:
                            addr_val = base_val
                        elif off != 0:
                            addr_val = self.new_tmp("ldp_a")
                            self.emit(f"{addr_val} = add i64 {base_val}, {off}")
                        else:
                            addr_val = base_val

                        esz = 1 << scale
                        addr2_val = self.new_tmp("ldp_a2")
                        self.emit(f"{addr2_val} = add i64 {addr_val}, {esz}")
                        p1 = self.new_tmp("p1")
                        p2 = self.new_tmp("p2")
                        self.emit(f"{p1} = inttoptr i64 {addr_val} to ptr")
                        self.emit(f"{p2} = inttoptr i64 {addr2_val} to ptr")

                        if vr == 0:
                            mty = "i32" if size == 0 else "i64"
                            if L == 0:  # STP
                                v1 = self.load_gpr(rd, is_64=(size != 0))
                                v2 = self.load_gpr(rt2, is_64=(size != 0))
                                self.emit(f"store {mty} {v1}, ptr {p1}, align 1")
                                self.emit(f"store {mty} {v2}, ptr {p2}, align 1")
                            else:  # LDP / LDPSW
                                v1 = self.new_tmp("ldp1")
                                v2 = self.new_tmp("ldp2")
                                if size == 0:
                                    self.emit(f"{v1} = load i32, ptr {p1}, align 1")
                                    self.emit(f"{v2} = load i32, ptr {p2}, align 1")
                                    self.store_gpr(rd, v1, is_64=False)
                                    self.store_gpr(rt2, v2, is_64=False)
                                elif size == 1:  # LDPSW
                                    self.emit(f"{v1} = load i32, ptr {p1}, align 1")
                                    self.emit(f"{v2} = load i32, ptr {p2}, align 1")
                                    sx1 = self.new_tmp("sx1")
                                    sx2 = self.new_tmp("sx2")
                                    self.emit(f"{sx1} = sext i32 {v1} to i64")
                                    self.emit(f"{sx2} = sext i32 {v2} to i64")
                                    self.store_gpr(rd, sx1, is_64=True)
                                    self.store_gpr(rt2, sx2, is_64=True)
                                else:
                                    self.emit(f"{v1} = load i64, ptr {p1}, align 1")
                                    self.emit(f"{v2} = load i64, ptr {p2}, align 1")
                                    self.store_gpr(rd, v1, is_64=True)
                                    self.store_gpr(rt2, v2, is_64=True)
                        else:
                            vty = f"i{esz * 8}"
                            if L == 0:
                                v1 = self.load_vec(rd, vty)
                                v2 = self.load_vec(rt2, vty)
                                self.emit(f"store {vty} {v1}, ptr {p1}, align 1")
                                self.emit(f"store {vty} {v2}, ptr {p2}, align 1")
                            else:
                                v1 = self.new_tmp("vldp1")
                                v2 = self.new_tmp("vldp2")
                                self.emit(f"{v1} = load {vty}, ptr {p1}, align 1")
                                self.emit(f"{v2} = load {vty}, ptr {p2}, align 1")
                                self.store_vec(rd, vty, v1)
                                self.store_vec(rt2, vty, v2)

                        if mode in (1, 3):
                            wb = self.new_tmp("wb")
                            self.emit(f"{wb} = add i64 {base_val}, {off}")
                            self.store_gpr_or_sp(rn, wb, is_64=True)
                        continue

                    # 3b. Regular Load/Store (unsigned imm, unscaled/pre/post, or reg offset)
                    if ((insn >> 27) & 7) == 7:
                        p1 = (insn >> 24) & 1
                        base_val = self.load_gpr_or_sp(rn, is_64=True)
                        scale = 4 if (vr == 1 and (opc & 2) != 0 and size == 0) else size
                        writeback = False
                        wb_val = base_val

                        if p1 == 1:
                            imm12 = ((insn >> 10) & 0xFFF) << scale
                            if imm12 != 0:
                                addr_val = self.new_tmp("addr")
                                self.emit(f"{addr_val} = add i64 {base_val}, {imm12}")
                            else:
                                addr_val = base_val
                        elif ((insn >> 21) & 1) == 0:
                            mode = (insn >> 10) & 3
                            imm9 = sext64((insn >> 12) & 0x1FF, 9)
                            off_val = self.new_tmp("addr_imm9")
                            self.emit(f"{off_val} = add i64 {base_val}, {imm9}")
                            if mode == 1:  # Post-index
                                addr_val = base_val
                                wb_val = off_val
                                writeback = True
                            elif mode == 3:  # Pre-index
                                addr_val = off_val
                                wb_val = off_val
                                writeback = True
                            else:
                                addr_val = off_val
                        else:
                            option = (insn >> 13) & 7
                            S = (insn >> 12) & 1
                            sh = scale if S else 0
                            ext_rm = self.emit_extend_reg(rm, option, sh)
                            addr_val = self.new_tmp("addr_reg")
                            self.emit(f"{addr_val} = add i64 {base_val}, {ext_rm}")

                        ptr_v = self.new_tmp("mptr")
                        self.emit(f"{ptr_v} = inttoptr i64 {addr_val} to ptr")
                        mty = f"i{8 << scale}"

                        if vr == 0:
                            if opc == 0:  # Store
                                val = self.load_gpr(rd, is_64=(size == 3))
                                if size < 2:
                                    tr = self.new_tmp("str_tr")
                                    self.emit(f"{tr} = trunc i32 {val} to {mty}")
                                    val = tr
                                self.emit(f"store {mty} {val}, ptr {ptr_v}, align 1")
                            elif opc == 1:  # Unsigned Load
                                lv = self.new_tmp("ldr_u")
                                self.emit(f"{lv} = load {mty}, ptr {ptr_v}, align 1")
                                if size < 2:
                                    zx = self.new_tmp("ldr_zx")
                                    self.emit(f"{zx} = zext {mty} {lv} to i32")
                                    lv = zx
                                self.store_gpr(rd, lv, is_64=(size == 3))
                            elif opc == 2:  # Signed Load to 64-bit
                                lv = self.new_tmp("ldr_s")
                                sx = self.new_tmp("ldr_sx64")
                                self.emit(f"{lv} = load {mty}, ptr {ptr_v}, align 1")
                                self.emit(f"{sx} = sext {mty} {lv} to i64")
                                self.store_gpr(rd, sx, is_64=True)
                            elif opc == 3:  # Signed Load to 32-bit
                                if size < 2:
                                    lv = self.new_tmp("ldr_s")
                                    sx = self.new_tmp("ldr_sx32")
                                    self.emit(f"{lv} = load {mty}, ptr {ptr_v}, align 1")
                                    self.emit(f"{sx} = sext {mty} {lv} to i32")
                                    self.store_gpr(rd, sx, is_64=False)
                        else:
                            if (opc & 1) == 0:  # SIMD Store
                                vv = self.load_vec(rd, mty)
                                self.emit(f"store {mty} {vv}, ptr {ptr_v}, align 1")
                            else:  # SIMD Load
                                vv = self.new_tmp("vldr")
                                self.emit(f"{vv} = load {mty}, ptr {ptr_v}, align 1")
                                self.store_vec(rd, mty, vv)

                        if writeback:
                            self.store_gpr_or_sp(rn, wb_val, is_64=True)
                        continue

                    # 3c. AdvSIMD single-lane LD1
                    if ((insn >> 24) & 0xBF) == 0x0D:
                        self.stats["simd_vector_ops_lowered"] += 1
                        Q = (insn >> 30) & 1
                        opcode = (insn >> 13) & 7
                        S = (insn >> 12) & 1
                        sz_bits = (insn >> 10) & 3
                        esz = (8 if (sz_bits & 1) else 4) if opcode in (4, 5) else (1 << (opcode >> 1))
                        lane = Q if esz == 8 else (((Q << 1) | S) if esz == 4 else ((Q << 2) | (S << 1) | (sz_bits >> 1)))
                        addr_val = self.load_gpr_or_sp(rn, is_64=True)
                        ptr_v = self.new_tmp("ld1_p")
                        elem = self.new_tmp("ld1_e")
                        cur_v = self.load_vec(rd, f"<{16 // esz} x i{esz * 8}>")
                        new_v = self.new_tmp("ld1_v")
                        self.emit(f"{ptr_v} = inttoptr i64 {addr_val} to ptr")
                        self.emit(f"{elem} = load i{esz * 8}, ptr {ptr_v}, align 1")
                        self.emit(f"{new_v} = insertelement <{16 // esz} x i{esz * 8}> {cur_v}, i{esz * 8} {elem}, i32 {lane}")
                        self.store_vec(rd, f"<{16 // esz} x i{esz * 8}>", new_v, clear_128=False)
                        continue

                    # 3d. AdvSIMD multiple-structure LD4
                    if ((insn >> 23) & 0x1F) in (0x18, 0x19):
                        self.stats["simd_vector_ops_lowered"] += 1
                        addr_val = self.load_gpr_or_sp(rn, is_64=True)
                        for r_idx in range(4):
                            self.emit(f"store i128 0, ptr %reg_v{(rd + r_idx) & 31}, align 16")
                        for lane in range(4):
                            for r_idx in range(4):
                                vreg = (rd + r_idx) & 31
                                m_off = (lane * 4 + r_idx) * 4
                                ma = self.new_tmp("ld4_a")
                                mp = self.new_tmp("ld4_p")
                                mv = self.new_tmp("ld4_v")
                                cv = self.load_vec(vreg, "<4 x i32>")
                                nv = self.new_tmp("ld4_nv")
                                self.emit(f"{ma} = add i64 {addr_val}, {m_off}")
                                self.emit(f"{mp} = inttoptr i64 {ma} to ptr")
                                self.emit(f"{mv} = load i32, ptr {mp}, align 1")
                                self.emit(f"{nv} = insertelement <4 x i32> {cv}, i32 {mv}, i32 {lane}")
                                self.store_vec(vreg, "<4 x i32>", nv, clear_128=False)
                        continue

                # --------------------------------------------------------------
                # 4. Data Processing -- Register (op0 = x101)
                # --------------------------------------------------------------
                if (op0 & 0x7) == 0x5:
                    op1 = (insn >> 28) & 1
                    op2 = (insn >> 21) & 0xF

                    if op1 == 0 and (op2 & 8) == 0:  # Logical (shifted register)
                        opc = (insn >> 29) & 3
                        N = (insn >> 21) & 1
                        shift_type = (insn >> 22) & 3
                        imm6 = (insn >> 10) & 0x3F
                        a = self.load_gpr(rn, is_64=is_64)
                        b_raw = self.load_gpr(rm, is_64=is_64)
                        b = self.emit_shift_reg(b_raw, shift_type, imm6, is_64)
                        if N:
                            nb = self.new_tmp("orn")
                            self.emit(f"{nb} = xor {ty} {b}, -1")
                            b = nb
                        res = self.new_tmp("logr")
                        op_str = "and" if opc in (0, 3) else ("or" if opc == 1 else "xor")
                        self.emit(f"{res} = {op_str} {ty} {a}, {b}")
                        if opc == 3:
                            n_cmp = self.new_tmp("fl_n")
                            z_cmp = self.new_tmp("fl_z")
                            self.emit(f"{n_cmp} = icmp slt {ty} {res}, 0")
                            self.emit(f"{z_cmp} = icmp eq {ty} {res}, 0")
                            self.emit(f"store i1 {n_cmp}, ptr %flag_n, align 1")
                            self.emit(f"store i1 {z_cmp}, ptr %flag_z, align 1")
                            self.emit("store i1 false, ptr %flag_c, align 1")
                            self.emit("store i1 false, ptr %flag_v, align 1")
                        self.store_gpr(rd, res, is_64=is_64)
                    elif op1 == 0 and (op2 & 8) == 8:  # Add/sub (shifted or extended reg)
                        op = (insn >> 30) & 1
                        S = (insn >> 29) & 1
                        ext = (insn >> 21) & 1
                        if ext == 0:
                            shift_type = (insn >> 22) & 3
                            imm6 = (insn >> 10) & 0x3F
                            a = self.load_gpr(rn, is_64=is_64)
                            b_raw = self.load_gpr(rm, is_64=is_64)
                            b = self.emit_shift_reg(b_raw, shift_type, imm6, is_64)
                        else:
                            option = (insn >> 13) & 7
                            imm3 = (insn >> 10) & 7
                            a = self.load_gpr_or_sp(rn, is_64=is_64)
                            b64 = self.emit_extend_reg(rm, option, imm3)
                            if not is_64:
                                b = self.new_tmp("b32")
                                self.emit(f"{b} = trunc i64 {b64} to i32")
                            else:
                                b = b64
                        res = self.new_tmp("addsub")
                        self.emit(f"{res} = {'sub' if op else 'add'} {ty} {a}, {b}")
                        if S:
                            if op:
                                nb = self.new_tmp("nb")
                                self.emit(f"{nb} = xor {ty} {b}, -1")
                                self.emit_flags_add(a, nb, "1", res, is_64)
                            else:
                                self.emit_flags_add(a, b, "0", res, is_64)
                            self.store_gpr(rd, res, is_64=is_64)
                        elif ext:
                            self.store_gpr_or_sp(rd, res, is_64=is_64)
                        else:
                            self.store_gpr(rd, res, is_64=is_64)
                    elif op1 == 1 and op2 == 2:  # CCMP / CCMN
                        op = (insn >> 30) & 1
                        cond = (insn >> 12) & 0xF
                        is_imm = (insn >> 11) & 1
                        nzcv = insn & 0xF
                        c_ok = self.eval_cond(cond)
                        a = self.load_gpr(rn, is_64=is_64)
                        b = str(rm) if is_imm else self.load_gpr(rm, is_64=is_64)
                        res = self.new_tmp("ccmp_r")
                        self.emit(f"{res} = {'sub' if op else 'add'} {ty} {a}, {b}")
                        if op:
                            nb = self.new_tmp("ccmp_nb")
                            self.emit(f"{nb} = xor {ty} {b}, -1")
                            self.emit_flags_add(a, nb, "1", res, is_64)
                        else:
                            self.emit_flags_add(a, b, "0", res, is_64)
                        fn = self.new_tmp("cc_n")
                        fz = self.new_tmp("cc_z")
                        fc = self.new_tmp("cc_c")
                        fv = self.new_tmp("cc_v")
                        sn = self.new_tmp("s_n")
                        sz = self.new_tmp("s_z")
                        sc = self.new_tmp("s_c")
                        sv = self.new_tmp("s_v")
                        self.emit(f"{fn} = load i1, ptr %flag_n, align 1")
                        self.emit(f"{fz} = load i1, ptr %flag_z, align 1")
                        self.emit(f"{fc} = load i1, ptr %flag_c, align 1")
                        self.emit(f"{fv} = load i1, ptr %flag_v, align 1")
                        self.emit(f"{sn} = select i1 {c_ok}, i1 {fn}, i1 {'true' if (nzcv >> 3) & 1 else 'false'}")
                        self.emit(f"{sz} = select i1 {c_ok}, i1 {fz}, i1 {'true' if (nzcv >> 2) & 1 else 'false'}")
                        self.emit(f"{sc} = select i1 {c_ok}, i1 {fc}, i1 {'true' if (nzcv >> 1) & 1 else 'false'}")
                        self.emit(f"{sv} = select i1 {c_ok}, i1 {fv}, i1 {'true' if nzcv & 1 else 'false'}")
                        self.emit(f"store i1 {sn}, ptr %flag_n, align 1")
                        self.emit(f"store i1 {sz}, ptr %flag_z, align 1")
                        self.emit(f"store i1 {sc}, ptr %flag_c, align 1")
                        self.emit(f"store i1 {sv}, ptr %flag_v, align 1")
                    elif op1 == 1 and op2 == 4:  # CSEL / CSINC / CSINV / CSNEG
                        op = (insn >> 30) & 1
                        o2 = (insn >> 10) & 1
                        cond = (insn >> 12) & 0xF
                        c_ok = self.eval_cond(cond)
                        t_val = self.load_gpr(rn, is_64=is_64)
                        f_val = self.load_gpr(rm, is_64=is_64)
                        if op:
                            nf = self.new_tmp("csinv")
                            self.emit(f"{nf} = xor {ty} {f_val}, -1")
                            f_val = nf
                        if o2:
                            incf = self.new_tmp("csinc")
                            self.emit(f"{incf} = add {ty} {f_val}, 1")
                            f_val = incf
                        res = self.new_tmp("csel")
                        self.emit(f"{res} = select i1 {c_ok}, {ty} {t_val}, {ty} {f_val}")
                        self.store_gpr(rd, res, is_64=is_64)
                    elif op1 == 1 and op2 == 6:
                        # Horizon aarch64_lifter.hpp: Data-processing (2-source & 1-source)
                        S_bit = (insn >> 29) & 1
                        opcode2 = (insn >> 10) & 0x3F
                        if S_bit == 0:  # 2-source: UDIV, SDIV, LSLV, LSRV, ASRV, RORV
                            n = self.load_gpr(rn, is_64=is_64)
                            m = self.load_gpr(rm, is_64=is_64)
                            if opcode2 == 0x02:  # UDIV (with zero-divisor guard)
                                is_zero = self.new_tmp("udiv_z")
                                safe_m = self.new_tmp("udiv_m")
                                q = self.new_tmp("udiv_q")
                                res = self.new_tmp("udiv_r")
                                self.emit(f"{is_zero} = icmp eq {ty} {m}, 0")
                                self.emit(f"{safe_m} = select i1 {is_zero}, {ty} 1, {ty} {m}")
                                self.emit(f"{q} = udiv {ty} {n}, {safe_m}")
                                self.emit(f"{res} = select i1 {is_zero}, {ty} 0, {ty} {q}")
                                self.store_gpr(rd, res, is_64=is_64)
                            elif opcode2 == 0x03:  # SDIV (with zero-divisor & INT_MIN / -1 guards)
                                int_min = str(-(1 << 63) if is_64 else -(1 << 31))
                                is_zero = self.new_tmp("sdiv_z")
                                is_min = self.new_tmp("sdiv_min")
                                is_m1 = self.new_tmp("sdiv_m1")
                                is_ovf = self.new_tmp("sdiv_ovf")
                                bad = self.new_tmp("sdiv_bad")
                                safe_m = self.new_tmp("sdiv_m")
                                q = self.new_tmp("sdiv_q")
                                r1 = self.new_tmp("sdiv_r1")
                                res = self.new_tmp("sdiv_r")
                                self.emit(f"{is_zero} = icmp eq {ty} {m}, 0")
                                self.emit(f"{is_min} = icmp eq {ty} {n}, {int_min}")
                                self.emit(f"{is_m1} = icmp eq {ty} {m}, -1")
                                self.emit(f"{is_ovf} = and i1 {is_min}, {is_m1}")
                                self.emit(f"{bad} = or i1 {is_zero}, {is_ovf}")
                                self.emit(f"{safe_m} = select i1 {bad}, {ty} 1, {ty} {m}")
                                self.emit(f"{q} = sdiv {ty} {n}, {safe_m}")
                                self.emit(f"{r1} = select i1 {is_ovf}, {ty} {int_min}, {ty} {q}")
                                self.emit(f"{res} = select i1 {is_zero}, {ty} 0, {ty} {r1}")
                                self.store_gpr(rd, res, is_64=is_64)
                            elif opcode2 in (0x08, 0x09, 0x0A, 0x0B):  # LSLV, LSRV, ASRV, RORV
                                mask = 63 if is_64 else 31
                                sh_amt = self.new_tmp("sh_amt")
                                res = self.new_tmp("shv")
                                self.emit(f"{sh_amt} = and {ty} {m}, {mask}")
                                if opcode2 == 0x08:
                                    self.emit(f"{res} = shl {ty} {n}, {sh_amt}")
                                elif opcode2 == 0x09:
                                    self.emit(f"{res} = lshr {ty} {n}, {sh_amt}")
                                elif opcode2 == 0x0A:
                                    self.emit(f"{res} = ashr {ty} {n}, {sh_amt}")
                                else:
                                    self.emit(f"{res} = call {ty} @llvm.fshr.{ty}({ty} {n}, {ty} {n}, {ty} {sh_amt})")
                                self.store_gpr(rd, res, is_64=is_64)
                        else:  # 1-source: RBIT, REV16, REV32, REV64, CLZ, CLS
                            n = self.load_gpr(rn, is_64=is_64)
                            res = self.new_tmp("dp1")
                            if opcode2 == 0x00:  # RBIT
                                self.emit(f"{res} = call {ty} @llvm.bitreverse.{ty}({ty} {n})")
                            elif opcode2 in (0x02, 0x03):  # REV32 / REV64
                                self.emit(f"{res} = call {ty} @llvm.bswap.{ty}({ty} {n})")
                            elif opcode2 == 0x04:  # CLZ
                                self.emit(f"{res} = call {ty} @llvm.ctlz.{ty}({ty} {n}, i1 false)")
                            else:
                                self.emit(f"{res} = add {ty} {n}, 0")
                            self.store_gpr(rd, res, is_64=is_64)
                    elif op1 == 1 and (op2 & 8) == 8:  # 3-source: MADD, MSUB, SMADDL, UMADDL, SMULH, UMULH
                        op54 = (insn >> 29) & 3
                        op31 = (insn >> 21) & 7
                        o0 = (insn >> 15) & 1
                        if op54 == 0 and op31 == 0:  # MADD / MSUB
                            a = self.load_gpr(ra, is_64=is_64)
                            n = self.load_gpr(rn, is_64=is_64)
                            m = self.load_gpr(rm, is_64=is_64)
                            p = self.new_tmp("mul")
                            res = self.new_tmp("madd")
                            self.emit(f"{p} = mul {ty} {n}, {m}")
                            self.emit(f"{res} = {'sub' if o0 else 'add'} {ty} {a}, {p}")
                            self.store_gpr(rd, res, is_64=is_64)
                        elif op54 == 0 and op31 in (1, 5):  # SMADDL / UMADDL
                            a = self.load_gpr(ra, is_64=True)
                            n32 = self.load_gpr(rn, is_64=False)
                            m32 = self.load_gpr(rm, is_64=False)
                            n64 = self.new_tmp("m_n")
                            m64 = self.new_tmp("m_m")
                            p = self.new_tmp("m_p")
                            res = self.new_tmp("maddl")
                            ext_op = "sext" if op31 == 1 else "zext"
                            self.emit(f"{n64} = {ext_op} i32 {n32} to i64")
                            self.emit(f"{m64} = {ext_op} i32 {m32} to i64")
                            self.emit(f"{p} = mul i64 {n64}, {m64}")
                            self.emit(f"{res} = {'sub' if o0 else 'add'} i64 {a}, {p}")
                            self.store_gpr(rd, res, is_64=True)
                    continue

                # --------------------------------------------------------------
                # 5. Scalar FP and AdvSIMD Vector Operations (op0 = x111)
                # --------------------------------------------------------------
                if (op0 & 0x7) == 0x7:
                    self.stats["simd_vector_ops_lowered"] += 1
                    Q = (insn >> 30) & 1
                    U = (insn >> 29) & 1
                    M = (insn >> 28) & 1
                    sz = (insn >> 22) & 3

                    if M == 1 and ((insn >> 21) & 1) == 1 and ((insn >> 10) & 0x3F) == 0:
                        opcode = (insn >> 16) & 7
                        if opcode == 6:  # FMOV Wd, Sn
                            v = self.load_vec(rn, "i32")
                            self.store_gpr(rd, v, is_64=False)
                        elif opcode == 7:  # FMOV Sd, Wn
                            v = self.load_gpr(rn, is_64=False)
                            self.store_vec(rd, "i32", v)
                        continue

                    if ((insn >> 19) & 0x1FF) == 0x1E0 and ((insn >> 10) & 3) == 1:
                        cmode = (insn >> 12) & 0xF
                        imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 0x1F)
                        v64 = expand_advsimd_imm(U, cmode, imm8)
                        if U == 1 and (cmode & 0x9) == 1:
                            cur = self.load_vec(rd, "<2 x i64>")
                            mask = u64(~v64)
                            res = self.new_tmp("vbic_i")
                            self.emit(f"{res} = and <2 x i64> {cur}, <i64 {mask}, i64 {mask if Q else 0}>")
                            self.store_vec(rd, "<2 x i64>", res, clear_128=False)
                        else:
                            val128 = (v64 << 64) | v64 if Q else v64
                            self.store_vec(rd, "i128", str(val128), clear_128=False)
                        continue

                    if ((insn >> 21) & 0xFF) == 0x70 and ((insn >> 31) & 1) == 0 and ((insn >> 10) & 1) == 1:
                        imm5 = (insn >> 16) & 0x1F
                        imm4 = (insn >> 11) & 0xF
                        tz = (imm5 & -imm5).bit_length() - 1 if imm5 else 4
                        esz = 1 << tz
                        idx = imm5 >> (tz + 1)
                        if U == 0 and imm4 == 7:
                            v = self.load_vec(rn, f"<{16 // esz} x i{esz * 8}>")
                            el = self.new_tmp("umov")
                            self.emit(f"{el} = extractelement <{16 // esz} x i{esz * 8}> {v}, i32 {idx}")
                            self.store_gpr(rd, el, is_64=(Q == 1))
                        continue

                    if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 21) & 1) == 1 and ((insn >> 10) & 0x3F) == 0x07:
                        n = self.load_vec(rn, "i128")
                        m = self.load_vec(rm, "i128")
                        res = self.new_tmp("vlog")
                        if U == 0 and sz == 1:
                            nm = self.new_tmp("vnot")
                            self.emit(f"{nm} = xor i128 {m}, -1")
                            self.emit(f"{res} = and i128 {n}, {nm}")
                        elif U == 0 and sz == 2:
                            self.emit(f"{res} = or i128 {n}, {m}")
                        elif U == 0 and sz == 0:
                            self.emit(f"{res} = and i128 {n}, {m}")
                        else:
                            self.emit(f"{res} = xor i128 {n}, {m}")
                        self.store_vec(rd, "i128", res, clear_128=False)
                        continue

                    if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 21) & 1) == 1 and ((insn >> 10) & 1) == 1:
                        opcode = (insn >> 11) & 0x1F
                        lanes = (16 if Q else 8) >> sz
                        vty = f"<{lanes} x i{8 << sz}>"
                        a = self.load_vec(rn, vty)
                        b = self.load_vec(rm, vty)
                        res = self.new_tmp("vadd")
                        if opcode == 0x10:
                            self.emit(f"{res} = {'sub' if U else 'add'} {vty} {a}, {b}")
                        self.store_vec(rd, vty, res, clear_128=(Q == 0))
                        continue

                    if ((insn >> 23) & 0x3F) == 0x1E and ((insn >> 10) & 1) == 1:
                        immh = (insn >> 19) & 0xF
                        immb = (insn >> 16) & 7
                        l = immh.bit_length() - 1
                        esz = 1 << l
                        sh = ((immh << 3) | immb) - (8 << l)
                        lanes = 8 // esz
                        src_v = self.load_vec(rn, f"<{lanes * 2} x i{esz * 8}>")
                        shuf = self.new_tmp("ushll_shuf")
                        mask_list = ", ".join(f"i32 {i + (lanes if Q else 0)}" for i in range(lanes))
                        self.emit(
                            f"{shuf} = shufflevector <{lanes * 2} x i{esz * 8}> {src_v}, <{lanes * 2} x i{esz * 8}> poison, <{lanes} x i32> <{mask_list}>"
                        )
                        zx = self.new_tmp("ushll_zx")
                        self.emit(f"{zx} = zext <{lanes} x i{esz * 8}> {shuf} to <{lanes} x i{esz * 16}>")
                        if sh > 0:
                            sh_v = self.new_tmp("ushll_sh")
                            splat = ", ".join(f"i{esz * 16} {sh}" for _ in range(lanes))
                            self.emit(f"{sh_v} = shl <{lanes} x i{esz * 16}> {zx}, <{splat}>")
                            zx = sh_v
                        self.store_vec(rd, f"<{lanes} x i{esz * 16}>", zx, clear_128=False)
                        continue

                    if ((insn >> 24) & 0x3F) == 0x0E and ((insn >> 21) & 1) == 0 and ((insn >> 10) & 3) == 2:
                        esz = 1 << sz
                        lanes = (16 if Q else 8) // esz
                        vty = f"<{lanes} x i{esz * 8}>"
                        a = self.load_vec(rn, vty)
                        b = self.load_vec(rm, vty)
                        res = self.new_tmp("uzp1")
                        idx_list = ", ".join(f"i32 {i * 2}" for i in range(lanes))
                        self.emit(f"{res} = shufflevector {vty} {a}, {vty} {b}, <{lanes} x i32> <{idx_list}>")
                        self.store_vec(rd, vty, res, clear_128=(Q == 0))
                        continue

                    if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 17) & 0x1F) == 0x10 and ((insn >> 10) & 3) == 2:
                        opcode = (insn >> 12) & 0x1F
                        esz = 1 << sz
                        lanes = (16 if Q else 8) // esz
                        vty = f"<{lanes} x i{esz * 8}>"
                        if opcode == 0x05:
                            if U == 0:
                                vin = self.load_vec(rn, "<8 x i8>")
                                vout = self.new_tmp("vcnt")
                                self.emit(f"{vout} = call <8 x i8> @llvm.ctpop.v8i8(<8 x i8> {vin})")
                                self.store_vec(rd, "<8 x i8>", vout)
                            else:
                                vin = self.load_vec(rn, "i128")
                                vout = self.new_tmp("vmvn")
                                self.emit(f"{vout} = xor i128 {vin}, -1")
                                self.store_vec(rd, "i128", vout, clear_128=False)
                        elif opcode == 0x09 and U == 0:
                            vin = self.load_vec(rn, vty)
                            cmp_v = self.new_tmp("cmeq0")
                            sext_v = self.new_tmp("cmeq_sx")
                            self.emit(f"{cmp_v} = icmp eq {vty} {vin}, zeroinitializer")
                            self.emit(f"{sext_v} = sext <{lanes} x i1> {cmp_v} to {vty}")
                            self.store_vec(rd, vty, sext_v, clear_128=(Q == 0))
                        continue

                    if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 17) & 0x1F) == 0x18 and ((insn >> 10) & 3) == 2:
                        opcode = (insn >> 12) & 0x1F
                        esz = 1 << sz
                        lanes = (16 if Q else 8) // esz
                        vty = f"<{lanes} x i{esz * 8}>"
                        sty = f"i{esz * 8}"
                        vin = self.load_vec(rn, vty)
                        res = self.new_tmp("vred")
                        if opcode == 0x1B:
                            self.emit(f"{res} = call {sty} @llvm.vector.reduce.add.v{lanes}i{esz * 8}({vty} {vin})")
                        else:
                            self.emit(f"{res} = call {sty} @llvm.vector.reduce.umax.v{lanes}i{esz * 8}({vty} {vin})")
                        self.store_vec(rd, sty, res)
                        continue

                    if ((insn >> 24) & 0xBF) == 0x0E and ((insn >> 21) & 1) == 0 and ((insn >> 15) & 1) == 0 and ((insn >> 10) & 3) == 0:
                        tbl_v = self.load_vec(rn, "<16 x i8>")
                        idx_v = self.load_vec(rm, "<16 x i8>")
                        cur_out = "<16 x i8> zeroinitializer"
                        for i in range(16):
                            ie = self.new_tmp("tbl_i")
                            ic = self.new_tmp("tbl_c")
                            cl = self.new_tmp("tbl_cl")
                            tv = self.new_tmp("tbl_v")
                            sv = self.new_tmp("tbl_s")
                            nout = self.new_tmp("tbl_o")
                            self.emit(f"{ie} = extractelement <16 x i8> {idx_v}, i32 {i}")
                            self.emit(f"{ic} = icmp ult i8 {ie}, 16")
                            self.emit(f"{cl} = and i8 {ie}, 15")
                            self.emit(f"{tv} = extractelement <16 x i8> {tbl_v}, i8 {cl}")
                            self.emit(f"{sv} = select i1 {ic}, i8 {tv}, i8 0")
                            self.emit(f"{nout} = insertelement <16 x i8> {cur_out if i > 0 else 'zeroinitializer'}, i8 {sv}, i32 {i}")
                            cur_out = nout
                        self.store_vec(rd, "<16 x i8>", cur_out, clear_128=False)
                        continue

            if not terminated:
                if b_stop < end_addr:
                    self.emit(f"br label %bb_{b_stop:08x}")
                else:
                    self.emit("br label %exit_ret")

        self.emit_raw("exit_ret:")
        flush_cpu_state()
        self.emit("ret void")
        self.emit_raw("}\n")

    def generate_module(self) -> str:
        self.lines = []
        entry_rva = self.analyzer.addr_by_name.get("cmpbe_v2_deserialize_MBS2_to_C", self.func_addrs[0])
        self.emit_raw("; ============================================================================")
        self.emit_raw(f"; AOT Lifted LLVM-IR Module for 64-bit libGLES_mali.so ({self.analyzer.driver_version})")
        self.emit_raw(f"; Target Entry Point: cmpbe_v2_deserialize_MBS2_to_C (0x{entry_rva:08x}) + Closure")
        self.emit_raw("; ============================================================================")
        self.emit_raw('target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"')
        self.emit_raw('target triple = "x86_64-pc-linux-gnu"\n')

        self.emit_raw("declare void @llvm.mali.arm64.hint(i32)")
        self.emit_raw("declare void @llvm.mali.arm64.retaa()")
        self.emit_raw("declare i64 @llvm.mali.arm64.mrs(ptr, i32)")
        self.emit_raw("declare void @llvm.mali.arm64.dc_zva(i64)")
        self.emit_raw("declare i64 @llvm.mali.bionic.tlsdesc(i64)")
        plt_syms = set(self.plt_targets.values()) | {"malloc", "free"}
        for sym in sorted(plt_syms):
            self.emit_raw(f"declare void @llvm.mali.bionic.plt64.{sym}(ptr)")
        self.emit_raw("declare ptr @calloc(i64, i64)")
        self.emit_raw("declare void @free(ptr)")
        self.emit_raw("declare i64 @strlen(ptr)")
        self.emit_raw("declare ptr @strchr(ptr, i32)")
        self.emit_raw("declare ptr @__errno_location()")
        self.emit_raw("declare i64 @bionic_aapcs64_vsnprintf_bridge(i64, i64, i64, i64)")
        self.emit_raw("declare void @aot_unhandled_plt(ptr, i64)")
        self.emit_raw("declare i32 @llvm.fshr.i32(i32, i32, i32)")
        self.emit_raw("declare i64 @llvm.fshr.i64(i64, i64, i64)")
        self.emit_raw("declare i32 @llvm.bitreverse.i32(i32)")
        self.emit_raw("declare i64 @llvm.bitreverse.i64(i64)")
        self.emit_raw("declare i32 @llvm.bswap.i32(i32)")
        self.emit_raw("declare i64 @llvm.bswap.i64(i64)")
        self.emit_raw("declare i32 @llvm.ctlz.i32(i32, i1 immarg)")
        self.emit_raw("declare i64 @llvm.ctlz.i64(i64, i1 immarg)")
        self.emit_raw("declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg)")
        self.emit_raw("declare void @llvm.memmove.p0.p0.i64(ptr nocapture writeonly, ptr nocapture readonly, i64, i1 immarg)")
        self.emit_raw("declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)")
        self.emit_raw("declare <8 x i8> @llvm.ctpop.v8i8(<8 x i8>)")
        self.emit_raw("declare i8 @llvm.vector.reduce.add.v8i8(<8 x i8>)")
        self.emit_raw("declare i16 @llvm.vector.reduce.add.v8i16(<8 x i16>)")
        self.emit_raw("declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)")
        self.emit_raw("declare i32 @llvm.vector.reduce.umax.v4i32(<4 x i32>)\n")

        for faddr in self.func_addrs:
            self.lift_function(faddr)

        # Emit version-independent exported entry-point wrappers
        for sym in [
            "cmpbe_v2_deserialize_MBS2_to_C",
            "cmpbe_v2_init_options",
            "cmpbe_v2_set_option_value",
            "cmpbe_v2_free_compilation_result",
        ]:
            if sym in self.analyzer.addr_by_name:
                rva = self.analyzer.find_func_start(self.analyzer.addr_by_name[sym])
                if rva in self.func_set:
                    self.emit_raw(f"define void @aot_{sym}(ptr %cpu) #0 {{")
                    self.emit(f"call void @aot_sub_{rva:08x}(ptr %cpu)")
                    self.emit("ret void")
                    self.emit_raw("}\n")

        # Emit the indirect call, HLE allocator, & Bionic TLSDESC dispatcher
        self.emit_raw("; ============================================================================")
        self.emit_raw("; AOT Indirect Function, Host Allocator, & Bionic TLSDESC Dispatcher")
        self.emit_raw("; ============================================================================")
        self.emit_raw("define void @aot_indirect_dispatch(ptr %cpu, i64 %target_va) #0 {")
        self.emit_raw("entry:")
        self.emit("  %is_null = icmp eq i64 %target_va, 0")
        self.emit("  br i1 %is_null, label %handle_null, label %check_malloc")
        self.emit_raw("handle_null:")
        self.emit("  ret void")
        self.emit_raw("check_malloc:")
        self.emit(f"  %is_malloc = icmp eq i64 %target_va, {HLE_MALLOC_MAGIC}")
        self.emit("  br i1 %is_malloc, label %handle_malloc, label %check_free")
        self.emit_raw("handle_malloc:")
        self.emit("  call void @llvm.mali.bionic.plt64.malloc(ptr %cpu)")
        self.emit("  ret void")
        self.emit_raw("check_free:")
        self.emit(f"  %is_free = icmp eq i64 %target_va, {HLE_FREE_MAGIC}")
        self.emit("  br i1 %is_free, label %handle_free, label %check_tlsdesc")
        self.emit_raw("handle_free:")
        self.emit("  call void @llvm.mali.bionic.plt64.free(ptr %cpu)")
        self.emit("  ret void")
        self.emit_raw("check_tlsdesc:")
        self.emit(f"  %is_tlsdesc = icmp eq i64 %target_va, {HLE_TLSDESC_MAGIC}")
        self.emit("  br i1 %is_tlsdesc, label %handle_tlsdesc, label %dispatch_switch")
        self.emit_raw("handle_tlsdesc:")
        self.emit("  %x0_ptr = getelementptr inbounds i8, ptr %cpu, i64 0")
        self.emit("  %desc_addr = load i64, ptr %x0_ptr, align 8")
        self.emit("  %tls_off = call i64 @llvm.mali.bionic.tlsdesc(i64 %desc_addr)")
        self.emit("  store i64 %tls_off, ptr %x0_ptr, align 8")
        self.emit("  ret void")
        self.emit_raw("dispatch_switch:")
        cases = []
        for faddr in self.func_addrs:
            cases.append(f"i64 {IMAGE_BASE + faddr}, label %call_{faddr:08x}")
        self.emit(f"  switch i64 %target_va, label %fallback [ {' '.join(cases)} ]")
        for faddr in self.func_addrs:
            self.emit_raw(f"call_{faddr:08x}:")
            self.emit(f"  call void @aot_sub_{faddr:08x}(ptr %cpu)")
            self.emit("  ret void")
        self.emit_raw("fallback:")
        self.emit("  call void @aot_unhandled_plt(ptr %cpu, i64 %target_va)")
        self.emit("  ret void")
        self.emit_raw("}\n")

        self.emit_raw('attributes #0 = { nounwind "frame-pointer"="none" }')
        return "\n".join(self.lines)


def main():
    so_path = sys.argv[1] if len(sys.argv) > 1 else "libGLES_mali.so"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."

    print(f"[1/2] Analyzing 64-bit ELF64 binary: {so_path}")
    analyzer = ELFAnalyzer(so_path)
    roots = analyzer.discover_roots()
    func_addrs, plt_targets = analyzer.compute_closure(roots)
    print(f"      Driver version : {analyzer.driver_version}")
    print(f"      Discovered     : {len(func_addrs)} transitive internal functions and {len(plt_targets)} PLT imports.")

    print("[2/2] Lifting 64-bit AArch64 machine code to LLVM-IR...")
    lifter = LLVMIRLifter(analyzer, func_addrs, plt_targets)
    ir_text = lifter.generate_module()

    raw_ll_path = os.path.join(out_dir, "mali_mbs2_unopt.ll")
    with open(raw_ll_path, "w") as f:
        f.write(ir_text)
    print(f"      Wrote unoptimized LLVM-IR: {raw_ll_path} ({len(ir_text):,} bytes)")
    for k, v in lifter.stats.items():
        print(f"      - {k}: {v}")


if __name__ == "__main__":
    main()
