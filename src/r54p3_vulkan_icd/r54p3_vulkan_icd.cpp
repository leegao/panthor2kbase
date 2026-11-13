// ============================================================================
// src/aot/r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp
// Thin Host-ABI Wrapper Around the Direct AArch64-to-x86-64 AOT-Lifted
// `libGLES_mali.so` (`r54p3-00eac0-6eda07`, `mali_mbs2_aot.o`), Dynamically
// Linked Against `libmali_kbase_emu.so` (`/dev/mali0` CSF + Lifted `.ko`).
//
// Architectural Overview (see Design Doc 62 & Checkpoint-v33):
//   1. `libGLES_mali.so` (`r54p3-00eac0-6eda07`, 52.9 MB AArch64 Bionic ELF)
//      is statically lifted via `mali_llvm_pass_driver --lift-so` / `horizon2llvm`
//      into native x86-64 PIC object code (`mali_mbs2_aot.o`), exporting all
//      456 Vulkan 1.3 dispatch entrypoints (`aot_vk*`), all 28 `/dev/mali0`
//      Kbase CSF UAPI wrappers (`aot_kbase_*`), and the MBS2 shader compiler
//      deserializer (`aot_cmpbe_v2_*`).
//   2. Rather than maintaining a parallel hand-decompiled C++ reimplementation
//      of the driver, `libr54p3_vulkan_icd.so` links directly against
//      `mali_mbs2_aot.o` and `libmali_kbase_emu.so` and delegates all Vulkan
//      ICD entrypoints, `.data.rel.ro` dispatch lookups (`0x02f9bfb0`),
//      SPIR-V shader compilation (`0x01c9ecb0`), and `/dev/mali0` CSF ringbuffer
//      submissions directly into the AOT-lifted `libGLES_mali.so` binary.
//   3. Preserves original `libGLES_mali.so` quirks and internal state-machine
//      behaviors (e.g., `VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS` in
//      `vkCreateBuffer`, reference-counted `pAllocator` asymmetry in
//      `vkCreateDescriptorSetLayout`/`vkCreatePipelineLayout`, and
//      `cctx + 0xc1` `VK_ERROR_DEVICE_LOST` sticky propagation across
//      `vkGetEventStatus`, `vkGetFenceStatus`, `vkWaitForFences`, and
//      `vkQueueSubmit`).
// ============================================================================

#define R54P3_THIN_AOT_WRAPPER_BUILD 1

#include "r54p3_dispatch_table.hpp"
#include "r54p3_cmpbe_boundary.hpp"

// Delegate the entire Vulkan 1.3 ICD & AOT-lifted `libGLES_mali.so` (`r54p3`)
// runtime bridge directly to the unified AOT ICD implementation linked with
// `mali_mbs2_aot.o` and `libmali_kbase_emu.so`.
#include "mali_vulkan_icd.cpp"

namespace mali_r54p3 {
namespace bridge {

// Query whether `libr54p3_vulkan_icd.so` is actively bound to the AOT-lifted
// `libGLES_mali.so` (`r54p3-00eac0-6eda07`) object (`mali_mbs2_aot.o`) and
// `/dev/mali0` emulator (`libmali_kbase_emu.so`).
extern "C" __attribute__((visibility("default")))
uint64_t r54p3_icd_get_lifted_entrypoint_count(void) {
    return aot_get_lifted_vk_entrypoint_count();
}

extern "C" __attribute__((visibility("default")))
uint64_t r54p3_icd_lookup_dispatch_rva(const char* name) {
    if (!name) return 0;
    g_aot_rt.ensureInitialized();
    uint64_t rva = g_aot_rt.lookupDispatchEntryRva(name);
    if (rva != 0) return rva;
    const auto* entry = mali_r54p3::vulkan::lookupDispatchEntryByName(name);
    return entry ? entry->target_rva : 0ULL;
}

extern "C" __attribute__((visibility("default")))
int32_t r54p3_icd_get_device_mali_fd(void* device) {
    if (!device) return -1;
    return static_cast<MaliIcdDevice*>(device)->mali_fd;
}

extern "C" __attribute__((visibility("default")))
void r54p3_icd_set_device_lost(void* device, uint8_t lost) {
    if (!device) return;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev->aot_cctx_mem.size() > 0xc1) {
        dev->aot_cctx_mem[0xc1] = lost;
    }
}

extern "C" __attribute__((visibility("default")))
uint8_t r54p3_icd_get_device_lost(void* device) {
    if (!device) return 0;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    return (dev->aot_cctx_mem.size() > 0xc1) ? dev->aot_cctx_mem[0xc1] : 0U;
}

extern "C" __attribute__((visibility("default")))
uint32_t r54p3_icd_get_lifted_vk_calls_executed(void) {
    return g_aot_rt.lifted_vk_calls_executed;
}

} // namespace bridge
} // namespace mali_r54p3
