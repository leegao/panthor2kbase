# Building and Testing `mali_aot_translator`

## System Prerequisites

- Linux `x86_64`
- GCC >= 9.0 or Clang >= 10.0 (`C++17`)
- GNU Make or CMake >= 3.16
- Python >= 3.8
- LLVM 15 (`libLLVM-15.so.1` for compiling `bin/mali_llvm_pass_driver` from `src/mali_llvm_passes.cpp`)
- Khronos Vulkan Loader (`libvulkan.so.1`) and Mesa Lavapipe (`libvulkan_lvp.so` from `mesa-vulkan-drivers`) or hardware Vulkan ICD (`libvulkan_radeon.so` / `libvulkan_intel.so`)

## 1. Building with GNU Make

```bash
make clean
make -j$(nproc)
```

This builds:
- `mali0_emu/lib/libmali0_emu.a`, `mali0_emu/lib/libmali0_emu.so`, and `mali0_emu/bin/test_mali0_vkcube_and_complex`
- `r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.so`, `r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.a`, and `r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json`
- `bin/mali_llvm_pass_driver` (6-pass C++17 AArch64-to-x86-64 LLVM lifter)
- `bin/mali_aot_runner`
- `bin/vkcube` (Khronos WSI 3D `vkcube` client running over `VK_KHR_surface` + `VK_EXT_headless_surface` + `VK_KHR_swapchain`)
- `bin/test_vulkan_icd_client` and `bin/test_vulkan_samples_compliance`

## 2. Building with CMake

```bash
mkdir -p cmake_build && cd cmake_build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
ctest --output-on-failure
```

## 3. Translating an ARM64 `libGLES_mali.so` Driver & Running `vkcube`

```bash
# Translate libGLES_mali.so -> build/<driver_tag>/mali_mbs2_aot.o & libvulkan_mali_aot_icd.so
./translate.sh /path/to/libGLES_mali.so

# Run the full 4-stage ICD + WSI vkcube + 10-suite Khronos Vulkan-Samples harness
# Second argument selects host rendering backend: "llvmpipe" (default) or "host" (probes RADV/ANV first)
./icd_harness.sh /path/to/libGLES_mali.so llvmpipe
./icd_harness.sh /path/to/libGLES_mali.so host

# Run WSI vkcube directly and optionally dump rendered swapchain frames to a binary PPM image
MALI_VKCUBE_DUMP_PPM=vkcube_frame.ppm ./bin/vkcube build/r54p3-00eac0-6eda07/mali_aot_icd.x86_64.json
```

## 4. Running the Complete 19-Test Automated Verification Suite

```bash
python3 tests/run_unit_tests.py
```
