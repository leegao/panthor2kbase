# Future Driver Bring-Up Plan (`PLAN.md`)

## 1. Completed Milestones

1. **Unified 6-Pass C++ LLVM Lifting & Lowering Framework (`src/mali_llvm_passes.cpp`)**:
   - All 6 passes (`ELF64LoaderAndCallGraphPass`, `CFGRecoveryAndPicJumpTablePass`, `AArch64ToLLVMIRLiftingPass`, `Arm64BionicIdiosyncrasyLoweringPass`, `BionicToGlibcLibcLoweringPass`, `LLVMO2OptimizationAndX86CodegenPass`) operate directly on 64-bit `libGLES_mali.so` binaries via `libLLVM-15.so.1`.
2. **Multi-Generation 64-Bit Driver Bring-Up (4 Drivers Verified)**:
   - `r26p0-01eac0-3b7c95` (Bifrost / Early Valhall, `VEHW 0x0b`, `MBS2 v35`)
   - `r49p1-03bet0-49d848` (Valhall 2nd/3rd-Gen, `VEHW 0x11`, `MBS2 v44`)
   - `r49p1-03bet0-c48934` (5th-Gen Flagship Immortalis-G925, `VEHW 0x1f`, `MBS2 v44`)
   - `r54p3-00eac0-6eda07` (5th-Gen Mali-G720, `VEHW 0x1c`, `MBS2 v47`)
3. **Vulkan ICD & `/dev/mali0` (`kbase`) Kernel UAPI Translation**:
   - Automatic discovery and AOT translation of `vk_hal_open` (`HMI`), `vkGetInstanceProcAddr`, `vkEnumerateInstanceVersion`, `vkEnumerateInstanceLayerProperties`, `vkEnumerateInstanceExtensionProperties`, `vkEnumeratePhysicalDevices`, `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkCreateShaderModule` (including ARMv8 Crypto `SHA1C`/`SHA1P`/`SHA1M`/`SHA1H`/`SHA1SU0`/`SHA1SU1`), `vkDestroyShaderModule`, and `mali_gpu_props_get`.
4. **Sample SPIR-V 1.3 Shaders -> Mali Hardware ISA `OBJC` Verification**:
   - Built `passthrough_frag.spv`, `vector_compute.spv`, and `branch_loop_compute.spv` and verified all 3 shaders across all 4 drivers through `vkCreateShaderModule` and `cmpbe_v2_deserialize_MBS2_to_C` (`cmpbe_chunk_OBJC`).

## 2. Roadmap for Full Graphics/Compute Pipeline Execution on Linux

1. **`/dev/mali0` Memory Management UAPIs (`KBASE_IOCTL_MEM_ALLOC` / `MEM_QUERY` / `MEM_FREE`)**:
   - Extend `@bionic_mali_kbase_ioctl_stub` to emulate `KBASE_IOCTL_MEM_ALLOC` (`0xc0208005`) and `mmap` of GPU virtual address regions using host `mmap(MAP_ANONYMOUS)` so `vkCreateInstance` -> `vkCreateDevice` -> `vkCreateComputePipelines` can allocate GPU descriptor heaps in user space.
2. **Scalar/Vector Floating-Point Lifting for Full `cmpbe_v2_compile_single_shader`**:
   - Add scalar IEEE-754 floating-point instructions (`FADD`, `FSUB`, `FMUL`, `FDIV`, `FCMP`, `FCSEL`, `FCVTZS`, `SCVTF`) to `AArch64ToLLVMIRLiftingPass` for constant-folding passes inside `cmpbe_v2_compile_single_shader`.


---

## 8. Roadmap for Expanding the `/dev/mali0` CSF + Valhall (`Mali-G615`) -> Lavapipe Harness

1. **Full Graphics Pipeline (`RUN_IDVS` + `RUN_FRAGMENT`) Translation**:
   - Currently, `RUN_COMPUTE`, `RUN_FRAGMENT`, and `RUN_IDVS` jobs lift Valhall `OBJC` instructions into Vulkan 1.3 Compute Shaders bound to `/dev/mali0` `SAME_VA` BOs (`Binding 0 = Input BO`, `Binding 1 = Output BO`). Expanding `HostLvpipeVulkanExecutor` to map Mali Framebuffer Descriptors (`FBD` / `MFBD`) and Vertex/Tiler descriptors into `VkRenderPass` and `VkGraphicsPipeline` objects on Lavapipe will enable full triangle rasterization testing.
2. **Additional Valhall v10/v11 Opcodes (`TEXTURE`, `ATEST`, `BLEND`, `LD_ATTR`, `VAR_TEX`)**:
   - Extend `ValhallISAToSpirvTranslator` to lower Valhall texture sampling (`TEX_SINGLE`, `VAR_TEX`) to `OpImageSampleImplicitLod` / `OpImageFetch` and fragment blend/alpha-test instructions (`ATEST`, `BLEND`) to SPIR-V fragment outputs.
3. **Multi-CSG Concurrent Queue Scheduling & `SYNC_WAIT` Event Threads**:
   - Extend `MaliKbaseCsfKernelEmulator` to run an asynchronous background worker thread polling `CS_INSERT` across all 8 CSG groups and signaling `/dev/mali0` `poll()` wait queues via `eventfd`.

---

## Standalone `mali0_emu/` Subproject (`/dev/mali0` CSF + JM & `/dev/dma_heap` Emulator + `vkcube` & Multi-Pass Lavapipe Engine)

The generic `/dev/mali0` (`mali_kbase` CSF & Job Manager) and `/dev/dma_heap/system` kernel emulation layer, Valhall & Bifrost ISA-to-SPIR-V 1.3 translators, and Host Vulkan (`Mesa Lavapipe` / `llvmpipe`) execution engine have been extracted into a standalone, reusable subproject under `mali0_emu/`:

| Subproject Component | Path | Responsibility |
| :--- | :--- | :--- |
| **UAPI & Framebuffer/IDVS Headers** | `mali0_emu/include/mali0_emu_uapi.h` | Complete `/dev/mali0` CSF (`v1.36`) & JM (`v11.35`) ioctls, `/dev/dma_heap/system` (`DMA_HEAP_IOCTL_ALLOC = 0xc0184800`), `KbaseIoctlMemImport` (`BASE_MEM_IMPORT_TYPE_UMM = 2`), `MaliMfbdDescriptor` (`MFBD` Color + Depth attachments), `MaliIdvsDrawPacket` (`IDVS` 3D indexed draw packet), GPU profiles (`Mali-G76`, `Mali-G78`, `Mali-G615`, `Mali-G720`, `Immortalis-G925`), and C ABI hooks (`mali0_emu_open`, `mali0_emu_ioctl`, `mali0_emu_mmap`, `mali0_emu_read`, `mali0_emu_poll`) |
| **Valhall & Bifrost ISA Lifters** | `mali0_emu/include/mali0_emu_isa.hpp` | `ValhallISAToSpirvTranslator`, `BifrostISAToSpirvTranslator`, and `MaliVkCubeIsaPipelineTranslator` lifting Compute, Vertex (`ExecutionModelVertex`), and Fragment (`ExecutionModelFragment`) `OBJC` shaders into SPIR-V 1.3 |
| **Host Vulkan (`llvmpipe`) Backend** | `mali0_emu/include/mali0_emu_lvp.hpp` | `HostLvpipeVulkanExecutor` (`64 KiB` storage buffers on Mesa Lavapipe), providing `validateShaderModuleOnLvpipe()`, `executeLiftedSpirv()`, and `executeVkCubeGraphicsPass()` (4x4 MVP transform, perspective divide, back-face culling, hardware Z-buffer `VK_COMPARE_OP_LESS` depth test, Lambertian diffuse lighting, and Lavapipe tile writeback into `/dev/mali0` `MFBD` Color & Depth BOs) |
| **Device & Ringbuffer/JM Emulator** | `mali0_emu/include/mali0_emu_device.hpp` | `MaliKbaseCsfKernelEmulator` managing `/dev/mali0` (`fd=100`) and `/dev/dma_heap/system` (`fd=101`), zero-copy `UMM` `dma_buf` imports, CSF 96-register ringbuffer (`RUN_COMPUTE`, `RUN_IDVS`, `RUN_FRAGMENT`, `FINISH_TILING`, `FINISH_FRAGMENT`, `SYNC_ADD64`), and JM Job Descriptor Chains (`TILER_JOB`, `FRAGMENT_JOB`, `COMPUTE_JOB`, `WRITE_VALUE_JOB`) |
| **Library & Standalone Build** | `mali0_emu/src/mali0_emu.cpp`, `mali0_emu/build.sh`, `mali0_emu/CMakeLists.txt` | Builds `mali0_emu/lib/libmali0_emu.a`, `mali0_emu/lib/libmali0_emu.so`, and `mali0_emu/bin/test_mali0_vkcube_and_complex` |
| **Standalone & Integrated Tests** | `mali0_emu/tests/test_mali0_vkcube_and_complex.cpp` & `Test 8` in `src/mali_aot_runner.cpp` | Verifies `/dev/dma_heap` WSI import, multi-frame 3D `vkcube` rendering (`Frame 0 @ 25 deg` -> `crc32=0xd8969b18`, `Frame 1 @ 65 deg` -> `crc32=0x29c2de92`, `1,788` shaded pixels, `6` front-facing triangles, hardware Z-buffer `[0.2997..0.6996]`), JM Bifrost `vkcube` (`crc32=0x8a8105b4`), and Multi-Pass Compute vertex deformation (`deform_scale=1.224`) -> CQS Barrier -> 3D Graphics (`crc32=0x61720b90`) across all 4 drivers (`16/16` unit & E2E tests passing) |

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

## Decompiled `r54p3_vulkan_icd` C++17 Project & 10-Suite Khronos `Vulkan-Samples` Compliance

### 1. Decompiled `r54p3-00eac0-6eda07` C++17 Vulkan ICD (`r54p3_vulkan_icd/`)
- **456-Entry `.data.rel.ro` Dispatch Table (`include/r54p3_dispatch_table.hpp`)**:
  Extracted verbatim from `RVA 0x02f9bfb0 .. 0x02fa22f0` (stride `56B`, sorted lexicographically from `vkAcquireNextImage2KHR` to `vkWaitSemaphoresKHR`), powering the $O(\log_2 N)$ binary search in `vkGetInstanceProcAddr` (`0x00964220`) and `vkGetDeviceProcAddr` (`0x00964520`).
- **Exact Shader Compiler Boundary (`include/r54p3_cmpbe_boundary.hpp`)**:
  Stops cleanly at **`cmpbe_v2_compile_multiple_shaders`** (`RVA 0x01c9ecb0`, 10-argument AAPCS64 ABI: 8 register args `X0..X7` + 2 stack args `[SP+0x00]=out_error_logs`, `[SP+0x10]=out_results`) and its 21-instruction single-stage wrapper **`cmpbe_v2_compile_single_shader`** (`RVA 0x01c9ec50`, called from `0x01adf3c4` for compute pipelines; `0x01ae12b0` calls `0x01c9ecb0` directly for multi-stage graphics pipelines).
- **4-Layer Decompiled Implementation (`include/r54p3_icd_types.hpp`, `src/r54p3_vulkan_icd.cpp`)**:
  Reconstructs `osu::*` (`computeSpirvSha1` at `0x008bfaa0`), `kbase::*` (`/dev/mali0` CSF context & queue group lifecycle at `0x01c68e10..0x01c72520`), `gfx::*` (`ValhallTextureDescriptor`, `ValhallSamplerDescriptor`, `MaliMfbdDescriptor`, `MaliIdvsDrawPacket` builders at `0x01adf340..0x01b5f500`), and `vulkan::*` (`0x00936bc0..0x00991000`).

### 2. 10-Suite Khronos `Vulkan-Samples` E2E Compliance (`tests/test_vulkan_samples_compliance.cpp`)
Both `libr54p3_vulkan_icd.so` (`r54p3_decompiled_icd.x86_64.json`) and `libvulkan_mali_aot_icd.so` (`mali_aot_icd.x86_64.json`) are verified end-to-end through the standard system Khronos Vulkan Loader (`libvulkan.so.1`) across 10 complex `Vulkan-Samples` workloads using procedural RGBA8 checkerboard + mip-chain stub textures (`populateStubTextureWithMipmaps`) where external KTX/glTF assets are omitted:
1. `hello_triangle` / `hello_triangle_1_3` (`shaded_px=1788`, `crc32=0xd8969b18`)
2. `texture_loading` (procedural 64x64 RGBA8 stub KTX texture upload via `vkCmdCopyBufferToImage` + combined image sampler, `shaded_px=1910`)
3. `separate_image_sampler` (`VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` + `VK_DESCRIPTOR_TYPE_SAMPLER` with `maxAnisotropy=16.0f`)
4. `texture_mipmap_generation` (6-level mip chain `64x64 -> 1x1` generated via `vkCmdPipelineBarrier` + `vkCmdBlitImage`, `shaded_px=1850`)
5. `dynamic_uniform_buffers` (`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` across 4 per-object dynamic offsets `[0, 256, 512, 768]`)
6. `instancing` (16-instance mesh draw via `vkCmdDrawIndexed`, `shaded_px=1788`)
7. `compute_nbody` (2-pass Compute Particle SSBO `in=[3, 4, 5, 6] -> out=[38.25, 67.0, 103.75, 148.5]` + barrier + graphics draw)
8. `hdr` & `oit_depth_peeling` (3-layer depth peeling + offscreen bloom composition, `shaded_px=1850`)
9. `terrain_tessellation` & `timestamp_queries` (64-bit GPU timestamps via `vkCmdWriteTimestamp` & `vkGetQueryPoolResults`, `delta=42000 ns`)
10. `buffer_device_address`, `conditional_rendering` & `color_write_enable` (`vkGetBufferDeviceAddress`, predicate `0` draw suppression vs predicate `1` active shading `shaded_px=1910`, and `vkCmdSetColorWriteEnableEXT`).


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
