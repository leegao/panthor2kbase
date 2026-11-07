# Mali AOT Binary Translator & Emulation Environment (`mali_aot`)

A comprehensive ahead-of-time (AOT) binary translation and execution environment for 64-bit ARM64-Bionic Mali GPU user-space drivers (`libGLES_mali.so` across Valhall, 5th-Gen, and Bifrost architectures) running natively on x86-64 Linux.

## Key Capabilities & Subprojects

1. **6-Pass C++17 LLVM 15 Lifter & Lowerer (`src/mali_llvm_passes.cpp` -> `bin/mali_llvm_pass_driver`)**:
   - Automatically discovers the complete `.data.rel.ro` Vulkan ICD dispatch table (`456` entries in `r54p3`, `422` in `r49p1_v5`, `408` in `r49p1_v4`, `223` in `r26p0`), lifts **39 core Vulkan ICD entrypoints (`@aot_vk*`)**, **4 CMPBE shader compiler entrypoints (`@aot_cmpbe_v2_*`)**, and **all 23 CSF / 16 JM `/dev/mali0` (`kbase`) UAPI wrappers (`@aot_kbase_*`)** into `x86-64-v3` ELF object code (`mali_mbs2_aot.o`).
   - Supports 64-bit integer high-multiply (`SMULH`/`UMULH`), full Scalar IEEE-754 Floating-Point (`SCVTF`, `UCVTF`, `FCVTZS`, `FCVTZU`, `FMOV`, `FABS`, `FNEG`, `FCVT`, `FCMP`/`FCMPE`, `FCSEL`, `FMUL`, `FDIV`, `FADD`, `FSUB`, `FMAX`, `FMIN`, `FMADD`, `FMSUB`), AdvSIMD vector ops, ARMv8 Crypto SHA-1 (`SHA1C`/`SHA1P`/`SHA1M`/`SHA1H`/`SHA1SU0`/`SHA1SU1`), and Bionic-to-glibc/UAPI lowering.
2. **Standalone `/dev/mali0` & `/dev/dma_heap` Emulator + Host Vulkan Rendering Engine (`mali0_emu/`)**:
   - Emulates `/dev/mali0` (`mali_kbase` CSF v1.36 & JM v11.35) and `/dev/dma_heap/system` (`DMA_HEAP_IOCTL_ALLOC` + `KBASE_IOCTL_MEM_IMPORT` zero-copy `UMM` BOs).
   - Disassembles and lifts 64-bit Valhall and 128-bit Bifrost Dual-Issue Clause `OBJC` machine code into SPIR-V 1.3 (`ExecutionModelGLCompute`, `ExecutionModelVertex`, `ExecutionModelFragment`).
   - Executes both compute dispatches (`vkCreateComputePipelines` + `vkCmdDispatch`) and **full 3D graphics passes (`VkRenderPass` + `VkFramebuffer` + `VkGraphicsPipeline` + `vkCmdBeginRenderPass` + `vkCmdBindVertexBuffers` + `vkCmdBindIndexBuffer` + `vkCmdDrawIndexed` + `vkCmdEndRenderPass` + `vkCmdCopyImageToBuffer`)** on the host Vulkan driver (`llvmpipe` or hardware `radv`/`anv` via `MALI0_EMU_BACKEND`).
3. **Translated & Decompiled x86-64 Khronos Vulkan 1.3 ICDs (`src/mali_vulkan_icd.cpp` & `r54p3_vulkan_icd/`)**:
   - Exposes `libvulkan_mali_aot_icd.so` (`mali_aot_icd.x86_64.json`) and `libr54p3_vulkan_icd.so` (`r54p3_decompiled_icd.x86_64.json`) to the standard system Khronos Vulkan Loader (`libvulkan.so.1`), including full WSI (`VK_KHR_surface`, `VK_EXT_headless_surface`, `VK_KHR_swapchain`) support.
4. **End-to-End WSI `vkcube` & 10-Suite Khronos `Vulkan-Samples` Compliance (`bin/vkcube`, `./icd_harness.sh`, `tests/run_unit_tests.py`)**:
   - Runs multi-frame 3D `vkcube` over `VK_KHR_swapchain` (`Frame 0..3` at `25°, 43°, 61°, 79°` -> `crc32 = 0xd8969b18, 0x5f95f74d, 0x7f6b3bf8, 0xfe159e0a`) and passes all **19/19** automated unit and E2E integration tests across all 4 Mali driver builds.

## Documentation Index

- [DESIGN.md](DESIGN.md): Overall AOT translation engine, WSI swapchain, and Host Vulkan graphics pipeline architecture.
- [PLAN.md](PLAN.md): Completed bring-up roadmap and multi-driver verification matrix.
- [PROGRESS.md](PROGRESS.md): Detailed implementation log and 19-test verification summary.
- [RE_NOTES.md](RE_NOTES.md): Reverse engineering findings across `r26p0`, `r49p1_v4`, `r49p1_v5`, and `r54p3`.
- [BRINGUP.md](BRINGUP.md): Automated driver bring-up guide and heuristics (`scripts/auto_bringup.py`).
- [LLVM_PASSES.md](LLVM_PASSES.md): 6-pass C++17 LLVM lifter and lowering architecture.
- [ICD_ARCHITECTURE.md](ICD_ARCHITECTURE.md): Detailed `r54p3` Vulkan 1.3 ICD architecture and 456-entry dispatch table.
- [MALI0_EMU_README.md](MALI0_EMU_README.md): Standalone `mali0_emu` kernel UAPI & Valhall/Bifrost-to-Vulkan engine documentation.
- [BUILD.md](BUILD.md): Step-by-step build, CMake/Make, and test instructions.

## Quick Build & Run

```bash
# Build all subprojects, AOT runner, and WSI vkcube via Make:
make -j$(nproc)

# Run standalone WSI 3D vkcube end-to-end via system libvulkan.so.1:
./bin/vkcube

# Run the 4-stage ICD + WSI vkcube + 10-suite Vulkan-Samples harness:
./icd_harness.sh /path/to/libGLES_mali.so llvmpipe

# Run the complete 19-test unit & E2E verification suite:
python3 tests/run_unit_tests.py
```
