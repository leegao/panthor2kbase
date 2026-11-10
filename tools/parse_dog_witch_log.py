#!/usr/bin/env python3
"""
Forensic parser and frame/render time profiler for panthor2kbase Winlator logs
(e.g. winlator_log.txt from DOG WITCH.exe, DiceAMillion.exe, Machick 2.exe, vkcube.exe).

Parses both:
  1. [wrapper] wsi_AcquireNextImageKHR / vkQueuePresentKHR swapchain frame boundaries
  2. [TELEMETRY] lines (including the new frame_ms(avg=...,p99=...,max=...,rnd=...,wait=...)
     and real_fps=... metrics added in Checkpoint v22).
"""

import re
import sys

TELEMETRY_RE = re.compile(
    r"\[(?P<ts>[0-9.]+)\]\s+INFO\s*:\s*\[TELEMETRY\]\s+"
    r"submits=(?P<submits>\d+)\s+"
    r"(?:frames=(?P<frames>\d+)\s+)?"
    r"jobs\([^)]+\)\s+"
    r"fps=(?P<fps>[0-9.]+)"
    r"(?:\(real=(?P<real_fps>[0-9.]+),sub_s=(?P<sub_s>[0-9.]+)\))?\s+"
    r"(?:frame_ms\(avg=(?P<f_avg>[0-9.]+),p99=(?P<f_p99>[0-9.]+),max=(?P<f_max>[0-9.]+),"
    r"rnd=(?P<f_rnd>[0-9.]+),wait=(?P<f_wait>[0-9.]+)\)\s+)?"
    r"submit_us\(avg=(?P<sub_avg>[0-9.]+),max=(?P<sub_max>[0-9.]+):"
    r"\s*norm=(?P<norm>[0-9.]+),(?:clean|flush)=(?P<clean>[0-9.]+),kick=(?P<kick>[0-9.]+)\)\s+"
    r"wait_us\(avg=(?P<w_avg>[0-9.]+)(?:,max=(?P<w_max>[0-9.]+))?\)\s+"
    r"bos=(?P<bos>\d+)\(va=(?P<va>[0-9.]+)MB,committed=(?P<comm>[0-9.]+)MB,"
    r"grow=(?P<grow>\d+),stepdown=(?P<step>\d+),cached=(?P<cached>\d+),imp=(?P<imp>\d+),"
    r"rss=(?P<rss>[0-9.]+)MB,avail=(?P<avail>[0-9.]+)MB"
)

WSI_FRAME_RE = re.compile(
    r"\[(?P<ts>[0-9.]+)\].*(?:wsi_AcquireNextImageKHR|QueuePresentKHR)"
)


def parse_log(path: str) -> None:
    wsi_timestamps = []
    telemetry_rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            wm = WSI_FRAME_RE.search(line)
            if wm:
                wsi_timestamps.append(float(wm.group("ts")))
                continue
            tm = TELEMETRY_RE.search(line)
            if tm:
                d = tm.groupdict()
                d["wsi_count_so_far"] = len(wsi_timestamps)
                telemetry_rows.append(d)

    print(
        f"Parsed {len(telemetry_rows)} [TELEMETRY] snapshots and "
        f"{len(wsi_timestamps)} WSI frame boundaries from {path}"
    )
    print(
        f"{'ts(s)':>9} {'submits':>8} {'real_fps':>9} {'tele_fps':>9} "
        f"{'frm_avg':>8} {'frm_p99':>8} {'rnd_ms':>8} {'wait_ms':>8} "
        f"{'norm_us':>8} {'kick_us':>8} {'va_MB':>7} {'rss_MB':>7} {'avail_MB':>8}"
    )
    prev_ts = None
    prev_wsi = 0
    for r in telemetry_rows:
        ts = float(r["ts"])
        dt = (ts - prev_ts) if prev_ts is not None and ts > prev_ts else 0.0
        wsi_delta = r["wsi_count_so_far"] - prev_wsi
        computed_real_fps = (wsi_delta / dt) if dt > 0 else 0.0
        real_fps = float(r["real_fps"]) if r.get("real_fps") else computed_real_fps
        f_avg = float(r["f_avg"]) if r.get("f_avg") else (1000.0 / real_fps if real_fps > 0 else 0.0)
        f_p99 = float(r["f_p99"]) if r.get("f_p99") else 0.0
        f_rnd = float(r["f_rnd"]) if r.get("f_rnd") else (float(r["sub_avg"]) / 1000.0)
        f_wait = float(r["f_wait"]) if r.get("f_wait") else (float(r["w_avg"]) / 1000.0)
        print(
            f"{ts:9.2f} {int(r['submits']):8d} {real_fps:9.1f} {float(r['fps']):9.1f} "
            f"{f_avg:8.2f} {f_p99:8.2f} {f_rnd:8.2f} {f_wait:8.2f} "
            f"{float(r['norm']):8.1f} {float(r['kick']):8.1f} "
            f"{float(r['va']):7.1f} {float(r['rss']):7.1f} {float(r['avail']):8.1f}"
        )
        prev_ts = ts
        prev_wsi = r["wsi_count_so_far"]


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <winlator_log.txt>")
        sys.exit(0)
    parse_log(sys.argv[1])
