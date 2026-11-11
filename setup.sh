#!/bin/bash
#
if [ ! -d "$(dirname "$0")/mesa" ]; then
    echo "mesa directory does not exist"
    exit 1
fi

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
export MESON_WORKING_DIR="$PWD"

# Sync mesa_overlay, subproject, and adapter sources into transient mesa directory
"$MESON_WORKING_DIR/scripts/apply_mesa_overlay.sh" "$MESON_WORKING_DIR/mesa"

meson setup build-x86-64/mesa mesa \
    -Dplatforms=x11,wayland \
    -Dgallium-drivers=[] \
    -Dvulkan-drivers=panfrost \
    -Dtools=panfrost \
    -Dprecomp-compiler=enabled \
    -Dinstall-precomp-compiler=true \
    -Dllvm=enabled \
    -Dmesa-clc=enabled -Dinstall-mesa-clc=true

ninja -C build-x86-64/mesa src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile

ln -sf "$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/clc/mesa_clc" "$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/clc/mesa-clc"
ln -sf "$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/spirv/vtn_bindgen2" "$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/spirv/vtn-bindgen2"
ln -sf "$MESON_WORKING_DIR/build-x86-64/mesa/src/panfrost/clc/panfrost_compile" "$MESON_WORKING_DIR/build-x86-64/mesa/src/panfrost/clc/panfrost-compile"

export PATH="$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/clc:$MESON_WORKING_DIR/build-x86-64/mesa/src/compiler/spirv:$MESON_WORKING_DIR/build-x86-64/mesa/src/panfrost/clc:$PATH"

export PKG_CONFIG_LIBDIR=$MESON_WORKING_DIR/shims

envsubst < android.toml > /tmp/android_wrapper.toml

meson setup build-android/mesa mesa --reconfigure \
    --cross-file /tmp/android_wrapper.toml \
    -Dbuildtype=debugoptimized \
    -Dplatforms=android,x11 \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Dplatform-sdk-version=30 \
    -Dglx=disabled \
    -Dgbm=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglvnd=disabled \
    -Dvalgrind=disabled \
    -Dgallium-drivers= \
    -Dshared-glapi=disabled \
    -Dzstd=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dvulkan-drivers=panfrost \
    -Dllvm=disabled
