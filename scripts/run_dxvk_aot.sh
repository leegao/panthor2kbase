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
  the AOT-lifted Mali-G615 Vulkan ICD (libr54p3_vulkan_icd.so) and NEVER fall back
  to host RADV / ANV / NVIDIA / Lavapipe ICDs.
HELP
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi



LAYER_DIR="$ROOT_DIR/build-x86-64"
# if [ ! -f "$LAYER_DIR/libVkLayer_WSI_panthor_wsi.so" ]; then
#     "$LAYER_DIR/build_layers.sh"
# fi

# Always rewrite layer JSON manifests with current host $LAYER_DIR absolute paths
# so running from an archive unpacked at a different directory works out-of-the-box
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

# Auto-extract prebuilt DXVK 1.10.3 DLLs (d3d9, d3d10, d3d10_1, d3d10core, d3d11, dxgi)
# from third_party/dxvk-1.10.3/dxvk-1.10.3.tar.xz into active WINEPREFIX (system32 & syswow64)
DXVK_TARBALL="$ROOT_DIR/third_party/dxvk-1.10.3/dxvk-1.10.3.tar.xz"
DXVK_DIST_DIR="$ROOT_DIR/third_party/prebuilts/dxvk-1.10.3"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d11,dxgi,d3d10core,d3d10,d3d10_1,d3d9=n,b}"

install_dxvk_dlls_into_prefix() {
    local prefix="$1"
    local target_exe="${2:-}"
    if [ ! -f "$DXVK_DIST_DIR/x64/d3d11.dll" ] || [ ! -f "$DXVK_DIST_DIR/x32/d3d11.dll" ]; then
        if [ ! -f "$DXVK_TARBALL" ]; then
            return 0
        fi
        mkdir -p "$DXVK_DIST_DIR"
        tar -xJf "$DXVK_TARBALL" -C "$DXVK_DIST_DIR" --strip-components=1
    fi
    local sys32="$prefix/drive_c/windows/system32"
    local syswow64="$prefix/drive_c/windows/syswow64"
    mkdir -p "$sys32" "$syswow64"
    for dll in d3d9.dll d3d10.dll d3d10_1.dll d3d10core.dll d3d11.dll dxgi.dll; do
        [ -f "$DXVK_DIST_DIR/x64/$dll" ] && cp -f "$DXVK_DIST_DIR/x64/$dll" "$sys32/$dll"
        [ -f "$DXVK_DIST_DIR/x32/$dll" ] && cp -f "$DXVK_DIST_DIR/x32/$dll" "$syswow64/$dll"
    done
    if [ -n "$target_exe" ] && [ -f "$target_exe" ]; then
        local exe_dir
        exe_dir="$(cd "$(dirname "$target_exe")" && pwd)"
        local arch_sub="x64"
        if command -v file >/dev/null 2>&1; then
            if file "$target_exe" | grep -q "PE32 executable" && ! file "$target_exe" | grep -q "PE32+"; then
                arch_sub="x32"
            fi
        fi
        for dll in d3d9.dll d3d10.dll d3d10_1.dll d3d10core.dll d3d11.dll dxgi.dll; do
            [ -f "$DXVK_DIST_DIR/$arch_sub/$dll" ] && cp -f "$DXVK_DIST_DIR/$arch_sub/$dll" "$exe_dir/$dll" 2>/dev/null || true
        done
    fi
}
install_dxvk_dlls_into_prefix "$WINEPREFIX" ""

ICD_JSON="${MALI_AOT_ICD_JSON:-$ROOT_DIR/build-x86-64/r54p3_decompiled_icd.x86_64.json}"
ICD_DIR="$(cd "$(dirname "$ICD_JSON")" && pwd)"
ICD_JSON="$ICD_DIR/$(basename "$ICD_JSON")"
if [ ! -f "$ICD_DIR/libr54p3_vulkan_icd.so" ]; then
    SO_IN="$ROOT_DIR/ddk/libGLES_mali.so"
    [ ! -f "$SO_IN" ] && [ -f "$ROOT_DIR/predownloaded/libGLES_mali.so" ] && SO_IN="$ROOT_DIR/predownloaded/libGLES_mali.so"
    "$ROOT_DIR/src/aot/translate.sh" "$SO_IN"
    CMAKE_BIN="$("$ROOT_DIR/scripts/find_cmake.sh")"
    "$CMAKE_BIN" --build "$ROOT_DIR/build-x86-64" --target r54p3_vulkan_icd -j"$(nproc)"
fi

# Ensure r54p3_decompiled_icd.x86_64.json contains an absolute library_path pointing to this host's
# $ICD_DIR/libr54p3_vulkan_icd.so so Wine child processes (which chdir to C:\windows\system32
# or the .exe directory) never fail dlopen("./libr54p3_vulkan_icd.so") and fall back to RADV!
cat << JSON_EOF > "$ICD_JSON"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$ICD_DIR/libr54p3_vulkan_icd.so",
        "api_version": "1.3.250"
    }
}
JSON_EOF

export NODEVICE_SELECT=1
export VK_LOADER_LAYERS_DISABLE="~implicit~"
export VK_DRIVER_FILES="$ICD_JSON"
export VK_ICD_FILENAMES="$ICD_JSON"
export VK_ADD_DRIVER_FILES=""
# export VK_LOADER_DRIVERS_SELECT="*mali_aot*"
export VK_LOADER_DRIVERS_DISABLE="*radeon*,*radv*,*lvp*,*intel*,*anv*,*nouveau*,*nvk*,*nvidia*"
export VK_LAYER_PATH="$ROOT_DIR/third_party/prebuilts/bcn_layer/x86-64:$ROOT_DIR/third_party/prebuilts/compat_layer/x86-64:$ROOT_DIR/third_party/prebuilts/vulkan_wsi_layer/x86-64"
export VK_ADD_LAYER_PATH="$LAYER_DIR"
export VK_INSTANCE_LAYERS="VK_LAYER_window_system_integration:VK_LAYER_COMPAT_DxvkMaliCompatLayer:VK_LAYER_BCN_BCnLayer"
export LD_LIBRARY_PATH="$LAYER_DIR:$ICD_DIR:$ROOT_DIR:${LD_LIBRARY_PATH:-}"
export DXVK_HUD="${DXVK_HUD:-devinfo,fps,submissions}"
export MALI_AOT_ICD_LOG="${MALI_AOT_ICD_LOG:-1}"

if [ $# -eq 0 ] || [ "${1:-}" = "--self-test" ] || [ "${1:-}" = "--test" ] || [ "${1:-}" = "-t" ]; then
    TEST_BIN="$ROOT_DIR/build-x86-64/tests/test_dxvk_aot_layers_e2e"
    [ ! -x "$TEST_BIN" ] && [ -x "$ROOT_DIR/build/test_dxvk_aot_layers_e2e" ] && TEST_BIN="$ROOT_DIR/build/test_dxvk_aot_layers_e2e"
    if [ ! -x "$TEST_BIN" ] || [ "$ROOT_DIR/tests/test_dxvk_aot_layers_e2e.cpp" -nt "$TEST_BIN" ]; then
        echo "[Build] Compiling test_dxvk_aot_layers_e2e via CMake..."
        CMAKE_BIN="$("${ROOT_DIR}/scripts/find_cmake.sh")"
        BUILD_TARGET="$ROOT_DIR/build-x86-64"
        "$CMAKE_BIN" -S "$ROOT_DIR" -B "$BUILD_TARGET" -DCMAKE_BUILD_TYPE=Release
        "$CMAKE_BIN" --build "$BUILD_TARGET" --target test_dxvk_aot_layers_e2e
        TEST_BIN="$BUILD_TARGET/tests/test_dxvk_aot_layers_e2e"
        [ ! -x "$TEST_BIN" ] && TEST_BIN="$BUILD_TARGET/test_dxvk_aot_layers_e2e"
    fi
    echo "================================================================================"
    echo " [DXVK 1.10.3 on AOT-Lifted libGLES_mali.so] Running End-to-End Verification"
    echo "   ICD JSON          : $ICD_JSON"
    echo "   ICD Shared Lib    : $ICD_DIR/libr54p3_vulkan_icd.so"
    echo "   VK_LAYER_PATH     : $VK_LAYER_PATH"
    echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS"
    echo "================================================================================"
    exec "$TEST_BIN"
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
echo "   ICD Shared Lib    : $ICD_DIR/libr54p3_vulkan_icd.so" >&2
echo "   VK_LAYER_PATH     : $VK_LAYER_PATH" >&2
echo "   VK_INSTANCE_LAYERS: $VK_INSTANCE_LAYERS" >&2
echo "================================================================================" >&2

if [[ "${1:-}" == *.exe ]] || [ -n "$CUSTOM_WINE" ]; then
    install_dxvk_dlls_into_prefix "$WINEPREFIX" "${1:-}"
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
