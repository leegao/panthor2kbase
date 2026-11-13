#!/usr/bin/env bash
# ============================================================================
# scripts/run_aot_lifter.sh
# Unified CLI Driver & Guidance Tool for Static AArch64-to-LLVM/C++ Binary
# Lifting, Bionic-to-glibc Stub Generation, XCB/Wayland/Display vkcube WSI,
# Generic x86-64 Vulkan Application Execution, vulkaninfo Introspection, DXVK
# 1.10.3 + bcn_layer + compat_layer Execution, and CSF Stall Forensic Analysis.
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
  --help                        Show this detailed command reference and examples.
  --gen-stubs                   Auto-generate Bionic-to-glibc PLT hooks (mali_stubs_*_hook)
                                and system property tables from host glibc headers and
                                libhybris-arlinux into src/aot/generated/mali_bionic_stubs_autogen.h.
  --inspect-props [so]          Extract and list all Android __system_property_get keys
                                referenced by an AArch64 driver binary (default: libGLES_mali.r44p1.so).
  --lift-so <path.so>           Lift an AArch64 Android ELF64 .so (libGLES_mali.so or
                                libvulkan_panfrost.so) in conformance-first mode (--no-opt-passes,
                                with --hook-Vulkan-calls).
  --vulkaninfo                  Run driver-accurate Vulkan physical-device, extension (170+ exts),
                                queue-family, memory-type, and pNext (DriverProperties) introspection
                                against the AOT-lifted libr54p3_vulkan_icd.so.
  --run-vkcube-xcb [frames]     Run Khronos vkcube against libr54p3_vulkan_icd.so using XCB/X11 WSI
                                (or headless display fallback when DISPLAY is unset).
  --run-vkcube-wayland [frames] Run Khronos vkcube against libr54p3_vulkan_icd.so using Wayland WSI
                                (VK_KHR_wayland_surface + wl_shm_pool buffer presentation).
  --run-app [--wayland|--x11] <cmd> [args...]
                                Run ANY generic x86-64 Vulkan application (e.g., vkgears, vulkaninfo,
                                vkcube, vkmark, SaschaWillems demos, or custom binaries) against the
                                AOT-lifted libr54p3_vulkan_icd.so ICD with full WSI & ICD logging.
                                Automatically detects `--wsi wayland` in application arguments and
                                preserves WAYLAND_DISPLAY and VK_KHR_wayland_surface.
  --run-panvk-app <cmd>         Run ANY generic x86-64 Vulkan application against the host-compiled
                                patched Mesa PanVK ICD (with libmali_kbase_emu.so + libpanthor_kbase_adapter.so).
  --capture-drm <out.pandrmcap> <cmd> [args...]  Capture Panthor DRM/CSF/ISA trace to .pandrmcap
  --replay-drm <in.pandrmcap> [opts...]          Replay .pandrmcap locally over adapter + emulator
  --capture-vk <out.panvkcap> <cmd> [args...]    Capture Vulkan API + SPIR-V + Memory trace via VK_LAYER_PANTHOR_capture
  --replay-vk <in.panvkcap> [opts...]            Replay .panvkcap locally on target Vulkan ICD
  --run-dxvk [args...]          Run DXVK 1.10.3 + bcn_layer + compat_layer on AOT-lifted libGLES_mali.so
                                (delegates to ./scripts/run_dxvk_aot.sh; supports --test / --self-test).
  --run-40-samples              Run the expanded 40 Khronos Vulkan-Samples + PanProbe compliance suite
                                against libr54p3_vulkan_icd.so via the system libvulkan.so.1 loader.
  --decode-csf <log.txt>        Decode CSF ring/stream instructions, breadcrumbs, and fault snapshots
                                from a captured Winlator log file.
  --analyze-stall <log>         Run the CSF Ringbuffer Wrap, Breadcrumb & Stall Forensic Analyzer
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
    local default_json="$ROOT_DIR/build-x86-64/r54p3_decompiled_icd.x86_64.json"
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

ensure_aot_icd_built() {
    local icd_json
    icd_json="$(resolve_active_icd_json)"
    local icd_dir
    icd_dir="$(dirname "$icd_json")"
    if [ ! -f "$icd_dir/libr54p3_vulkan_icd.so" ]; then
        echo "[AOT Runner] libr54p3_vulkan_icd.so not found; building via ./src/aot/translate.sh..." >&2
        "$ROOT_DIR/src/aot/translate.sh"
        CMAKE_BIN="$("$ROOT_DIR/scripts/find_cmake.sh")"
        "$CMAKE_BIN" --build "$ROOT_DIR/build-x86-64" --target r54p3_vulkan_icd -j"$(nproc)"
    fi
}

case "$CMD" in
    --gen-stubs)
        python3 scripts/gen_bionic_glibc_stubs.py
        ;;
    --inspect-props)
        SO_PATH="${1:-$ROOT_DIR/ddk/libGLES_mali.so}"
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
        SO_PATH="${1:-$ROOT_DIR/ddk/libGLES_mali.so}"
        [ ! -f "$SO_PATH" ] && [ -f "$ROOT_DIR/predownloaded/libGLES_mali.so" ] && SO_PATH="$ROOT_DIR/predownloaded/libGLES_mali.so"
        echo "[AOT Lifter] Lifting $SO_PATH in conformance-first mode (--no-opt-passes --hook-Vulkan-calls)..."
        ./src/aot/translate.sh "$SO_PATH"
        ;;
    --vulkaninfo)
        ensure_aot_icd_built
        ICD_JSON="$(resolve_active_icd_json)"
        VK_DRIVER_FILES="$ICD_JSON" VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance | grep -E "Active PhysicalDevice|vulkaninfo pNext"
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
        # Inspect application arguments for explicit `--wsi wayland` or `--wayland`
        # so `--run-app /usr/bin/vkcube --wsi wayland` automatically preserves
        # WAYLAND_DISPLAY and VK_KHR_wayland_surface!
        prev_arg=""
        for a in "$@"; do
            if [ "$a" = "--wayland" ] || [ "$a" = "--wsi=wayland" ]; then
                WSI_PREF="wayland"
            elif [ "$prev_arg" = "--wsi" ] && [ "$a" = "wayland" ]; then
                WSI_PREF="wayland"
            elif [ "$prev_arg" = "--wsi" ] && { [ "$a" = "xcb" ] || [ "$a" = "xlib" ] || [ "$a" = "x11" ]; }; then
                WSI_PREF="x11"
            fi
            prev_arg="$a"
        done

        ensure_aot_icd_built
        LAYER_DIR="$ROOT_DIR/src/aot/layers"
        if [ ! -f "$LAYER_DIR/libVkLayer_WSI_panthor_wsi.so" ]; then
            "$LAYER_DIR/build_layers.sh" >/dev/null 2>&1 || true
        fi
        cat << JSON_EOF > "$LAYER_DIR/VkLayer_WSI_panthor_wsi.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_WSI_PanthorWsiLayer",
        "type": "GLOBAL",
        "library_path": "$LAYER_DIR/libVkLayer_WSI_panthor_wsi.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "Panthor WSI Compatibility Layer (VK_KHR_surface + X11/XCB/Wayland/Win32/Headless)"
    }
}
JSON_EOF
        ICD_JSON="$(resolve_active_icd_json)"
        ICD_DIR="$(dirname "$ICD_JSON")"
        export MALI_AOT_ICD_LOG="${MALI_AOT_ICD_LOG:-1}"
        export NODEVICE_SELECT="${NODEVICE_SELECT:-1}"
        export VK_LOADER_LAYERS_DISABLE="${VK_LOADER_LAYERS_DISABLE:-~implicit~}"
        export VK_LAYER_PATH="$LAYER_DIR${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
        export VK_ADD_LAYER_PATH="$LAYER_DIR${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
        export VK_LAYER_PATH="$ROOT_DIR/third_party/prebuilts/bcn_layer/x86-64:$ROOT_DIR/third_party/prebuilts/compat_layer/x86-64:$ROOT_DIR/third_party/prebuilts/vulkan_wsi_layer/x86-64:$VK_LAYER_PATH"
        export VK_INSTANCE_LAYERS="VK_LAYER_window_system_integration"

        # export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_WSI_PanthorWsiLayer}"
        export LD_LIBRARY_PATH="$LAYER_DIR:$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}"
        export VK_DRIVER_FILES="$ICD_JSON"
        export VK_ICD_FILENAMES="$ICD_JSON"
        export VK_ADD_DRIVER_FILES=""
        # export VK_LOADER_DRIVERS_SELECT="*mali_aot*"
        export VK_LOADER_DRIVERS_DISABLE="*radeon*,*radv*,*lvp*,*intel*,*anv*,*nouveau*,*nvk*,*nvidia*"

        APP_BIN="$(command -v "$1" 2>/dev/null || echo "$1")"
        echo "================================================================================" >&2
        echo " [AOT Runner] Launching Generic x86-64 Vulkan Application" >&2
        echo "   Command          : $* ($APP_BIN)" >&2
        echo "   Active ICD JSON  : $ICD_JSON" >&2
        echo "   ICD Shared Lib   : $ICD_DIR/libr54p3_vulkan_icd.so" >&2
        echo "   Host DISPLAY     : ${DISPLAY:-(unset)}" >&2
        echo "   WAYLAND_DISPLAY  : ${WAYLAND_DISPLAY:-(unset)} (mode=$WSI_PREF)" >&2
        echo "   MALI_AOT_ICD_LOG : $MALI_AOT_ICD_LOG" >&2
        echo "================================================================================" >&2

        if [ "$WSI_PREF" = "wayland" ]; then
            exec env -u MALI_AOT_DISABLE_WAYLAND_EXT "$@"
        elif [ "$WSI_PREF" = "x11" ] || { [ "$WSI_PREF" = "auto" ] && [ -n "${DISPLAY:-}" ]; }; then
            # Unset WAYLAND_DISPLAY when using X11/XCB so apps like vkgears choose
            # xcb_wsi_interface(), while keeping VK_KHR_wayland_surface advertised
            # in vkEnumerateInstanceExtensionProperties unless explicitly disabled.
            exec env -u WAYLAND_DISPLAY "$@"
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
        LAYER_DIR="$ROOT_DIR/src/aot/layers"
        if [ ! -f "$LAYER_DIR/libVkLayer_WSI_panthor_wsi.so" ]; then
            "$LAYER_DIR/build_layers.sh" >/dev/null 2>&1 || true
        fi
        PANVK_ICD_JSON="${PANVK_HOST_ICD_JSON:-$ROOT_DIR/build-x86-64/panfrost_host_icd.x86_64.json}"
        if [ -f "$PANVK_ICD_JSON" ]; then
            export VK_DRIVER_FILES="${VK_DRIVER_FILES:-$PANVK_ICD_JSON}"
            export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$PANVK_ICD_JSON}"
        fi
        export VK_LAYER_PATH="$LAYER_DIR${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
        export VK_ADD_LAYER_PATH="$LAYER_DIR${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
        # export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_WSI_PanthorWsiLayer}"
        EMU_SO="$ROOT_DIR/build-x86-64/libmali_kbase_emu.so"
        [ ! -f "$EMU_SO" ] && EMU_SO="$ROOT_DIR/libmali_kbase_emu.so"
        ADAPT_SO="$ROOT_DIR/build-x86-64/libpanthor_kbase_adapter.so"
        [ ! -f "$ADAPT_SO" ] && ADAPT_SO="$ROOT_DIR/libpanthor_kbase_adapter.so"
        if [ "${1:-}" = "--x11" ] || [ -n "${DISPLAY:-}" ]; then
            [ "${1:-}" = "--x11" ] && shift
            PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                env -u WAYLAND_DISPLAY \
                LD_PRELOAD="$EMU_SO:$ADAPT_SO" \
                LD_LIBRARY_PATH="$LAYER_DIR:$ROOT_DIR/build-x86-64:$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                "$@"
        else
            PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                LD_PRELOAD="$EMU_SO:$ADAPT_SO" \
                LD_LIBRARY_PATH="$LAYER_DIR:$ROOT_DIR/build-x86-64:$ROOT_DIR:${LD_LIBRARY_PATH:-}" \
                "$@"
        fi
        ;;
    --run-40-samples)
        ensure_aot_icd_built
        ICD_JSON="$(resolve_active_icd_json)"
        VK_DRIVER_FILES="$ICD_JSON" VK_ICD_FILENAMES="$ICD_JSON" ./src/aot/bin/test_vulkan_samples_compliance
        ;;
    --decode-csf)
        python3 scripts/decode_csf_breadcrumbs_and_dump.py "$@"
        ;;
    --analyze-stall)
        python3 scripts/analyze_csf_stall_log.py "$@"
        ;;
    --capture-drm)
        CAP_FILE="${1:?missing .pandrmcap path}"
        shift
        PANTHOR_DRM_CAPTURE_PATH="$CAP_FILE" "$0" --run-panvk-app "$@"
        ;;
    --replay-drm)
        exec "$ROOT_DIR/panthor_drm_replayer" "$@"
        ;;
    --capture-vk)
        CAP_FILE="${1:?missing .panvkcap path}"
        shift
        VK_LAYER_PATH="$ROOT_DIR/src/aot/layers:${VK_LAYER_PATH:-}" \
            VK_INSTANCE_LAYERS="VK_LAYER_PANTHOR_capture:${VK_INSTANCE_LAYERS:-}" \
            PANTHOR_VK_CAPTURE_PATH="$CAP_FILE" \
            "$0" --run-app "$@"
        ;;
    --replay-vk)
        exec "$ROOT_DIR/panthor_vk_replayer" "$@"
        ;;
    --run-dxvk)
        exec "$ROOT_DIR/scripts/run_dxvk_aot.sh" "$@"
        ;;
    *)
        echo "Unknown command: $CMD" >&2
        usage
        exit 1
        ;;
esac
