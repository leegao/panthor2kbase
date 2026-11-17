#!/usr/bin/env bash
set -euo pipefail

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"
CMAKE_BIN_DIR="$(dirname "$("$MESON_WORKING_DIR/scripts/find_cmake.sh" 2>/dev/null || echo /usr/bin/cmake)")"
export PATH="$CMAKE_BIN_DIR:$PATH"
export PATH="$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/clc:$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/spirv:$MESON_WORKING_DIR/build-x86-64/mesa/src/panfrost/clc:$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

# Fast checksum helper (uses POSIX `cksum` CRC-32 with `sha256sum` fallback).
file_fast_cksum() {
   if command -v cksum >/dev/null 2>&1; then
      cksum "$1" | awk '{print $1 "-" $2}'
   else
      sha256sum "$1" | awk '{print $1}'
   fi
}

# Overlay sync is centralized in scripts/apply_mesa_overlay.sh (uses fast cmp -s byte comparison
# in sync_tree_if_changed and touch "$dst_file" so Ninja recompiles on change).
mkdir -p shims/lib build-android/scratch
"$MESON_WORKING_DIR/scripts/apply_mesa_overlay.sh" "$MESON_WORKING_DIR/mesa"

NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_CXX=""
NDK_CC=""
if [ -x "$NDK_BIN/aarch64-linux-android30-clang++" ]; then
   NDK_CXX="$NDK_BIN/aarch64-linux-android30-clang++"
   NDK_CC="$NDK_BIN/aarch64-linux-android30-clang"
elif [ -x "$NDK_BIN/aarch64-linux-android35-clang++" ]; then
   NDK_CXX="$NDK_BIN/aarch64-linux-android35-clang++"
   NDK_CC="$NDK_BIN/aarch64-linux-android35-clang"
fi

if [ -n "$NDK_CXX" ]; then
   ADAPTER_SUM="$(cksum src/adapter/panthor_kbase_adapter.cpp src/adapter/mali_kbase_caps.cpp src/adapter/panthor_drm_capture_replay.cpp src/adapter/log.cpp src/adapter/drm_mode_stubs.c include/panthor_kbase_adapter.h include/mali_kbase_caps.h include/mali_kbase_uapi.h include/panthor_drm.h 2>/dev/null | cksum | awk '{print $1 "-" $2}')"
   PREV_ADAPTER_SUM=""
   if [ -f build-android/scratch/.libdrm_sources.sha256 ]; then
      PREV_ADAPTER_SUM="$(cat build-android/scratch/.libdrm_sources.sha256)"
   fi
   if [ ! -f shims/lib/libdrm.a ] || [ "$ADAPTER_SUM" != "$PREV_ADAPTER_SUM" ]; then
      echo "  [ndk] Rebuilding shims/lib/libdrm.a via CMake (digest changed: $ADAPTER_SUM)..."
      CMAKE_BIN="$("$MESON_WORKING_DIR/scripts/find_cmake.sh" 2>/dev/null || command -v cmake)"
      if [ -n "$CMAKE_BIN" ]; then
         if [ -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" ]; then
            "$CMAKE_BIN" -S "$MESON_WORKING_DIR" -B "$MESON_WORKING_DIR/build-android" \
               -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
               -DANDROID_ABI=arm64-v8a \
               -DANDROID_PLATFORM=android-30 \
               -DPANTHOR_KBASE_BUILD_TESTS=OFF \
               -DPANTHOR_KBASE_BUILD_AOT=OFF
         else
            "$CMAKE_BIN" -S "$MESON_WORKING_DIR" -B "$MESON_WORKING_DIR/build-android" \
               -DCMAKE_SYSTEM_NAME=Linux \
               -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
               -DANDROID=ON \
               -DANDROID_ABI=arm64-v8a \
               -DANDROID_PLATFORM=android-30 \
               -DCMAKE_C_COMPILER="$NDK_CC" \
               -DCMAKE_CXX_COMPILER="$NDK_CXX" \
               -DCMAKE_AR="$NDK_BIN/llvm-ar" \
               -DCMAKE_RANLIB="$NDK_BIN/llvm-ranlib" \
               -DPANTHOR_KBASE_BUILD_TESTS=OFF \
               -DPANTHOR_KBASE_BUILD_AOT=OFF
         fi
         "$CMAKE_BIN" --build "$MESON_WORKING_DIR/build-android" --target drm_shim panthor_kbase_adapter_static panthor_kbase_adapter mali_kbase_emu mali_kbase_emu_static -j"$(nproc)"
         cp -f "$MESON_WORKING_DIR/build-android/libdrm.a" shims/lib/libdrm.a
         cp -f "$MESON_WORKING_DIR/build-android/libdrm.a" shims/libdrm.a
         cp -f "$MESON_WORKING_DIR/build-android/libpanthor_kbase_adapter.a" shims/libpanthor_kbase_adapter.a
         cp -f "$MESON_WORKING_DIR/build-android/libpanthor_kbase_adapter.a" shims/lib/libpanthor_kbase_adapter.a
      else
         "$NDK_CXX" -std=c++17 -O2 -fPIC -ffile-prefix-map="$MESON_WORKING_DIR"=. -Wall -Wextra -Iinclude -Isrc/adapter \
            -c src/adapter/panthor_kbase_adapter.cpp -o build-android/scratch/panthor_kbase_adapter.o
         "$NDK_CXX" -std=c++17 -O2 -fPIC -ffile-prefix-map="$MESON_WORKING_DIR"=. -Wall -Wextra -Iinclude -Isrc/adapter \
            -c src/adapter/mali_kbase_caps.cpp -o build-android/scratch/mali_kbase_caps.o
         "$NDK_CXX" -std=c++17 -O2 -fPIC -ffile-prefix-map="$MESON_WORKING_DIR"=. -Wall -Iinclude -Isrc/adapter -Isrc/emulator \
            -c src/adapter/panthor_drm_capture_replay.cpp -o build-android/scratch/panthor_drm_capture_replay.o
         "$NDK_CXX" -std=c++17 -O2 -fPIC -ffile-prefix-map="$MESON_WORKING_DIR"=. -Wall -Wextra -Iinclude -Isrc/adapter \
            -c src/adapter/log.cpp -o build-android/scratch/log.o
         "$NDK_CC" -std=c11 -O2 -fPIC -ffile-prefix-map="$MESON_WORKING_DIR"=. -Wall -Wextra -Iinclude -Isrc/adapter \
            -c src/adapter/drm_mode_stubs.c -o build-android/scratch/drm_mode_stubs.o
         "$NDK_BIN/llvm-ar" rcs shims/lib/libdrm.a \
            build-android/scratch/panthor_kbase_adapter.o \
            build-android/scratch/mali_kbase_caps.o \
            build-android/scratch/panthor_drm_capture_replay.o \
            build-android/scratch/log.o \
            build-android/scratch/drm_mode_stubs.o
         cp -f shims/lib/libdrm.a shims/libdrm.a
         cp -f shims/lib/libdrm.a shims/libpanthor_kbase_adapter.a
         cp -f shims/lib/libdrm.a shims/lib/libpanthor_kbase_adapter.a
      fi
      echo "$ADAPTER_SUM" > build-android/scratch/.libdrm_sources.sha256
   fi
fi


CS_BUILDER_SUM=""
if [ -f mesa_overlay/src/panfrost/genxml/cs_builder.h ]; then
   CS_BUILDER_SUM="$(file_fast_cksum mesa_overlay/src/panfrost/genxml/cs_builder.h)"
fi

MESA_BUILD_TARGET=""
if [ -f build-android/mesa/build.ninja ]; then
   MESA_BUILD_TARGET="build-android/mesa"
elif [ -f build-android/build.ninja ]; then
   MESA_BUILD_TARGET="build-android"
fi

if [ -n "$MESA_BUILD_TARGET" ]; then
   PREV_CS_SUM=""
   if [ -f "$MESA_BUILD_TARGET/.cs_builder_abi.sha256" ]; then
      PREV_CS_SUM="$(cat "$MESA_BUILD_TARGET/.cs_builder_abi.sha256")"
   elif [ -f build-android/scratch/.cs_builder_abi.sha256 ]; then
      PREV_CS_SUM="$(cat build-android/scratch/.cs_builder_abi.sha256)"
   fi
   if [ "$CS_BUILDER_SUM" != "$PREV_CS_SUM" ]; then
      echo "  [abi] cs_builder.h digest changed ($CS_BUILDER_SUM); cleaning panvk CSF .o files in $MESA_BUILD_TARGET..."
      rm -f "$MESA_BUILD_TARGET"/src/panfrost/vulkan/libpanfrost_vk_*.a.p/*.o 2>/dev/null || true
      rm -f "$MESA_BUILD_TARGET"/src/panfrost/vulkan/libvulkan_panfrost.so.p/*.o 2>/dev/null || true
      [ -d mesa/src/panfrost/vulkan/csf ] && touch mesa/src/panfrost/vulkan/csf/*.c 2>/dev/null || true
      echo "$CS_BUILDER_SUM" > "$MESA_BUILD_TARGET/.cs_builder_abi.sha256"
      echo "$CS_BUILDER_SUM" > build-android/scratch/.cs_builder_abi.sha256
   fi

   meson compile -C "$MESA_BUILD_TARGET"

   # Copy out unstripped and stripped libvulkan_panfrost.so into build-android/
   SRC_PANVK=""
   if [ -f "$MESA_BUILD_TARGET/src/panfrost/vulkan/libvulkan_panfrost.so" ]; then
      SRC_PANVK="$MESA_BUILD_TARGET/src/panfrost/vulkan/libvulkan_panfrost.so"
   fi
   if [ -n "$SRC_PANVK" ]; then
      mkdir -p build-android
      cp -f "$SRC_PANVK" build-android/libvulkan_panfrost.unstripped.so
      if [ -x "$NDK_BIN/llvm-strip" ]; then
         "$NDK_BIN/llvm-strip" -o build-android/libvulkan_panfrost.so build-android/libvulkan_panfrost.unstripped.so
      elif command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
         aarch64-linux-gnu-strip -o build-android/libvulkan_panfrost.so build-android/libvulkan_panfrost.unstripped.so
      elif command -v strip >/dev/null 2>&1; then
         strip -o build-android/libvulkan_panfrost.so build-android/libvulkan_panfrost.unstripped.so
      else
         cp -f build-android/libvulkan_panfrost.unstripped.so build-android/libvulkan_panfrost.so
      fi
      echo "  -> Exported build-android/libvulkan_panfrost.so (stripped) & build-android/libvulkan_panfrost.unstripped.so"
   fi
fi

# Build/export host x86-64 libvulkan_panfrost.so if build-x86-64/mesa is configured for panfrost
if [ -f build-x86-64/mesa/build.ninja ] && grep -q "src/panfrost/vulkan/libvulkan_panfrost.so:" build-x86-64/mesa/build.ninja 2>/dev/null; then
   echo "  [host-panvk] Building x86-64 host build-x86-64/libvulkan_panfrost.so..."
   ninja -C build-x86-64/mesa src/panfrost/vulkan/libvulkan_panfrost.so -j"$(nproc 2>/dev/null || echo 4)"
   if [ -f build-x86-64/mesa/src/panfrost/vulkan/libvulkan_panfrost.so ]; then
      cp -f build-x86-64/mesa/src/panfrost/vulkan/libvulkan_panfrost.so build-x86-64/libvulkan_panfrost.unstripped.so
      strip -o build-x86-64/libvulkan_panfrost.so build-x86-64/libvulkan_panfrost.unstripped.so 2>/dev/null || \
         cp -f build-x86-64/libvulkan_panfrost.unstripped.so build-x86-64/libvulkan_panfrost.so
      cat << EOF_ICD > build-x86-64/panfrost_host_icd.x86_64.json
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libvulkan_panfrost.so",
        "api_version": "1.3.250"
    }
}
EOF_ICD
      echo "  -> Exported build-x86-64/libvulkan_panfrost.so & build-x86-64/panfrost_host_icd.x86_64.json"
   fi
fi
