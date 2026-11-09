#!/usr/bin/env bash
# ============================================================================
# scripts/run_dxvk_aot.sh
# End-to-End Runner for DXVK 1.10.3 + VK_LAYER_BCN_BCnLayer +
# VK_LAYER_COMPAT_DxvkMaliCompatLayer on AOT-Lifted libGLES_mali.so over
# libmali_kbase_emu.so (addressing doco.qQbva8ur3ZuSywz3TW_n8w,
# doco.9Y6t2YtDqXpP6E05C5IxTQ, doco.NQzTOnW2AxIUKYv6lrlodA, and
# doco.m9f7z4wAxIUKYp6v9w2aa in Doc 31).
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
    cat << 'HELP'
Usage:
  ./scripts/run_dxvk_aot.sh [--self-test | --test | -t]
  ./scripts/run_dxvk_aot.sh [--wine <path/to/wine>] [--run-exe | --exe] <path/to/app.exe> [args...]
  ./scripts/run_dxvk_aot.sh <command> [args...]

Environment / Routing Guarantees:
  Forces Khronos libvulkan.so.1, Wine (winevulkan.so), and DXVK 1.10.3 to select ONLY
  the AOT-lifted Mali-G615 Vulkan ICD (libvulkan_mali_aot_icd.so) and NEVER fall back
  to host RADV / ANV / NVIDIA / Lavapipe ICDs.
HELP
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

if [ ! -f "$ROOT_DIR/libpanthor_kbase_adapter.so" ] || [ ! -f "$ROOT_DIR/libmali_kbase_emu.so" ]; then
    make libpanthor_kbase_adapter.so libmali_kbase_emu.so
fi

LAYER_DIR="$ROOT_DIR/src/aot/layers"
if [ ! -f "$LAYER_DIR/libVkLayer_BCN_bcn_layer.so" ] || \
   [ ! -f "$LAYER_DIR/libVkLayer_COMPAT_dxvk_mali_compat.so" ]; then
    "$LAYER_DIR/build_layers.sh"
else
    # Always rewrite layer JSON manifests with current host $LAYER_DIR absolute paths
    # so running from an archive unpacked at a different directory works out-of-the-box
    cat << JSON_EOF > "$LAYER_DIR/VkLayer_BCN_bcn_layer.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_BCN_BCnLayer1",
        "type": "GLOBAL",
        "library_path": "$LAYER_DIR/libVkLayer_BCN_bcn_layer.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "BCn Emulation Layer for Mobile GPUs (Host AOT Build)"
    }
}
JSON_EOF
    cat << JSON_EOF > "$LAYER_DIR/VkLayer_COMPAT_dxvk_mali_compat.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_COMPAT_DxvkMaliCompatLayer1",
        "type": "GLOBAL",
        "library_path": "$LAYER_DIR/libVkLayer_COMPAT_dxvk_mali_compat.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "DXVK 1.10.3 Feature & Limit Compatibility Layer for Mali r54p3 AOT ICD"
    }
}
JSON_EOF
fi

ICD_JSON="${MALI_AOT_ICD_JSON:-$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json}"
ICD_DIR="$(cd "$(dirname "$ICD_JSON")" && pwd)"
ICD_JSON="$ICD_DIR/$(basename "$ICD_JSON")"
if [ ! -f "$ICD_DIR/libvulkan_mali_aot_icd.so" ]; then
    "$ROOT_DIR/src/aot/translate.sh" "$ROOT_DIR/libGLES_mali.so"
fi

# Ensure mali_aot_icd.x86_64.json contains an absolute library_path pointing to this host's
# $ICD_DIR/libvulkan_mali_aot_icd.so so Wine child processes (which chdir to C:\windows\system32
# or the .exe directory) never fail dlopen("./libvulkan_mali_aot_icd.so") and fall back to RADV!
cat << JSON_EOF > "$ICD_JSON"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$ICD_DIR/libvulkan_mali_aot_icd.so",
        "api_version": "1.3.250"
    }
}
JSON_EOF

export NODEVICE_SELECT=1
export VK_LOADER_LAYERS_DISABLE="~implicit~"
export VK_DRIVER_FILES="$ICD_JSON"
export VK_ICD_FILENAMES="$ICD_JSON"
export VK_ADD_DRIVER_FILES=""
export VK_LOADER_DRIVERS_SELECT="*mali_aot*"
export VK_LOADER_DRIVERS_DISABLE="*radeon*,*radv*,*lvp*,*intel*,*anv*,*nouveau*,*nvk*,*nvidia*"
export VK_LAYER_PATH="$LAYER_DIR"
export VK_INSTANCE_LAYERS="VK_LAYER_COMPAT_DxvkMaliCompatLayer:VK_LAYER_BCN_BCnLayer"
export LD_LIBRARY_PATH="$LAYER_DIR:$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}"
export DXVK_FILTER_DEVICE_NAME="${DXVK_FILTER_DEVICE_NAME:-Mali}"
export DXVK_HUD="${DXVK_HUD:-devinfo,fps,submissions}"
export MALI_AOT_ICD_LOG="${MALI_AOT_ICD_LOG:-1}"

if [ $# -eq 0 ] || [ "${1:-}" = "--self-test" ] || [ "${1:-}" = "--test" ] || [ "${1:-}" = "-t" ]; then
    if [ ! -x "$ROOT_DIR/build/test_dxvk_aot_layers_e2e" ] || \
       [ "$ROOT_DIR/tests/test_dxvk_aot_layers_e2e.cpp" -nt "$ROOT_DIR/build/test_dxvk_aot_layers_e2e" ]; then
        echo "[Build] Compiling $ROOT_DIR/build/test_dxvk_aot_layers_e2e..."
        mkdir -p "$ROOT_DIR/build"
        g++ -std=c++17 -O2 -Iinclude -Iinclude/uapi -Isrc/aot/include \
            "$ROOT_DIR/tests/test_dxvk_aot_layers_e2e.cpp" \
            -L"$ROOT_DIR" -lpanthor_kbase_adapter -lmali_kbase_emu -ldl -lpthread \
            -Wl,-rpath,'$ORIGIN/..' -o "$ROOT_DIR/build/test_dxvk_aot_layers_e2e"
    fi
    echo "================================================================================"
    echo " [DXVK 1.10.3 on AOT-Lifted libGLES_mali.so] Running End-to-End Verification"
    echo "   ICD JSON          : $ICD_JSON"
    echo "   ICD Shared Lib    : $ICD_DIR/libvulkan_mali_aot_icd.so"
    echo "   VK_LAYER_PATH     : $VK_LAYER_PATH"
    echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS"
    echo "   DXVK_FILTER_DEVICE: $DXVK_FILTER_DEVICE_NAME"
    echo "================================================================================"
    exec "$ROOT_DIR/build/test_dxvk_aot_layers_e2e"
fi

CUSTOM_WINE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --wine)
            CUSTOM_WINE="${2:-}"
            shift 2
            ;;
        --wine=*)
            CUSTOM_WINE="${1#--wine=}"
            shift
            ;;
        --run-exe|--exe)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

find_wine_bin() {
    if [ -n "$CUSTOM_WINE" ] && [ -x "$CUSTOM_WINE" ]; then
        echo "$CUSTOM_WINE"
        return 0
    fi
    if [ -n "${WINE:-}" ] && [ -x "${WINE}" ]; then
        echo "${WINE}"
        return 0
    fi
    for cand in \
        "$ROOT_DIR/wine-11.21-amd64-wow64/bin/wine" \
        "$ROOT_DIR/../wine-11.21-amd64-wow64/bin/wine" \
        "$(command -v wine64 2>/dev/null || true)" \
        "$(command -v wine 2>/dev/null || true)"; do
        if [ -n "$cand" ] && [ -x "$cand" ]; then
            echo "$cand"
            return 0
        fi
    done
    return 1
}

echo "================================================================================" >&2
echo " [DXVK AOT Runner] Launching Target on AOT-Lifted Mali-G615 ICD" >&2
echo "   Target Command    : $*" >&2
echo "   Active ICD JSON   : $ICD_JSON" >&2
echo "   ICD Shared Lib    : $ICD_DIR/libvulkan_mali_aot_icd.so" >&2
echo "   VK_LAYER_PATH     : $VK_LAYER_PATH" >&2
echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS" >&2
echo "   DXVK_FILTER_DEVICE: $DXVK_FILTER_DEVICE_NAME" >&2
echo "================================================================================" >&2

if [[ "${1:-}" == *.exe ]] || [ -n "$CUSTOM_WINE" ]; then
    WINE_BIN="$(find_wine_bin || true)"
    if [ -z "$WINE_BIN" ]; then
        echo "[ERROR] wine64/wine not found in PATH or ./wine-11.21-amd64-wow64/bin/wine to launch $1" >&2
        exit 1
    fi
    if [ -n "${DISPLAY:-}" ]; then
        exec env -u WAYLAND_DISPLAY "$WINE_BIN" "$@"
    else
        exec "$WINE_BIN" "$@"
    fi
else
    if [ -n "${DISPLAY:-}" ]; then
        exec env -u WAYLAND_DISPLAY "$@"
    else
        exec "$@"
    fi
fi
