#!/usr/bin/env bash
# ==============================================================================
# scripts/pack_sandbox_bundles.sh
# Packs panthor2kbase into offline Gemini Sandbox Bundles:
# 1. panthor-kbase-source-checkpoint.zip (separate source archive, <16 MB)
# 2. panthor2kbase-offline-sandbox-bundle.zip (unified offline bundle containing
#    Mesa, NDK, reference binaries, predownloaded dependencies, wheels,
#    host tools, SANDBOX_BUNDLE.md, and manifest.sha256).
# Splits ONLY the unified bundle into 95 MiB chunks to maximize payload within
# the 10-file Gemini Task Agent limit (1 source archive + up to 9 chunk parts <= 1 GB).
# ==============================================================================
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-./sandbox_bundles}"
SPLIT_100M=1
if [[ "${2:-}" == "--no-split" ]]; then
   SPLIT_100M=0
fi

mkdir -p "$OUT_DIR"
echo "=============================================================================="
echo " Packing panthor2kbase Offline Gemini Sandbox Bundles -> $OUT_DIR"
echo "=============================================================================="

# ------------------------------------------------------------------------------
# Helper: Restore mesa_overlay, include, and src/adapter into mesa/
# ------------------------------------------------------------------------------
restore_mesa_overlay() {
   if [[ -d "mesa" && -d "mesa_overlay" ]]; then
      echo "  [sync] Restoring mesa_overlay & adapter files into mesa..."
      "${ROOT_DIR}/scripts/apply_mesa_overlay.sh" "${ROOT_DIR}/mesa"
   fi
}

# ------------------------------------------------------------------------------
# Helper: Ensure Python 3.11 wheels for offline sandbox verification
# ------------------------------------------------------------------------------
ensure_python_wheels() {
   local wheels_dir="$1"
   mkdir -p "$wheels_dir"
   local has_all=1
   for pkg in ortools capstone z3_solver pyelftools mako meson; do
      if ! compgen -G "$wheels_dir/${pkg}*.whl" >/dev/null; then
         has_all=0
         break
      fi
   done
   if [[ "$has_all" -eq 0 ]]; then
      echo "  [pip] Downloading Python 3.11 wheels (ortools, capstone, z3-solver, pyelftools, mako, meson)..."
      python3 -m pip download \
         --only-binary=:all: \
         --platform manylinux2014_x86_64 \
         --python-version 3.11 \
         --implementation cp \
         --abi cp311 \
         -d "$wheels_dir" \
         ortools capstone z3-solver pyelftools Mako meson || true
   fi
}

# ==============================================================================
# 1. panthor-kbase-source-checkpoint.zip (Separate source archive, <16 MB)
# ==============================================================================
# B1_TARGET="$OUT_DIR/panthor-kbase-source-checkpoint.zip"
# B1_MANIFEST="$OUT_DIR/panthor-kbase-source-checkpoint.manifest"
# B1_CUR_STATE="$(git rev-parse HEAD 2>/dev/null || echo "no-git")-$(git status --porcelain 2>/dev/null | sha256sum | awk '{print $1}')"
#
# echo "[1/4] Packing Standalone Source Checkpoint: panthor-kbase-source-checkpoint.zip ..."
# if [[ -f "$B1_TARGET" && -f "$B1_MANIFEST" && "$(cat "$B1_MANIFEST")" == "$B1_CUR_STATE" ]]; then
#    echo "  [skip] Source checkpoint already up to date (git state unchanged)."
# else
#    ./mkarchive.sh "$B1_TARGET"
#    echo "$B1_CUR_STATE" > "$B1_MANIFEST"
#    ls -lh "$B1_TARGET"
# fi

# ==============================================================================
# 2. Build Sub-Components for Unified Offline Bundle
# ==============================================================================

# 2A. Minimal Android NDK r27d
NDK_SRC="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
if [[ ! -d "$NDK_SRC" && -d "../android-ndk-r27d" ]]; then
   NDK_SRC="$(cd ../android-ndk-r27d && pwd)"
fi
NDK_PKG="$OUT_DIR/ndk-minimal-aarch64-r27d.tar.xz"
if [[ -d "$NDK_SRC" ]]; then
   echo "[2/4] Ensuring Android NDK minimal component..."
   if [[ -f "$NDK_PKG" ]]; then
      echo "  [skip] NDK package already exists, skipping."
   else
      echo "  [pack] Archiving minimal Android NDK r27d -> $NDK_PKG..."
      NDK_PARENT="$(dirname "$NDK_SRC")"
      NDK_BASE="$(basename "$NDK_SRC")"
      tar -C "$NDK_PARENT" -c -I 'xz -T0' -f "$NDK_PKG" \
         "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/bin" \
         "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/lib" \
         "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include" \
         "$NDK_BASE/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android"
   fi
else
   echo "[2/4] Warning: ANDROID_NDK_HOME not found at $NDK_SRC"
fi

# 2B. Clean Mesa PanVK Offline Source + Prebuilt Host Shader Tools
MESA_PKG="$OUT_DIR/mesa-panvk-offline-bundle.tar.xz"
MESA_MANIFEST="$OUT_DIR/mesa-panvk-offline-bundle.manifest"
if [[ -d "mesa" ]]; then
   echo "[2/4] Ensuring Clean Mesa PanVK component..."
   EXTRA_HOST_PATHS=()
   [[ -d "build-x86-64/mesa/src/compiler/clc" ]] && EXTRA_HOST_PATHS+=("build-x86-64/mesa/src/compiler/clc")
   [[ -d "build-x86-64/mesa/src/compiler/spirv" ]] && EXTRA_HOST_PATHS+=("build-x86-64/mesa/src/compiler/spirv")
   [[ -d "build-x86-64/mesa/src/panfrost/clc" ]] && EXTRA_HOST_PATHS+=("build-x86-64/mesa/src/panfrost/clc")
   [[ -d "build-host/src/compiler/clc" ]] && EXTRA_HOST_PATHS+=("build-host/src/compiler/clc")
   [[ -d "build-host/src/compiler/spirv" ]] && EXTRA_HOST_PATHS+=("build-host/src/compiler/spirv")
   [[ -d "build-host/src/panfrost/clc" ]] && EXTRA_HOST_PATHS+=("build-host/src/panfrost/clc")

   MESA_INPUTS_HASH=$( (
      git -C mesa rev-parse HEAD 2>/dev/null || echo "mesa-head"
      git -C mesa diff --cached 2>/dev/null || true
      for p in "${EXTRA_HOST_PATHS[@]}"; do
         [[ -d "$p" ]] && find "$p" -type f -exec sha256sum {} + 2>/dev/null
      done
   ) | sha256sum | awk '{print $1}' )

   if [[ -f "$MESA_PKG" && -f "$MESA_MANIFEST" && "$(cat "$MESA_MANIFEST")" == "$MESA_INPUTS_HASH" ]]; then
      echo "  [skip] Mesa offline package already up to date."
   else
      echo "  [clean] Cleaning unstaged changes from mesa/ (git clean -fd && git restore .)..."
      git -C mesa clean -fd
      git -C mesa restore .

      echo "  [pack] Archiving clean mesa/ + host tools -> $MESA_PKG..."
      tar -c -I 'xz -T0' -f "$MESA_PKG" \
         --exclude='mesa/.git' --exclude='mesa/build*' \
         mesa "${EXTRA_HOST_PATHS[@]}"

      echo "$MESA_INPUTS_HASH" > "$MESA_MANIFEST"
      restore_mesa_overlay
   fi
fi

# 2C. Host Verification Tools
TOOLS_PKG="$OUT_DIR/host-tools-and-vulkan-bins-x86_64.tar.xz"
TOOLS_MANIFEST="$OUT_DIR/host-tools-and-vulkan-bins-x86_64.manifest"
TOOL_ITEMS=()
for item in tools scripts; do
   [[ -e "$item" ]] && TOOL_ITEMS+=("$item")
done

if [[ ${#TOOL_ITEMS[@]} -gt 0 ]]; then
   echo "[2/4] Ensuring Host Verification Tools component..."
   TOOLS_INPUT_HASH=$(find "${TOOL_ITEMS[@]}" -type f -exec sha256sum {} + 2>/dev/null | sha256sum | awk '{print $1}')
   if [[ -f "$TOOLS_PKG" && -f "$TOOLS_MANIFEST" && "$(cat "$TOOLS_MANIFEST")" == "$TOOLS_INPUT_HASH" ]]; then
      echo "  [skip] Host tools package already up to date."
   else
      tar -c -I 'xz -T0' -f "$TOOLS_PKG" "${TOOL_ITEMS[@]}"
      echo "$TOOLS_INPUT_HASH" > "$TOOLS_MANIFEST"
   fi
fi

# 2D. Ensure Predownloaded Archives, Sources, and Python 3.11 Wheels
PRE_DIR="predownloaded"
mkdir -p "$PRE_DIR"

# Already fetched, use predownloaded as source of truth
# DOWNLOADS_DIR="/home/leegao/Downloads"
# DOWNLOADED_ZIPS=(
#    "vulkan-wsi-layer-main.zip"
#    "mesa-demos-main.zip"
#    "dxvk-1.10.3.zip"
#    "libhybris-arlinux.zip"
#    "android_kernel_modules_nothing_mt6878-mt6878-Tetris-u.zip"
#    "Mesa-PanVK-Android-v26.3.1-795ca7ff-ludashi.zip"
#    "libadrenotools-master.zip"
#    "cmake.zip"
#    "panthor-linux6.1-main.zip"
#    "gpu.zip"
#    "panvk-kbase-android-main.zip"
#    "Vulkan-Samples-main(1).zip"
# )
#
# for z in "${DOWNLOADED_ZIPS[@]}"; do
#    if [[ ! -f "$PRE_DIR/$z" ]]; then
#       if [[ -f "./$z" ]]; then
#          mv -f "./$z" "$PRE_DIR/$z"
#       elif [[ -f "$DOWNLOADS_DIR/$z" ]]; then
#          echo "  [copy] Copying $z from $DOWNLOADS_DIR -> $PRE_DIR/..."
#          cp -f "$DOWNLOADS_DIR/$z" "$PRE_DIR/$z"
#       fi
#    fi
# done

# if [[ ! -f "$PRE_DIR/bcn_layer-main.zip" ]]; then
#    if [[ -f "./bcn_layer-main.zip" ]]; then
#       mv -f "./bcn_layer-main.zip" "$PRE_DIR/bcn_layer-main.zip"
#    else
#       echo "  [fetch] Fetching bcn_layer source..."
#       if ! curl -sL https://github.com/leegao/bcn_layer/archive/refs/heads/main.zip -o "$PRE_DIR/bcn_layer-main.zip"; then
#          [[ -d "/home/leegao/meh/bcn_layer" ]] && git -C /home/leegao/meh/bcn_layer archive -o "$PWD/$PRE_DIR/bcn_layer-main.zip" HEAD
#       fi
#    fi
# fi
#
# if [[ ! -f "$PRE_DIR/compat_layer-main.zip" ]]; then
#    if [[ -f "./compat_layer-main.zip" ]]; then
#       mv -f "./compat_layer-main.zip" "$PRE_DIR/compat_layer-main.zip"
#    else
#       echo "  [fetch] Fetching compat_layer source..."
#       if ! curl -sL https://github.com/leegao/compat_layer/archive/refs/heads/main.zip -o "$PRE_DIR/compat_layer-main.zip"; then
#          [[ -d "/home/leegao/meh/compat_layer" ]] && git -C /home/leegao/meh/compat_layer archive -o "$PWD/$PRE_DIR/compat_layer-main.zip" HEAD
#       fi
#    fi
# fi

# for extra_zip in wrapper-src.zip llvm-include.zip; do
#    if [[ ! -f "$PRE_DIR/$extra_zip" && -f "./$extra_zip" ]]; then
#       mv -f "./$extra_zip" "$PRE_DIR/$extra_zip"
#    fi
# done

ensure_python_wheels "$PRE_DIR/wheels"

# ==============================================================================
# 3. Assemble Unified Offline Sandbox Bundle: panthor2kbase-offline-sandbox-bundle.zip
#    Contains:
#    - ndk-minimal-aarch64-r27d.tar.xz
#    - mesa-panvk-offline-bundle.tar.xz
#    - host-tools-and-vulkan-bins-x86_64.tar.xz
#    - Proprietary vendor binaries: libGLES_mali.*, mali_kbase*
#    - predownloaded/ (16 zip archives + wheels/)
#    - SANDBOX_BUNDLE.md (inventory & usage guide)
#    - manifest.sha256 (per-file SHA-256 integrity checksums)
# ==============================================================================
echo "[3/4] Packing Unified Offline Sandbox Bundle: panthor2kbase-offline-sandbox-bundle.zip..."

LOCAL_REF_BINS=(
   "libGLES_mali.r44p1.so"
   "libGLES_mali.so"
   "mali_kbase_mt6878_r44.ko"
   "mali_kbase.ko"
)

UNIFIED_TARGET="$OUT_DIR/panthor2kbase-offline-sandbox-bundle.zip"
UNIFIED_MANIFEST="$OUT_DIR/panthor2kbase-offline-sandbox-bundle.manifest"
UNIFIED_HASH_FILE="$OUT_DIR/panthor2kbase-offline-sandbox-bundle.manifest.hash"

# Compute hash of all bundle inputs
UNIFIED_INPUTS_HASH=$( (
   [[ -f "$NDK_PKG" ]] && sha256sum "$NDK_PKG"
   [[ -f "$MESA_PKG" ]] && sha256sum "$MESA_PKG"
   # [[ -f "$TOOLS_PKG" ]] && sha256sum "$TOOLS_PKG"
   for f in "${LOCAL_REF_BINS[@]}"; do
      [[ -f "$f" ]] && sha256sum "$f"
   done
   [[ -f "SANDBOX_BUNDLE.md" ]] && sha256sum "SANDBOX_BUNDLE.md"
   [[ -d "$PRE_DIR" ]] && find "$PRE_DIR" -type f -exec sha256sum {} + 2>/dev/null | sort -k2
) | sha256sum | awk '{print $1}' )

if [[ -f "$UNIFIED_TARGET" && -f "$UNIFIED_HASH_FILE" && "$(cat "$UNIFIED_HASH_FILE")" == "$UNIFIED_INPUTS_HASH" ]]; then
   echo "  [skip] Unified offline bundle already up to date (all component checksums match)."
else
   echo "  [stage] Staging all components into unified bundle..."
   STAGE_DIR="$OUT_DIR/.unified_bundle_staging"
   rm -rf "$STAGE_DIR"
   mkdir -p "$STAGE_DIR"

   # Link components
   # [[ -f "$NDK_PKG" ]] && ln -f "$NDK_PKG" "$STAGE_DIR/"
   [[ -f "$MESA_PKG" ]] && ln -f "$MESA_PKG" "$STAGE_DIR/"
   # [[ -f "$TOOLS_PKG" ]] && ln -f "$TOOLS_PKG" "$STAGE_DIR/"
   [[ -d "$ROOT_DIR/kernel" ]] && cp -rf "$ROOT_DIR/kernel" "$STAGE_DIR/"
   for f in "${LOCAL_REF_BINS[@]}"; do
      [[ -f "$f" ]] && ln -f "$f" "$STAGE_DIR/"
   done
   [[ -f "SANDBOX_BUNDLE.md" ]] && cp -f "SANDBOX_BUNDLE.md" "$STAGE_DIR/"

   # Link predownloaded directory (preserving wheels/ subfolder)
   cp -al "$PRE_DIR" "$STAGE_DIR/" 2>/dev/null || cp -rf "$PRE_DIR" "$STAGE_DIR/"

   # Generate full per-file SHA-256 manifest inside the bundle
   echo "  [manifest] Generating SHA-256 manifest..."
   (
      cd "$STAGE_DIR"
      find . -type f ! -name "manifest.sha256" -exec sha256sum {} + | sort -k2 > manifest.sha256
   )
   cp -f "$STAGE_DIR/manifest.sha256" "$UNIFIED_MANIFEST"
   echo "$UNIFIED_INPUTS_HASH" > "$UNIFIED_HASH_FILE"

   # Create unified bundle zip (excluding source checkpoint)
   echo "  [pack] Zipping unified bundle -> $UNIFIED_TARGET..."
   (
      cd "$STAGE_DIR"
      zip -1 -q -r "$OLDPWD/$UNIFIED_TARGET" . -x "*panthor-kbase-source-checkpoint.zip*"
   )
   rm -rf "$STAGE_DIR"

   # Clean old chunks so fresh split is triggered
   rm -f "${UNIFIED_TARGET}_part_"*
fi
ls -lh "$UNIFIED_TARGET"

# Clean up any legacy bundles & parts from earlier layouts
rm -f "$OUT_DIR"/winlator* "$OUT_DIR"/device-reference* "$OUT_DIR"/*ndk-minimal*part* "$OUT_DIR"/*mesa-panvk*part* 2>/dev/null || true

# ==============================================================================
# 4. Split ONLY the Unified Bundle into 100 MiB Chunks
# ==============================================================================
if [[ "$SPLIT_100M" -eq 1 ]]; then
   echo "[4/4] Checking split chunk limits for unified bundle (<= 95 MiB chunks)..."
   sz=$(stat -c%s "$UNIFIED_TARGET" 2>/dev/null || echo 0)
   PART_PREFIX="${UNIFIED_TARGET}_part_"
   FIRST_PART="${PART_PREFIX}aa"
   if [[ "$sz" -gt 99614720 ]]; then
      if [[ -f "$FIRST_PART" && "$FIRST_PART" -nt "$UNIFIED_TARGET" ]]; then
         echo "  [split] Chunks for $(basename "$UNIFIED_TARGET") already up to date, skipping."
      else
         echo "  [split] Splitting $(basename "$UNIFIED_TARGET") ($sz bytes) into 95M parts..."
         rm -f "${PART_PREFIX}"*
         split -b 100M "$UNIFIED_TARGET" "$PART_PREFIX"
      fi
   fi
fi

echo "=============================================================================="
echo " Bundle Packing Complete! Contents of $OUT_DIR:"
ls -lh "$OUT_DIR"
echo "=============================================================================="
