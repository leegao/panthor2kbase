export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"

export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

mkdir -p shims/include shims/lib build-android-ndk
cp -rf src/mesa_shims/* shims/include/ 2>/dev/null || true
cp -rf include/* shims/include/

NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [ -x "$NDK_BIN/aarch64-linux-android30-clang++" ]; then
   "$NDK_BIN/aarch64-linux-android30-clang++" -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
      -c src/adapter/panthor_kbase_adapter.cpp -o build-android-ndk/panthor_kbase_adapter.o
   "$NDK_BIN/aarch64-linux-android30-clang++" -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
      -c src/adapter/log.cpp -o build-android-ndk/log.o
   "$NDK_BIN/aarch64-linux-android30-clang" -std=c11 -O2 -fPIC -Wall -Wextra -Iinclude -Isrc/adapter \
      -c src/adapter/drm_mode_stubs.c -o build-android-ndk/drm_mode_stubs.o
   "$NDK_BIN/llvm-ar" rcs shims/lib/libdrm.a \
      build-android-ndk/panthor_kbase_adapter.o \
      build-android-ndk/log.o \
      build-android-ndk/drm_mode_stubs.o
   cp -f shims/lib/libdrm.a shims/libdrm.a
   cp -f shims/lib/libdrm.a shims/libpanthor_kbase_adapter.a
   cp -f shims/lib/libdrm.a shims/lib/libpanthor_kbase_adapter.a
fi

cp -rf mesa_overlay/* mesa/
mkdir -p mesa/src/panfrost/uapi_adapter \
         mesa/subprojects/panthor_kbase_adapter/include \
         mesa/subprojects/panthor_kbase_adapter/src/adapter
cp -r include/* mesa/src/panfrost/uapi_adapter/
cp -r src/adapter/* mesa/src/panfrost/uapi_adapter/
cp -r include/* mesa/subprojects/panthor_kbase_adapter/include/
cp -r src/adapter/* mesa/subprojects/panthor_kbase_adapter/src/adapter/
cp mesa_subproject/meson.build mesa/subprojects/panthor_kbase_adapter/meson.build
meson compile -C build-android
