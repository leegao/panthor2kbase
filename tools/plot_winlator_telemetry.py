#!/usr/bin/env python3
"""
Ingests winlator_log.txt (or any specified .txt log file containing [TELEMETRY],
[KBASE_COUNTERS], and/or [wrapper] wsi_AcquireNextImageKHR lines) and plots
frame timings, submission latencies (norm / clean / kick / wait), FPS, and
GPU/system memory usage over time.

Addressing Reviewer Comment doco.r_qcwlva6K6J_gAXgKXMxQ on Doc 39.

Usage:
    python3 tools/plot_winlator_telemetry.py [path/to/winlator_log.txt] [-o output.png] [--csv output.csv]
"""

import argparse
import csv
import os
import re
import sys
from typing import Dict, List, Any

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


TELE_RE = re.compile(
    r"\[(?P<ts>\d+\.\d+)\].*?\[TELEMETRY\]\s+"
    r"submits=(?P<submits>\d+)\s+"
    r"(?:frames=(?P<frames>\d+)\s+)?"
    r"jobs\(vt=(?P<vt>\d+),seq=(?P<seq0>\d+)/(?P<seq1>\d+)/(?P<seq2>\d+)\)\s+"
    r"fps=(?P<tele_fps>[\d\.]+)"
    r"(?:.*?frame_ms\(avg=(?P<frm_avg>[\d\.]+),p99=(?P<frm_p99>[\d\.]+),max=(?P<frm_max>[\d\.]+),"
    r"(?:render|rnd)=(?P<frm_rnd>[\d\.]+),wait=(?P<frm_wait>[\d\.]+)"
    r"(?:,real_fps=(?P<real_fps>[\d\.]+))?\))?\s*"
    r"submit_us\(avg=(?P<sub_avg>[\d\.]+),max=(?P<sub_max>[\d\.]+):\s*"
    r"norm=(?P<norm_us>[\d\.]+),(?:clean|flush)=(?P<clean_us>[\d\.]+),kick=(?P<kick_us>[\d\.]+)\)\s+"
    r"wait_us\(avg=(?P<wait_avg>[\d\.]+),max=(?P<wait_max>[\d\.]+)"
    r"(?:,fast=(?P<fast_waits>\d+),poll=(?P<poll_waits>\d+))?\)\s+"
    r"bos=(?P<bos>\d+)\("
    r"va=(?P<va_mb>[\d\.]+)MB,"
    r"(?:committed=(?P<committed_mb>[\d\.]+)MB,)?"
    r"(?:phys=(?P<phys_mb>[\d\.]+)MB,)?"
    r"(?:grow=(?P<grow>\d+),)?"
    r"(?:stepdown=(?P<stepdown>\d+),)?"
    r"cached=(?P<cached>\d+),imp=(?P<imp>\d+),"
    r"rss=(?P<rss_mb>[\d\.]+)MB,avail=(?P<avail_mb>[\d\.]+)MB"
)

WSI_RE = re.compile(r"\[(?P<ts>\d+\.\d+)\].*?wsi_AcquireNextImageKHR")


def parse_log(log_path: str) -> List[Dict[str, Any]]:
    wsi_timestamps: List[float] = []
    rows: List[Dict[str, Any]] = []

    with open(log_path, "r", errors="replace") as f:
        for line in f:
            wm = WSI_RE.search(line)
            if wm:
                wsi_timestamps.append(float(wm.group("ts")))
                continue
            tm = TELE_RE.search(line)
            if not tm:
                continue
            d = tm.groupdict()
            ts = float(d["ts"])
            submits = int(d["submits"])
            tele_fps = float(d["tele_fps"])
            sub_avg = float(d["sub_avg"])
            sub_max = float(d["sub_max"])
            norm_us = float(d["norm_us"])
            clean_us = float(d["clean_us"])
            kick_us = float(d["kick_us"])
            wait_avg = float(d["wait_avg"])
            wait_max = float(d["wait_max"])
            va_mb = float(d["va_mb"])
            committed_mb = float(d["committed_mb"]) if d.get("committed_mb") else 0.0
            phys_mb = float(d["phys_mb"]) if d.get("phys_mb") else committed_mb
            rss_mb = float(d["rss_mb"])
            avail_mb = float(d["avail_mb"])
            grow = int(d["grow"]) if d.get("grow") else 0
            stepdown = int(d["stepdown"]) if d.get("stepdown") else 0

            frm_avg = float(d["frm_avg"]) if d.get("frm_avg") else 0.0
            frm_p99 = float(d["frm_p99"]) if d.get("frm_p99") else 0.0
            frm_max = float(d["frm_max"]) if d.get("frm_max") else 0.0
            frm_rnd = float(d["frm_rnd"]) if d.get("frm_rnd") else 0.0
            frm_wait = float(d["frm_wait"]) if d.get("frm_wait") else 0.0
            real_fps = float(d["real_fps"]) if d.get("real_fps") else 0.0

            rows.append({
                "ts": ts,
                "submits": submits,
                "tele_fps": tele_fps,
                "real_fps": real_fps,
                "wsi_total": len(wsi_timestamps),
                "frm_avg_ms": frm_avg,
                "frm_p99_ms": frm_p99,
                "frm_max_ms": frm_max,
                "frm_rnd_ms": frm_rnd,
                "frm_wait_ms": frm_wait,
                "sub_avg_us": sub_avg,
                "sub_max_us": sub_max,
                "norm_us": norm_us,
                "clean_us": clean_us,
                "kick_us": kick_us,
                "wait_avg_us": wait_avg,
                "wait_max_us": wait_max,
                "va_mb": va_mb,
                "committed_mb": committed_mb,
                "phys_mb": phys_mb,
                "rss_mb": rss_mb,
                "avail_mb": avail_mb,
                "grow": grow,
                "stepdown": stepdown,
            })

    if not rows:
        return rows

    t0 = rows[0]["ts"]
    for i, r in enumerate(rows):
        r["rel_s"] = r["ts"] - t0
        if i == 0:
            dt = max(r["ts"] - (wsi_timestamps[0] if wsi_timestamps else r["ts"] - 1.0), 0.1)
            dwsi = r["wsi_total"]
            dsub = r["submits"]
        else:
            dt = r["ts"] - rows[i - 1]["ts"]
            if dt <= 0 or dt > 120.0:
                dt = 1.0
            dwsi = max(0, r["wsi_total"] - rows[i - 1]["wsi_total"])
            dsub = max(0, r["submits"] - rows[i - 1]["submits"])

        r["submits_per_s"] = r["tele_fps"] * 2.0
        if r["real_fps"] <= 0.0:
            if dwsi > 0 and dt > 0:
                r["real_fps"] = dwsi / dt
            else:
                r["real_fps"] = r["tele_fps"] / 5.0
        r["submits_per_frame"] = (dsub / dwsi) if dwsi > 0 else (r["submits_per_s"] / max(r["real_fps"], 1.0))
        if r["frm_avg_ms"] <= 0.0 and r["real_fps"] > 0:
            r["frm_avg_ms"] = 1000.0 / r["real_fps"]
            r["frm_rnd_ms"] = (r["sub_avg_us"] * r["submits_per_frame"]) / 1000.0
            r["frm_wait_ms"] = max(0.0, r["frm_avg_ms"] - r["frm_rnd_ms"])

    return rows


def plot_telemetry(rows: List[Dict[str, Any]], out_png: str, title_label: str) -> None:
    xs = list(range(1, len(rows) + 1))
    submits_lbl = [r["submits"] for r in rows]

    fig, axes = plt.subplots(4, 1, figsize=(14, 16), sharex=True)
    fig.suptitle(f"Panthor2Kbase Telemetry & Frame Timing Breakdown Over Time ({title_label})", fontsize=14, fontweight="bold")

    # Panel 1: Real HUD FPS vs Telemetry FPS & Submits/Frame
    ax1 = axes[0]
    ax1.plot(xs, [r["real_fps"] for r in rows], label="Real Presented FPS (HUD / WSI)", color="#1b9e77", linewidth=2.2)
    ax1.plot(xs, [r["tele_fps"] for r in rows], label="Old [TELEMETRY] fps= (submits*0.5/dt)", color="#d95f02", linestyle="--", alpha=0.75)
    ax1.plot(xs, [r["submits_per_frame"] for r in rows], label="GROUP_SUBMITs per Frame", color="#7570b3", linewidth=1.6)
    ax1.set_ylabel("FPS / Submits per Frame")
    ax1.set_title("1. Frame Rate (True HUD FPS vs. Raw Submit Rate) & Submits per Frame")
    ax1.grid(True, linestyle=":", alpha=0.6)
    ax1.legend(loc="upper right")

    # Panel 2: Per-Submit CPU Breakdown (norm_us, clean_us, kick_us, sub_avg_us, wait_avg_us)
    ax2 = axes[1]
    ax2.plot(xs, [r["sub_avg_us"] for r in rows], label="submit_us (Total Avg)", color="#e7298a", linewidth=2.0)
    ax2.plot(xs, [r["norm_us"] for r in rows], label="norm_us (CSF Stream Normalizer)", color="#d95f02", linewidth=1.8)
    ax2.plot(xs, [r["kick_us"] for r in rows], label="kick_us (CS_QUEUE_KICK)", color="#66a61e", linewidth=1.5)
    ax2.plot(xs, [r["clean_us"] for r in rows], label="clean_us (CPU Cache Flush)", color="#e6ab02", linewidth=1.5)
    ax2.set_ylabel("Microseconds (us)")
    ax2.set_title("2. Per-Submit CPU Latency Breakdown (submit_us = norm_us + clean_us + kick_us)")
    ax2.grid(True, linestyle=":", alpha=0.6)
    ax2.legend(loc="upper left")

    # Panel 3: Per-Frame Wall-Clock Breakdown (frame_avg_ms, render_ms, wait_ms)
    ax3 = axes[2]
    ax3.plot(xs, [r["frm_avg_ms"] for r in rows], label="Frame Time Avg (ms)", color="#1f78b4", linewidth=2.0)
    ax3.plot(xs, [r["frm_rnd_ms"] for r in rows], label="CPU Submit / Render Time per Frame (ms)", color="#e31a1c", linewidth=1.8)
    ax3.plot(xs, [r["frm_wait_ms"] for r in rows], label="SYNCOBJ_WAIT / GPU Sync Time per Frame (ms)", color="#33a02c", linewidth=1.6)
    ax3.set_ylabel("Milliseconds (ms)")
    ax3.set_title("3. Per-Frame Timing Breakdown (Total Frame ms vs. CPU Submit/Render ms vs. Sync Wait ms)")
    ax3.grid(True, linestyle=":", alpha=0.6)
    ax3.legend(loc="upper left")

    # Panel 4: Memory Footprint & System Available RAM Over Time
    ax4 = axes[3]
    ax4.plot(xs, [r["va_mb"] for r in rows], label="GPU Virtual Address Space (va_MB)", color="#6a3d9a", linewidth=2.0)
    ax4.plot(xs, [r["rss_mb"] for r in rows], label="Process RSS (rss_MB)", color="#ff7f00", linewidth=2.0)
    ax4.plot(xs, [r["committed_mb"] for r in rows], label="Adapter Committed BO RAM (committed_MB)", color="#1f78b4", linewidth=1.6)
    ax4.plot(xs, [r["avail_mb"] for r in rows], label="System MemAvailable (avail_MB)", color="#b15928", linewidth=2.2, linestyle="-.")
    ax4.set_ylabel("Megabytes (MB)")
    ax4.set_xlabel("Telemetry Sample Index (Annotated with Total Submits)")
    ax4.set_title("4. GPU & System Memory Pressure Over Time (va_MB, rss_MB, committed_MB, avail_MB)")
    ax4.grid(True, linestyle=":", alpha=0.6)
    ax4.legend(loc="upper left")

    step = max(1, len(xs) // 12)
    tick_indices = xs[::step]
    tick_labels = [f"#{i}\n({submits_lbl[i-1]} sub)" for i in tick_indices]
    ax4.set_xticks(tick_indices)
    ax4.set_xticklabels(tick_labels, rotation=30, ha="right", fontsize=8)

    plt.tight_layout(rect=[0, 0.01, 1, 0.97])
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    fig.savefig(out_png, dpi=150)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description="Ingest winlator_log.txt and plot frame timings & telemetry trends over time.")
    parser.add_argument("log_file", nargs="?", default="winlator_log.txt", help="Path to winlator_log.txt or other telemetry .txt log")
    parser.add_argument("-o", "--output", default="artifacts/winlator_telemetry_plot.png", help="Output PNG plot path")
    parser.add_argument("--csv", default="artifacts/winlator_telemetry_metrics.csv", help="Output CSV metrics path")
    args = parser.parse_args()

    if not os.path.exists(args.log_file):
        print(f"Error: log file '{args.log_file}' not found.", file=sys.stderr)
        return 1

    rows = parse_log(args.log_file)
    if not rows:
        print(f"No [TELEMETRY] lines found in '{args.log_file}'.", file=sys.stderr)
        return 1

    if args.csv:
        os.makedirs(os.path.dirname(os.path.abspath(args.csv)), exist_ok=True)
        with open(args.csv, "w", newline="") as cf:
            writer = csv.DictWriter(cf, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)

    plot_telemetry(rows, args.output, os.path.basename(args.log_file))
    print(f"Parsed {len(rows)} telemetry windows from {args.log_file}:")
    print(f"  - Plot saved to: {args.output}")
    print(f"  - CSV saved to : {args.csv}")
    print(f"  - Peak norm_us : {max(r['norm_us'] for r in rows):.1f} us (avg {sum(r['norm_us'] for r in rows)/len(rows):.1f} us)")
    print(f"  - Peak va_MB   : {max(r['va_mb'] for r in rows):.1f} MB | Min avail_MB: {min(r['avail_mb'] for r in rows):.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
