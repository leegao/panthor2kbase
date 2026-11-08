#!/usr/bin/env python3
"""Fallback archive builder invoked by mkarchive.sh."""
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

GEMINI_EXCLUDED_FILES = {
    "libGLES_mali.so",
    "libGLES_mali.r44p1.so",
    "mali_kbase.ko",
    "libvulkan_wrapper.so",
}

GEMINI_EXCLUDED_PREFIXES = (
    "src/aot/build/",
    "src/aot/bin/",
    "src/aot/r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.",
    "src/aot/r54p3_vulkan_icd/lib/r54p3_vulkan_icd.o",
    "shims/lib/libpanthor_kbase_adapter.so",
)


def should_exclude(rel_path: str, target_file: str, gemini_mode: bool) -> bool:
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
    if gemini_mode:
        if rel_norm in GEMINI_EXCLUDED_FILES:
            return True
        if any(rel_norm.startswith(p) for p in GEMINI_EXCLUDED_PREFIXES):
            return True
        if rel_norm.startswith("tests/") and not rel_norm.endswith((".c", ".cpp", ".h", ".py", ".sh", ".md", ".txt")):
            return True
    return False


def main() -> int:
    args = sys.argv[1:]
    gemini_mode = False
    while args and args[0] in ("-g", "--gemini"):
        gemini_mode = True
        args.pop(0)
    target_file = args[0] if args else "archive.zip"
    root_dir = "."
    count = 0
    with zipfile.ZipFile(
        target_file, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as zf:
        for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
            rel_dir = os.path.relpath(dirpath, root_dir)
            if rel_dir == ".":
                rel_dir = ""
            kept_dirs = []
            for d in sorted(dirnames):
                cand = f"{rel_dir}/{d}" if rel_dir else d
                if should_exclude(cand, target_file, gemini_mode):
                    continue
                full_d = os.path.join(dirpath, d)
                if os.path.islink(full_d):
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
                if should_exclude(rel_file, target_file, gemini_mode):
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
    print(f"[mkarchive.py] Packaged {count} entries into {target_file} (gemini_mode={gemini_mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
