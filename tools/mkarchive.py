#!/usr/bin/env python3
"""Archive builder invoked by mkarchive.sh."""
import os
import sys
import zipfile

# Directories excluded from the archive (applied to directories only)
EXCLUDED_TOP_DIRS = {
    ".cache",
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
    "machick_freeze.txt",
    "dogwitch_with_trace_group_fatal_0x02.txt",
    "dogwitch_with_trace_group_fatal_0x02_second_run.txt",
    "dogwitch_tiler_oom.txt",
    "winlator_log.txt",
    "vkcube",
}

GEMINI_EXCLUDED_PREFIXES = (
    "src/aot/bin/",
    "src/aot/r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.",
    "src/aot/r54p3_vulkan_icd/lib/r54p3_vulkan_icd.o",
    "shims/lib/libpanthor_kbase_adapter.so",
)


def is_elf_file(full_path: str) -> bool:
    try:
        with open(full_path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except Exception:
        return False


def should_exclude_dir(rel_dir: str) -> bool:
    rel_norm = rel_dir.replace("\\", "/").strip("./")
    if not rel_norm:
        return False
    parts = rel_norm.split("/")
    for p in parts:
        if p in ("__pycache__", ".git", ".cache"):
            return True
    top = parts[0]
    if top in EXCLUDED_TOP_DIRS:
        return True
    if top == "build" or top.startswith(("build-", "build_")):
        return True
    return False


def should_exclude_file(rel_file: str, target_file: str, gemini_mode: bool, full_path: str = "") -> bool:
    rel_norm = rel_file.replace("\\", "/").strip("./")
    if not rel_norm:
        return True
    if rel_norm == target_file.strip("./") or rel_norm.endswith(".zip"):
        return True
    parts = rel_norm.split("/")

    for p in parts[:-1]:
        if p in ("__pycache__", ".git", ".cache"):
            return True

    # If inside an excluded top directory
    if len(parts) > 1:
        top = parts[0]
        if top in EXCLUDED_TOP_DIRS or top == "build" or top.startswith(("build-", "build_")):
            return True

    # Root-level files (e.g. build_all.sh, build_and_test.sh, compile.sh, Makefile, etc.)
    if len(parts) == 1:
        fname = parts[0]
        if fname in EXCLUDED_ROOT_FILES:
            return True
        if fname.endswith((".zip", ".tar", ".tar.gz", ".tar.xz", ".tgz")):
            return True
        if gemini_mode:
            if fname in GEMINI_EXCLUDED_FILES:
                return True
            if fname.endswith((".so", ".ko", ".o", ".a")):
                return True
            if full_path and is_elf_file(full_path) and not fname.endswith(".sh"):
                return True
        return False

    # Subdirectory files
    if gemini_mode:
        if rel_norm in GEMINI_EXCLUDED_FILES:
            return True
        if any(rel_norm.startswith(pre) for pre in GEMINI_EXCLUDED_PREFIXES):
            return True
        # In src/aot/build/, preserve JSON configs (e.g. mali_aot_icd.x86_64.json), exclude compiled binaries
        if rel_norm.startswith("src/aot/build/"):
            if rel_norm.endswith(".json"):
                return False
            return True
        # In tests/, exclude compiled ELF executables (e.g. unit_tests_edge_cases), but keep all test source/shader files
        if rel_norm.startswith("tests/"):
            if full_path and is_elf_file(full_path):
                return True
        # Exclude temporary object/library files
        if rel_norm.endswith((".o", ".gcda", ".gcno")):
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
                if should_exclude_dir(cand):
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
                full_path = os.path.join(dirpath, fname)
                if should_exclude_file(rel_file, target_file, gemini_mode, full_path):
                    continue
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
