# panthor2kbase Tools, Scripts, Runnables & Libraries Reference

This document provides a comprehensive reference for all scripts, utilities, verification harnesses, and compiled libraries in the `panthor2kbase` project.

---

## Table of Contents

1. [Architecture & Unified Build Directory Layout](#1-architecture--unified-build-directory-layout)
2. [Compiled Libraries & Shared Objects (.so / .a)](#2-compiled-libraries--shared-objects-so--a)
   - [libpanthor_kbase_adapter.so / .a](#libpanthor_kbase_adapterso--a)
   - [libmali_kbase_emu.so / .a](#libmali_kbase_emuso--a)
   - [libmali_uapi_tracer.so](#libmali_uapi_tracerso)
   - [libvulkan_panfrost.so & libvulkan_panfrost.unstripped.so](#libvulkan_panfrostso--libvulkan_panfrostunstrippedso)
   - [libvulkan_panfrost_patched.so](#libvulkan_panfrost_patchedso)
   - [libvulkan_mali_aot_icd.so](#libvulkan_mali_aot_icdso)
   - [libVkLayer_BCN_bcn_layer.so](#libvklayer_bcn_bcn_layerso)
   - [libVkLayer_COMPAT_dxvk_mali_compat.so](#libvklayer_compat_dxvk_mali_compatso)
   - [libVkLayer_PANTHOR_capture.so](#libvklayer_panthor_captureso)
   - [libdrm.a](#libdrma)
3. [Core Build & Compilation Scripts](#3-core-build--compilation-scripts)
   - [build_all.sh](#build_allsh)
   - [compile.sh](#compilesh)
   - [setup.sh](#setupsh)
   - [scripts/apply_mesa_overlay.sh](#scriptsapply_mesa_overlaysh)
   - [scripts/find_cmake.sh](#scriptsfind_cmakesh)
   - [src/aot/translate.sh](#srcaottranslatesh)
   - [src/aot/layers/build_layers.sh](#srcaotlayersbuild_layerssh)
   - [src/aot/r54p3_vulkan_icd/build.sh](#srcaotr54p3_vulkan_icdbuildsh)
4. [Deployment & Android Device Push Scripts](#4-deployment--android-device-push-scripts)
   - [push.sh](#pushsh)
   - [push_zenith.sh](#push_zenithsh)
   - [scripts/trace_android_winlator.sh](#scriptstrace_android_winlatorsh)
5. [Verification Suites & Test Runnables](#5-verification-suites--test-runnables)
   - [run_all_tests.sh](#run_all_testssh)
   - [tests/unit_tests_edge_cases](#testsunit_tests_edge_cases)
   - [tests/run_vulkan_samples_and_equivalence_suite](#testsrun_vulkan_samples_and_equivalence_suite)
   - [tests/panprobe/panprobe_runner_suite](#testspanprobepanprobe_runner_suite)
   - [tests/test_ludashi_panvk_aot_and_host](#teststest_ludashi_panvk_aot_and_host)
   - [tests/test_wayland_and_vkgears_e2e](#teststest_wayland_and_vkgears_e2e)
   - [tests/test_dxvk_aot_layers_e2e](#teststest_dxvk_aot_layers_e2e)
   - [scripts/run_dxvk_aot.sh](#scriptsrun_dxvk_aotsh)
   - [scripts/run_conformance.sh](#scriptsrun_conformancesh)
   - [scripts/run_coverage.sh](#scriptsrun_coveragesh)
6. [Formal Mathematical Verifiers & Offline Analysis Tools](#6-formal-mathematical-verifiers--offline-analysis-tools)
   - [src/verifier/ortools_uapi_verifier.py](#srcverifierortools_uapi_verifierpy)
   - [src/verifier/aarch64_mc_lifter.py](#srcverifieraarch64_mc_lifterpy)
   - [scripts/uapi_trace_and_diff.py](#scriptsuapi_trace_and_diffpy)
   - [scripts/analyze_csf_stall_log.py](#scriptsanalyze_csf_stall_logpy)
   - [scripts/decode_csf_breadcrumbs_and_dump.py](#scriptsdecode_csf_breadcrumbs_and_dumppy)
   - [scripts/pack_sandbox_bundles.sh](#scriptspack_sandbox_bundlessh)

---

## 1. Architecture & Unified Build Directory Layout

All build outputs are organized into dedicated architecture directories to prevent leaking binaries into the source tree or repository root:

```
panthor2kbase/
├── build-x86-64/                           # Host x86-64 compilation target
│   ├── mesa/                               # Mesa build tree (symlinked as build-host)
│   ├── adapter/                            # Adapter object files
│   ├── emulator/                           # Mali kbase emulator object files
│   ├── aot/                                # AOT compiler, lifted objects, replayers
│   ├── tests/                              # Built test executables
│   ├── scratch/                            # Cache & intermediate artifacts
│   ├── libpanthor_kbase_adapter.so / .a    # Common host native libraries
│   ├── libmali_kbase_emu.so / .a
│   ├── libvulkan_mali_aot_icd.so
│   ├── libvulkan_panfrost_patched.so
│   └── libdrm.a
│
├── build-android/                          # Target Android AArch64 (API 30/35)
│   ├── mesa/                               # Android Mesa PanVK build tree
│   ├── adapter/                            # Cross-compiled adapter object files
│   ├── emulator/                           # Cross-compiled emulator object files
│   ├── aot/                                # Android AOT translation objects
│   ├── tests/                              # Android unit tests
│   ├── scratch/                            # Scratch cache for compile.sh & push.sh
│   ├── libvulkan_panfrost.so               # Stripped production Vulkan driver
│   ├── libvulkan_panfrost.unstripped.so    # Unstripped driver with full DWARF symbols
│   ├── libpanthor_kbase_adapter.so / .a
│   ├── libmali_kbase_emu.so / .a
│   └── libdrm.a
│
└── tests/ & src/aot/bin/                   # Lightweight forwarding wrapper scripts
```

The repository root is strictly clean of generated `.so` or `.a` binaries (only vendor input blobs such as `libGLES_mali*.so` reside at root). Executables in `tests/` and `src/aot/bin/` are lightweight POSIX shell wrappers that set `LD_LIBRARY_PATH` and forward execution into `build-x86-64/`.

---

## 2. Compiled Libraries & Shared Objects (.so / .a)

### `libpanthor_kbase_adapter.so` / `.a`

#### 1. What it does
Implements the core translation layer between the upstream Linux Panthor DRM UAPI (`DRM_IOCTL_PANTHOR_*`) and the ARM Mali CSF `kbase` kernel driver UAPI (`/dev/mali0`). It intercepts DRM ioctls issued by Mesa PanVK and translates them into corresponding `KBASE_IOCTL_*` commands, managing virtual memory (VM), buffer object (BO) allocations, user-space Command Stream Frontend (CSF) ringbuffers, sync objects, and tiler heaps.

#### 2. Why it exists
Upstream Mesa PanVK expects the Linux kernel Panthor driver (`/dev/dri/cardX` / `renderD128`). However, Android devices (such as MediaTek MT6878 / Dimensity 7050) ship proprietary kernel trees running `mali_kbase.ko` (`/dev/mali0`). This adapter allows unmodified upstream PanVK to drive Mali CSF GPUs on kbase kernels.

#### 3. How to run / use it
- **Host (shared):** Preloaded or linked via `-lpanthor_kbase_adapter`.
- **Host (static):** Embedded directly via `-Wl,--whole-archive libpanthor_kbase_adapter.a -Wl,--no-whole-archive`.
- **Target Android:** Integrated into `libvulkan_panfrost.so` or loaded via `LD_PRELOAD`:
  ```bash
  export LD_PRELOAD=/data/local/tmp/libpanthor_kbase_adapter.so
  ```

#### 4. Output & Interpretation
When `PANVK_ADAPTER_LOG_LEVEL=INFO` (or `TRACE`), logs are emitted to stderr or the configured log file:
```
[628681.701163] INFO : parse_kbase_gpuprops: raw_gpu_id=0xb8031030 -> panthor_gpu_id=0xb8031030 (arch=11) core_features=0x4 shader_present=0x3 mmu_features=0x2830
[628681.701715] INFO : ioctl intercepted: req=DRM_IOCTL_PANTHOR_VM_CREATE(0xc0106441) fd=3 nr=65 type=d
[628681.701722] INFO : DRM_PANTHOR_VM_CREATE -> vm_id=1 user_va_range=0x800000000000
[628681.701746] INFO : DRM_PANTHOR_BO_CREATE size=2097152 -> handle=1 va=0x791c98500000 cpu=0x791c98500000 flags=0x280f
```
**Interpretation:**
- `arch=11`: Hardware identified as Valhall v11 (Mali-G615).
- `vm_id=1`: Panthor VM successfully created and mapped to a kbase context.
- `flags=0x280f`: Panthor BO flags translated to kbase `BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_GROW_ON_GPF`.

---

### `libmali_kbase_emu.so` / `.a`

#### 1. What it does
Provides a complete user-space software emulation of `/dev/mali0` and `mali_kbase.ko`. It handles `open`, `close`, `mmap`, and all Mali CSF `ioctl` calls in memory, simulating GPU memory allocation, page tables, ringbuffer slots, and hardware property registers.

#### 2. Why it exists
Enables running and verifying the entire Vulkan graphics driver stack, DXVK, and test suites on standard host Linux machines (x86-64) without requiring an actual Mali GPU or root privileges.

#### 3. How to run / use it
- In tests, link against `build-x86-64/libmali_kbase_emu.so` or static archive `libmali_kbase_emu.a`.
- Statically embedded in `libvulkan_mali_aot_icd.so` so the ICD has no external kernel dependencies on host.

#### 4. Output & Interpretation
Tracing statistics via `mali_kbase_emu_dump_stats()`:
```
=== Mali Kbase Emulator Statistics ===
  Total ioctls handled : 142
  KBASE_IOCTL_MEM_ALLOC : 38 (Allocated: 64.5 MiB)
  KBASE_IOCTL_CS_QUEUE_REGISTER : 2
  KBASE_IOCTL_CS_QUEUE_KICK : 12
  Active Contexts : 1
======================================
```
**Interpretation:** Confirms that command stream queues were successfully registered and kicked without hardware faults.

---

### `libmali_uapi_tracer.so`

#### 1. What it does
An `LD_PRELOAD` tracing library that intercepts libc calls (`open`, `ioctl`, `mmap`, `munmap`, `close`) targeting `/dev/mali0`. It decodes the binary parameters of every Mali UAPI call and logs structured records. It also inspects CSF ringbuffer pages to decode Valhall instruction streams.

#### 2. Why it exists
Used on real Android devices (or under Winlator / Ludashi benchmark) to capture gold-standard kernel interaction traces from proprietary vendor drivers (`libGLES_mali.so`), allowing exact bug-for-bug equivalence verification.

#### 3. How to run / use it
On Android via Winlator container environment variables:
```bash
export LD_PRELOAD=/data/local/tmp/libmali_uapi_tracer.so
export MALI_UAPI_TRACE_OUT=/sdcard/Download/mali_uapi_trace.jsonl
```
Or automated via `scripts/trace_android_winlator.sh`.

#### 4. Output & Interpretation
Produces lines in JSON Lines (`.jsonl`) format:
```json
{"event": "ioctl", "fd": 4, "cmd": "KBASE_IOCTL_MEM_ALLOC", "nr": 16, "va": "0x7a0537000000", "size_pages": 4096, "flags": "0x2a0f"}
{"event": "csf_ring_kick", "queue": 0, "cs_head": 128, "cs_tail": 256, "insns": ["MOVE48 r66, 0x0", "FLUSH_CACHE2 clean_and_invalidate"]}
```
**Interpretation:** Shows the exact virtual address, page count, and ringbuffer instructions submitted to the kernel.

---

### `libvulkan_panfrost.so` & `libvulkan_panfrost.unstripped.so`

#### 1. What it does
The production Mesa PanVK Vulkan driver compiled for Android AArch64.
- `libvulkan_panfrost.so`: Stripped binary (approx. 18 MiB) ready for device deployment.
- `libvulkan_panfrost.unstripped.so`: Unstripped binary (approx. 130 MiB) containing full DWARF debug symbols.

#### 2. Why it exists
This is the primary deliverable of the project: a working open-source Vulkan driver for MediaTek Dimensity 7050 / MT6878 Mali-G615 MC2 on Android.

#### 3. How to run / use it
Pushed to the Android device via `bash push.sh`. On the device, it is placed in the Vulkan driver search path (e.g., inside Ludashi benchmark or Winlator imagefs `/usr/lib/libvulkan_panfrost.so`).

#### 4. Output & Interpretation
Check device file properties:
```bash
adb shell "ls -lh /data/data/com.ludashi.benchmark/files/imagefs/usr/lib/libvulkan_panfrost.so"
```
Output:
```
-rwxr-xr-x 1 u0_a123 u0_a123 18M 2026-11-09 20:49 libvulkan_panfrost.so
```

---

### `libvulkan_panfrost_patched.so`

#### 1. What it does
Host x86-64 build of Mesa PanVK patched with the Panthor-to-Kbase adapter.

#### 2. Why it exists
Enables executing PanVK directly on the host development machine on top of `libmali_kbase_emu.so` for automated CI regression testing without physical hardware.

#### 3. How to run / use it
Invoked via test suites or directly with `VK_ICD_FILENAMES`:
```bash
VK_ICD_FILENAMES=build-x86-64/panvk_icd.x86_64.json ./build-x86-64/tests/test_ludashi_panvk_aot_and_host
```

---

### `libvulkan_mali_aot_icd.so`

#### 1. What it does
A standalone x86-64 Vulkan ICD generated by machine-translating the vendor ARM64 `libGLES_mali.so` to x86-64 LLVM IR and linking it statically with `libmali_kbase_emu.a`.

#### 2. Why it exists
Provides a bit-exact vendor reference implementation on x86-64 to cross-check PanVK behavior against ARM's proprietary driver.

#### 3. How to run / use it
```bash
VK_ICD_FILENAMES=build-x86-64/mali_aot_icd.x86_64.json vkcube
```

---

### `libVkLayer_BCN_bcn_layer.so`

#### 1. What it does
A Vulkan layer that intercepts BC1-BC7 (DXT/BCn) compressed texture formats not natively supported on mobile GPUs and automatically decompresses them via compute shaders into RGBA8 formats.

#### 2. Why it exists
Desktop Windows Direct3D games running under DXVK require BCn texture compression formats. Mobile Mali hardware lacks native BCn decoders. This layer transparently adds BCn capability.

#### 3. How to run / use it
```bash
export VK_LAYER_PATH=src/aot/layers
export VK_INSTANCE_LAYERS=VK_LAYER_BCN_BCnLayer
```

---

### `libVkLayer_COMPAT_dxvk_mali_compat.so`

#### 1. What it does
A Vulkan layer providing workarounds for DXVK on Mali: adjusts physical device limits, relaxes uniform buffer alignment restrictions, and handles shader demote workarounds.

#### 2. Why it exists
Resolves specific edge cases and asserts in DXVK 1.10.3 when initializing Direct3D 11 devices on Mali hardware.

#### 3. How to run / use it
```bash
export VK_LAYER_PATH=src/aot/layers
export VK_INSTANCE_LAYERS=VK_LAYER_COMPAT_DxvkMaliCompatLayer
```

---

### `libVkLayer_PANTHOR_capture.so`

#### 1. What it does
A Vulkan validation/capture layer that logs pipeline state, descriptor bindings, and render passes for reproducible test case generation.

#### 2. Why it exists
Captures standalone C++ reproducing testcases from complex Windows games.

---

### `libdrm.a`

#### 1. What it does
Minimal static archive providing libdrm symbols (`drmIoctl`, `drmGetVersion`, `drmCommandWriteRead`, etc.) backed by the Panthor adapter.

#### 2. Why it exists
Embedded directly into Mesa and test executables so no external system libdrm package is required.

---

## 3. Core Build & Compilation Scripts

### `build_all.sh`

#### 1. What it does
The master build orchestrator. Configures and builds:
1. Host x86-64 libraries, emulator, adapter, and test harnesses in `build-x86-64/`.
2. Cross-compiles Android AArch64 adapter and tools in `build-android/` (if `ANDROID_NDK_HOME` is set).
3. Invokes `compile.sh` to compile the Android Mesa PanVK driver.

#### 2. Why it exists
Provides a one-command build for the entire project across both host and cross-compilation targets.

#### 3. How to run it
```bash
bash build_all.sh
```

#### 4. Output & Interpretation
```
================================================================================
 [1/3] Building Host x86-64 Adapter, Emulator & Tooling (build-x86-64)
================================================================================
-- Configuring Panthor-to-Kbase C++ Adapter Suite for Host (x86_64)
[100%] Built target unit_tests_edge_cases
================================================================================
 [2/3] Cross-Compiling Android AArch64 Adapter & Tools (build-android)
================================================================================
[100%] Built target libpanthor_kbase_adapter
================================================================================
 [3/3] Compiling Mesa PanVK Android Driver (compile.sh)
================================================================================
Stripped Android driver -> /home/.../build-android/libvulkan_panfrost.so
```
**Interpretation:** Verifies clean build across all modules with both host and Android binaries placed in their respective `build-*/` directories.

---

### `compile.sh`

#### 1. What it does
Applies the `mesa_overlay/` onto `mesa/` and compiles the Android AArch64 Mesa PanVK driver using Meson and Ninja. Strips the output and saves both unstripped and stripped variants into `build-android/`.

#### 2. Why it exists
Encapsulates all NDK cross-compilation compiler flags, include paths, and meson subproject configuration for building PanVK.

#### 3. How to run it
```bash
bash compile.sh
```

#### 4. Output & Interpretation
```
  [overlay] Applied mesa_overlay/ over mesa/
  [compile] ninja: no work to do.
  [archive] Unstripped: build-android/libvulkan_panfrost.unstripped.so (131M)
  [strip]   Stripped:   build-android/libvulkan_panfrost.so (18M)
```

---

### `setup.sh`

#### 1. What it does
Initializes the Meson build configurations in `build-x86-64/mesa` and `build-android/mesa`, applies the overlay, and sets up cross-compilation toolchains.

#### 2. Why it exists
Runs once during repository checkout or when reconfiguring build options.

#### 3. How to run it
```bash
bash setup.sh
```

---

### `scripts/apply_mesa_overlay.sh`

#### 1. What it does
Single authoritative script that synchronizes files from `mesa_overlay/` into `mesa/`.

#### 2. Why it exists
`mesa/` is a transient upstream source tree. All adapter modifications and driver patches live cleanly in `mesa_overlay/`. This script applies them idempotently without duplication.

#### 3. How to run it
```bash
bash scripts/apply_mesa_overlay.sh
```

---

### `scripts/find_cmake.sh`

#### 1. What it does
Locates an available `cmake` executable: searches system `PATH`, `predownloaded/cmake`, or local sandbox paths.

#### 2. Why it exists
Ensures build scripts work seamlessly inside air-gapped container sandboxes (e.g. gVisor or Gemini Task Agent environments) where CMake may not be in standard `/usr/bin/`.

#### 3. How to run it
```bash
CMAKE_BIN="$(scripts/find_cmake.sh)"
echo "Using CMake at: $CMAKE_BIN"
```

---

### `src/aot/translate.sh`

#### 1. What it does
Drives the 6-pass AOT lifter that lifts ARM64 machine code from `libGLES_mali.so` to x86-64 LLVM IR. Compiles the resulting IR and statically links it with `libmali_kbase_emu.a` to produce `libvulkan_mali_aot_icd.so` and `mali_aot_runner`.

#### 2. Why it exists
Produces the host x86-64 vendor reference ICD with zero dynamic dependencies on external kernel drivers.

#### 3. How to run it
```bash
bash src/aot/translate.sh
```

#### 4. Output & Interpretation
```
================================================================================
 [SUCCESS] 64-Bit C++ LLVM AOT Translation Complete!
   x86-64 Object  : build-x86-64/mali_mbs2_aot.o
   Runner Binary  : build-x86-64/mali_aot_runner
   Vulkan ICD     : build-x86-64/libvulkan_mali_aot_icd.so
================================================================================
```

---

### `src/aot/layers/build_layers.sh`

#### 1. What it does
Compiles the DXVK compatibility layer, BCn compute layer, and capture layers using CMake, and outputs the corresponding JSON manifests (`VkLayer_BCN_bcn_layer.json`, etc.).

#### 2. Why it exists
Builds the layer infrastructure required for DXVK on Mali.

#### 3. How to run it
```bash
bash src/aot/layers/build_layers.sh
```

---

### `src/aot/r54p3_vulkan_icd/build.sh`

#### 1. What it does
Compiles the decompiled r54p3 Vulkan ICD dispatch harness.

---

## 4. Deployment & Android Device Push Scripts

### `push.sh`

#### 1. What it does
Compiles the Android driver via `compile.sh`, computes the checksum of `build-android/libvulkan_panfrost.so`, compares it against `build-android/scratch/*.cksum`, and pushes only changed files to the Android device via ADB:
- Pushes to `/data/local/tmp/`.
- Copies into `com.ludashi.benchmark` container (`files/imagefs/usr/lib/libvulkan_panfrost.so`).

#### 2. Why it exists
Automates fast on-device iterative development and avoids unnecessary transfers of unchanged 18MB binaries.

#### 3. How to run it
```bash
bash push.sh           # Only pushes if binary changed
bash push.sh --force   # Unconditionally pushes
```

#### 4. Output & Interpretation
```
  [push] adb push build-android/libvulkan_panfrost.so -> /data/local/tmp/
  -rwxr-xr-x 1 u0_a123 u0_a123 18M files/imagefs/usr/lib/libvulkan_panfrost.so
```

---

### `push_zenith.sh`

#### 1. What it does
Extracts reference PanVK Android driver (`libvulkan_panfrost.so`) inline from `predownloaded/Mesa-PanVK-Android-v26.3.1-795ca7ff-ludashi.zip` (or `predownloaded/panvk-kbase-android-libvulkan_panfrost.so.zip`) into `build-android/scratch/` and pushes it to the device.

#### 2. Why it exists
Enables rapid A/B comparison testing against the baseline reference build on a physical device.

#### 3. How to run it
```bash
bash push_zenith.sh
bash push_zenith.sh --force
```

---

### `scripts/trace_android_winlator.sh`

#### 1. What it does
Deploys `libmali_uapi_tracer.so` to the Android device via ADB (using checksum caching), configures the remote output path (`/sdcard/Download/mali_uapi_trace.jsonl`), and explains how to pull and diff the trace against reference logs.

#### 2. Why it exists
Captures live `/dev/mali0` UAPI interaction from Winlator or Android games on a physical device.

#### 3. How to run it
```bash
bash scripts/trace_android_winlator.sh
bash scripts/trace_android_winlator.sh --force
```

#### 4. Output & Interpretation
```
================================================================================
 Android / Winlator /dev/mali0 UAPI + CSF Ring Tracer Utility
================================================================================
 1. Pushing tracer to device (/data/local/tmp/libmali_uapi_tracer.so):
  [push] adb push ... -> /data/local/tmp/

 2. In Winlator Container Environment Variables, add:
      MALI_UAPI_TRACE_OUT=/sdcard/Download/mali_uapi_trace.jsonl
      LD_PRELOAD=/data/local/tmp/libmali_uapi_tracer.so

 3. After running vkcube.exe in Winlator, pull and diff the trace:
      adb pull /sdcard/Download/mali_uapi_trace.jsonl artifacts/device_mali_uapi_trace.jsonl
      python3 scripts/uapi_trace_and_diff.py /path/to/winlator_log.txt
================================================================================
```

---

## 5. Verification Suites & Test Runnables

### `run_all_tests.sh`

#### 1. What it does
Executes the comprehensive 8-suite test pipeline:
- **Suite 1:** Edge-case unit tests & `mali_kbase.ko` flag invariants.
- **Suite 2:** Panprobe compliance suite (DXVK, Bachata S4, VKD3D tests).
- **Suite 3:** Ludashi PanVK cross-verification suite.
- **Suite 4:** Wayland WSI & `vkgears` E2E test.
- **Suite 5:** Khronos Vulkan-Samples 20-workload compliance harness.
- **Suite 6:** DXVK 1.10.3 D3D11 AOT layers E2E test.
- **Suite 7:** Google OR-Tools CP-SAT formal mathematical equivalence prover.
- **Suite 8:** AArch64 machine-code lifter & symbolic bitfield verifier.

#### 2. Why it exists
Ensures complete correctness and zero regression before commits or CI runs.

#### 3. How to run it
```bash
bash run_all_tests.sh
```

#### 4. Output & Interpretation
```
================================================================================
 [ALL SUITES PASSED] Every Test, Verifier, and Equivalence Artifact Succeeded! 
================================================================================
```

---

### `tests/unit_tests_edge_cases`

#### 1. What it does
Direct C unit test executable validating:
- Exact `BASE_MEM_*` bitfield constants against disassembled kernel modules.
- CSF bytecode normalizer and instruction alignment.
- Valhall v10/v11+ ring slot emission.
- /dev/mali0 state machine transitions.
- Grow-on-GPF on-demand backing, `KBASE_IOCTL_MEM_COMMIT`, and DXVK 16MiB stepdown.

#### 2. How to run it
```bash
./tests/unit_tests_edge_cases
```

#### 3. Output & Interpretation
```
  -> Exact BASE_MEM_* bit positions verified against mali_kbase.ko: PASS
  -> CSF Bytecode Normalizer & RAII indent invariant: PASS
  -> Ring Slot Emitter Valhall v10/v11+: PASS
  -> /dev/mali0 State Machine & mali_kbase.ko Flag Regression Tests: PASS
[Unit Tests] ALL EDGE CASES PASSED!
```

---

### `tests/run_vulkan_samples_and_equivalence_suite`

#### 1. What it does
Runs 20 real Vulkan workloads (textured quads, dynamic uniforms, mipmaps, MSAA, compute, depth-stencil) through host-patched PanVK and the kbase emulator.

#### 2. How to run it
```bash
./tests/run_vulkan_samples_and_equivalence_suite
```

---

### `tests/panprobe/panprobe_runner_suite`

#### 1. What it does
Runs the Panprobe verification matrix, checking requirements for DXVK (feature level 10/11), VKD3D-Proton (FL12_0), and Bachata S4 custom border colors.

#### 2. How to run it
```bash
./tests/panprobe/panprobe_runner_suite
```

---

### `tests/test_ludashi_panvk_aot_and_host`

#### 1. What it does
Disassembles and verifies Ludashi reference AArch64 machine instructions against the adapter's ring slot emitter, and runs the host PanVK driver on top of the kbase emulator.

#### 2. How to run it
```bash
./tests/test_ludashi_panvk_aot_and_host
```

---

### `tests/test_wayland_and_vkgears_e2e`

#### 1. What it does
Tests Vulkan loader direct linkage (`-lvulkan`), Wayland surface extension filtering (`MALI_AOT_DISABLE_WAYLAND_EXT`), surface formats (SRGB), and 3D gear drawing commands.

#### 2. How to run it
```bash
./tests/test_wayland_and_vkgears_e2e
```

---

### `tests/test_dxvk_aot_layers_e2e`

#### 1. What it does
Verifies DXVK 1.10.3 buffer initialization and submission routines running through `VK_LAYER_COMPAT_DxvkMaliCompatLayer` and `VK_LAYER_BCN_BCnLayer`.

#### 2. How to run it
```bash
./tests/test_dxvk_aot_layers_e2e
```

---

### `scripts/run_dxvk_aot.sh`

#### 1. What it does
Runs Windows Direct3D executables (e.g. `AIO-Graphics-Test.exe`, `vkcube.exe`) under DXVK 1.10.3 on top of the AOT-lifted Mali driver on host Linux.

#### 2. How to run it
```bash
bash scripts/run_dxvk_aot.sh --self-test
bash scripts/run_dxvk_aot.sh --run-exe /path/to/game.exe --cube
```

#### 3. Output & Interpretation
```
=== [Checkpoint v16 E2E Suite] Starting Verification ===
  [PASS] Test 1: MOVE32 r66, 0x0 upgraded to 64-bit MOVE48 r66, 0x0 before FLUSH_CACHE2
  [PASS] Test 2: All 39 vkgears/DXVK 1.10.3 device & physical-device entrypoints verified
  [PASS] Test 3: DXVK 1.10.3 + Compat Layer + BCn Transcode verified (pixel[0..15]=0xff0000ff)
=== [Checkpoint v16 E2E Suite] ALL TESTS PASSED 100% ===
```

---

### `scripts/run_conformance.sh`

#### 1. What it does
Runs the 5 standardized conformance workloads and generates `artifacts/conformance_report.json`.

#### 2. How to run it
```bash
bash scripts/run_conformance.sh
```

---

### `scripts/run_coverage.sh`

#### 1. What it does
Configures a dedicated coverage build in `build-coverage/` with GCC `--coverage`, executes all unit and equivalence test suites, and generates `coverage_output/coverage_report.txt` and HTML reports.

#### 2. How to run it
```bash
bash scripts/run_coverage.sh
```

---

## 6. Formal Mathematical Verifiers & Offline Analysis Tools

### `src/verifier/ortools_uapi_verifier.py`

#### 1. What it does
Formally proves safety and equivalence invariants using Google OR-Tools CP-SAT constraint satisfaction solver:
- **UAPI State Machine Safety:** Proves that with adapter guards enabled, invalid ioctl sequences cannot cause unhandled kernel faults (status: `INFEASIBLE`, i.e., no violation exists).
- **Negative Control:** Confirms that without adapter guards, the solver finds violating states (`OPTIMAL`).
- **CSF Instruction Stream Equivalence:** Proves all instruction upgrade paths preserve synchronization semantics.
- **Valhall ISA Equivalence:** Proves identical output across all inputs for lifted shader routines.
- **Memory Aliasing Safety:** Proves shareability fault `0x88` is unreachable.
- **Grow-On-GPF Safety:** Proves CPU `SIGBUS` is unreachable during page faults.

#### 2. How to run it
```bash
python3 src/verifier/ortools_uapi_verifier.py
```

#### 3. Output & Interpretation
```json
{
  "uapi_guarded_proof": {
    "suite": "UAPI_State_Machine_Preconditions",
    "adapter_guards_enabled": true,
    "solver_status": "INFEASIBLE",
    "proven_violation_free": true
  },
  "grow_on_gpf_and_cpu_mmap_commit_proof": {
    "solver_status": "INFEASIBLE",
    "proven_cpu_sigbus_unreachable": true,
    "machick2_saved_mib": 521.88
  }
}
```
**Interpretation:** `solver_status: "INFEASIBLE"` in CP-SAT indicates that the search space contains zero counterexamples—proving the system is mathematically violation-free.

---

### `src/verifier/aarch64_mc_lifter.py`

#### 1. What it does
Disassembles binary machine code directly from MediaTek `mali_kbase.ko` and verifies internal bitfield positions, flags, and ioctl signatures against hardware specifications. Replays historical Winlator traces and verifies that all recorded memory allocations match live behavior.

#### 2. How to run it
```bash
python3 src/verifier/aarch64_mc_lifter.py
```

#### 3. Output & Interpretation
```
  [3] Lifted kbase_check_alloc_sizes      : PC=0x07044, insns=37, grow_on_gpf_bit=9
  [4] Lifted kbase_mem_alloc              : PC=0x1359c, insns=16, coherency_conflict=0xc00
  [6] Replay Historical Winlator Log      : reproduced_exact_enomem_failures=True
  [9] CP-SAT Binary vs. Emulator Proof    : status=INFEASIBLE, proven_equivalent=True
Saved Pure-LLVM MC Lifter & Trace Verification Report -> artifacts/aarch64_mc_lifter_report.json
```

---

### `scripts/uapi_trace_and_diff.py`

#### 1. What it does
Compares live `/dev/mali0` UAPI traces captured on device against baseline traces (e.g. Ludashi benchmark or Winlator logs), highlighting discrepancies in ioctl arguments, memory flags, and ringbuffer commands.

#### 2. How to run it
```bash
python3 scripts/uapi_trace_and_diff.py winlator_log.txt
```

---

### `scripts/analyze_csf_stall_log.py`

#### 1. What it does
Analyzes GPU CSF ringbuffer hang dumps, parsing queue read/write pointers, ring slot opcodes, and scoreboard states to pinpoint the exact instruction causing a GPU timeout.

#### 2. How to run it
```bash
python3 scripts/analyze_csf_stall_log.py /path/to/dmesg_gpu_stall.log
```

---

### `scripts/decode_csf_breadcrumbs_and_dump.py`

#### 1. What it does
Decodes binary CSF ringbuffer memory dumps and extracts breadcrumb sync values.

---

### `scripts/pack_sandbox_bundles.sh`

#### 1. What it does
Packages all dependencies, source archives, predownloaded toolchains, proprietary blobs, and python wheels into split zip bundles (`panthor2kbase-offline-sandbox-bundle.zip_part_*`) under 95 MiB each, generating `manifest.sha256`.

#### 2. Why it exists
Enables migrating the complete development and verification environment into offline sandbox environments without internet access.

#### 3. How to run it
```bash
bash scripts/pack_sandbox_bundles.sh
```
