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
     Uses content-hash baseline tracking (`mesa/.mesa_overlay_state.json`) plus
     mtime comparison so freshly unpacked upstream `mesa/` trees (even when
     extracted with `tar -m` / newer mtimes) NEVER overwrite `mesa_overlay/`
     with unmodified upstream files. Only when `mesa/.mesa_overlay_state.json`
     proves that `mesa/<relpath>` was previously synced from `mesa_overlay/`
     and subsequently edited in-place in `mesa/` does `auto` propagate
     `mesa/<relpath> -> mesa_overlay/<relpath>`.
  2. `--mode to-mesa` (`mesa_overlay/ -> mesa/`):
     Applies all files from `mesa_overlay/` into `mesa/` and updates
     `mesa/.mesa_overlay_state.json`.
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
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

STATE_FILENAME = ".mesa_overlay_state.json"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_sync_state(mesa_dir: Path) -> Dict[str, str]:
    state_path = mesa_dir / STATE_FILENAME
    if not state_path.is_file():
        return {}
    try:
        data = json.loads(state_path.read_text(encoding="utf-8"))
        if isinstance(data, dict):
            return {str(k): str(v) for k, v in data.items()}
    except Exception:
        pass
    return {}


def save_sync_state(mesa_dir: Path, state: Dict[str, str]) -> None:
    if not mesa_dir.is_dir():
        return
    state_path = mesa_dir / STATE_FILENAME
    try:
        state_path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except Exception:
        pass


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

    state = load_sync_state(mesa_dir)
    state_dirty = False

    copied_to_mesa = 0
    copied_to_overlay = 0
    drifted = 0

    for rel in sorted(rel_set):
        rel_key = str(rel)
        ov_file = overlay_dir / rel
        ms_file = mesa_dir / rel

        ov_exists = ov_file.is_file()
        ms_exists = ms_file.is_file()

        if ov_exists and ms_exists:
            if filecmp.cmp(str(ov_file), str(ms_file), shallow=False):
                if mode != "check" and not dry_run:
                    h = sha256_file(ov_file)
                    if state.get(rel_key) != h:
                        state[rel_key] = h
                        state_dirty = True
                continue
            drifted += 1
            ov_hash = sha256_file(ov_file)
            ms_hash = sha256_file(ms_file)
            if mode == "check":
                if not quiet:
                    print(f"[DRIFT] {rel}: mesa_overlay ({ov_hash[:10]}) != mesa ({ms_hash[:10]})")
                continue
            elif mode == "to-mesa":
                direction = "to-mesa"
            elif mode == "from-mesa":
                direction = "from-mesa"
            else:
                # Safe 2-way auto mode:
                # Only sync from-mesa -> mesa_overlay if mesa/.mesa_overlay_state.json
                # records that mesa/<rel> was previously synced to ov_hash AND mesa/<rel>
                # was subsequently modified in-place with a newer mtime.
                prev_hash = state.get(rel_key)
                ov_mtime = ov_file.stat().st_mtime
                ms_mtime = ms_file.stat().st_mtime
                if prev_hash is not None and prev_hash == ov_hash and ms_hash != prev_hash and ms_mtime > ov_mtime + 1e-3:
                    direction = "from-mesa"
                else:
                    direction = "to-mesa"

            if direction == "to-mesa":
                if not quiet:
                    print(f"[sync_mesa_overlay] mesa_overlay/{rel} -> {mesa_dir.name}/{rel}")
                if not dry_run:
                    ms_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ov_file, ms_file)
                    state[rel_key] = ov_hash
                    state_dirty = True
                copied_to_mesa += 1
            else:
                if not quiet:
                    print(f"[sync_mesa_overlay] {mesa_dir.name}/{rel} -> mesa_overlay/{rel}")
                if not dry_run:
                    ov_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ms_file, ov_file)
                    state[rel_key] = ms_hash
                    state_dirty = True
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
                    state[rel_key] = sha256_file(ov_file)
                    state_dirty = True
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
                    state[rel_key] = sha256_file(ms_file)
                    state_dirty = True
                copied_to_overlay += 1

    if state_dirty and not dry_run and mode != "check":
        save_sync_state(mesa_dir, state)

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
        help="Sync direction: 'auto' (hash+mtime 2-way sync), 'to-mesa' (overlay -> mesa), 'from-mesa' (mesa -> overlay), or 'check'",
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
