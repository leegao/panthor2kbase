# Decompiled `r54p3-00eac0-6eda07` Vulkan 1.3 ICD Architecture (`r54p3_vulkan_icd`)

## 1. Executive Summary & Architectural Layering

`r54p3_vulkan_icd` is a standalone, modular C++17 decompilation of the **Vulkan 1.3 Installable Client Driver (ICD)** side of the 64-bit Arm Mali Valhall (`Mali-G615` / `Immortalis-G715`, 10th-Gen CSF) proprietary driver `r54p3-00eac0` (`BuildID: 6eda0790b338813537b23805dd61ffdb8fb47441`).

Per design requirements, the decompilation covers the entire Vulkan frontend, descriptor/resource/pipeline state machines, Valhall hardware descriptor builders (`MFBD`, `IDVS`, `SRT`, `SPD`, `TSD`, `FAU`, Texture & Sampler descriptors), and the `/dev/mali0` (`kbase` CSF) UAPI submission layer, **stopping cleanly at the shader compiler backend boundary**:
- **`cmpbe_v2_compile_multiple_shaders`** (`RVA 0x01c9ecb0`, 3,964 bytes)
- **`cmpbe_v2_compile_single_shader`** (`RVA 0x01c9ec50`, 96 bytes — a 21-instruction wrapper around `0x01c9ecb0`)

### 4-Layer Internal Architecture of `r54p3-00eac0-6eda07`

| Layer | Namespace | RVA Range in `r54p3` | Responsibility | Decompiled C++17 Files |
| :--- | :--- | :--- | :--- | :--- |
| **Layer 0: OS & Crypto Utilities** | `osu::*`, `stdlib::*` | `0x008bf000 .. 0x00936bb0` | ARMv8 Crypto Extension SHA-1 (`SHA1C`/`SHA1P`/`SHA1M`/`SHA1SU0`/`SHA1SU1` at `0x008bfaa0`) for 20-byte SPIR-V module caching, mutexes, aligned slab allocators | `include/r54p3_icd_types.hpp`, `src/r54p3_vulkan_icd.cpp` |
| **Layer 1: `/dev/mali0` Kernel UAPI** | `kbase::*` | `0x01c68e00 .. 0x01c73000` | Opens `/dev/mali0`, negotiates CSF UAPI `v1.36`, maps `BASE_MEM_MAP_TRACKING_HANDLE` (`0x3000`) & `USER_REG_PAGE` (`0x2f000`), initializes `JIT`/`EXEC_VA` zones & Tiler Heap, registers/binds CSF Command Stream (`CSI`) queues, maps 3-page `CSFFW_USER_IO` doorbell/input/output pages, and rings doorbells | `src/r54p3_vulkan_icd.cpp` (`mali_r54p3::kbase::*`) |
| **Layer 2: Valhall Hardware & Compiler Bridge** | `gfx::*` | `0x01adf000 .. 0x01b87000` | Translates Vulkan objects into Valhall 10th-Gen hardware descriptors (32B Texture/Sampler descriptors, 64B `MFBD`, `IDVS` packets, `SPD`), packs `cmpbe_v2_options_t`, and invokes `cmpbe_v2_compile_multiple_shaders` (`0x01c9ecb0`) | `include/r54p3_cmpbe_boundary.hpp`, `src/r54p3_vulkan_icd.cpp` (`mali_r54p3::gfx::*`) |
| **Layer 3: Khronos Vulkan 1.3 ICD Frontend** | `vulkan::*` | `0x00936bc0 .. 0x00991000` | Implements all **456** Vulkan 1.3 + KHR/EXT/ARM entrypoints, the binary-search `.data.rel.ro` dispatch table (`0x02f9bfb0`), Android HAL `HAL_MODULE_INFO_SYM` (`0x03070018`), and Khronos Loader ICD v6 negotiation | `include/r54p3_dispatch_table.hpp`, `src/r54p3_vulkan_icd.cpp` |

---

## 2. Reverse-Engineered Dispatch Table & Loader Negotiation (`0x02f9bfb0`)

In `r54p3-00eac0-6eda07`, `vkGetInstanceProcAddr` (`RVA 0x00964220`) and `vkGetDeviceProcAddr` (`RVA 0x00964520`) perform an $O(\log_2 N)$ binary search over a lexicographically sorted table in `.data.rel.ro` at **`RVA 0x02f9bfb0 .. 0x02fa22f0`**:
- **Entry Count**: **456** Vulkan entrypoints (`vkAcquireNextImage2KHR` at index `0` through `vkWaitSemaphoresKHR` at index `455`).
- **Entry Stride**: **56 bytes** (`7 x uint64_t`):
  - `+0x00`: `const char* name` (RVA in `.rodata`, e.g., `0x001121a0` for `"vkAcquireNextImage2KHR"`)
  - `+0x08`: `uint64_t name_len`
  - `+0x10`: `uint64_t ext_id`
  - `+0x18`: `uint64_t ext_slot_0`
  - `+0x20`: `uint64_t scope_flags` (`1` = Instance, `2` = PhysicalDevice, `3` = Device)
  - `+0x28`: `void* target_fn` (RVA in `.text`, e.g., `0x0095fc10` for `vkCreateDevice`)
  - `+0x30`: `uint64_t api_version_mask`

All 456 entries are preserved verbatim in `include/r54p3_dispatch_table.hpp` (`mali_r54p3::vulkan::kR54p3DispatchTable`).

---

## 3. Decompiled Internal Object Sizes & Layouts (`vulkan::*`)

Every Vulkan object constructor in `r54p3` allocates its internal C++ structure via the `VkAllocationCallbacks` helper at `RVA 0x00936bc0` (`X1 = size`, `X2 = alignment`, `X3 = VkSystemAllocationScope`). Exact sizes recovered from `r54p3` disassembly:

| Vulkan Handle | Creation Entrypoint RVA | Internal Allocation Call RVA | Internal Size (Bytes) | Alignment | Key Internal Fields |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`VkDevice`** | `0x0095fc10` -> `0x0097adb0` | `0x0097af40` | **5,248 B** (`0x1480`) | `16 B` | `+0x00`: `loader_magic` (`0x01CDC0DE`), `+0x10`: `VkAllocationCallbacks`, `+0x70`: `kbase::Context*`, `+0x120`: `kbase::CsiQueue[4]`, `+0x380`: `cmpbe::CmpbeV2Context`, `+0x500`: `SRT`/`SPD`/`TSD`/`FAU`/`CQS` BO GPU VAs |
| **`VkCommandBuffer`** | `0x00939ef0` -> `0x01ae9860` | `0x01ae9898` | **2,816 B** (`0x0b00`) | `16 B` | `+0x00`: `loader_magic` (`0x01CDC0DE`), `+0x08`: `Device*`, `+0x40`: bound `Pipeline*`, `+0x48`: bound `DescriptorSet*[8]`, `+0x90`: active `RenderPass*` & `Framebuffer*`, `+0x100`: `push_constants[128]`, `+0x180`: CSF command stream buffer |
| **`VkImage`** | `0x0096eba0` -> `0x01b02760` | `0x0096ec14` | **472 B** (`0x01d8`) | `8 B` | `+0x00`: `image_type`/`format`/`extent`, `+0x24`: `mip_levels`/`array_layers`/`samples`, `+0x48`: AFBC & Valhall plane layout descriptors, `+0x110`: bound `/dev/mali0` BO GPU VA |
| **`VkImageView`** | `0x00975d30` -> `0x01b05630` | `0x00975d8c` | **288 B** (`0x0120`) | `8 B` | `+0x00`: parent `Image*`, `+0x20`: 32-byte hardware `ValhallTextureDescriptor` (`surface_gpu_va`, `width-1`, `height-1`, format/swizzle word) |
| **`VkBuffer`** | `0x009392f0` | `0x00939338` | **64 B** (`0x0040`) | `8 B` | `+0x00`: `size_bytes`, `+0x08`: `usage_flags`, `+0x18`: `bound_gpu_va` (returned directly by `vkGetBufferDeviceAddress` at `0x00939900`) |
| **`VkDeviceMemory`** | `0x009631e0` | `0x00963240` | **136 B** (`0x0088`) | `8 B` | `+0x00`: `gpu_va`, `+0x08`: `size_bytes`, `+0x10`: `va_pages`, `+0x18`: `kbase_flags` (`SAME_VA`, `CACHED_CPU`, `COHERENT_SYSTEM`) |
| **`VkShaderModule`** | `0x0098e4e0` | `0x0098e510` | **56 B** (`0x0038`) | `8 B` | `+0x00`: `code_size`, `+0x08`: `spirv_words*`, `+0x18..+0x2b`: **20-byte SHA-1 digest** computed by `osu::computeSpirvSha1` (`0x008bfaa0`) |
| **`VkSampler`** | `0x0098d500` | `0x0098d53c` | **80 B** (`0x0050`) | `8 B` | `+0x10`: 32-byte hardware `ValhallSamplerDescriptor` (filter mode, wrap modes, LOD clamps, max anisotropy) |
| **`VkFramebuffer`** | `0x00965b60` | `0x00965ba8` | **120 B** (`0x0078`) | `8 B` | `+0x00`: `RenderPass*`, `+0x10`: `width`/`height`/`layers`, `+0x28`: 64-byte hardware `MaliMfbdDescriptor` (`color_rt0_gpu_va`, `depth_bo_gpu_va`, clear values) |

---

## 4. Exact Shader Compilation Boundary (`cmpbe_v2_compile_multiple_shaders` @ `0x01c9ecb0`)

### 4.1 Disassembly of `cmpbe_v2_compile_single_shader` (`0x01c9ec50 .. 0x01c9eca4`)

Decoding the 21 instructions at `RVA 0x01c9ec50` reveals the exact calling convention of `cmpbe_v2_compile_multiple_shaders` (`0x01c9ecb0`):

```armasm
0x01c9ec50: paciasp
0x01c9ec54: sub  sp, sp, #0x60
0x01c9ec58: stp  x29, x30, [sp, #0x50]
0x01c9ec5c: add  x29, sp, #0x50
0x01c9ec60: ldr  x8, [sp, #0x60]          ; 9th arg of single_shader: out_result
0x01c9ec64: mrs  x9, tpidr_el0
0x01c9ec68: str  x1, [sp, #0x40]          ; local_options_array[0] = x1 (options)
0x01c9ec6c: str  x2, [sp, #0x38]          ; local_inputs_array[0]  = x2 (shader_input)
0x01c9ec70: str  w4, [sp, #0x34]          ; local_stages_array[0]  = w4 (stage_flag)
0x01c9ec74: stp  x7, x8, [sp, #0x00]      ; [sp+0x00] = out_error_log, [sp+0x08] = out_result
0x01c9ec78: ldr  x9, [x9, #0x28]          ; Bionic stack protector canary
0x01c9ec7c: str  x9, [sp, #0x48]
0x01c9ec80: mov  x4, x3                   ; x4 = pipeline_link_info
0x01c9ec84: mov  w6, w5                   ; w6 = compile_flags_0
0x01c9ec88: mov  w7, w6                   ; w7 = compile_flags_1
0x01c9ec8c: mov  w1, #1                   ; w1 = shader_count (1)
0x01c9ec90: add  x2, sp, #0x40            ; x2 = &local_options_array[0]
0x01c9ec94: add  x3, sp, #0x38            ; x3 = &local_inputs_array[0]
0x01c9ec98: add  x5, sp, #0x34            ; x5 = &local_stages_array[0]
0x01c9ec9c: bl   0x01c9ecb0               ; -> cmpbe_v2_compile_multiple_shaders
0x01c9eca0: ldp  x29, x30, [sp, #0x50]
0x01c9eca4: add  sp, sp, #0x60
0x01c9eca8: retab
```

### 4.2 Call Sites in `gfx::*`

1. **Compute Pipeline Compilation (`vkCreateComputePipelines` `0x00957ab0` -> `0x01adf340`)**:
   - Initializes a 72-byte `CmpbeV2Options` (`cmpbe_v2_init_options` at `0x01c9ec10`).
   - Sets option `9` (`CMPBE_OPT_SHADER_STAGE_MASK = 0x20`) and option `11` (`CMPBE_OPT_COMPUTE_LOCAL_SIZE`).
   - Calls `cmpbe_v2_compile_single_shader` at `0x01adf3c4`.
   - Uploads the returned `objc_words` into an `EXEC_VA` `/dev/mali0` BO at `0x01b07080`.
2. **Graphics Pipeline Multi-Stage Compilation (`vkCreateGraphicsPipelines` `0x00965f40` -> `0x01ae1100`)**:
   - Initializes per-stage `CmpbeV2Options` (`0x01ae11e0`), setting options `2, 5, 6, 7, 10, 17` (including `CMPBE_OPT_VALHALL_IDVS_MODE = 1`).
   - Calls `cmpbe_v2_compile_multiple_shaders` directly at `0x01ae12b0` to jointly compile and link Vertex (`IDVS` Position + Varying) and Fragment stages.
   - Uploads both stage binaries into `EXEC_VA` BOs and frees the temporary `CmpbeV2CompilationResult` via `cmpbe_v2_free_compilation_result` (`0x01c9fd20`).

### 4.3 Plugging in an External Shader Compiler Backend

External compiler backends can be attached at runtime via `mali_r54p3::cmpbe::registerExternalCmpbeCompiler(fn)` in `include/r54p3_cmpbe_boundary.hpp`. When no external hook is registered, `defaultCmpbeV2CompileMultipleShaders` synthesizes executable Valhall 64-bit `OBJC` machine code so `libr54p3_vulkan_icd.so` runs end-to-end against `mali0_emu`.


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
