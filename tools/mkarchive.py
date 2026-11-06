#!/usr/bin/env python3
"""Fallback archive builder invoked by mkarchive.sh when the `zip` binary is absent."""
import os
import sys
import zipfile

EXCLUDED_TOP_DIRS = {
    ".cache",
    "build",
    ".git",
    "archives",
    "External",
    "kittytoy",
    "screenshots",
    "mesa",
}

EXCLUDED_ROOT_FILES = {
    "llvm.zip",
    "include.zip",
    "AGENTS.md",
    "CLAUDE.md",
    "archive.zip",
}


def should_exclude(rel_path: str, target_file: str) -> bool:
    rel_norm = rel_path.replace("\\", "/").lstrip("./")
    if not rel_norm:
        return True
    if rel_norm == target_file.lstrip("./"):
        return True
    parts = rel_norm.split("/")
    top = parts[0]
    if top in EXCLUDED_TOP_DIRS or top.startswith("build"):
        return True
    if len(parts) == 1:
        if top in EXCLUDED_ROOT_FILES or top.endswith(".zip"):
            return True
    return False


def main() -> int:
    target_file = sys.argv[1] if len(sys.argv) > 1 else "archive.zip"
    root_dir = "."
    count = 0
    with zipfile.ZipFile(
        target_file, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as zf:
        for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
            rel_dir = os.path.relpath(dirpath, root_dir)
            if rel_dir == ".":
                rel_dir = ""
            # Prune excluded top-level directories in-place
            kept_dirs = []
            for d in sorted(dirnames):
                cand = f"{rel_dir}/{d}" if rel_dir else d
                if should_exclude(cand, target_file):
                    continue
                full_d = os.path.join(dirpath, d)
                if os.path.islink(full_d):
                    # Preserve directory symlinks like -y in zip
                    zi = zipfile.ZipInfo(cand)
                    zi.create_system = 3
                    zi.external_attr = (0o120777 << 16)
                    zf.writestr(zi, os.readlink(full_d))
                    count += 1
                    continue
                kept_dirs.append(d)
            dirnames[:] = kept_dirs

            for fname in sorted(filenames):
                rel_file = f"{rel_dir}/{fname}" if rel_dir else fname
                if should_exclude(rel_file, target_file):
                    continue
                full_path = os.path.join(dirpath, fname)
                if os.path.islink(full_path):
                    zi = zipfile.ZipInfo(rel_file)
                    zi.create_system = 3
                    zi.external_attr = (0o120777 << 16)
                    zf.writestr(zi, os.readlink(full_path))
                    count += 1
                elif os.path.isfile(full_path):
                    zf.write(full_path, arcname=rel_file)
                    count += 1
    print(f"[mkarchive.py] Packaged {count} entries into {target_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
