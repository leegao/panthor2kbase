#!/usr/bin/env bash
# src/aot/r54p3_vulkan_icd/build.sh
# Builds decompiled r54p3 Vulkan ICD via centralized CMake
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

CMAKE_BIN="$("${REPO_ROOT}/scripts/find_cmake.sh")"
BUILD_TARGET="${BUILD_DIR:-$REPO_ROOT/build-x86-64}"
"$CMAKE_BIN" -S "$REPO_ROOT" -B "$BUILD_TARGET" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_TARGET" --target r54p3_vulkan_icd

cat << JSON_EOF > "$SCRIPT_DIR/lib/r54p3_decompiled_icd.x86_64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$SCRIPT_DIR/lib/libr54p3_vulkan_icd.so",
        "api_version": "1.3.250"
    }
}
JSON_EOF
echo "[SUCCESS] Built $SCRIPT_DIR/lib/libr54p3_vulkan_icd.so and r54p3_decompiled_icd.x86_64.json via CMake"
