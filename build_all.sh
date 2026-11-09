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

# Auto-detect CMake (system PATH or bundled Android SDK cmake)
CMAKE_BIN=""
if command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN="$(command -v cmake)"
else
    for cand in \
        "$ROOT_DIR/../work/cmake_root/3.22.1/bin/cmake" \
        "/working_dir/c_3106ac7496559798/work/cmake_root/3.22.1/bin/cmake" \
        "$HOME/Android/Sdk/cmake/"*/bin/cmake; do
        if [ -x "$cand" ]; then
            CMAKE_BIN="$cand"
            export PATH="$(dirname "$cand"):$PATH"
            break
        fi
    done
fi

echo "================================================================================"
echo " [1/3] Building Host x86-64 Panthor-to-Kbase Adapter, /dev/mali0 Emulator & AOT "
echo "================================================================================"
if [ -n "$CMAKE_BIN" ]; then
    echo "  -> Using CMake: $CMAKE_BIN"
    "$CMAKE_BIN" -S "$ROOT_DIR" -B "$ROOT_DIR/build-cmake-host" -DCMAKE_BUILD_TYPE=Release
    "$CMAKE_BIN" --build "$ROOT_DIR/build-cmake-host" -j"$(nproc 2>/dev/null || echo 4)"
fi
make all

echo "================================================================================"
echo " [2/3] Building Generic LD_PRELOAD /dev/mali0 UAPI Tracer (Host & Android NDK) "
echo "================================================================================"
gcc -Wall -Wextra -Werror -O2 -fPIC -shared -Iinclude \
    src/tools/mali_uapi_tracer_preload.c -ldl -lpthread \
    -o libmali_uapi_tracer.so
echo "  -> Built host x86-64: ./libmali_uapi_tracer.so"

NDK_CLANG=""
for cand in \
    "${ANDROID_NDK_HOME:-}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" \
    "$HOME/Android/Sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" \
    "/working_dir/c_3106ac7496559798/work/ndk/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"; do
    if [ -x "$cand" ]; then
        NDK_CLANG="$cand"
        break
    fi
done

if [ -n "$NDK_CLANG" ]; then
    mkdir -p build-android-tools
    "$NDK_CLANG" -Wall -Wextra -Werror -O2 -fPIC -shared -Iinclude \
        src/tools/mali_uapi_tracer_preload.c -ldl \
        -o build-android-tools/libmali_uapi_tracer_android_aarch64.so
    echo "  -> Built Android aarch64: ./build-android-tools/libmali_uapi_tracer_android_aarch64.so"
fi

if [ -d mesa ] && [ -d build-android ]; then
    echo "================================================================================"
    echo " [3/3] Running ./compile.sh for Android aarch64 NDK Mesa PanVK Build           "
    echo "================================================================================"
    ./compile.sh
fi

echo "================================================================================"
echo " [SUCCESS] All Host, CMake, AOT, Tracer, and Test Binaries Built Cleanly!      "
echo "================================================================================"
