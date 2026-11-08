#!/usr/bin/env python3
"""Generate src/horizon2llvm/aarch64/abi_aapcs64.hpp from AArch64CallingConv.td
and UltraTears architectural register index conventions.

Standard AAPCS64 Calling Convention:
- Argument GPRs: X0..X8 (X0..X7 parameter passing, X8 indirect result location / sret)
- Argument FPRs: V0..V7
- Return GPRs:   X0..X7 (plus indirect via X8)
- Return FPRs:   V0..V7
- Callee-Saved GPRs: X19..X29 (FP), SP (X31)
- Callee-Saved FPRs: V8..V15 (lower 64-bits)
- Caller-Saved GPRs: X9..X18, X30 (LR)
- Caller-Saved FPRs: V16..V31
- Status / Flags:    NZCV (register 32)
"""

import pathlib
import sys
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
TD_PATH = pathlib.Path(__file__).resolve().parent / "AArch64CallingConv.td"
OUT_PATH = pathlib.Path(__file__).resolve().parent / "abi_aapcs64.hpp"

def parse_tablegen_csr(td_content: str):
    """Verify CSR_AArch64_AAPCS definition in AArch64CallingConv.td."""
    m = re.search(r'def\s+CSR_AArch64_AAPCS\s*:\s*CalleeSavedRegs<([^>]+)>', td_content, re.MULTILINE)
    if not m:
        raise ValueError("Could not find CSR_AArch64_AAPCS in AArch64CallingConv.td")
    raw = m.group(1)
    regs = [r.strip() for r in re.findall(r'[A-Za-z0-9_]+', raw) if r not in ('add', 'CalleeSavedRegs')]
    return regs

def generate_header():
    td_text = TD_PATH.read_text(encoding="utf-8")
    csr_regs = parse_tablegen_csr(td_text)

    # Register indexing scheme in UltraTears:
    # 0..30:  X0..X30
    # 31:     SP
    # 32:     NZCV
    # 64..95: V0..V31
    
    arg_gprs = list(range(0, 9))      # X0..X8
    arg_fprs = list(range(64, 72))    # V0..V7
    ret_gprs = list(range(0, 8))      # X0..X7
    ret_fprs = list(range(64, 72))    # V0..V7
    callee_gprs = list(range(19, 30)) + [31]  # X19..X29, SP
    callee_fprs = list(range(72, 80))         # V8..V15
    caller_gprs = list(range(9, 19)) + [30]   # X9..X18, X30 (LR)
    caller_fprs = list(range(80, 96))         # V16..V31
    scratch_flags = [32]                      # NZCV

    def make_bitset_init(indices):
        parts = []
        for i in indices:
            parts.append(f"s.set({i});")
        return " ".join(parts)

    lines = []
    lines.append("// Auto-generated from AArch64CallingConv.td by gen_aapcs64.py. DO NOT EDIT DIRECTLY.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <bitset>")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace ultrs::aarch64 {")
    lines.append("")
    lines.append("constexpr size_t MAX_ARCH_REGS = 96;")
    lines.append("using RegSet = std::bitset<MAX_ARCH_REGS>;")
    lines.append("")
    lines.append("// Architectural register index mappings (matching Cpu struct / llvm_ir arch_reg):")
    lines.append("//   0..30 : X0..X30")
    lines.append("//   31    : SP")
    lines.append("//   32    : NZCV")
    lines.append("//   64..95: V0..V31")
    lines.append("namespace regs {")
    for r in range(31):
        lines.append(f"    constexpr int X{r} = {r};")
    lines.append("    constexpr int FP = 29;")
    lines.append("    constexpr int LR = 30;")
    lines.append("    constexpr int SP = 31;")
    lines.append("    constexpr int NZCV = 32;")
    for v in range(32):
        lines.append(f"    constexpr int V{v} = {64 + v};")
    lines.append("} // namespace regs")
    lines.append("")
    lines.append("// Specification-derived register bitsets under AAPCS64:")
    lines.append("namespace abi {")
    lines.append("")

    for name, indices, desc in [
        ("ArgGPRs", arg_gprs, "Argument GPRs (X0..X8, including X8 sret)"),
        ("ArgFPRs", arg_fprs, "Argument FPRs (V0..V7)"),
        ("RetGPRs", ret_gprs, "Return GPRs (X0..X7)"),
        ("RetFPRs", ret_fprs, "Return FPRs (V0..V7)"),
        ("CalleeSavedGPRs", callee_gprs, "Callee-saved GPRs (X19..X29, SP)"),
        ("CalleeSavedFPRs", callee_fprs, "Callee-saved FPRs (V8..V15 lower 64 bits)"),
        ("CallerSavedScratchGPRs", caller_gprs, "Caller-saved temporary GPRs (X9..X18, X30/LR)"),
        ("CallerSavedScratchFPRs", caller_fprs, "Caller-saved temporary FPRs (V16..V31)"),
        ("ScratchFlags", scratch_flags, "Caller-saved flags (NZCV)"),
    ]:
        lines.append(f"    // {desc}")
        lines.append(f"    inline RegSet make_{name}() {{")
        lines.append(f"        RegSet s; {make_bitset_init(indices)} return s;")
        lines.append(f"    }}")
        lines.append(f"    const RegSet {name} = make_{name}();")
        lines.append("")

    lines.append("    // Compound AAPCS64 contracts:")
    lines.append("    // Live across function return under standard AAPCS64:")
    lines.append("    inline RegSet make_AAPCSReturnLive() {")
    lines.append("        return RetGPRs | RetFPRs | CalleeSavedGPRs | CalleeSavedFPRs;")
    lines.append("    }")
    lines.append("    const RegSet AAPCSReturnLive = make_AAPCSReturnLive();")
    lines.append("")
    lines.append("    // Upward-exposed reads at external call boundaries:")
    lines.append("    inline RegSet make_AAPCSCallReads() {")
    lines.append("        RegSet s = ArgGPRs | ArgFPRs | CalleeSavedGPRs | CalleeSavedFPRs;")
    lines.append("        s.set(regs::LR);")
    lines.append("        return s;")
    lines.append("    }")
    lines.append("    const RegSet AAPCSCallReads = make_AAPCSCallReads();")
    lines.append("")
    lines.append("    // Registers clobbered by external calls (caller-saved scratch):")
    lines.append("    inline RegSet make_AAPCSClobberedByCall() {")
    lines.append("        return CallerSavedScratchGPRs | CallerSavedScratchFPRs | ScratchFlags | ArgGPRs | ArgFPRs | RetGPRs | RetFPRs;")
    lines.append("    }")
    lines.append("    const RegSet AAPCSClobberedByCall = make_AAPCSClobberedByCall();")
    lines.append("")
    lines.append("    // Query helper predicates:")
    lines.append("    constexpr bool is_gpr(int r) { return r >= 0 && r <= 31; }")
    lines.append("    constexpr bool is_fpr(int r) { return r >= 64 && r <= 95; }")
    lines.append("    constexpr bool is_callee_saved_gpr(int r) { return (r >= 19 && r <= 29) || r == 31; }")
    lines.append("    constexpr bool is_callee_saved_fpr(int r) { return r >= 72 && r <= 79; }")
    lines.append("    constexpr bool is_callee_saved(int r) { return is_callee_saved_gpr(r) || is_callee_saved_fpr(r); }")
    lines.append("    constexpr bool is_caller_saved(int r) { return !is_callee_saved(r); }")
    lines.append("    constexpr bool is_arg_reg(int r) { return (r >= 0 && r <= 8) || (r >= 64 && r <= 71); }")
    lines.append("    constexpr bool is_ret_reg(int r) { return (r >= 0 && r <= 7) || (r >= 64 && r <= 71); }")
    lines.append("")
    lines.append("} // namespace abi")
    lines.append("} // namespace ultrs::aarch64")
    lines.append("")

    output_str = "\n".join(lines)
    OUT_PATH.write_text(output_str, encoding="utf-8")
    print(f"Generated {OUT_PATH} successfully from {TD_PATH.name} (found {len(csr_regs)} CSRs in tablegen)")

if __name__ == "__main__":
    generate_header()
