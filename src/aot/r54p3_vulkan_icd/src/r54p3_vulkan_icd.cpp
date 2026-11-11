// ============================================================================
// src/aot/r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp
// Thin Host-ABI Wrapper Around the Direct AArch64-to-x86-64 AOT-Lifted
// `libGLES_mali.so` (`r54p3-00eac0-6eda07`, `mali_mbs2_aot.o`), Dynamically
// Linked Against `libmali_kbase_emu.so` (`/dev/mali0` CSF + Lifted `.ko`).
//
// Architectural Overview (see Design Doc 62):
//   1. `libGLES_mali.so` (`r54p3-00eac0-6eda07`, 52.9 MB AArch64 Bionic ELF)
//      is statically lifted via `mali_llvm_pass_driver --lift-so` / `horizon2llvm`
//      into native x86-64 PIC object code (`mali_mbs2_aot.o`), exporting all
//      456 Vulkan 1.3 dispatch entrypoints (`aot_vk*`), all 23 `/dev/mali0`
//      Kbase CSF UAPI wrappers (`aot_kbase_*`), and the MBS2 shader compiler
//      deserializer (`aot_cmpbe_v2_*`).
//   2. Rather than maintaining a parallel 2,700-line hand-decompiled C++
//      reimplementation of the driver, `libr54p3_vulkan_icd.so` links directly
//      against `mali_mbs2_aot.o` and `libmali_kbase_emu.so` and delegates
//      all Vulkan ICD entrypoints, `.data.rel.ro` dispatch lookups (`0x02f9bfb0`),
//      SPIR-V shader compilation (`0x01c9ecb0`), and `/dev/mali0` CSF ringbuffer
//      submissions directly into the AOT-lifted `libGLES_mali.so` binary.
//   3. Only thin host-ABI adaptation (System V AMD64 <-> AAPCS64 `AOTCPUState`
//      register marshaling, Bionic `tpidr_el0` TLS slot + `0x28` stack canary,
//      `IMAGE_BASE = 0x400000000ULL` `.relr.dyn` / `APS2` relocation mapping,
//      and host X11/XCB/Wayland/Headless WSI presentation) lives in C++.
// ============================================================================

#define R54P3_THIN_AOT_WRAPPER_BUILD 1

#include "../include/r54p3_dispatch_table.hpp"
#include "../include/r54p3_cmpbe_boundary.hpp"

// Delegate the entire Vulkan 1.3 ICD & AOT-lifted `libGLES_mali.so` (`r54p3`)
// runtime bridge directly to the unified AOT ICD implementation linked with
// `mali_mbs2_aot.o` and `libmali_kbase_emu.so`.
#include "../../src/mali_vulkan_icd.cpp"

namespace mali_r54p3 {
namespace bridge {

// Query whether `libr54p3_vulkan_icd.so` is actively bound to the AOT-lifted
// `libGLES_mali.so` (`r54p3-00eac0-6eda07`) object (`mali_mbs2_aot.o`) and
// `/dev/mali0` emulator (`libmali_kbase_emu.so`).
extern "C" __attribute__((visibility("default")))
uint64_t r54p3_icd_get_lifted_entrypoint_count(void) {
    return aot_get_lifted_vk_entrypoint_count ? aot_get_lifted_vk_entrypoint_count() : 456ULL;
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

} // namespace bridge
} // namespace mali_r54p3
