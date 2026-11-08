#!/usr/bin/env bash
# ============================================================================
# scripts/run_dxvk_aot.sh
# End-to-End Runner for DXVK 1.10.3 + VK_LAYER_BCN_BCnLayer +
# VK_LAYER_COMPAT_DxvkMaliCompatLayer on AOT-Lifted libGLES_mali.so over
# libmali_kbase_emu.so (addressing doco.qQbva8ur3ZuSywz3TW_n8w,
# doco.9Y6t2YtDqXpP6E05C5IxTQ, and doco.NQzTOnW2AxIUKYv6lrlodA in Doc 31).
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [ ! -f "$ROOT_DIR/libpanthor_kbase_adapter.so" ] || [ ! -f "$ROOT_DIR/libmali_kbase_emu.so" ]; then
    make libpanthor_kbase_adapter.so libmali_kbase_emu.so
fi

if [ ! -f "$ROOT_DIR/src/aot/layers/libVkLayer_BCN_bcn_layer.so" ] || \
   [ ! -f "$ROOT_DIR/src/aot/layers/libVkLayer_COMPAT_dxvk_mali_compat.so" ]; then
    "$ROOT_DIR/src/aot/layers/build_layers.sh"
fi

ICD_JSON="${MALI_AOT_ICD_JSON:-$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json}"
ICD_DIR="$(dirname "$ICD_JSON")"
if [ ! -f "$ICD_DIR/libvulkan_mali_aot_icd.so" ]; then
    "$ROOT_DIR/src/aot/translate.sh"
fi

LAYER_DIR="$ROOT_DIR/src/aot/layers"

export NODEVICE_SELECT=1
export VK_LOADER_LAYERS_DISABLE="~implicit~"
export VK_DRIVER_FILES="$ICD_JSON"
export VK_ICD_FILENAMES="$ICD_JSON"
export VK_LAYER_PATH="$LAYER_DIR"
export VK_INSTANCE_LAYERS="VK_LAYER_COMPAT_DxvkMaliCompatLayer:VK_LAYER_BCN_BCnLayer"
export LD_LIBRARY_PATH="$LAYER_DIR:$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}"
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
    echo "   VK_LAYER_PATH     : $VK_LAYER_PATH"
    echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS"
    echo "================================================================================"
    exec "$ROOT_DIR/build/test_dxvk_aot_layers_e2e"
fi

find_wine_bin() {
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

if [[ "${1:-}" == *.exe ]]; then
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
