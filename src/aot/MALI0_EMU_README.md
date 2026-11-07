# `mali0_emu` — Standalone `/dev/mali0` (CSF & Job Manager) + `/dev/dma_heap` Emulator & Valhall/Bifrost-to-Lavapipe Engine

`mali0_emu` is a self-contained, reusable x86-64 Linux userspace emulation library for Arm Mali GPUs (`Bifrost`, `Valhall`, and `5th-Gen`). It emulates the `/dev/mali0` (`mali_kbase`) and `/dev/dma_heap/system` kernel drivers across both **Command Stream Frontend (`CSF`, UAPI v1.36)** and **Job Manager (`JM`, UAPI v11.35)** architectures, translates compiled **Valhall 64-bit ISA** and **Bifrost 128-bit Dual-Issue Clause ISA** (`OBJC`) shaders into valid **SPIR-V 1.3** modules (`ExecutionModelVertex`, `ExecutionModelFragment`, and `ExecutionModelGLCompute`), and executes both **Compute** and **3D Graphics (`vkcube` & Multi-Pass Compute $\rightarrow$ Graphics)** workloads on the host CPU via **Mesa Lavapipe (`llvmpipe`, Vulkan 1.3)**.

## Directory Structure

* `include/mali0_emu_uapi.h` — `/dev/mali0` CSF & JM ioctl numbers, structs (`KbaseIoctlMemAllocEx`, `KbaseIoctlMemImport`, `KbaseIoctlCsQueueGroupCreate`, `KbaseIoctlJobSubmit`, `BaseJdAtomV2`, `BaseJdEventV2`), `/dev/dma_heap/system` (`DMA_HEAP_IOCTL_ALLOC = 0xc0184800`), `MaliMfbdDescriptor` (`MFBD` Color + Depth framebuffer descriptor), `MaliIdvsDrawPacket` (`IDVS` 3D indexed draw packet), and C ABI hooks (`mali0_emu_open`, `mali0_emu_ioctl`, `mali0_emu_mmap`, `mali0_emu_read`, `mali0_emu_poll`).
* `include/mali0_emu_isa.hpp` — `ValhallISAToSpirvTranslator`, `BifrostISAToSpirvTranslator`, and `MaliVkCubeIsaPipelineTranslator` (disassembling and lifting Compute, Vertex, and Fragment `OBJC` machine code into SPIR-V 1.3).
* `include/mali0_emu_lvp.hpp` — `HostLvpipeVulkanExecutor` (`dlopen("libvulkan.so.1")` on Mesa Lavapipe `llvmpipe`), providing `validateShaderModuleOnLvpipe()`, `executeLiftedSpirv()`, and `executeVkCubeGraphicsPass()` (4x4 MVP vertex transformation, perspective divide, back-face culling, hardware Z-buffer `VK_COMPARE_OP_LESS` depth testing, Lambertian diffuse fragment shading, and Lavapipe tile writeback into `/dev/mali0` `MFBD` Color & Depth BOs).
* `include/mali0_emu_device.hpp` — `MaliKbaseCsfKernelEmulator` implementing the full `/dev/mali0` and `/dev/dma_heap/system` file descriptor, BO virtual memory manager, CSF 96-register ringbuffer interpreter (`RUN_COMPUTE`, `RUN_IDVS`, `RUN_FRAGMENT`, `CALL`, `LOAD_MULTIPLE`, `SYNC_ADD64`), KCPU fence queue, and JM Job Descriptor Chain walker (`WRITE_VALUE`, `COMPUTE`, `TILER`, `FRAGMENT`).
* `include/mali0_emu.hpp` — Umbrella header + `mali0_emu_vkcube` 3D cube geometry and 4x4 perspective/rotation MVP matrix builders.
* `src/mali0_emu.cpp` — Standalone library compilation unit exporting C ABI entrypoints (`libmali0_emu.a` and `libmali0_emu.so`).
* `tests/test_mali0_vkcube_and_complex.cpp` — Standalone verification suite testing `vkcube` (multi-frame 3D rotating cube with `/dev/dma_heap` WSI import + `MFBD` Color & Depth attachments on both CSF Valhall and JM Bifrost) and a multi-pass Compute $\rightarrow$ Graphics mesh-deformation pipeline on Host Vulkan (`llvmpipe`).

## Building & Running Standalone Tests

```bash
cd mali0_emu
./build.sh
```

---

## Native x86-64 Khronos Vulkan ICD (`libvulkan_mali_aot_icd.so` + `mali_aot_icd.x86_64.json`)

The AOT-translated 64-bit ARM64 Mali driver (`mali_mbs2_aot.o` + `libGLES_mali.so`) and `mali0_emu` are exposed as a standard x86-64 Linux Khronos Vulkan Installable Client Driver (`src/mali_vulkan_icd.cpp` -> `build/<driver>/libvulkan_mali_aot_icd.so` + `mali_aot_icd.x86_64.json`) loadable directly by the system Khronos Vulkan Loader (`libvulkan.so.1`) via `VK_ICD_FILENAMES`:

| ICD Component / Stage | Implementation (`src/mali_vulkan_icd.cpp` & `mali0_emu`) | Verification (`./icd_harness.sh` & `Test 17`) |
| :--- | :--- | :--- |
| **Khronos ICD Interface v6** | Exports `vk_icdNegotiateLoaderICDInterfaceVersion` (v6), `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, and prefixes all dispatchable handles (`VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, `VkCommandBuffer`) with `ICD_LOADER_MAGIC = 0x01CDC0DE` | Negotiates cleanly with system `libvulkan.so.1` and `VK_LAYER_MESA_device_select` |
| **AOT Instance & PhysicalDevice** | Calls AOT-translated `aot_vk_hal_open` (`"vk0"`), `aot_vkEnumerateInstanceVersion` (`1.3.239`), `aot_vkEnumerateInstanceExtensionProperties` (`8` extensions), and `aot_mali_gpu_props_get` (`/dev/mali0` `GPUPROPS`) | Reports `VendorID = 0x13b5` (Arm Ltd.), `DeviceID = 0xb8830001`, `"Arm Mali-G615 (AOT r54p3-00eac0-6eda07 via mali0_emu [llvmpipe])"` |
| **AOT Logical Device & Memory** | `vkCreateDevice` runs AOT `aot_kbase_version_check_*`, `aot_kbase_set_flags`, `aot_kbase_mem_jit_init`, `aot_kbase_mem_exec_init`, `aot_kbase_cs_queue_register`, and `aot_kbase_cs_queue_bind`; `vkAllocateMemory` allocates `/dev/mali0` `SAME_VA` BOs via `aot_kbase_mem_alloc` | Binds 3-page `CSFFW_USER_IO` doorbell/ringbuffer and maps `/dev/mali0` BOs via `vkMapMemory` |
| **AOT Shader & CSF Queue Execution** | `vkCreateShaderModule` runs AOT `aot_vkCreateShaderModule` (ARMv8 Crypto SHA-1) + `aot_cmpbe_v2_deserialize_MBS2_to_C` and uploads Valhall/Bifrost `OBJC` to an `EXEC_VA` `/dev/mali0` BO; `vkQueueSubmit` writes 64-bit CSF packets (`RUN_COMPUTE`, `RUN_IDVS`, `RUN_FRAGMENT`, `SYNC_ADD64`) and calls `aot_kbase_cs_queue_kick` | Executes `vector_compute.spv` (`[38.25, 67.0, 103.75, 148.5]`), `branch_loop_compute.spv` (`[15.0, 16.25, 17.5, 18.75]`), and multi-frame 3D `vkcube` (`1,788` shaded px, `crc32=0xd8969b18` / `0x29c2de92`) |
| **Configurable Host Backend (`MALI0_EMU_BACKEND`)** | `HostLvpipeVulkanExecutor` in `mali0_emu/include/mali0_emu_lvp.hpp` loads backend ICD shared libraries directly via `vk_icdGetInstanceProcAddr` without loader recursion (`MALI0_EMU_BACKEND=llvmpipe` loads `libvulkan_lvp.so`; `MALI0_EMU_BACKEND=host` probes hardware ICDs `libvulkan_radeon.so`, `libvulkan_intel.so` first and falls back to `libvulkan_lvp.so`) | Verified across both `llvmpipe` and `host` backend modes (`17/17` unit & E2E tests passing) |



---

## Full Vulkan ICD Dispatch Table Discovery, Scalar FP Lifting, Host `VkGraphicsPipeline` Execution & WSI `vkcube` (`19/19` Tests Passing)

1. **Full `.data.rel.ro` Vulkan Dispatch Table Discovery & 39 Lifted `@aot_vk*` Entrypoints (`src/mali_llvm_passes.cpp`)**:
   - `Pass 1` (`ELF64LoaderAndCallGraphPass`) automatically locates the `.data.rel.ro` Vulkan ICD dispatch table across all 4 Mali driver generations (`r26p0`: 223 entries @ `0x02344de8`, stride 40B; `r49p1_v4`: 408 entries @ `0x0332f908`, stride 64B; `r49p1_v5`: 422 entries @ `0x0311a970`, stride 64B; `r54p3`: 456 entries @ `0x02f9bfb0`, stride 56B) and exports `@aot_get_vk_dispatch_table_rva`, `@aot_get_vk_dispatch_table_count`, and `@aot_get_vk_dispatch_table_stride`.
   - `Pass 3` (`AArch64ToLLVMIRLiftingPass`) adds 64-bit integer high-multiply (`SMULH`/`UMULH`), full Scalar IEEE-754 Floating-Point (`SCVTF`, `UCVTF`, `FCVTZS`, `FCVTZU`, `FMOV` imm/reg, `FABS`, `FNEG`, `FCVT` f32/f64, `FCMP`/`FCMPE` with IEEE-754 NZCV flag synthesis, `FCSEL`, `FMUL`, `FDIV`, `FADD`, `FSUB`, `FMAX`, `FMIN`, `FNMUL`, `FMADD`, `FMSUB`, `FNMADD`, `FNMSUB`), AdvSIMD scalar `DUP`, and `UMOV` `i8`/`i16` zero-extension to `i32`, lifting and exporting **39 `@aot_vk*` entrypoints** in `mali_mbs2_aot.o`.
2. **Real Host Vulkan `VkRenderPass` + `VkGraphicsPipeline` Execution in `mali0_emu` (`mali0_emu/include/mali0_emu_lvp.hpp` & `mali0_emu_isa.hpp`)**:
   - Fixed `OpEntryPoint` word counts in `MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv` (`9` words) and `liftFragmentObjcToSpirv` (`7` words).
   - Implemented `HostLvpipeVulkanExecutor::executeLiftedGraphicsPipelineOnLvpipe`, which compiles the lifted Vertex and Fragment SPIR-V 1.3 modules via `vkCreateShaderModule`, creates an offscreen `VkImage` (`VK_FORMAT_R8G8B8A8_UNORM`), `VkImageView`, `VkRenderPass`, `VkFramebuffer`, and `VkGraphicsPipeline` (`vkCreateGraphicsPipelines`), and records/submits `vkCmdBeginRenderPass` -> `vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS)` -> `vkCmdBindVertexBuffers` -> `vkCmdBindIndexBuffer` -> `vkCmdDrawIndexed` -> `vkCmdEndRenderPass` -> `vkCmdCopyImageToBuffer` on the host Vulkan driver (`llvmpipe` or hardware `radv`/`anv`).
3. **Full Khronos WSI Swapchain (`VK_KHR_surface` + `VK_EXT_headless_surface` + `VK_KHR_swapchain`) & Standalone `bin/vkcube` (`tests/vkcube_wsi_e2e.cpp`)**:
   - Both `libvulkan_mali_aot_icd.so` (`src/mali_vulkan_icd.cpp`) and `libr54p3_vulkan_icd.so` (`r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp`) implement `vkCreateHeadlessSurfaceEXT`, `vkGetPhysicalDeviceSurfaceSupportKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, `vkGetPhysicalDeviceSurfaceFormatsKHR`, `vkGetPhysicalDeviceSurfacePresentModesKHR`, `vkCreateSwapchainKHR` (backed by `/dev/dma_heap/system` `DMA_HEAP_IOCTL_ALLOC` + `/dev/mali0` `KBASE_IOCTL_MEM_IMPORT` zero-copy `UMM` BOs), `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, and `vkQueuePresentKHR` (with optional `MALI_VKCUBE_DUMP_PPM` frame dumping).
   - Verified across 4 rotating WSI swapchain frames (`Frame 0..3` at `25°, 43°, 61°, 79°` -> `shaded_px = 1788, 1910, 1850, 1668`, `crc32 = 0xd8969b18, 0x5f95f74d, 0x7f6b3bf8, 0xfe159e0a`) with all **19/19** unit and E2E tests in `tests/run_unit_tests.py` passing.
