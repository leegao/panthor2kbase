#!/usr/bin/env bash
# ==============================================================================
# scripts/pack_sandbox_bundles.sh
# Packs and optionally splits (-b 95M for <=100 MB per-file Gemini Task Agent
# upload limit across up to 10 files = 1 GB budget) all offline bundles required
# for an air-gapped Gemini Task Agent VM sandbox to build, cross-compile, lift,
# emulate, and debug panthor2kbase + Mesa PanVK.
# ==============================================================================
set -euo pipefail

OUT_DIR="${1:-./sandbox_bundles}"
SPLIT_100M=1
if [[ "${2:-}" == "--no-split" ]]; then
   SPLIT_100M=0
fi

mkdir -p "$OUT_DIR"
echo "=============================================================================="
echo " Packing panthor2kbase Offline Gemini Sandbox Bundles -> $OUT_DIR"
echo "=============================================================================="

# Bundle 1: Source Checkpoint (<10 MB Gemini-ready source archive)
echo "[1/6] Packing Bundle 1: panthor-kbase-source-checkpoint.zip (--gemini mode)..."
./mkarchive.sh --gemini "$OUT_DIR/panthor-kbase-source-checkpoint.zip"

# Bundle 2: Minimal Android NDK r27d (aarch64-linux-android30/35 clang/lld/llvm-ar + sysroot)
NDK_SRC="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
if [[ ! -d "$NDK_SRC" && -d "../android-ndk-r27d" ]]; then
   NDK_SRC="$(cd ../android-ndk-r27d && pwd)"
fi
if [[ -d "$NDK_SRC" ]]; then
   echo "[2/6] Packing Bundle 2: ndk-minimal-aarch64-r27d.tar.xz from $NDK_SRC..."
   NDK_PARENT="$(dirname "$NDK_SRC")"
   NDK_BASE="$(basename "$NDK_SRC")"
   tar -C "$NDK_PARENT" -cJf "$OUT_DIR/ndk-minimal-aarch64-r27d.tar.xz" \
      "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/bin" \
      "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/lib" \
      "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include" \
      "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android"
   ls -lh "$OUT_DIR/ndk-minimal-aarch64-r27d.tar.xz"
else
   echo "[2/6] Skipping Bundle 2 (ANDROID_NDK_HOME not found at $NDK_SRC)"
fi

# Bundle 3: Mesa PanVK Offline Source + Prebuilt Host Shader Tools (mesa_clc, vtn_bindgen2, panfrost_compile) + Build Host Tools (ninja, meson, cmake, mako, pyyaml)
if [[ -d "mesa" ]]; then
   echo "[3/6] Packing Bundle 3: mesa-panvk-offline-bundle.tar.xz..."
   EXTRA_HOST_PATHS=()
   [[ -d "build-host/src/compiler/clc" ]] && EXTRA_HOST_PATHS+=("build-host/src/compiler/clc")
   [[ -d "build-host/src/compiler/spirv" ]] && EXTRA_HOST_PATHS+=("build-host/src/compiler/spirv")
   [[ -d "build-host/src/panfrost/clc" ]] && EXTRA_HOST_PATHS+=("build-host/src/panfrost/clc")
   tar -cJf "$OUT_DIR/mesa-panvk-offline-bundle.tar.xz" \
      --exclude='mesa/.git' --exclude='mesa/build*' \
      mesa "${EXTRA_HOST_PATHS[@]}"
   ls -lh "$OUT_DIR/mesa-panvk-offline-bundle.tar.xz"
else
   echo "[3/6] Skipping Bundle 3 (./mesa directory not present)"
fi

# Bundle 4: Proprietary Reference Binaries & Kernel Source (libGLES_mali.r54p3.so, libGLES_mali.r44p1.so, mali_kbase.ko, mt6878 kernel src, ludashi panvk)
echo "[4/6] Packing Bundle 4: device-reference-mt6878-r44p1-r54p3.tar.xz..."
REF_ITEMS=()
for item in libGLES_mali.so libGLES_mali.r44p1.so mali_kbase.ko r44p1_bins ludashi_panvk mt6878_kernel_src bcn_layer-main dxvk-1.10.3; do
   [[ -e "$item" ]] && REF_ITEMS+=("$item")
done
if [[ ${#REF_ITEMS[@]} -gt 0 ]]; then
   tar -cJf "$OUT_DIR/device-reference-mt6878-r44p1-r54p3.tar.xz" "${REF_ITEMS[@]}"
   ls -lh "$OUT_DIR/device-reference-mt6878-r44p1-r54p3.tar.xz"
fi

# Bundle 5: Host Build & Verification Sidecar Tools (cmake, ninja, meson, ortools wheel, Khronos vkcube/vkgears/vulkaninfo)
echo "[5/6] Packing Bundle 5: host-tools-and-vulkan-bins-x86_64.tar.xz..."
TOOL_ITEMS=()
for item in src/aot/bin/vkcube_khronos src/aot/bin/vkcube tools scripts; do
   [[ -e "$item" ]] && TOOL_ITEMS+=("$item")
done
if [[ ${#TOOL_ITEMS[@]} -gt 0 ]]; then
   tar -cJf "$OUT_DIR/host-tools-and-vulkan-bins-x86_64.tar.xz" "${TOOL_ITEMS[@]}"
   ls -lh "$OUT_DIR/host-tools-and-vulkan-bins-x86_64.tar.xz"
fi

# Bundle 6: Device Trace Logs (winlator_log.txt, vkcube_winlator.txt, crash2.txt)
echo "[6/6] Packing Bundle 6: winlator-trace-logs.tar.xz..."
LOG_ITEMS=()
for item in winlator_log.txt vkcube_winlator.txt crash2.txt; do
   [[ -f "$item" ]] && LOG_ITEMS+=("$item")
done
if [[ ${#LOG_ITEMS[@]} -gt 0 ]]; then
   tar -cJf "$OUT_DIR/winlator-trace-logs.tar.xz" "${LOG_ITEMS[@]}"
   ls -lh "$OUT_DIR/winlator-trace-logs.tar.xz"
fi

# Split any archive > 95 MiB into 95 MiB chunks (fits <= 100 MB per-file upload limit)
if [[ "$SPLIT_100M" -eq 1 ]]; then
   for f in "$OUT_DIR"/*; do
      [[ -f "$f" ]] || continue
      sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
      if [[ "$sz" -gt 99614720 ]]; then
         echo "  [split] Splitting $(basename "$f") ($sz bytes) into 95M parts..."
         split -b 95M "$f" "${f}_part_"
      fi
   done
fi

echo "=============================================================================="
echo " Bundle Packing Complete! Contents of $OUT_DIR:"
ls -lh "$OUT_DIR"
echo "=============================================================================="
