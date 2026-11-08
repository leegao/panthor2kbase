#!/usr/bin/env bash
set -euo pipefail

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"
export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

# Helper: copy directory tree preserving timestamps of unchanged files (cmp -s)
# so ninja does not recompile all of Mesa when mesa_overlay/ headers (like cs_builder.h) haven't changed!
sync_tree_if_changed() {
   local src_dir="$1"
   local dst_dir="$2"
   [ -d "$src_dir" ] || return 0
   mkdir -p "$dst_dir"
   while IFS= read -r -d '' src_file; do
      local rel="${src_file#$src_dir/}"
      local dst_file="$dst_dir/$rel"
      mkdir -p "$(dirname "$dst_file")"
      if [ ! -f "$dst_file" ] || ! cmp -s "$src_file" "$dst_file"; then
         cp -pf "$src_file" "$dst_file"
         echo "  [sync] Updated $dst_file"
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
   rebuild_libdrm=0
   if [ ! -f shims/lib/libdrm.a ]; then
      rebuild_libdrm=1
   else
      for f in src/adapter/panthor_kbase_adapter.cpp src/adapter/log.cpp src/adapter/drm_mode_stubs.c include/panthor_kbase_adapter.h include/mali_kbase_ioctl.h include/panthor_drm.h; do
         if [ -f "$f" ] && [ "$f" -nt shims/lib/libdrm.a ]; then
            rebuild_libdrm=1
            break
         fi
      done
   fi
   if [ "$rebuild_libdrm" -eq 1 ]; then
      echo "  [ndk] Rebuilding shims/lib/libdrm.a (adapter sources changed)..."
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
      cp -pf shims/lib/libdrm.a shims/libdrm.a
      cp -pf shims/lib/libdrm.a shims/libpanthor_kbase_adapter.a
      cp -pf shims/lib/libdrm.a shims/lib/libpanthor_kbase_adapter.a
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
         cp -pf mesa_subproject/meson.build mesa/subprojects/panthor_kbase_adapter/meson.build
      fi
   fi
fi

if [ -d build-android ]; then
   meson compile -C build-android
fi
