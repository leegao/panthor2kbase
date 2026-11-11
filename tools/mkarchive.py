#!/usr/bin/env python3
"""
Archive builder invoked by mkarchive.sh.

Supports:
  1. Slim Mode (--gemini / --slim / -s / -g):
     Generates a complete source, overlay, and build script archive (<10 MB for Google Drive)
     containing all source code, tests, scripts, shims, and CRUCIALLY all 22 files in mesa_overlay/
     and mesa_subproject/, while excluding large binary kernel modules, compiled shared libraries,
     compiled ELF binaries, and multi-megabyte log files.
  2. Full Mode (--full / default):
     Generates a standalone repository package (<100 MB for sandbox sharing) including
     kernel modules (kernel/mali_kbase_mt6878_r44p1.ko, kernel/mali_kbase_husky_r54p1.ko),
     firmware (kernel/mali_csffw.bin), qemu-arm-static, shims/lib/libdrm.a,
     all tests, and the complete mesa_overlay/ tree, while excluding the 1+ GB upstream mesa/ checkout,
     .git, .cache, and build directories.
"""
import os
import sys
import zipfile

EXCLUDED_TOP_DIRS = {
    ".cache",
    ".git",
    "archives",
    "External",
    "kittytoy",
    "screenshots",
    "mesa",
    "sandbox_bundles",
}

EXCLUDED_ROOT_FILES = {
    "llvm.zip",
    "include.zip",
    "AGENTS.md",
    "CLAUDE.md",
    "archive.zip",
    "patch.diff",
}

LARGE_LOG_FILES = {
    "machick_freeze.txt",
    "dogwitch_with_trace_group_fatal_0x02.txt",
    "dogwitch_with_trace_group_fatal_0x02_second_run.txt",
    "dogwitch_tiler_oom.txt",
    "winlator_log.txt",
    "wrapper_log.txt",
}

FULL_DUPLICATE_BINARIES = {
    "kernel/mali_kbase_mtk.ko",
    "kernel/mali_kbase_husky.ko",
    "libGLES_mali.so",
    "libGLES_mali.r44p1.so",
    "src/aot/r54p3_vulkan_icd/lib/libGLES_mali.so",
    "libvulkan_wrapper.so",
    "mali_kbase.ko",
    "vkcube",
}


def is_elf_file(full_path: str) -> bool:
    try:
        with open(full_path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def should_exclude_dir(rel_dir: str) -> bool:
    rel_norm = rel_dir.replace("\\", "/").removeprefix("./").strip("/")
    if not rel_norm:
        return False
    parts = rel_norm.split("/")
    for p in parts:
        if p in ("__pycache__", ".git", ".cache"):
            return True
    top = parts[0]
    if top in ("mesa_overlay", "mesa_subproject"):
        return False
    if top in EXCLUDED_TOP_DIRS:
        return True
    if top == "build" or top.startswith(("build-", "build_")):
        return True
    return False


def should_exclude_file(rel_file: str, target_file: str, slim_mode: bool, full_path: str = "") -> bool:
    rel_norm = rel_file.replace("\\", "/").removeprefix("./").strip("/")
    if not rel_norm:
        return True
    target_norm = target_file.replace("\\", "/").removeprefix("./").strip("/")
    if rel_norm == target_norm:
        return True

    parts = rel_norm.split("/")
    if len(parts) > 1:
        top = parts[0]
        if top in ("mesa_overlay", "mesa_subproject"):
            pass
        elif top in EXCLUDED_TOP_DIRS or top == "build" or top.startswith(("build-", "build_")):
            return True

    fname = parts[-1]
    if fname.endswith((".zip", ".tar", ".tar.gz", ".tar.xz", ".tgz", ".pyc", ".gcda", ".gcno")):
        return True

    if len(parts) == 1:
        if fname in EXCLUDED_ROOT_FILES:
            return True

    if slim_mode:
        if fname in LARGE_LOG_FILES:
            return True
        if fname in ("libGLES_mali.so", "libGLES_mali.r44p1.so", "mali_kbase.ko", "libvulkan_wrapper.so", "vkcube"):
            return True
        if any(rel_norm.startswith(pre) for pre in ("src/aot/bin/", "predownloaded/", "src/aot/r54p3_vulkan_icd/lib/", "sandbox_bundles/")):
            return True
        if fname.endswith((".ko", ".so", ".o", ".a", ".dll", ".ll")):
            return True
        if full_path and is_elf_file(full_path) and not fname.endswith(".sh"):
            return True
    else:
        if rel_norm in FULL_DUPLICATE_BINARIES:
            return True
        if fname in LARGE_LOG_FILES:
            return True
        if rel_norm.startswith("build"):
            return True

    return False


def create_archive(target_file: str, slim_mode: bool) -> int:
    root_dir = "."
    count = 0
    os.makedirs(os.path.dirname(os.path.abspath(target_file)), exist_ok=True)
    with zipfile.ZipFile(
        target_file, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as zf:
        for dirpath, dirnames, filenames in os.walk(root_dir, followlinks=False):
            rel_dir = os.path.relpath(dirpath, root_dir).replace("\\", "/")
            if rel_dir == ".":
                rel_dir = ""
            kept_dirs = []
            for d in sorted(dirnames):
                child_rel = f"{rel_dir}/{d}" if rel_dir else d
                full_d = os.path.join(dirpath, d)
                if should_exclude_dir(child_rel):
                    continue
                if os.path.islink(full_d):
                    zi = zipfile.ZipInfo(child_rel)
                    zi.create_system = 3
                    zi.external_attr = 0o120777 << 16
                    zf.writestr(zi, os.readlink(full_d))
                    count += 1
                else:
                    kept_dirs.append(d)
            dirnames[:] = kept_dirs

            for fname in sorted(filenames):
                rel_file = f"{rel_dir}/{fname}" if rel_dir else fname
                full_path = os.path.join(dirpath, fname)
                if should_exclude_file(rel_file, target_file, slim_mode, full_path):
                    continue
                if os.path.islink(full_path):
                    zi = zipfile.ZipInfo(rel_file)
                    zi.create_system = 3
                    zi.external_attr = 0o120777 << 16
                    zf.writestr(zi, os.readlink(full_path))
                    count += 1
                elif os.path.isfile(full_path):
                    zf.write(full_path, arcname=rel_file)
                    count += 1
    size_mb = os.path.getsize(target_file) / (1024 * 1024)
    print(f"[mkarchive.py] Packaged {count} entries into {target_file} ({size_mb:.2f} MB, slim_mode={slim_mode})")
    return 0


def main() -> int:
    args = sys.argv[1:]
    slim_mode = False
    clean_args = []
    for a in args:
        if a in ("-g", "--gemini", "-s", "--slim"):
            slim_mode = True
        elif a in ("-f", "--full"):
            slim_mode = False
        else:
            clean_args.append(a)
    target_file = clean_args[0] if clean_args else "archive.zip"
    return create_archive(target_file, slim_mode)


if __name__ == "__main__":
    raise SystemExit(main())
