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

if [ -d $ROOT_DIR/third_party/compat_layer ] && ([ -f $ROOT_DIR/third_party/compat_layer/CMakeLists.txt ]); then
    echo "  -> Building third_party/compat_layer"
    "$CMAKE_BIN" -S "$ROOT_DIR/third_party/compat_layer" -B "$ROOT_DIR/build-x86-64/compat_layer" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_WARN_DEPRECATED=OFF

    cmake --build "$ROOT_DIR/build-x86-64/compat_layer" -j"$(nproc 2>/dev/null || echo 4)"
    echo "  -> Built libdxvk_mali_compat_layer.so in build-x86-64/compat_layer"
    cp "$ROOT_DIR/build-x86-64/compat_layer/libdxvk_mali_compat_layer.so" "$ROOT_DIR/build-x86-64/"
    cat > "$ROOT_DIR/build-x86-64/libdxvk_mali_compat_layer.json" << EOF
{
    "file_format_version" : "1.0.0",
    "layer" : {
        "name": "VK_LAYER_COMPAT_DxvkMaliCompatLayer",
        "type": "GLOBAL",
        "library_path": "./libdxvk_mali_compat_layer.so",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "Descriptor Buffer Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "DxvkMaliCompatLayer_NegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "DxvkMaliCompatLayer_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "DxvkMaliCompatLayer_GetDeviceProcAddr"
        },
        "enable_environment": {
            "ENABLE_MALI_COMPAT": "1"
        },
        "disable_environment": {
            "DISABLE_MALI_COMPAT": "1"
        }
    }
}
EOF
    PREBUILT_COMPAT_DIR="$ROOT_DIR/third_party/prebuilts/compat_layer/x86-64"
    mkdir -p "$PREBUILT_COMPAT_DIR"
    cp "$ROOT_DIR/build-x86-64/libdxvk_mali_compat_layer.so" "$PREBUILT_COMPAT_DIR/"
    cp "$ROOT_DIR/build-x86-64/libdxvk_mali_compat_layer.json" "$PREBUILT_COMPAT_DIR/"
    echo "  -> Cached layer to $PREBUILT_COMPAT_DIR"
fi

if [ -d $ROOT_DIR/third_party/bcn_layer ] && ([ -f $ROOT_DIR/third_party/bcn_layer/CMakeLists.txt ]); then
    echo "  -> Building third_party/bcn_layer"
    "$CMAKE_BIN" -S "$ROOT_DIR/third_party/bcn_layer" -B "$ROOT_DIR/build-x86-64/bcn_layer" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_WARN_DEPRECATED=OFF

    cmake --build "$ROOT_DIR/build-x86-64/bcn_layer" -j"$(nproc 2>/dev/null || echo 4)"
    echo "  -> Built libbcn_layer.so in build-x86-64/bcn_layer"
    cp "$ROOT_DIR/build-x86-64/bcn_layer/libbcn_layer.so" "$ROOT_DIR/build-x86-64/"
    cat > "$ROOT_DIR/build-x86-64/libbcn_layer.json" << EOF
{
    "file_format_version" : "1.0.0",
    "layer" : {
        "name": "VK_LAYER_BCN_BCnLayer",
        "type": "GLOBAL",
        "library_path": "./libbcn_layer.so",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "BCn Decompression Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "BCnLayer_NegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "BCnLayer_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "BCnLayer_GetDeviceProcAddr"
        },
        "enable_environment": {
            "ENABLE_BCN": "1"
        },
        "disable_environment": {
            "DISABLE_BCN": "1"
        }
    }
}
EOF
    PREBUILT_BCN_DIR="$ROOT_DIR/third_party/prebuilts/bcn_layer/x86-64"
    mkdir -p "$PREBUILT_BCN_DIR"
    cp "$ROOT_DIR/build-x86-64/libbcn_layer.so" "$PREBUILT_BCN_DIR/"
    cp "$ROOT_DIR/build-x86-64/libbcn_layer.json" "$PREBUILT_BCN_DIR/"
    echo "  -> Cached layer to $PREBUILT_BCN_DIR"
fi

WSI_LAYER_DIR=""
if [ -d "$ROOT_DIR/third_party/vulkan-wsi-layer" ] && [ -f "$ROOT_DIR/third_party/vulkan-wsi-layer/CMakeLists.txt" ]; then
    WSI_LAYER_DIR="$ROOT_DIR/third_party/vulkan-wsi-layer"
fi

PREBUILT_WSI_DIR="$ROOT_DIR/third_party/prebuilts/vulkan_wsi_layer/x86-64"

if [ -n "$WSI_LAYER_DIR" ]; then
    echo "  -> Building $WSI_LAYER_DIR"
    "$CMAKE_BIN" -S "$WSI_LAYER_DIR" -B "$ROOT_DIR/build-x86-64/vulkan_wsi_layer" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_WARN_DEPRECATED=OFF \
        -DVULKAN_CXX_INCLUDE="$ROOT_DIR/include" \
        -DSELECT_EXTERNAL_ALLOCATOR=dma_buf_heaps \
        -DWSIALLOC_MEMORY_HEAP_NAME=system-uncached

    cmake --build "$ROOT_DIR/build-x86-64/vulkan_wsi_layer" -j"$(nproc 2>/dev/null || echo 4)"
    echo "  -> Built libVkLayer_window_system_integration.so in build-x86-64/vulkan_wsi_layer"

    cp "$ROOT_DIR/build-x86-64/vulkan_wsi_layer/libVkLayer_window_system_integration.so" "$ROOT_DIR/build-x86-64/"
    cp "$ROOT_DIR/build-x86-64/vulkan_wsi_layer/VkLayer_window_system_integration.json" "$ROOT_DIR/build-x86-64/"

    mkdir -p "$PREBUILT_WSI_DIR"
    cp "$ROOT_DIR/build-x86-64/vulkan_wsi_layer/libVkLayer_window_system_integration.so" "$PREBUILT_WSI_DIR/"
    cp "$ROOT_DIR/build-x86-64/vulkan_wsi_layer/VkLayer_window_system_integration.json" "$PREBUILT_WSI_DIR/"
    echo "  -> Cached layer to $PREBUILT_WSI_DIR"
elif [ -f "$PREBUILT_WSI_DIR/libVkLayer_window_system_integration.so" ]; then
    echo "  -> Using prebuilt vulkan_wsi_layer from third_party/prebuilts"
    cp "$PREBUILT_WSI_DIR/libVkLayer_window_system_integration.so" "$ROOT_DIR/build-x86-64/"
    cp "$PREBUILT_WSI_DIR/VkLayer_window_system_integration.json" "$ROOT_DIR/build-x86-64/"
fi

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
    if [ -d $ROOT_DIR/third_party/compat_layer ] && ([ -f $ROOT_DIR/third_party/compat_layer/CMakeLists.txt ]); then
        echo "  -> Building third_party/compat_layer"
        "$CMAKE_BIN" -S "$ROOT_DIR/third_party/compat_layer" -B "$ROOT_DIR/build-android/compat_layer" \
            -DCMAKE_SYSTEM_NAME=Android \
            -DANDROID_PLATFORM=26 \
            -DANDROID_ABI=arm64-v8a \
            -DANDROID_TOOLCHAIN=clang \
            -DANDROID_STL=c++_static \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="${NDK_TOOLCHAIN}" \
            -DCMAKE_WARN_DEPRECATED=OFF

        cmake --build "$ROOT_DIR/build-android/compat_layer" -j"$(nproc 2>/dev/null || echo 4)"
        echo "  -> Built libdxvk_mali_compat_layer.so in build-android/compat_layer"
        cp "$ROOT_DIR/build-android/compat_layer/libdxvk_mali_compat_layer.so" "$ROOT_DIR/build-android/"
        cat > "$ROOT_DIR/build-android/libdxvk_mali_compat_layer.json" << EOF
{
    "file_format_version" : "1.0.0",
    "layer" : {
        "name": "VK_LAYER_COMPAT_DxvkMaliCompatLayer",
        "type": "GLOBAL",
        "library_path": "./libdxvk_mali_compat_layer.so",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "Descriptor Buffer Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "DxvkMaliCompatLayer_NegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "DxvkMaliCompatLayer_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "DxvkMaliCompatLayer_GetDeviceProcAddr"
        },
        "enable_environment": {
            "ENABLE_MALI_COMPAT": "1"
        },
        "disable_environment": {
            "DISABLE_MALI_COMPAT": "1"
        }
    }
}
EOF
    fi

    if [ -d $ROOT_DIR/third_party/bcn_layer ] && ([ -f $ROOT_DIR/third_party/bcn_layer/CMakeLists.txt ]); then
        echo "  -> Building third_party/bcn_layer"
        "$CMAKE_BIN" -S "$ROOT_DIR/third_party/bcn_layer" -B "$ROOT_DIR/build-android/bcn_layer" \
            -DCMAKE_SYSTEM_NAME=Android \
            -DANDROID_PLATFORM=26 \
            -DANDROID_ABI=arm64-v8a \
            -DANDROID_TOOLCHAIN=clang \
            -DANDROID_STL=c++_static \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="${NDK_TOOLCHAIN}" \
            -DCMAKE_WARN_DEPRECATED=OFF

        cmake --build "$ROOT_DIR/build-android/bcn_layer" -j"$(nproc 2>/dev/null || echo 4)"
        echo "  -> Built libbcn_layer.so in build-android/bcn_layer"
        cp "$ROOT_DIR/build-android/bcn_layer/libbcn_layer.so" "$ROOT_DIR/build-android/"
        cat > "$ROOT_DIR/build-android/libbcn_layer.json" << EOF
{
    "file_format_version" : "1.0.0",
    "layer" : {
        "name": "VK_LAYER_BCN_BCnLayer",
        "type": "GLOBAL",
        "library_path": "./libbcn_layer.so",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "BCn Decompression Layer",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "BCnLayer_NegotiateLoaderLayerInterfaceVersion",
            "vkGetInstanceProcAddr": "BCnLayer_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "BCnLayer_GetDeviceProcAddr"
        },
        "enable_environment": {
            "ENABLE_BCN": "1"
        },
        "disable_environment": {
            "DISABLE_BCN": "1"
        }
    }
}
EOF
    fi
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
