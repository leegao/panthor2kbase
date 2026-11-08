#!/usr/bin/env bash
# ============================================================================
# scripts/run_aot_lifter.sh
# Unified CLI Driver & Guidance Tool for Static AArch64-to-LLVM/C++ Binary
# Lifting, Bionic-to-glibc Stub Generation, XCB/Display vkcube WSI, vulkaninfo
# Introspection, and 40-Sample + PanProbe Compliance Testing.
# (Addresses Doc 18 Comment doco.RSWY0VtAij-YjN-cZ79deA)
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
    cat << 'HELP'
================================================================================
 Panthor-Kbase AOT Lifter & Vulkan ICD Verification CLI (scripts/run_aot_lifter.sh)
================================================================================
Usage:
  ./scripts/run_aot_lifter.sh <command> [options]

Commands:
  --help                   Show this detailed command reference and examples.
  --gen-stubs              Auto-generate Bionic-to-glibc PLT hooks (mali_stubs_*_hook)
                           and system property tables from host glibc headers and
                           libhybris-arlinux into src/aot/generated/mali_bionic_stubs_autogen.h.
  --inspect-props [so]     Extract and list all Android __system_property_get keys
                           referenced by an AArch64 driver binary (default: libGLES_mali.r44p1.so).
  --lift-so <path.so>      Lift an AArch64 Android ELF64 .so (libGLES_mali.so or
                           libvulkan_panfrost.so) in conformance-first mode (--no-opt-passes,
                           with zero custom IR optimization passes; only clang -O2 runs at codegen).
  --vulkaninfo             Run driver-accurate Vulkan physical-device, extension (170 exts),
                           queue-family, memory-type, and pNext (DriverProperties) introspection
                           against the AOT-lifted libvulkan_mali_aot_icd.so.
  --run-vkcube-xcb         Run Khronos vkcube against libvulkan_mali_aot_icd.so using XCB/X11 WSI
                           (WAYLAND_DISPLAY= VK_ICD_FILENAMES=.../mali_aot_icd.x86_64.json vkcube --wsi xcb)
                           or headless display fallback when DISPLAY is unset.
  --run-40-samples         Run the expanded 40 Khronos Vulkan-Samples + PanProbe compliance suite
                           against libvulkan_mali_aot_icd.so via the system libvulkan.so.1 loader.
  --decode-csf <log.txt>   Decode CSF ring/stream instructions, breadcrumbs, and fault snapshots
                           from a captured Winlator log file.
================================================================================
HELP
}

if [ $# -eq 0 ] || [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

CMD="$1"
shift

case "$CMD" in
    --gen-stubs)
        python3 scripts/gen_bionic_glibc_stubs.py
        ;;
    --inspect-props)
        SO_PATH="${1:-/working_dir/c_3106ac7496559798/user_uploaded_files/ttl=63d/user_uploaded/libGLES_mali.r44p1.so}"
        python3 -c "
import re, sys
with open('$SO_PATH', 'rb') as f:
    data = f.read()
props = sorted(set(m.group(0).decode('ascii') for m in re.finditer(rb'(?:debug|ro|persist|vendor)\.(?:mali|gpu|egl|vk|sf|build|hardware|debuggable)[a-zA-Z0-9_.\-]+', data)))
print(f'Found {len(props)} system properties in $SO_PATH:')
for p in props:
    print('  -', p)
"
        ;;
    --lift-so)
        SO_PATH="${1:-src/aot/libGLES_mali.so}"
        echo "[AOT Lifter] Lifting $SO_PATH in conformance-first mode (--no-opt-passes)..."
        ./src/aot/translate.sh "$SO_PATH"
        ;;
    --vulkaninfo)
        ICD_JSON="$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json"
        VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance | grep -E "Active PhysicalDevice|vulkaninfo pNext"
        ;;
    --run-vkcube-xcb)
        ICD_JSON="$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json"
        if [ -n "${DISPLAY:-}" ]; then
            WAYLAND_DISPLAY= LD_LIBRARY_PATH="$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07:." \
                VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/vkcube_khronos --wsi xcb --c "${1:-10}"
        else
            MALI_VKCUBE_HIRES=1 VK_ICD_FILENAMES="$ICD_JSON" \
                ./src/aot/bin/vkcube_khronos --wsi display --c "${1:-10}"
        fi
        ;;
    --run-40-samples)
        ICD_JSON="$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json"
        VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance
        ;;
    --decode-csf)
        python3 scripts/decode_csf_breadcrumbs_and_dump.py "$@"
        ;;
    *)
        echo "Unknown command: $CMD" >&2
        usage
        exit 1
        ;;
esac
