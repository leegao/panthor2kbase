#ifndef VALHALL_VULKAN_RENDERER_H
#define VALHALL_VULKAN_RENDERER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Self-Contained CSF-Hooked Valhall-on-Vulkan Shader Renderer
 *
 * Hooks directly into libmali_kbase_emu's csf_job_hook callback
 * (`mali_kbase_emu_csf_job_hook_fn`) when a CSF hardware job
 * (RUN_COMPUTE 0x04/0x27, RUN_COMPUTE_INDIRECT 0x2c, RUN_TILING 0x05/0x28,
 * RUN_IDVS 0x06/0x29, RUN_FRAGMENT 0x07/0x2a, RUN_FULLSCREEN 0x08/0x2b,
 * RUN_VERTEX 0x26) is dispatched by the CSFFW command stream interpreter.
 *
 * Operates completely independently of the high-level Vulkan ICD:
 *   1. Decodes CSF 32-bit/64-bit register file (cs_regs[0..127])
 *   2. Resolves GPU virtual addresses via mali_kbase_emu_is_valid_gpu_va()
 *   3. Parses Mali Valhall v10/v11 hardware descriptors:
 *      - Shader Program Descriptors (SPD: Position, Varying, Fragment, Compute)
 *      - Shader Resource Tables (SRT: Buffer, Texture, Sampler, Attribute descriptors)
 *      - Fast Access Uniform (FAU) tables & Viewport descriptors
 *      - Thread Storage Descriptors (TSD), Tiler Context Descriptors (TCD),
 *        Depth/Stencil (ZSD), Blend (BD)
 *      - Framebuffer Descriptors (FBD) & Render Target Descriptors (RTD)
 *   4. Disassembles and lifts Valhall 64-bit ISA binaries into SPIR-V 1.3 modules
 *      using ValhallISAToSpirvTranslator
 *   5. Executes real Vulkan graphics and compute pipelines on a self-contained host
 *      VkDevice (Lavapipe / host ICD) and writes rendered pixels & storage buffer
 *      outputs directly back into guest GPU VA memory mapped by /dev/mali0.
 *
 * Environment:
 *   MALI_EMU_HEADLESS=1  Disable the renderer entirely (no host VkDevice, no
 *                        Lavapipe submit/readback). Any other value or unset
 *                        leaves the renderer enabled (default).
 */

void valhall_vk_renderer_init(void);
void valhall_vk_renderer_shutdown(void);

void valhall_vk_csf_job_hook(uint8_t cs_opcode,
                             uint64_t insn,
                             const uint64_t *cs_regs,
                             void *user_data);

uint64_t valhall_vk_renderer_get_frame_count(void);
uint64_t valhall_vk_renderer_get_compute_count(void);
uint64_t valhall_vk_renderer_get_last_render_target_va(void);

#ifdef __cplusplus
}
#endif

#endif /* VALHALL_VULKAN_RENDERER_H */
