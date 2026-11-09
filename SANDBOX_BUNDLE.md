# panthor2kbase Offline Sandbox Bundle Inventory & Guide

This bundle contains all prebuilt toolchains, reference source repositories, proprietary vendor driver binaries, kernel modules, and Python wheels required to build, cross-compile, lift, emulate, and verify the `panthor2kbase` Mali CSF UAPI adapter and Mesa PanVK offline in an air-gapped Gemini Task Agent sandbox VM without internet access.

---

## 1. Quick Start: Reassembling & Extracting in Sandbox

All split parts (≤ 95 MiB each) must be concatenated to recover the unified bundle:

```bash
# 1. Reassemble unified bundle archive
cat panthor2kbase-offline-sandbox-bundle.zip_part_* > panthor2kbase-offline-sandbox-bundle.zip

# 2. Verify SHA-256 integrity
sha256sum -c manifest.sha256 || true

# 3. Extract bundle
unzip panthor2kbase-offline-sandbox-bundle.zip -d ./offline_bundle

# 4. Extract Android NDK (if building ARM64 AArch64 targets)
tar -xJf offline_bundle/ndk-minimal-aarch64-r27d.tar.xz -C /tmp/
export ANDROID_NDK_HOME="/tmp/android-ndk-r27d"
export PATH="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"

# 5. Extract Mesa PanVK source
tar -xJf offline_bundle/mesa-panvk-offline-bundle.tar.xz -C ./

# 6. Install Python 3.11 wheels offline
pip install --no-index --find-links=offline_bundle/predownloaded/wheels \
   ortools capstone z3-solver pyelftools mako meson
```

---

## 2. File Inventory & Usage

### A. Core Build Toolchains & Compilers

| File | Purpose / Role |
| :--- | :--- |
| `ndk-minimal-aarch64-r27d.tar.xz` | Minimal Android NDK r27d toolchain (`aarch64-linux-android30`/`35` clang, clang++, lld, llvm-ar, and sysroot). Used to compile `libdrm.a`, `libpanthor_kbase_adapter.a`, and Mesa PanVK (`vulkan.panfrost.so`). |
| `mesa-panvk-offline-bundle.tar.xz` | Clean offline Mesa source tree (`5a07217f034` with logging hooks staged) alongside prebuilt host shader compilers (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile`). Enables full PanVK CSF compilation without online code generators. |
| `host-tools-and-vulkan-bins-x86_64.tar.xz` | Host Linux x86_64 verification tools (`vkcube`, `vkcube_khronos`), build scripts, and test harnesses. |

### B. Proprietary Vendor Binaries & Kernel Modules (MT6878 / Dimensity 7050)

| File | Purpose / Role |
| :--- | :--- |
| `libGLES_mali.so` | Vendor proprietary ARM Mali user-space driver (DDK r54p3). Used for binary reverse-engineering, ioctl trace equivalence verification, and shader comparison. |
| `libGLES_mali.r44p1.so` | Vendor proprietary ARM Mali user-space driver (DDK r44p1). Used as reference for Mali CSF UAPI structure layouts on MT6878. |
| `mali_kbase.ko` | MediaTek MT6878 stock Mali kbase kernel module. Used to verify kernel ioctl numbers, CSF trace groups, and ringbuffer interfaces. |
| `mali_kbase_mt6878_r44.ko` | DDK r44p1 build of `mali_kbase.ko` for ABI and ioctl behavior cross-referencing. |

### C. Reference Sources, Layers & Shims (`predownloaded/`)

| File | Purpose / Role |
| :--- | :--- |
| `predownloaded/bcn_layer-main.zip` | BCN software compute texture decompression Vulkan layer source (`VK_EXT_custom_border_color`, BC1-BC7 software compute decoder). |
| `predownloaded/compat_layer-main.zip` | DXVK / D3D11 Mali compatibility Vulkan layer source (handles extension aliasing, relaxed descriptor limits, and workaround injection). |
| `predownloaded/wrapper-src.zip` | Android Vulkan wrapper source (`src/vulkan/wrapper/` entrypoints, hook stubs, bcdec). |
| `predownloaded/llvm-include.zip` | LLVM C/C++ API header tree required for AOT lifting passes. |
| `predownloaded/vulkan-wsi-layer-main.zip` | Headless WSI layer for Android Vulkan swapchains. |
| `predownloaded/panvk-kbase-android-main.zip` | a "reference" direct patched panvk-on-kbase for android implementation (does not work for g615) |
| `predownloaded/panthor-linux6.1-main.zip` | Upstream Linux 6.1 DRM Panthor kernel driver source reference. |
| `predownloaded/android_kernel_modules_nothing_mt6878-mt6878-Tetris-u.zip` | Device kernel module sources for Nothing Phone 2a (MT6878 Tetris-u). |
| `predownloaded/Mesa-PanVK-Android-v26.3.1-795ca7ff-ludashi.zip` | Prebuilt reference PanVK Android binary driver (Ludashi test build). |
| `predownloaded/dxvk-1.10.3.zip` | DXVK 1.10.3 source code for Direct3D 9/10/11 on Vulkan. |
| `predownloaded/Vulkan-Samples-main.zip` | Khronos official Vulkan mobile performance samples. |
| `predownloaded/libadrenotools-master.zip` | Adrenotools custom driver loading / hooking library. |
| `predownloaded/libhybris-arlinux.zip` | Libhybris Android-to-Linux Bionic translation shims. |
| `predownloaded/cmake.zip` | CMake build scripts and modules. |
| `predownloaded/pixel_gpu_src.zip` | Mali GPU hardware src for a generic Pixel Mali device |
| `predownloaded/husky_gpu_src.zip` | Mali GPU hardware src for Pixel 8 Pro |
| `predownloaded/mali_kbase_husky.ko` | Pixel 8 pro kernel driver for kbase |
| `predownloaded/mali_pixel_husky.ko` | Pixel 8 pro kernel driver for kbase (Pixel mods) |

### D. Python 3.11 Wheels (`predownloaded/wheels/`)

| Wheel Package | Purpose / Role |
| :--- | :--- |
| `ortools` | Google Operations Research tools for AOT shader instruction scheduling and register allocation constraint solving. |
| `capstone` | Multi-architecture disassembly engine (Valhall / CSF instruction decoding). |
| `z3-solver` | SMT solver for equivalence verification between Mali vendor bytecode and PanVK NIR. |
| `pyelftools` | Pure Python ELF parsing library for inspecting symbol tables and relocations. |
| `mako` | Python templating engine required by Mesa build system (Mesa XML/Python code generators). |
| `meson` | Meson build system for compiling Mesa and test binaries offline. |
| `numpy`, `pandas`, `protobuf`, `markupsafe`, `absl-py`, `immutabledict`, etc. | Required runtime dependencies for ortools, mako, and verification scripts. |

---

## 3. Cryptographic Verification

A complete per-file SHA-256 hash manifest is provided in `manifest.sha256` inside the bundle root. Run:

```bash
sha256sum -c manifest.sha256
```
to verify that every archive, binary, and wheel arrived intact.
