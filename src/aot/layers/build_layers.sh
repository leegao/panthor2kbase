#!/usr/bin/env bash
# src/aot/layers/build_layers.sh
# Builds DXVK, WSI, and Panthor Vulkan capture layers via centralized CMake
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$DIR/../../.." && pwd)"

CMAKE_BIN="$("${ROOT_DIR}/scripts/find_cmake.sh")"
BUILD_TARGET="${BUILD_DIR:-$ROOT_DIR/build-x86-64}"
"$CMAKE_BIN" -S "$ROOT_DIR" -B "$BUILD_TARGET" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_TARGET" --target VkLayer_WSI_panthor_wsi VkLayer_PANTHOR_capture

cat << JSON_EOF > "$DIR/VkLayer_BCN_bcn_layer.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_BCN_BCnLayer",
        "type": "GLOBAL",
        "library_path": "$DIR/libVkLayer_BCN_bcn_layer.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "BCn Emulation Layer for Mobile GPUs (Host AOT Build)"
    }
}
JSON_EOF

cat << JSON_EOF > "$DIR/VkLayer_COMPAT_dxvk_mali_compat.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_COMPAT_DxvkMaliCompatLayer",
        "type": "GLOBAL",
        "library_path": "$DIR/libVkLayer_COMPAT_dxvk_mali_compat.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "DXVK 1.10.3 Feature & Limit Compatibility Layer for Mali r54p3 AOT ICD"
    }
}
JSON_EOF

cat << JSON_EOF > "$DIR/VkLayer_WSI_panthor_wsi.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_WSI_PanthorWsiLayer",
        "type": "GLOBAL",
        "library_path": "$DIR/libVkLayer_WSI_panthor_wsi.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "Panthor WSI Compatibility Layer (VK_KHR_surface + X11/XCB/Wayland/Win32/Headless)"
    }
}
JSON_EOF
