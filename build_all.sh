#!/usr/bin/env bash
# ============================================================================
# build_all.sh
# Master Build Script for the Combined Panthor-to-Kbase Adapter, User-Space
# /dev/mali0 Emulator, Generic UAPI Tracer, AOT Translator & Decompiled r54p3
# ICD, and Android NDK Build.
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

# Auto-detect CMake via centralized locator
CMAKE_BIN="$("${ROOT_DIR}/scripts/find_cmake.sh")"
export PATH="$(dirname "$CMAKE_BIN"):$PATH"

echo "================================================================================"
echo " [1/3] Building Host x86-64 Panthor-to-Kbase Adapter, /dev/mali0 Emulator & AOT "
echo "================================================================================"
echo "  -> Using CMake: $CMAKE_BIN"
"$CMAKE_BIN" -S "$ROOT_DIR" -B "$ROOT_DIR/build-x86-64" -DCMAKE_BUILD_TYPE=Release
"$CMAKE_BIN" --build "$ROOT_DIR/build-x86-64" -j"$(nproc 2>/dev/null || echo 4)"

echo "================================================================================"
echo " [2/3] Building Android Panthor-to-Kbase Adapter, /dev/mali0 Emulator & Tracer  "
echo "================================================================================"
NDK_TOOLCHAIN=""
for cand in \
    "${ANDROID_NDK_HOME:-}/build/cmake/android.toolchain.cmake" \
    "$HOME/Android/Sdk/ndk/27.0.12077973/build/cmake/android.toolchain.cmake" \
    "$ROOT_DIR/../ndk/android-ndk-r27d/build/cmake/android.toolchain.cmake"; do
    if [ -f "$cand" ]; then
        NDK_TOOLCHAIN="$cand"
        break
    fi
done

if [ -n "$NDK_TOOLCHAIN" ]; then
    echo "  -> Using Android NDK Toolchain: $NDK_TOOLCHAIN"
    "$CMAKE_BIN" -S "$ROOT_DIR" -B "$ROOT_DIR/build-android" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-30 \
        -DPANTHOR_KBASE_BUILD_TESTS=OFF \
        -DPANTHOR_KBASE_BUILD_AOT=OFF
    "$CMAKE_BIN" --build "$ROOT_DIR/build-android" -j"$(nproc 2>/dev/null || echo 4)"
    echo "  -> Built Android aarch64 native libraries in build-android/"
else
    echo "  -> Android NDK toolchain not found, skipping Android CMake build."
fi

if [ -d mesa ] && ([ -f build-android/mesa/build.ninja ] || [ -f build-android/build.ninja ]); then
    echo "================================================================================"
    echo " [3/3] Running ./compile.sh for Android aarch64 NDK Mesa PanVK Build           "
    echo "================================================================================"
    ./compile.sh
fi

echo "================================================================================"
echo " [SUCCESS] All Host, CMake, AOT, Tracer, and Test Binaries Built Cleanly!      "
echo "================================================================================"
