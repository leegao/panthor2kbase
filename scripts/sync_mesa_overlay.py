#!/usr/bin/env python3
"""
scripts/sync_mesa_overlay.py
----------------------------
Two-way synchronization and drift-audit utility between `mesa_overlay/` (tracked
in git and included in slim archives) and `mesa/` (full Mesa 26.3.0 tree).

Addressing Doc 56 comment `doco.eAzfyYFe1LW8n5dega5p5A`:
  "Add a utility to do this 2-way sync so that mesa_overlay is kept up-to-date"

Modes:
  1. `--mode auto` (default):
     For each file tracked in `mesa_overlay/` (and any modified/untracked files
     under `mesa/` if `mesa/.git` exists or relative to the overlay manifest),
     compares content and mtime between `mesa/<relpath>` and `mesa_overlay/<relpath>`,
     copying the newer modified version to the older/missing side so edits made
     directly in `mesa/` automatically propagate back into `mesa_overlay/` and
     vice versa.
  2. `--mode to-mesa` (`mesa_overlay/ -> mesa/`):
     Applies all files from `mesa_overlay/` into `mesa/`.
  3. `--mode from-mesa` (`mesa/ -> mesa_overlay/`):
     Captures all modified overlay files (plus any new files in `mesa/` listed
     via `--add <relpath>`) from `mesa/` back into `mesa_overlay/`.
  4. `--check`:
     Audits whether `mesa_overlay/` and `mesa/` are in 100% byte-for-byte sync
     (exits 0 if in sync, 1 if any drift is detected).
"""

import argparse
import filecmp
import hashlib
import os
import shutil
import sys
from pathlib import Path
from typing import List, Set, Tuple


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_overlay_relpaths(overlay_dir: Path) -> List[Path]:
    relpaths: List[Path] = []
    if not overlay_dir.is_dir():
        return relpaths
    for root, _, files in os.walk(overlay_dir):
        for fn in files:
            if fn.endswith(".orig") or fn.endswith(".rej") or fn.endswith("~"):
                continue
            full = Path(root) / fn
            relpaths.append(full.relative_to(overlay_dir))
    relpaths.sort()
    return relpaths


def sync_two_way(
    mesa_dir: Path,
    overlay_dir: Path,
    mode: str = "auto",
    extra_paths: List[str] = None,
    dry_run: bool = False,
    quiet: bool = False,
) -> Tuple[int, int, int]:
    """
    Returns (copied_to_mesa, copied_to_overlay, drifted_count).
    """
    overlay_dir.mkdir(parents=True, exist_ok=True)
    rel_set: Set[Path] = set(collect_overlay_relpaths(overlay_dir))
    if extra_paths:
        for p in extra_paths:
            clean = Path(p.strip("/"))
            rel_set.add(clean)

    copied_to_mesa = 0
    copied_to_overlay = 0
    drifted = 0

    for rel in sorted(rel_set):
        ov_file = overlay_dir / rel
        ms_file = mesa_dir / rel

        ov_exists = ov_file.is_file()
        ms_exists = ms_file.is_file()

        if ov_exists and ms_exists:
            if filecmp.cmp(str(ov_file), str(ms_file), shallow=False):
                continue
            drifted += 1
            if mode == "check":
                if not quiet:
                    print(f"[DRIFT] {rel}: mesa_overlay ({sha256_file(ov_file)[:10]}) != mesa ({sha256_file(ms_file)[:10]})")
                continue
            elif mode == "to-mesa":
                direction = "to-mesa"
            elif mode == "from-mesa":
                direction = "from-mesa"
            else:
                # auto: compare mtime (with 1ms tolerance; if mesa file is newer, sync from-mesa -> mesa_overlay)
                ov_mtime = ov_file.stat().st_mtime
                ms_mtime = ms_file.stat().st_mtime
                direction = "from-mesa" if ms_mtime > ov_mtime + 1e-3 else "to-mesa"

            if direction == "to-mesa":
                if not quiet:
                    print(f"[sync_mesa_overlay] mesa_overlay/{rel} -> {mesa_dir.name}/{rel}")
                if not dry_run:
                    ms_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ov_file, ms_file)
                copied_to_mesa += 1
            else:
                if not quiet:
                    print(f"[sync_mesa_overlay] {mesa_dir.name}/{rel} -> mesa_overlay/{rel}")
                if not dry_run:
                    ov_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ms_file, ov_file)
                copied_to_overlay += 1

        elif ov_exists and not ms_exists:
            if not mesa_dir.is_dir():
                continue
            drifted += 1
            if mode == "check":
                if not quiet:
                    print(f"[MISSING IN MESA] {rel}")
                continue
            if mode in ("auto", "to-mesa"):
                if not quiet:
                    print(f"[sync_mesa_overlay] (new) mesa_overlay/{rel} -> {mesa_dir.name}/{rel}")
                if not dry_run:
                    ms_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ov_file, ms_file)
                copied_to_mesa += 1

        elif ms_exists and not ov_exists:
            drifted += 1
            if mode == "check":
                if not quiet:
                    print(f"[MISSING IN OVERLAY] {rel}")
                continue
            if mode in ("auto", "from-mesa"):
                if not quiet:
                    print(f"[sync_mesa_overlay] (new) {mesa_dir.name}/{rel} -> mesa_overlay/{rel}")
                if not dry_run:
                    ov_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ms_file, ov_file)
                copied_to_overlay += 1

    return copied_to_mesa, copied_to_overlay, drifted


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="2-way synchronization utility between mesa_overlay/ and mesa/."
    )
    parser.add_argument(
        "--mesa-dir",
        default=str(repo_root / "mesa"),
        help="Path to target Mesa source tree (default: <repo>/mesa)",
    )
    parser.add_argument(
        "--overlay-dir",
        default=str(repo_root / "mesa_overlay"),
        help="Path to mesa_overlay directory (default: <repo>/mesa_overlay)",
    )
    parser.add_argument(
        "--mode",
        choices=["auto", "to-mesa", "from-mesa", "check"],
        default="auto",
        help="Sync direction: 'auto' (mtime-based 2-way sync), 'to-mesa' (overlay -> mesa), 'from-mesa' (mesa -> overlay), or 'check'",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Shortcut for --mode check (returns non-zero if any file differs)",
    )
    parser.add_argument(
        "--add",
        action="append",
        default=[],
        help="Additional relative path(s) inside mesa/ to begin tracking in mesa_overlay/",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report actions without modifying files",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress per-file progress output",
    )
    args = parser.parse_args()

    mode = "check" if args.check else args.mode
    mesa_dir = Path(args.mesa_dir).resolve()
    overlay_dir = Path(args.overlay_dir).resolve()

    to_mesa, to_ov, drifted = sync_two_way(
        mesa_dir=mesa_dir,
        overlay_dir=overlay_dir,
        mode=mode,
        extra_paths=args.add,
        dry_run=args.dry_run,
        quiet=args.quiet,
    )

    total_tracked = len(collect_overlay_relpaths(overlay_dir))
    if mode == "check":
        if drifted > 0:
            if not args.quiet:
                print(f"[sync_mesa_overlay] CHECK FAILED: {drifted}/{total_tracked} overlay files out of sync.")
            return 1
        if not args.quiet:
            print(f"[sync_mesa_overlay] CHECK OK: all {total_tracked} files in mesa_overlay/ match {mesa_dir.name}/.")
        return 0

    if not args.quiet:
        print(
            f"[sync_mesa_overlay] Completed ({mode}): {total_tracked} tracked files "
            f"(copied {to_mesa} -> {mesa_dir.name}/, {to_ov} -> mesa_overlay/)."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
