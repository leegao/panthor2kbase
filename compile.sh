export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"

export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

mkdir -p shims/include && cp -rf src/mesa_shims/* shims/include/
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
