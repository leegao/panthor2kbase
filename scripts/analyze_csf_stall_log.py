#!/usr/bin/env python3
"""
analyze_csf_stall_log.py — Standalone CSF Ringbuffer Wrap, Breadcrumb & Stall Log Analyzer

Parses any Winlator / Panthor-to-Kbase adapter log (`winlator_log.txt` or logcat) and:
  1. Tracks every `DRM_PANTHOR_GROUP_SUBMIT` per subqueue (`VT(0)`, `FRAG(1)`, `COMPUTE(2)`),
     reconstructing exact ringbuffer offsets (`ring_off = prev_insert % 65536`), slot lengths
     (`slot_bytes = insert - prev_insert`), and 64 KiB boundary straddles/wraps.
  2. Automatically detects and highlights any ring slot that straddles the 64 KiB boundary
     (`prev_ring_off + slot_bytes > 65536` without a NOP wrap), explaining which breadcrumbs
     (`PRE_CALL`, `POST_CALL`, `POST_WAIT`, `LS_COPY`, `SEQNO`) landed inside vs. outside `[0, 65536)`.
  3. Summarizes all `KBASE_IOCTL_MEM_ALIAS` errors, `CSF_NOTIFICATION: GPU_QUEUE_GROUP_ERROR`
     faults, and `CSF SUBQUEUE & SYNCOBJ DIAGNOSTIC SNAPSHOT` dumps.
"""

import re
import sys
from collections import defaultdict

RINGBUF_SIZE = 65536
SUBQ_NAMES = {0: "VT(0)", 1: "FRAG(1)", 2: "COMPUTE(2)", 3: "SUBQ(3)"}

RE_SUBMIT = re.compile(
    r"\[(?P<ts>[0-9.]+)\]\s+\w+\s*:\s*DRM_PANTHOR_GROUP_SUBMIT\s+"
    r"q=(?P<q>\d+)\s+stream=(?P<stream>0x[0-9a-fA-F]+)\s+sz=(?P<sz>\d+)\s+"
    r"sync_upgrades=(?P<su>\d+)\s+heap_nops=(?P<hn>\d+)\s+fin_frag_clears=(?P<ffc>\d+)\s+"
    r"->\s+insert=(?P<insert>\d+)\s+jobs=(?P<jobs>\d+)"
)

RE_SNAPSHOT_HDR = re.compile(
    r"\[(?P<ts>[0-9.]+)\]\s+WARN\s*:\s+CSF SUBQUEUE & SYNCOBJ DIAGNOSTIC SNAPSHOT \(reason:\s*(?P<reason>[^)]+)\)"
)

RE_SUBQ_LINE = re.compile(
    r"\[(?P<name>VT\(0\)|FRAG\(1\)|COMPUTE\(2\)|SUBQ\(3\))\]\s+"
    r"csg=(?P<csg>\d+)\s+csi=(?P<csi>\d+)\s+jobs=(?P<jobs>\d+)\s+"
    r"seqno=(?P<seqno>\d+)\(err=(?P<err>0x[0-9a-fA-F]+)\)\s+"
    r"ls=(?P<ls>\d+)\s+pre=(?P<pre>\d+)\s+post=(?P<post>\d+)\s+wait=(?P<wait>\d+)\s+prog=(?P<prog>0x[0-9a-fA-F]+)"
)

RE_CS_IO_LINE = re.compile(
    r"CS_INSERT=(?P<ins>\d+)(?:\(ring_off=\d+\))?\s+"
    r"CS_EXTRACT=(?P<ext>\d+)(?:\(ring_off=\d+,\s*init=(?P<init>\d+)\)|\(init=(?P<init2>\d+)\))\s+"
    r"CS_ACTIVE=(?P<act>\d+)\s+heap\(vt_s=(?P<vts>\d+)\s+vt_e=(?P<vte>\d+)\s+frag_e=(?P<fe>\d+)\)\s+->\s+(?P<stage>.+)"
)

RE_CSF_ERR = re.compile(
    r"\[(?P<ts>[0-9.]+)\]\s+ERROR:\s+CSF_NOTIFICATION:\s+GPU_QUEUE_GROUP_ERROR\s+(?P<rest>.+)"
)

RE_MEM_ALIAS_ERR = re.compile(
    r"\[(?P<ts>[0-9.]+)\]\s+\w+\s*:\s+IOCTL\s+KBASE_IOCTL_MEM_ALIAS.*ret=(?P<ret>-?\d+)\s+errno=(?P<errno>\d+)"
)


def classify_breadcrumbed_320b_straddle(ring_off: int, slot_bytes: int):
    """Explain exactly which qwords of a 320-byte breadcrumbed slot fit before 65536."""
    fit_bytes = RINGBUF_SIZE - ring_off
    overflow_bytes = (ring_off + slot_bytes) - RINGBUF_SIZE
    stages = [
        ("SET_STATE/REQ_RES/HEAP_SET/FLUSH", 0, 96),
        ("PRE_CALL (diag_bo+24)", 96, 128),
        ("CALL (secondary CS stream)", 128, 152),
        ("POST_CALL (diag_bo+32)", 152, 184),
        ("WAIT sb_mask", 184, 192),
        ("POST_WAIT (diag_bo+40)", 192, 224),
        ("LS_COPY (diag_bo+16)", 224, 256),
        ("SEQNO (event_bo+0)", 256, 288),
        ("SYNC_ADD32.csg + ERROR_BARRIER", 288, 320),
    ]
    executed = []
    truncated = []
    for label, start_b, end_b in stages:
        if end_b <= fit_bytes:
            executed.append(f"{label} [+{start_b}..+{end_b}B]")
        else:
            truncated.append(f"{label} [+{start_b}..+{end_b}B -> ring+{ring_off+start_b}..{ring_off+end_b}]")
    return fit_bytes, overflow_bytes, executed, truncated


def analyze_log(path: str) -> int:
    prev_insert = defaultdict(int)
    submits_per_q = defaultdict(list)
    straddles = []
    csf_errors = []
    alias_errors = []
    snapshots = []
    current_snap = None

    with open(path, "r", errors="replace") as f:
        for line_no, raw_line in enumerate(f, 1):
            line = raw_line.rstrip("\n")

            m_sub = RE_SUBMIT.search(line)
            if m_sub:
                q = int(m_sub.group("q"))
                ins_after = int(m_sub.group("insert"))
                jobs = int(m_sub.group("jobs"))
                ins_before = prev_insert[q]
                delta = ins_after - ins_before
                ring_off = ins_before % RINGBUF_SIZE
                # Check if a 320B or 192B slot straddled a 65536 boundary without wrapping
                if (ins_before // RINGBUF_SIZE) != ((ins_after - 1) // RINGBUF_SIZE) and (ins_after % RINGBUF_SIZE) < 512 and delta <= 512:
                    fit_b, ovf_b, exec_s, trunc_s = classify_breadcrumbed_320b_straddle(ring_off, delta)
                    straddles.append({
                        "line": line_no,
                        "ts": m_sub.group("ts"),
                        "q": q,
                        "job": jobs,
                        "ins_before": ins_before,
                        "ins_after": ins_after,
                        "ring_off": ring_off,
                        "slot_bytes": delta,
                        "fit_bytes": fit_b,
                        "overflow_bytes": ovf_b,
                        "executed": exec_s,
                        "truncated": trunc_s,
                        "stream": m_sub.group("stream"),
                        "sz": int(m_sub.group("sz")),
                    })
                prev_insert[q] = ins_after
                submits_per_q[q].append((jobs, ins_before, ins_after, ring_off, delta, m_sub.group("stream"), int(m_sub.group("sz"))))
                continue

            m_err = RE_CSF_ERR.search(line)
            if m_err:
                csf_errors.append((line_no, m_err.group("ts"), m_err.group("rest")))
                continue

            m_al = RE_MEM_ALIAS_ERR.search(line)
            if m_al and int(m_al.group("ret")) < 0:
                alias_errors.append((line_no, m_al.group("ts"), int(m_al.group("errno"))))
                continue

            m_sh = RE_SNAPSHOT_HDR.search(line)
            if m_sh:
                current_snap = {
                    "line": line_no,
                    "ts": m_sh.group("ts"),
                    "reason": m_sh.group("reason"),
                    "subqueues": [],
                }
                snapshots.append(current_snap)
                continue

            if current_snap is not None:
                m_sq = RE_SUBQ_LINE.search(line)
                if m_sq:
                    current_snap["subqueues"].append(m_sq.groupdict())
                    continue
                m_io = RE_CS_IO_LINE.search(line)
                if m_io and current_snap["subqueues"]:
                    current_snap["subqueues"][-1].update(m_io.groupdict())
                    continue

    print("=" * 88)
    print(f" CSF RINGBUFFER & STALL FORENSIC REPORT: {path}")
    print("=" * 88)
    print("1. Submission Totals per Subqueue:")
    for q in sorted(submits_per_q.keys()):
        q_list = submits_per_q[q]
        last_job, _, last_ins, last_off, last_delta, last_strm, last_sz = q_list[-1]
        print(
            f"   - {SUBQ_NAMES.get(q, f'Q({q})'):10s}: {len(q_list):4d} jobs submitted | "
            f"final CS_INSERT={last_ins} (ring_off={last_ins % RINGBUF_SIZE}, last_slot={last_off}..{last_off+last_delta}) | "
            f"last_stream={last_strm} ({last_sz}B)"
        )

    print(f"\n2. Kernel / Hardware Fault Summary:")
    print(f"   - KBASE_IOCTL_MEM_ALIAS errors              : {len(alias_errors)}")
    print(f"   - CSF_NOTIFICATION GPU_QUEUE_GROUP_ERRORs   : {len(csf_errors)}")
    print(f"   - 64 KiB Ringbuffer Boundary Straddles/Overflows: {len(straddles)}")
    print(f"   - Diagnostic Snapshots Captured             : {len(snapshots)}")

    if straddles:
        print("\n3. Detected 64 KiB Ringbuffer Boundary Straddle / Overflow Events:")
        for st in straddles:
            qname = SUBQ_NAMES.get(st["q"], f"Q({st['q']})")
            print(
                f"   [!] Line {st['line']} (ts={st['ts']}): {qname} Job #{st['job']} "
                f"(stream={st['stream']}, sz={st['sz']}B)"
            )
            print(
                f"       insert_offset: {st['ins_before']} -> {st['ins_after']} "
                f"(slot_bytes={st['slot_bytes']}B at ring_off={st['ring_off']})"
            )
            print(
                f"       64 KiB Boundary: {st['fit_bytes']}B landed inside [ring+{st['ring_off']}..65535], "
                f"while {st['overflow_bytes']}B OVERFLOWED past 65536 [65536..{65536+st['overflow_bytes']-1}]!"
            )
            print(f"       Executed before 65536 wrap : {', '.join(st['executed'])}")
            print(f"       Truncated/Skipped at wrap  : {', '.join(st['truncated'])}")

    if snapshots:
        print(f"\n4. Diagnostic Snapshots ({len(snapshots)} total, showing first & last):")
        indices = [0] if len(snapshots) == 1 else [0, len(snapshots) - 1]
        for idx in indices:
            sn = snapshots[idx]
            print(f"   --- Snapshot #{idx + 1} at line {sn['line']} (ts={sn['ts']}, reason={sn['reason']}) ---")
            for sq in sn["subqueues"]:
                ins_v = int(sq.get("ins", 0))
                ext_v = int(sq.get("ext", 0))
                print(
                    f"     [{sq['name']}] jobs={sq['jobs']} seqno={sq['seqno']} ls={sq['ls']} "
                    f"pre={sq['pre']} post={sq['post']} wait={sq['wait']} | "
                    f"CS_INSERT={ins_v}(off={ins_v % RINGBUF_SIZE}) CS_EXTRACT={ext_v}(off={ext_v % RINGBUF_SIZE}) "
                    f"CS_ACTIVE={sq.get('act', '?')} -> {sq.get('stage', '?')}"
                )
    print("=" * 88)
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path/to/winlator_log.txt>")
        sys.exit(1)
    sys.exit(analyze_log(sys.argv[1]))
