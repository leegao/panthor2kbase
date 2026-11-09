#!/usr/bin/env bash
set -euo pipefail

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"
export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

CS_BUILDER_CHANGED=0

# Fast checksum helper (uses POSIX `cksum` CRC-32 with `sha256sum` fallback).
file_fast_cksum() {
   if command -v cksum >/dev/null 2>&1; then
      cksum "$1" | awk '{print $1 "-" $2}'
   else
      sha256sum "$1" | awk '{print $1}'
   fi
}

# Helper: compare exact byte contents of source and destination files via `cmp -s`
# (zero subshell/pipe hash overhead per file, while remaining 100% immune to timestamp skew).
# - If a src pull / unzip touched all timestamps in mesa_overlay/ without
#   changing file contents, `cmp -s` matches and destination mtime is untouched
#   (zero unnecessary Mesa rebuilds).
# - If file contents actually changed (even if extracted from an archive with an
#   older timestamp), copy and `touch` the destination file so Ninja is
#   guaranteed to recompile all dependent object files.
sync_tree_if_changed() {
   local src_dir="$1"
   local dst_dir="$2"
   [ -d "$src_dir" ] || return 0
   mkdir -p "$dst_dir"
   while IFS= read -r -d '' src_file; do
      local rel="${src_file#$src_dir/}"
      local dst_file="$dst_dir/$rel"
      local need_copy=0
      if [ ! -f "$dst_file" ]; then
         mkdir -p "$(dirname "$dst_file")"
         need_copy=1
      elif ! cmp -s "$src_file" "$dst_file"; then
         need_copy=1
      fi
      if [ "$need_copy" -eq 1 ]; then
         cp -f "$src_file" "$dst_file"
         touch "$dst_file"
         echo "  [sync] Updated $dst_file (content changed)"
         if [[ "$rel" == *"cs_builder.h" ]]; then
            CS_BUILDER_CHANGED=1
         fi
      fi
   done < <(find "$src_dir" -type f -print0)
}

mkdir -p shims/include shims/lib build-android-ndk
sync_tree_if_changed "src/mesa_shims" "shims/include"
sync_tree_if_changed "include" "shims/include"

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
   ADAPTER_SUM="$(cksum src/adapter/panthor_kbase_adapter.cpp src/adapter/log.cpp src/adapter/drm_mode_stubs.c include/panthor_kbase_adapter.h include/mali_kbase_uapi.h include/panthor_drm.h 2>/dev/null | cksum | awk '{print $1 "-" $2}')"
   PREV_ADAPTER_SUM=""
   if [ -f build-android-ndk/.libdrm_sources.sha256 ]; then
      PREV_ADAPTER_SUM="$(cat build-android-ndk/.libdrm_sources.sha256)"
   fi
   if [ ! -f shims/lib/libdrm.a ] || [ "$ADAPTER_SUM" != "$PREV_ADAPTER_SUM" ]; then
      echo "  [ndk] Rebuilding shims/lib/libdrm.a (digest changed: $ADAPTER_SUM)..."
      "$NDK_CXX" -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
         -c src/adapter/panthor_kbase_adapter.cpp -o build-android-ndk/panthor_kbase_adapter.o
      "$NDK_CXX" -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
         -c src/adapter/log.cpp -o build-android-ndk/log.o
      "$NDK_CC" -std=c11 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
         -c src/adapter/drm_mode_stubs.c -o build-android-ndk/drm_mode_stubs.o
      "$NDK_BIN/llvm-ar" rcs shims/lib/libdrm.a \
         build-android-ndk/panthor_kbase_adapter.o \
         build-android-ndk/log.o \
         build-android-ndk/drm_mode_stubs.o
      cp -f shims/lib/libdrm.a shims/libdrm.a
      cp -f shims/lib/libdrm.a shims/libpanthor_kbase_adapter.a
      cp -f shims/lib/libdrm.a shims/lib/libpanthor_kbase_adapter.a
      echo "$ADAPTER_SUM" > build-android-ndk/.libdrm_sources.sha256
   fi
fi

if [ -d mesa ]; then
   sync_tree_if_changed "mesa_overlay" "mesa"
   mkdir -p mesa/src/panfrost/uapi_adapter \
            mesa/subprojects/panthor_kbase_adapter/include \
            mesa/subprojects/panthor_kbase_adapter/src/adapter
   sync_tree_if_changed "include" "mesa/src/panfrost/uapi_adapter"
   sync_tree_if_changed "src/adapter" "mesa/src/panfrost/uapi_adapter"
   sync_tree_if_changed "include" "mesa/subprojects/panthor_kbase_adapter/include"
   sync_tree_if_changed "src/adapter" "mesa/subprojects/panthor_kbase_adapter/src/adapter"
   if [ -f mesa_subproject/meson.build ]; then
      if [ ! -f mesa/subprojects/panthor_kbase_adapter/meson.build ] || ! cmp -s mesa_subproject/meson.build mesa/subprojects/panthor_kbase_adapter/meson.build; then
         cp -f mesa_subproject/meson.build mesa/subprojects/panthor_kbase_adapter/meson.build
         touch mesa/subprojects/panthor_kbase_adapter/meson.build
      fi
   fi
fi

CS_BUILDER_SUM=""
if [ -f mesa_overlay/src/panfrost/genxml/cs_builder.h ]; then
   CS_BUILDER_SUM="$(file_fast_cksum mesa_overlay/src/panfrost/genxml/cs_builder.h)"
fi

for build_dir in build-android mesa/build; do
   if [ -d "$build_dir" ]; then
      PREV_CS_SUM=""
      if [ -f "$build_dir/.cs_builder_abi.sha256" ]; then
         PREV_CS_SUM="$(cat "$build_dir/.cs_builder_abi.sha256")"
      fi
      if [ "$CS_BUILDER_CHANGED" -eq 1 ] || [ "$CS_BUILDER_SUM" != "$PREV_CS_SUM" ]; then
         echo "  [abi] cs_builder.h digest changed ($CS_BUILDER_SUM); cleaning panvk CSF .o files in $build_dir..."
         rm -f "$build_dir"/src/panfrost/vulkan/libpanfrost_vk_*.a.p/*.o 2>/dev/null || true
         rm -f "$build_dir"/src/panfrost/vulkan/libvulkan_panfrost.so.p/*.o 2>/dev/null || true
         [ -d mesa/src/panfrost/vulkan/csf ] && touch mesa/src/panfrost/vulkan/csf/*.c 2>/dev/null || true
         echo "$CS_BUILDER_SUM" > "$build_dir/.cs_builder_abi.sha256"
      fi
   fi
done

if [ -d build-android ]; then
   meson compile -C build-android
fi
