#!/usr/bin/env bash
# ============================================================================
# scripts/run_dxvk_aot.sh
# End-to-End Runner for DXVK 1.10.3 + VK_LAYER_BCN_BCnLayer +
# VK_LAYER_COMPAT_DxvkMaliCompatLayer on AOT-Lifted libGLES_mali.so over
# libmali_kbase_emu.so (addressing doco.qQbva8ur3ZuSywz3TW_n8w and
# doco.9Y6t2YtDqXpP6E05C5IxTQ in Doc 31).
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [ ! -f "$ROOT_DIR/src/aot/layers/libVkLayer_BCN_bcn_layer.so" ] ||    [ ! -f "$ROOT_DIR/src/aot/layers/libVkLayer_COMPAT_dxvk_mali_compat.so" ]; then
    "$ROOT_DIR/src/aot/layers/build_layers.sh"
fi

ICD_JSON="${MALI_AOT_ICD_JSON:-$ROOT_DIR/src/aot/build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json}"
ICD_DIR="$(dirname "$ICD_JSON")"
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

if [ $# -eq 0 ] || [ "${1:-}" = "--self-test" ]; then
    echo "================================================================================"
    echo " [DXVK 1.10.3 on AOT-Lifted libGLES_mali.so] Running End-to-End Verification"
    echo "   ICD JSON          : $ICD_JSON"
    echo "   VK_LAYER_PATH     : $VK_LAYER_PATH"
    echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS"
    echo "================================================================================"
    exec "$ROOT_DIR/build/test_dxvk_aot_layers_e2e"
fi

if [[ "${1:-}" == *.exe ]]; then
    WINE_BIN="${WINE:-$(command -v wine64 || command -v wine || true)}"
    if [ -z "$WINE_BIN" ]; then
        echo "[ERROR] wine64/wine not found in PATH to launch $1" >&2
        exit 1
    fi
    if [ -n "${DISPLAY:-}" ]; then
        exec env -u WAYLAND_DISPLAY MALI_AOT_DISABLE_WAYLAND_EXT=1 "$WINE_BIN" "$@"
    else
        exec "$WINE_BIN" "$@"
    fi
else
    if [ -n "${DISPLAY:-}" ]; then
        exec env -u WAYLAND_DISPLAY MALI_AOT_DISABLE_WAYLAND_EXT=1 "$@"
    else
        exec "$@"
    fi
fi
