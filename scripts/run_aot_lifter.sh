#!/usr/bin/env bash
# ============================================================================
# scripts/run_aot_lifter.sh
# Unified CLI Driver & Guidance Tool for Static AArch64-to-LLVM/C++ Binary
# Lifting, Bionic-to-glibc Stub Generation, XCB/Display vkcube WSI, Generic
# x86-64 Vulkan Application Execution, vulkaninfo Introspection, and CSF Stall
# Forensic Analysis.
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
                           (env -u WAYLAND_DISPLAY VK_ICD_FILENAMES=.../mali_aot_icd.x86_64.json vkcube --wsi xcb)
                           or headless display fallback when DISPLAY is unset.
  --run-app [--wayland|--x11] <cmd> [args...]
                           Run ANY generic x86-64 Vulkan application (e.g., vkgears, vulkaninfo,
                           vkmark, SaschaWillems demos, or custom binaries) against the AOT-lifted
                           libvulkan_mali_aot_icd.so ICD with full WSI & ICD diagnostic logging.
                           Note: Uses `env -u WAYLAND_DISPLAY` when X11 `DISPLAY` is available so
                           apps like `vkgears` do not see an empty `WAYLAND_DISPLAY=""` string and
                           fail in `wl_display_connect()`. Pass `--wayland` to keep `WAYLAND_DISPLAY`.
  --run-panvk-app <cmd>    Run ANY generic x86-64 Vulkan application against the host-compiled
                           patched Mesa PanVK ICD (with libmali_kbase_emu.so + libpanthor_kbase_adapter.so).
  --run-40-samples         Run the expanded 40 Khronos Vulkan-Samples + PanProbe compliance suite
                           against libvulkan_mali_aot_icd.so via the system libvulkan.so.1 loader.
  --decode-csf <log.txt>   Decode CSF ring/stream instructions, breadcrumbs, and fault snapshots
                           from a captured Winlator log file.
  --analyze-stall <log>    Run the CSF Ringbuffer Wrap, Breadcrumb & Stall Forensic Analyzer
                           (scripts/analyze_csf_stall_log.py) on a captured Winlator log.
================================================================================
HELP
}

if [ $# -eq 0 ] || [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

CMD="$1"
shift

resolve_active_icd_json() {
    local default_json="$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json"
    if [ -n "${MALI_AOT_ICD_JSON:-}" ] && [ -f "${MALI_AOT_ICD_JSON}" ]; then
        echo "${MALI_AOT_ICD_JSON}"
        return 0
    fi
    if [ -f "$default_json" ]; then
        echo "$default_json"
        return 0
    fi
    local latest
    latest="$(ls -1t "$ROOT_DIR"/src/aot/build/*/mali_aot_icd.x86_64.json 2>/dev/null | head -n 1 || true)"
    if [ -n "$latest" ]; then
        echo "$latest"
        return 0
    fi
    echo "$default_json"
}

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
        ICD_JSON="$(resolve_active_icd_json)"
        VK_DRIVER_FILES="$ICD_JSON" VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance | grep -E "Active PhysicalDevice|vulkaninfo pNext"
        ;;
    --run-vkcube-xcb)
        ICD_JSON="$(resolve_active_icd_json)"
        ICD_DIR="$(dirname "$ICD_JSON")"
        export MALI_AOT_ICD_LOG="${MALI_AOT_ICD_LOG:-1}"
        if [ -n "${DISPLAY:-}" ]; then
            env -u WAYLAND_DISPLAY MALI_AOT_DISABLE_WAYLAND_EXT=1 \
                LD_LIBRARY_PATH="$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                VK_DRIVER_FILES="$ICD_JSON" \
                VK_ICD_FILENAMES="$ICD_JSON" \
                ./src/aot/bin/vkcube_khronos --wsi xcb --c "${1:-10}"
        else
            MALI_VKCUBE_HIRES=1 \
                LD_LIBRARY_PATH="$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                VK_DRIVER_FILES="$ICD_JSON" \
                VK_ICD_FILENAMES="$ICD_JSON" \
                ./src/aot/bin/vkcube_khronos --wsi display --c "${1:-10}"
        fi
        ;;
    --run-app)
        WSI_PREF="auto"
        if [ "${1:-}" = "--wayland" ]; then
            WSI_PREF="wayland"
            shift
        elif [ "${1:-}" = "--x11" ] || [ "${1:-}" = "--xcb" ]; then
            WSI_PREF="x11"
            shift
        fi
        if [ $# -eq 0 ]; then
            echo "[ERROR] --run-app requires a command (e.g. ./scripts/run_aot_lifter.sh --run-app vkgears)" >&2
            exit 1
        fi
        ICD_JSON="$(resolve_active_icd_json)"
        ICD_DIR="$(dirname "$ICD_JSON")"
        export MALI_AOT_ICD_LOG="${MALI_AOT_ICD_LOG:-1}"
        export LD_LIBRARY_PATH="$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}"
        export VK_DRIVER_FILES="$ICD_JSON"
        export VK_ICD_FILENAMES="$ICD_JSON"

        APP_BIN="$(command -v "$1" 2>/dev/null || echo "$1")"
        echo "================================================================================" >&2
        echo " [AOT Runner] Launching Generic x86-64 Vulkan Application" >&2
        echo "   Command          : $* ($APP_BIN)" >&2
        echo "   Active ICD JSON  : $ICD_JSON" >&2
        echo "   ICD Shared Lib   : $ICD_DIR/libvulkan_mali_aot_icd.so" >&2
        echo "   Host DISPLAY     : ${DISPLAY:-(unset)}" >&2
        echo "   WAYLAND_DISPLAY  : ${WAYLAND_DISPLAY:-(unset)} (mode=$WSI_PREF)" >&2
        echo "   MALI_AOT_ICD_LOG : $MALI_AOT_ICD_LOG" >&2
        echo "================================================================================" >&2

        # CRITICAL FIX: Never pass `WAYLAND_DISPLAY=` (which sets WAYLAND_DISPLAY=""
        # as a non-NULL empty string in environ, causing apps like `vkgears` that test
        # `if (getenv("WAYLAND_DISPLAY"))` to pick Wayland and abort with
        # `failed to connect to display` when `wl_display_connect(NULL)` fails on "").
        # Instead, use `env -u WAYLAND_DISPLAY` when X11 DISPLAY is available (so
        # `getenv("WAYLAND_DISPLAY") == NULL` and apps connect via XCB/XWayland), or
        # preserve a non-empty WAYLAND_DISPLAY when DISPLAY is unset or --wayland is used.
        if [ "$WSI_PREF" = "x11" ] || { [ "$WSI_PREF" = "auto" ] && [ -n "${DISPLAY:-}" ]; }; then
            exec env -u WAYLAND_DISPLAY MALI_AOT_DISABLE_WAYLAND_EXT=1 "$@"
        elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
            exec "$@"
        elif command -v xvfb-run >/dev/null 2>&1; then
            echo "[AOT Runner] Neither DISPLAY nor WAYLAND_DISPLAY is set; launching under xvfb-run..." >&2
            exec env -u WAYLAND_DISPLAY xvfb-run -a "$@"
        else
            echo "[AOT Runner] WARN: Neither DISPLAY nor WAYLAND_DISPLAY is set in environment." >&2
            exec env -u WAYLAND_DISPLAY "$@"
        fi
        ;;
    --run-panvk-app)
        if [ "${1:-}" = "--x11" ] || [ -n "${DISPLAY:-}" ]; then
            [ "${1:-}" = "--x11" ] && shift
            PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                env -u WAYLAND_DISPLAY \
                LD_PRELOAD="$ROOT_DIR/libmali_kbase_emu.so:$ROOT_DIR/libpanthor_kbase_adapter.so" \
                LD_LIBRARY_PATH="$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                "$@"
        else
            PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                LD_PRELOAD="$ROOT_DIR/libmali_kbase_emu.so:$ROOT_DIR/libpanthor_kbase_adapter.so" \
                LD_LIBRARY_PATH="$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                "$@"
        fi
        ;;
    --run-40-samples)
        ICD_JSON="$(resolve_active_icd_json)"
        VK_DRIVER_FILES="$ICD_JSON" VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance
        ;;
    --decode-csf)
        python3 scripts/decode_csf_breadcrumbs_and_dump.py "$@"
        ;;
    --analyze-stall)
        python3 scripts/analyze_csf_stall_log.py "$@"
        ;;
    *)
        echo "Unknown command: $CMD" >&2
        usage
        exit 1
        ;;
esac
