#!/usr/bin/env python3
"""
decode_csf_breadcrumbs_and_dump.py
----------------------------------
Standalone decoder and analyzer for:
  1. 64-byte per-subqueue CSF breadcrumb cells in `event_bo`:
     +0  (u64): seqno     - Completed ring job sequence number
     +8  (u64): err       - Subqueue CSF error word (SET_STATE ERROR / SYNC_ADD64 error propagate)
     +16 (u64): ls_copy   - Snapshot of r90:r91 (subq_ctx_va) stored before CALL
     +24 (u64): pre_call  - Breadcrumb written immediately BEFORE CALL (r92, r94)
     +32 (u64): post_call - Breadcrumb written immediately AFTER CALL returns, before WAIT #all_sb
     +40 (u64): post_wait - Breadcrumb written immediately AFTER WAIT #all_sb, before SYNC_ADD64
  2. Live `panthor_kbase_dump_csf_diagnostics()` snapshots inside `winlator_log.txt`
  3. 64-bit Valhall CSF bytecode instructions (decoded via `specs/mali_csf_kbase_spec.json`)

Usage:
  python3 scripts/decode_csf_breadcrumbs_and_dump.py [path/to/winlator_log.txt]
  python3 scripts/decode_csf_breadcrumbs_and_dump.py --insn 0x2500424400000004 0x0b000001003f0002
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_JSON = REPO_ROOT / "specs" / "mali_csf_kbase_spec.json"


def load_spec():
    if SPEC_JSON.exists():
        with open(SPEC_JSON, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"csf_instructions": {}}


def decode_csf_insn(qword: int, spec: dict) -> str:
    opcode = (qword >> 56) & 0xFF
    if qword == 0:
        return "NOP (0x0000000000000000)"
    ins_map = {}
    for name, info in spec.get("csf_instructions", {}).items():
        ins_map.setdefault(info["opcode"], []).append((name, info))
    if opcode == 0x01:
        dst = (qword >> 48) & 0xFF
        imm48 = qword & 0xFFFFFFFFFFFF
        return f"MOVE48 r{dst}, 0x{imm48:012x}"
    if opcode == 0x02:
        dst = (qword >> 48) & 0xFF
        imm32 = qword & 0xFFFFFFFF
        return f"MOVE32 r{dst}, 0x{imm32:08x}"
    if opcode == 0x03:
        wmask = (qword >> 16) & 0xFFFF
        return f"WAIT mask=0x{wmask:04x} (slots={[b for b in range(16) if wmask & (1 << b)]})"
    if opcode == 0x0B:
        inc_frag = qword & 0x1
        free_chk = (qword >> 1) & 0x1
        wmask = (qword >> 16) & 0xFFFF
        sig = (qword >> 32) & 0xF
        return (
            f"FINISH_FRAGMENT inc_frag={inc_frag}, free_chunk={free_chk}, "
            f"wait_mask=0x{wmask:04x}, sig_slot={sig}"
        )
    if opcode == 0x20:
        src0 = (qword >> 40) & 0xFF
        src1 = (qword >> 32) & 0xFF
        return f"CALL addr=r{src0}:r{src0+1}, len=r{src1}"
    if opcode in (0x25, 0x26, 0x33, 0x34):
        names = {0x25: "SYNC_ADD32", 0x26: "SYNC_SET32", 0x33: "SYNC_ADD64", 0x34: "SYNC_SET64"}
        addr_reg = (qword >> 40) & 0xFF
        data_reg = (qword >> 32) & 0xFF
        sig_slot = (qword >> 32) & 0xF if addr_reg == 0 else 0
        wmask = (qword >> 16) & 0xFFFF
        scope = (qword >> 1) & 0x3
        err_prop = qword & 0x1
        scope_str = {0: "SYSTEM(0)", 2: "CSG(2)"}.get(scope, f"UNKNOWN({scope})")
        return (
            f"{names[opcode]} [r{addr_reg}], r{data_reg}, scope={scope_str}, "
            f"err_prop={err_prop}, wait_mask=0x{wmask:04x}"
        )
    if opcode in (0x27, 0x35):
        name = "SYNC_WAIT32" if opcode == 0x27 else "SYNC_WAIT64"
        addr_reg = (qword >> 40) & 0xFF
        data_reg = (qword >> 32) & 0xFF
        cond = (qword >> 28) & 0xF
        wmask = (qword >> 16) & 0xFFFF
        cond_str = {0: "LE", 1: "GT", 2: "EQ", 3: "NE", 4: "LT", 5: "GE"}.get(cond, str(cond))
        return f"{name} [r{addr_reg}], r{data_reg}, cond={cond_str}, wait_mask=0x{wmask:04x}"
    if opcode == 0x31:
        op_mode = (qword >> 32) & 0x7
        wmask = (qword >> 16) & 0xFFFF
        mode_str = {0: "VT_START(0)", 1: "VT_END(1)", 3: "FRAG_END(3)"}.get(op_mode, str(op_mode))
        return f"HEAP_OPERATION mode={mode_str}, wait_mask=0x{wmask:04x}"
    if opcode in ins_map:
        return f"{ins_map[opcode][0][0]} (0x{qword:016x})"
    return f"OP_0x{opcode:02x} (0x{qword:016x})"


def classify_subqueue_state(jobs: int, seqno: int, pre: int, post: int, wait: int, active: int) -> str:
    if seqno >= jobs:
        return "IDLE / COMPLETE (seqno == jobs)"
    if pre < jobs and post < jobs:
        if pre == 0 and post == 0 and wait == 0:
            return (
                f"IN-FLIGHT JOB #{seqno + 1}..{jobs} (breadcrumbs disabled or stream not yet reached CALL; "
                f"CS_ACTIVE={active})"
            )
        return f"STALLED BEFORE CALL on Job #{pre + 1} (CS_ACTIVE={active})"
    if pre > post:
        return (
            f"BLOCKED INSIDE SECONDARY COMMAND BUFFER on Job #{pre} "
            f"(pre_call={pre} > post_call={post}, likely waiting at SYNC_WAIT32/64 or WAIT)"
        )
    if post > wait:
        return (
            f"BLOCKED AT END-OF-SLOT SCOREBOARD WAIT (#all_sb) on Job #{post} "
            f"(post_call={post} > post_wait={wait}, hardware iterator or deferred sync still active)"
        )
    return f"BLOCKED AT FINAL SYNC_ADD64 on Job #{wait} (post_wait={wait} > seqno={seqno})"


def parse_winlator_log(log_path: Path, spec: dict) -> int:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    subq_re = re.compile(
        r"\[(?P<name>VT\(0\)|FRAG\(1\)|COMPUTE\(2\))\]\s+csg=(?P<csg>\d+)\s+csi=(?P<csi>\d+)\s+"
        r"jobs=(?P<jobs>\d+)\s+seqno=(?P<seqno>\d+)\(err=0x(?P<err>[0-9a-fA-F]+)\)\s+"
        r"ls=(?P<ls>\d+)\s+pre=(?P<pre>\d+)\s+post=(?P<post>\d+)\s+wait=(?P<wait>\d+)"
    )
    io_re = re.compile(
        r"CS_INSERT=(?P<ins>\d+)\s+CS_EXTRACT=(?P<ext>\d+)\s+unconsumed=(?P<unc>\d+)B\s+"
        r"CS_ACTIVE=(?P<act>\d+)\s+DOORBELL=(?P<db>\d+)"
    )
    fault_re = re.compile(
        r"CS_FAULT=0x(?P<flt>[0-9a-fA-F]+)\(exc=0x(?P<fexc>[0-9a-fA-F]+)\s+(?P<fname>[^)]+)\)\s+"
        r"CS_FATAL=0x(?P<fat>[0-9a-fA-F]+)\(exc=0x(?P<xexc>[0-9a-fA-F]+)\s+(?P<xname>[^)]+)\)"
    )
    notif_re = re.compile(
        r"GPU_QUEUE_GROUP_ERROR\s+kbase_group=(?P<grp>\d+)\s+type=(?P<type>\S+)\s+csi=(?P<csi>\d+)\s+"
        r"status=0x(?P<status>[0-9a-fA-F]+)\s+\(exc=0x(?P<exc>[0-9a-fA-F]+)\s+(?P<ename>[^)]+)\)\s+"
        r"sideband=0x(?P<sb>[0-9a-fA-F]+)"
    )
    alias_err_re = re.compile(r"IOCTL KBASE_IOCTL_MEM_ALIAS.*ret=-1 errno=(?P<errno>\d+)")

    print("=" * 80)
    print(f" CSF Breadcrumb & Diagnostic Log Decoder: {log_path}")
    print("=" * 80)

    alias_errs = [m.group(0) for line in lines for m in [alias_err_re.search(line)] if m]
    if alias_errs:
        print(f"\n[!] Detected {len(alias_errs)} KBASE_IOCTL_MEM_ALIAS error(s):")
        for e in alias_errs[:5]:
            print(f"    {e}")

    for line in lines:
        nm = notif_re.search(line)
        if nm:
            status_val = int(nm.group("status"), 16)
            exc_val = status_val & 0xFF
            acc_type = (status_val >> 8) & 0x3
            addr_valid = (status_val >> 10) & 0x1
            jasid_valid = (status_val >> 11) & 0x1
            true_name = (
                "GPU_SHAREABILITY_FAULT (mali_base_jm_kernel.h:798)"
                if exc_val == 0x88
                else nm.group("ename")
            )
            print(
                f"\n[!] Kernel CSF Notification on CSG {nm.group('grp')} (CSI {nm.group('csi')}):\n"
                f"    raw status = 0x{status_val:08x} -> exc=0x{exc_val:02x} ({true_name})\n"
                f"    access_type=0x{acc_type:x}, address_valid={addr_valid}, jasid_valid={jasid_valid}\n"
                f"    sideband   = 0x{int(nm.group('sb'), 16):016x}"
            )

    snapshots = 0
    for idx, line in enumerate(lines):
        sm = subq_re.search(line)
        if not sm:
            continue
        snapshots += 1
        name = sm.group("name")
        jobs = int(sm.group("jobs"))
        seqno = int(sm.group("seqno"))
        err = int(sm.group("err"), 16)
        ls_val = int(sm.group("ls"))
        pre = int(sm.group("pre"))
        post = int(sm.group("post"))
        wait = int(sm.group("wait"))
        act = 0
        ins_off, ext_off, unc = 0, 0, 0
        if idx + 1 < len(lines):
            im = io_re.search(lines[idx + 1])
            if im:
                ins_off = int(im.group("ins"))
                ext_off = int(im.group("ext"))
                unc = int(im.group("unc"))
                act = int(im.group("act"))
        state_desc = classify_subqueue_state(jobs, seqno, pre, post, wait, act)
        print(
            f"\n  Subqueue {name}: jobs={jobs}, seqno={seqno} (err=0x{err:x}), "
            f"ls_copy={ls_val}, pre_call={pre}, post_call={post}, post_wait={wait}"
        )
        print(f"    USER_IO : CS_INSERT={ins_off}, CS_EXTRACT={ext_off} (unconsumed={unc}B), CS_ACTIVE={act}")
        print(f"    DIAGNOSIS: {state_desc}")
        if idx + 2 < len(lines):
            fm = fault_re.search(lines[idx + 2])
            if fm:
                print(
                    f"    FAULTS  : CS_FAULT=0x{fm.group('flt')} ({fm.group('fname')}), "
                    f"CS_FATAL=0x{fm.group('fat')} ({fm.group('xname')})"
                )

    print("\n" + "=" * 80)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Decode CSF breadcrumbs, ring dumps, and 64-bit CSF instructions.")
    parser.add_argument("log_file", nargs="?", help="Path to winlator_log.txt")
    parser.add_argument("--insn", nargs="+", help="One or more 64-bit hex CSF instructions to decode")
    args = parser.parse_args()

    spec = load_spec()
    if args.insn:
        for raw in args.insn:
            val = int(raw, 16) if raw.startswith("0x") else int(raw)
            print(f"0x{val:016x} -> {decode_csf_insn(val, spec)}")
        return 0

    if args.log_file:
        return parse_winlator_log(Path(args.log_file), spec)

    default_log = ROOT_DIR / "winlator_log.txt"
    if default_log.exists():
        return parse_winlator_log(default_log, spec)

    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
