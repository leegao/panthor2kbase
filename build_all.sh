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

echo "================================================================================"
echo " [1/4] Building Host x86-64 Panthor-to-Kbase Adapter, /dev/mali0 Emulator & Tests"
echo "================================================================================"
make all

echo "================================================================================"
echo " [2/4] Building Generic LD_PRELOAD /dev/mali0 UAPI Tracer (Host & Android NDK) "
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

echo "================================================================================"
echo " [3/4] Building Ludashi panvk-kbase-android & Host Patched PanVK Test Harness  "
echo "================================================================================"
g++ -std=c++17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc/adapter -Isrc/emulator \
    tests/test_ludashi_panvk_aot_and_host.cpp \
    -L. -lpanthor_kbase_adapter -lmali_kbase_emu \
    -Wl,-rpath,'$ORIGIN/..' -Wl,-rpath,'$ORIGIN' -ldl -lpthread \
    -o tests/test_ludashi_panvk_aot_and_host
echo "  -> Built ./tests/test_ludashi_panvk_aot_and_host"
g++ -std=c++17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc/adapter -Isrc/emulator \
    tests/panprobe/panprobe_runner_suite.cpp \
    -L. -lpanthor_kbase_adapter -lmali_kbase_emu \
    -Wl,-rpath,'$ORIGIN/../..' -Wl,-rpath,'$ORIGIN/..' -Wl,-rpath,'$ORIGIN' -ldl -lpthread \
    -o tests/panprobe/panprobe_runner_suite
echo "  -> Built ./tests/panprobe/panprobe_runner_suite"

echo "================================================================================"
echo " [4/4] Building AOT Translator, Decompiled r54p3 ICD, vkcube & 20-Sample Suite "
echo "================================================================================"
g++ -std=c++17 -O2 src/aot/tests/vkcube_wsi_e2e.cpp -o src/aot/bin/vkcube -ldl -lpthread
g++ -std=c++20 -O2 -Isrc/aot/include src/aot/tests/test_vulkan_samples_compliance.cpp \
    -o src/aot/bin/test_vulkan_samples_compliance -ldl -lpthread
g++ -std=c++17 -O2 src/aot/tests/test_vulkan_icd_client.cpp \
    -o src/aot/bin/test_vulkan_icd_client -ldl -lpthread

if [ -f src/aot/build/r54p3-00eac0-6eda07/mali_mbs2_aot.o ]; then
    g++ -O3 -std=c++17 -fno-strict-aliasing -shared -fPIC -Isrc/aot/include \
        src/aot/src/mali_vulkan_icd.cpp src/aot/build/r54p3-00eac0-6eda07/mali_mbs2_aot.o \
        -L. -lmali_kbase_emu -Wl,-rpath,'$ORIGIN/../../../..:$ORIGIN' -ldl -lm \
        -o src/aot/build/r54p3-00eac0-6eda07/libvulkan_mali_aot_icd.so
fi

if [ -d mesa ] && [ -d build-android ]; then
    echo "================================================================================"
    echo " [Optional] Running ./compile.sh for Android aarch64 NDK Mesa PanVK Build      "
    echo "================================================================================"
    ./compile.sh
fi

echo "================================================================================"
echo " [SUCCESS] All Host, AOT, Tracer, and Test Binaries Built Cleanly!             "
echo "================================================================================"
