#!/usr/bin/env python3
"""
scripts/auto_bringup.py
=======================
Automated Bring-Up Diagnostic, Pattern-Recognition, and AOT Verification Tool
for 64-Bit ARM64-Bionic Mali GPU Drivers (libGLES_mali.so).

Automates the 6-step bring-up workflow derived from patterns observed across
r26p0-01eac0, r49p1-03bet0 (Valhall & 5th-Gen builds), and r54p3-00eac0:
  1. ELF64 & PT_LOAD Virtual-to-File Skew Analysis
  2. Symbol Source Detection (.eh_frame DWARF FDEs, .dynsym, .gnu_debugdata)
  3. Structural Pattern Fingerprinting:
       - Context allocator callback offsets (ldp xA, xB, [x0, #imm] -> +0x60, +0x78, +0x80)
       - Vulkan ICD HAL Module Info (HMI, vk_hal_open, vkGetInstanceProcAddr table,
         vkCreateShaderModule, vkDestroyShaderModule)
       - Mali /dev/mali0 (kbase) Kernel UAPI (mali_gpu_props_get ioctl pair delta == 0x34)
       - Stack / Return-Address Protection (TPIDR_EL0 stack canary vs. PACIASP/RETAA)
       - MBS2 binary format version (v35, v44, v47) & VEHW hardware core ID
  4. Transitive Call-Graph Closure & Instruction Set Coverage Audit (via libLLVM-15.so.1)
  5. Bionic PLT Import, Vendor Hook (MediaTek gpud / Android liblog), & /dev/mali0 UAPI Audit
  6. Automated End-to-End C++ LLVM Pass Translation & Native x86-64 Verification
"""

import argparse
import ctypes
import json
import os
import subprocess
import struct
import sys

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT_DIR, "src"))
import mali_arm64_lifter as lifter_mod  # noqa: E402


CORE_NAMES = {
    0x0B: "Bifrost / Valhall Early (0x0b)",
    0x11: "Valhall 2nd/3rd-Gen (0x11, e.g. Mali-G78/G710)",
    0x1B: "Valhall / 5th-Gen (0x1b)",
    0x1C: "5th-Gen Mali (0x1c, e.g. Mali-G720/Immortalis-G720)",
    0x1F: "5th-Gen Flagship Mali (0x1f, e.g. Immortalis-G925)",
}

SUPPORTED_PLT_SYMS = {
    "memcpy", "__memcpy_chk",
    "memmove", "__memmove_chk",
    "memset", "__memset_chk",
    "strlen", "__strlen_chk",
    "strchr", "strcspn", "strncasecmp", "strcmp", "strncmp", "strncat", "memcmp",
    "malloc", "calloc", "free",
    "vsnprintf", "__vsnprintf_chk",
    "__errno",
    "__stack_chk_fail", "abort",
    "ioctl", "open", "__open_2", "open64", "close",
    "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_self", "pthread_once",
    "__android_log_print", "__android_log_write", "property_get", "property_get_int32", "__system_property_get",
    "gpudInitialize", "gpudIsEnabled", "gpudGetDjb2Hash",
    "gpudVkCreateShaderModule", "gpudVkDestroyShaderModule", "gpudVkHackShaderModule",
    "clock_gettime", "fflush", "fprintf", "fwrite", "__fwrite_chk", "getenv", "prctl", "usleep",
}


def check_insn_supported(insn: int) -> bool:
    if insn == 0:
        return True
    op0 = (insn >> 25) & 0xF
    if (op0 & 0xE) == 0x8:
        return ((insn >> 23) & 0x7) in range(8)
    if (op0 & 0xE) == 0xA:
        top6 = (insn >> 26) & 0x3F
        if top6 in (0x05, 0x25) or ((insn >> 24) & 0xFF) == 0x54 or ((insn >> 25) & 0x3F) in (0x1A, 0x1B):
            return True
        if (insn & 0xFE000000) == 0xD6000000 and ((insn >> 21) & 0xF) in (0, 1, 2, 4):
            return True
        if (insn & 0xFFF00000) in (0xD5000000, 0xD5100000, 0xD5200000, 0xD5300000):
            return True
        return False
    if (op0 & 0x5) == 0x4:
        return (
            ((insn >> 24) & 0x3F) == 0x08
            or ((insn >> 27) & 7) in (5, 7)
            or ((insn >> 24) & 0xBF) == 0x0D
            or ((insn >> 23) & 0x1F) in (0x18, 0x19)
        )
    if (op0 & 0x7) == 0x5:
        op1 = (insn >> 28) & 1
        op2 = (insn >> 21) & 0xF
        if op1 == 0 or (op1 == 1 and op2 in (2, 4, 6)):
            return True
        if op1 == 1 and (op2 & 8) == 8 and ((insn >> 29) & 3) == 0 and ((insn >> 21) & 7) in (0, 1, 2, 5, 6):
            return True
        return False
    if (op0 & 0x7) == 0x7:
        if ((insn >> 24) & 0x5F) == 0x1E and ((insn >> 21) & 1) == 1:
            return True
        if ((insn >> 24) & 0x5F) == 0x1F:
            return True
        if (insn & 0xFFE0FC00) == 0x5E000400:
            return True
        if (insn & 0xFFFFFC00) in (0x5E280800, 0x5E281800):
            return True
        if (insn & 0xFFE08C00) == 0x5E000000:
            return True
        if (insn & 0xBF3FFC00) == 0x2E200800:
            return True
        if (insn & 0xBF208400) == 0x2E000000:
            return True
        if ((insn >> 19) & 0x1FF) == 0x1E0 and ((insn >> 10) & 3) == 1:
            return True
        if ((insn >> 21) & 0xFF) == 0x70 and ((insn >> 31) & 1) == 0 and ((insn >> 10) & 1) == 1:
            return True
        if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 21) & 1) == 1 and ((insn >> 10) & 0x3F) == 0x07:
            return True
        if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 21) & 1) == 1 and ((insn >> 10) & 1) == 1:
            return True
        if ((insn >> 23) & 0x3F) == 0x1E and ((insn >> 10) & 1) == 1:
            return True
        if ((insn >> 24) & 0x3F) == 0x0E and ((insn >> 21) & 1) == 0 and ((insn >> 10) & 3) == 2:
            return True
        if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 17) & 0x1F) == 0x10 and ((insn >> 10) & 3) == 2:
            return True
        if ((insn >> 24) & 0x1F) == 0x0E and ((insn >> 17) & 0x1F) == 0x18 and ((insn >> 10) & 3) == 2:
            return True
        if ((insn >> 24) & 0xBF) == 0x0E and ((insn >> 21) & 1) == 0 and ((insn >> 15) & 1) == 0 and ((insn >> 10) & 3) == 0:
            return True
    return False


def analyze_driver(so_path: str, run_e2e: bool = True):
    analyzer = lifter_mod.ELFAnalyzer(so_path)
    raw = analyzer.raw

    # Build ID
    build_id = "none"
    if ".note.gnu.build-id" in analyzer.sections:
        _, off, sz, _ = analyzer.sections[".note.gnu.build-id"]
        if sz >= 20:
            build_id = raw[off + 16 : off + 36].hex()

    # PT_LOAD skew analysis
    e_phoff = struct.unpack_from("<Q", raw, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", raw, 54)
    segment_skews = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", raw, e_phoff + i * e_phentsize
        )
        if p_type == 1:
            skew = p_vaddr - p_offset
            segment_skews.append({"seg": i, "vaddr": hex(p_vaddr), "offset": hex(p_offset), "skew": hex(skew)})

    # Discover Vulkan HMI, vkCreateShaderModule, and mali_gpu_props_get
    hmi_rva = 0
    for name, val, _, _ in analyzer.dynsyms:
        if name == "HMI" and val != 0:
            hmi_rva = val
            break

    vk_create_shader_rva = 0
    if ".rodata" in analyzer.sections and ".data.rel.ro" in analyzer.sections:
        ro_addr, _, ro_sz, _ = analyzer.sections[".rodata"]
        relro_addr, _, relro_sz, _ = analyzer.sections[".data.rel.ro"]
        ro_bytes = bytes(analyzer.elf[ro_addr : ro_addr + ro_sz])
        target_str = b"vkCreateShaderModule\x00"
        idx = ro_bytes.find(target_str)
        while idx != -1 and idx > 0 and ro_bytes[idx - 1] != 0:
            idx = ro_bytes.find(target_str, idx + 1)
        if idx != -1:
            str_va = ro_addr + idx
            for pos in range(0, relro_sz - 48, 8):
                val = struct.unpack_from("<Q", analyzer.elf, relro_addr + pos)[0] & 0xFFFFFFFFFFFF
                if val == str_va:
                    c40 = struct.unpack_from("<Q", analyzer.elf, relro_addr + pos + 40)[0] & 0xFFFFFFFFFFFF
                    c32 = struct.unpack_from("<Q", analyzer.elf, relro_addr + pos + 32)[0] & 0xFFFFFFFFFFFF
                    vk_create_shader_rva = analyzer.find_func_start(c40) or analyzer.find_func_start(c32) or 0
                    break

    kbase_gpuprops_rva = 0
    if ".text" in analyzer.sections:
        t_addr, _, t_sz, _ = analyzer.sections[".text"]
        prev_hit = 0
        for pc in range(t_addr, t_addr + t_sz, 4):
            w = struct.unpack_from("<I", analyzer.elf, pc)[0]
            if w == 0x72A80201:
                if prev_hit != 0 and (pc - prev_hit) == 0x34:
                    kbase_gpuprops_rva = analyzer.find_func_start(pc) or 0
                    break
                prev_hit = pc

    # Closure of cmpbe_v2 entry points
    roots = [
        analyzer.addr_by_name[s]
        for s in [
            "cmpbe_v2_deserialize_MBS2_to_C",
            "cmpbe_v2_init_options",
            "cmpbe_v2_set_option_value",
            "cmpbe_v2_free_compilation_result",
        ]
        if s in analyzer.addr_by_name
    ]
    funcs, plts = analyzer.compute_closure(roots)

    entry_rva = analyzer.addr_by_name["cmpbe_v2_deserialize_MBS2_to_C"]
    ctx_malloc_off = 0x78
    pool_alloc_mode = "ADRP+ADD (direct)"
    for off in range(0, 140, 4):
        w = struct.unpack_from("<I", analyzer.elf, entry_rva + off)[0]
        if (w & 0xFFC003E0) == 0xA9400000:
            ctx_malloc_off = ((w >> 15) & 0x7F) << 3
        if (w & 0xFFC0001F) == 0xF9400001:  # ldr x1, [xN, #imm]
            pool_alloc_mode = "ADRP+LDR (.got indirect)"

    # Scan instructions in closure
    llvm = ctypes.CDLL("/usr/lib/x86_64-linux-gnu/libLLVM-15.so.1")
    llvm.LLVMInitializeAArch64TargetInfo()
    llvm.LLVMInitializeAArch64TargetMC()
    llvm.LLVMInitializeAArch64Disassembler()
    llvm.LLVMCreateDisasm.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    llvm.LLVMCreateDisasm.restype = ctypes.c_void_p
    llvm.LLVMDisasmInstruction.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t
    ]
    llvm.LLVMDisasmInstruction.restype = ctypes.c_size_t
    dcr = llvm.LLVMCreateDisasm(b"aarch64-linux-android", None, 0, None, None)
    out_buf = ctypes.create_string_buffer(256)

    total_insns = 0
    pac_bti_count = 0
    retaa_count = 0
    mrs_tpidr_count = 0
    unhandled_insns = []
    for fn in funcs:
        _, flen = analyzer.func_by_addr.get(fn, (f"sub_{fn:08x}", 4))
        for pc in range(fn, fn + flen, 4):
            total_insns += 1
            w = struct.unpack_from("<I", analyzer.elf, pc)[0]
            if w in (0xD503233F, 0xD50323BF, 0xD503245F, 0xD503249F, 0xD50324DF):
                pac_bti_count += 1
            elif w in (0xD65F0BFF, 0xD65F0FFF):
                retaa_count += 1
            elif (w & 0xFFE00000) == 0xD5200000 and ((w >> 21) & 1) == 1 and ((w >> 5) & 0xFFFF) in (0xDE82, 0xDE83):
                mrs_tpidr_count += 1
            if not check_insn_supported(w):
                llvm.LLVMDisasmInstruction(dcr, bytes(analyzer.elf[pc : pc + 4]), 4, pc, out_buf, 256)
                unhandled_insns.append({"pc": hex(pc), "encoding": hex(w), "disasm": out_buf.value.decode().strip()})

    unhandled_plts = [sym for sym in set(plts.values()) if sym not in SUPPORTED_PLT_SYMS]

    # Scan embedded MBS2 shader blobs
    mbs2_blobs = []
    for off in range(0, len(raw) - 32, 4):
        if raw[off : off + 4] == b"MBS2":
            plen, ver = struct.unpack_from("<II", raw, off + 4)
            if 1 <= ver <= 100 and 32 <= plen <= 16384 and raw[off + 12 : off + 16] == b"VEHW":
                core_id = struct.unpack_from("<I", raw, off + 20)[0]
                mbs2_blobs.append((off, plen + 8, ver, core_id))

    mbs2_ver = mbs2_blobs[0][2] if mbs2_blobs else None
    core_id = mbs2_blobs[0][3] if mbs2_blobs else None

    e2e_passed = False
    if run_e2e and not unhandled_insns and not unhandled_plts:
        res = subprocess.run([os.path.join(ROOT_DIR, "run.sh"), so_path], capture_output=True, text=True)
        e2e_passed = (res.returncode == 0 and "ALL 64-BIT AOT VERIFICATION TESTS COMPLETED" in res.stdout)

    report = {
        "file": os.path.basename(so_path),
        "file_size_bytes": len(raw),
        "driver_version": analyzer.driver_version,
        "build_id_short": build_id[:6],
        "has_gnu_debugdata": ".gnu_debugdata" in analyzer.sections,
        "eh_frame_functions": len(analyzer.func_by_addr),
        "segment_skews": segment_skews,
        "entry_rva": hex(entry_rva),
        "vk_hmi_rva": hex(hmi_rva),
        "vk_create_shader_module_rva": hex(vk_create_shader_rva),
        "kbase_gpu_props_get_rva": hex(kbase_gpuprops_rva),
        "ctx_malloc_offset": hex(ctx_malloc_off),
        "ctx_free_offset": hex(ctx_malloc_off + 8),
        "pool_alloc_resolution": pool_alloc_mode,
        "protection_mode": "ARMv8.3-A PAC/BTI/RETAA" if (pac_bti_count > 0 or retaa_count > 0) else "Bionic TPIDR_EL0 Stack Canary (+0x28)",
        "closure_functions": len(funcs),
        "closure_instructions": total_insns,
        "pac_bti_stripped": pac_bti_count,
        "retaa_lowered": retaa_count,
        "mrs_tpidr_lowered": mrs_tpidr_count,
        "plt_imports": sorted(set(plts.values())),
        "unhandled_instructions": unhandled_insns,
        "unhandled_plt_imports": unhandled_plts,
        "mbs2_version": mbs2_ver,
        "vehw_core_id": hex(core_id) if core_id is not None else "unknown",
        "vehw_core_desc": CORE_NAMES.get(core_id, f"Unknown (0x{core_id:x})") if core_id is not None else "unknown",
        "embedded_mbs2_blobs": len(mbs2_blobs),
        "e2e_native_x86_64_verified": e2e_passed,
    }
    return report


def main():
    parser = argparse.ArgumentParser(description="Automated 64-Bit Mali Driver Bring-Up & Diagnostic Tool")
    parser.add_argument("binaries", nargs="+", help="Path(s) to 64-bit libGLES_mali.so binaries")
    parser.add_argument("--no-e2e", action="store_true", help="Skip running translate.sh / run.sh E2E verification")
    parser.add_argument("--json-out", help="Write JSON report to file")
    args = parser.parse_args()

    reports = []
    for path in args.binaries:
        print(f"================================================================================")
        print(f" [Auto-Bringup] Auditing 64-bit Mali Driver: {path}")
        print(f"================================================================================")
        rep = analyze_driver(os.path.abspath(path), run_e2e=not args.no_e2e)
        reports.append(rep)
        print(f"  Driver Version        : {rep['driver_version']} (build-id: {rep['build_id_short']})")
        print(f"  Target GPU Core       : {rep['vehw_core_desc']} | MBS2 Version: v{rep['mbs2_version']}")
        print(f"  Ctx Allocator Offset  : malloc={rep['ctx_malloc_offset']}, free={rep['ctx_free_offset']} ({rep['pool_alloc_resolution']})")
        print(f"  Vulkan HMI / Shader   : HMI={rep['vk_hmi_rva']}, vkCreateShaderModule={rep['vk_create_shader_module_rva']}, kbase={rep['kbase_gpu_props_get_rva']}")
        print(f"  Protection Mode       : {rep['protection_mode']}")
        print(f"  CMPBE Closure         : {rep['closure_functions']} functions, {rep['closure_instructions']} instructions")
        print(f"  Unhandled Insns/PLTs  : {len(rep['unhandled_instructions'])} insns, {len(rep['unhandled_plt_imports'])} PLTs")
        if not args.no_e2e:
            print(f"  E2E Native x86-64 Run : {'PASS (100% Verified)' if rep['e2e_native_x86_64_verified'] else 'FAIL'}")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(reports, f, indent=2)


if __name__ == "__main__":
    main()
