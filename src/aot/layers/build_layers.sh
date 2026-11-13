#!/usr/bin/env bash
# src/aot/layers/build_layers.sh
# Builds the Panthor Vulkan capture layer via centralized CMake
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$DIR/../../.." && pwd)"

CMAKE_BIN="$("${ROOT_DIR}/scripts/find_cmake.sh")"
BUILD_TARGET="${BUILD_DIR:-$ROOT_DIR/build-x86-64}"
"$CMAKE_BIN" -S "$ROOT_DIR" -B "$BUILD_TARGET" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$BUILD_TARGET" --target VkLayer_PANTHOR_capture
