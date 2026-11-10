// Unified AArch64 -> LLVM IR Lowering & Optimization Passes Header (src/aot/horizon2llvm/passes.hpp)
//
// Declares the unified AArch64-to-LLVM-IR lowering and optimization passes supporting BOTH:
//   - Mode A (`LoweringMode::UserspaceBionicSo`): User-space ELF64 ET_DYN shared libraries
//     (`libGLES_mali.so` / `libvulkan_panfrost.so`) with Bionic TLS (`TPIDR_EL0`), PLT/GOT
//     dynamic symbol redirection, and Vulkan entrypoint hooks.
//   - Mode B (`LoweringMode::KernelModuleKo`): Linux kernel ELF64 ET_REL relocatable modules
//     (`mali_kbase.ko` MT6878 r44p1 & `mali_kbase_husky.ko` Pixel 8 Pro Tensor G3 r54p1/r54p3)
//     with `.rela.text` relocation resolution, direct `.rodata` image mapping (`lifted_ko_rodata_ptr`),
//     EL1 system registers (`SP_EL0`, `DAIF`, `TTBR1_EL1`), 39-bit to 48-bit `TASK_SIZE_64`
//     `access_ok` widening, Shadow Call Stack (`x18`) & `LiftedKoHeap` sanitizer guards, and
//     unmarshaled `struct kbase_ko_host_abi` callout dispatch.
//
// Core passes:
//   1. AArch64SysBarrierLoweringPass    (inline in passes.cpp: ARMv8 MRS/MSR/DMB/DC ZVA lowering)
//   2. GuestStackAndAlignmentPass       (lowering/pass_guest_stack_alignment.cpp)
//   3. AArch64IdiomDeconstructPass      (lowering/pass_aarch64_idiom_deconstruct.cpp)
//   4. JumpTableLoweringPass            (lowering/pass_jump_table_lowering.cpp)
//   5. FCMPPredicateFusionPass          (lowering/pass_fcmp_predicate_fusion.cpp)
//   6. NZCVPredicateFusionPass          (lowering/pass_nzcv_predicate_fusion.cpp)
//   7. NZCVDeadFlagElimPass             (lowering/pass_nzcv_dead_flag_elim.cpp)
//   8. LinkRegisterElisionPass          (lowering/pass_link_register_elision.cpp)
//   9. TailCallDevirtualizationPass     (lowering/pass_tail_call_devirt.cpp)
//  10. AAPCS64ABIContractPass           (lowering/pass_aapcs64_abi_contract.cpp)
//  11. CpuStatePromotionPass            (lowering/pass_cpu_state_promotion.cpp)
#pragma once

#include <numeric>
#include "aarch64/aarch64_lifter.hpp"
#include "llvm_ir.hpp"

namespace ultrs::passes {

using namespace ultrs::llvm_ir;
using ultrs::lifter::block_label;
using ultrs::lifter::fn_symbol;
using ultrs::lifter::FpOp;
using ultrs::lifter::LaneOp;

enum class LoweringMode {
    UserspaceBionicSo = 0,
    KernelModuleKo = 1,
    RawBinary = 2,
};

// 1. AArch64SysBarrierLoweringPass (ARMv8 MRS/MSR/DMB/DSB/ISB/DC ZVA lowering, parameterized by LoweringMode)
class AArch64SysBarrierLoweringPass final : public ModulePass, public PassInfoMixin<AArch64SysBarrierLoweringPass> {
public:
    explicit AArch64SysBarrierLoweringPass(LoweringMode mode = LoweringMode::UserspaceBionicSo)
        : mode_(mode) {}
    static const char* static_name() { return "aarch64-sys-barrier-lowering-pass"; }
    const char* pass_name() const override { return "aarch64-sys-barrier-lowering-pass"; }
    bool run(Module& M) override;
private:
    LoweringMode mode_;
};

// 2. GuestStackAndAlignmentPass
class GuestStackAndAlignmentPass final : public ModulePass, public PassInfoMixin<GuestStackAndAlignmentPass> {
public:
    explicit GuestStackAndAlignmentPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "guest-stack-alignment-pass"; }
    const char* pass_name() const override { return "guest-stack-alignment-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 3. AArch64IdiomDeconstructPass
class AArch64IdiomDeconstructPass final : public ModulePass, public PassInfoMixin<AArch64IdiomDeconstructPass> {
public:
    explicit AArch64IdiomDeconstructPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "aarch64-idiom-deconstruct-pass"; }
    const char* pass_name() const override { return "aarch64-idiom-deconstruct-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 4. JumpTableLoweringPass
class JumpTableLoweringPass final : public ModulePass, public PassInfoMixin<JumpTableLoweringPass> {
public:
    static const char* static_name() { return "jump-table-lowering-pass"; }
    const char* pass_name() const override { return "jump-table-lowering-pass"; }
    bool run(Module& M) override;
};

// 5. FCMPPredicateFusionPass
class FCMPPredicateFusionPass final : public ModulePass, public PassInfoMixin<FCMPPredicateFusionPass> {
public:
    explicit FCMPPredicateFusionPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "fcmp-predicate-fusion-pass"; }
    const char* pass_name() const override { return "fcmp-predicate-fusion-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 6. NZCVPredicateFusionPass
class NZCVPredicateFusionPass final : public ModulePass, public PassInfoMixin<NZCVPredicateFusionPass> {
public:
    explicit NZCVPredicateFusionPass(bool enabled = true, bool enable_interblock_dse = true)
        : enabled_(enabled), enable_interblock_dse_(enable_interblock_dse) {}
    static const char* static_name() { return "nzcv-predicate-fusion-pass"; }
    const char* pass_name() const override { return "nzcv-predicate-fusion-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
    bool enable_interblock_dse_;
};

// 7. NZCVDeadFlagElimPass
class NZCVDeadFlagElimPass final : public ModulePass, public PassInfoMixin<NZCVDeadFlagElimPass> {
public:
    explicit NZCVDeadFlagElimPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "nzcv-dead-flag-elim-pass"; }
    const char* pass_name() const override { return "nzcv-dead-flag-elim-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 8. LinkRegisterElisionPass
class LinkRegisterElisionPass final : public ModulePass, public PassInfoMixin<LinkRegisterElisionPass> {
public:
    explicit LinkRegisterElisionPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "link-register-elision-pass"; }
    const char* pass_name() const override { return "link-register-elision-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 9. TailCallDevirtualizationPass
class TailCallDevirtualizationPass final : public ModulePass, public PassInfoMixin<TailCallDevirtualizationPass> {
public:
    explicit TailCallDevirtualizationPass(bool enabled = true) : enabled_(enabled) {}
    static const char* static_name() { return "tail-call-devirt-pass"; }
    const char* pass_name() const override { return "tail-call-devirt-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
};

// 10. AAPCS64ABIContractPass
class AAPCS64ABIContractPass final : public ModulePass, public PassInfoMixin<AAPCS64ABIContractPass> {
public:
    explicit AAPCS64ABIContractPass(bool enabled = true, bool allow_external_scratch_elim = false)
        : enabled_(enabled), allow_external_scratch_elim_(allow_external_scratch_elim) {}
    static const char* static_name() { return "aapcs64-abi-contract-pass"; }
    const char* pass_name() const override { return "aapcs64-abi-contract-pass"; }
    bool run(Module& M) override;
private:
    bool enabled_;
    bool allow_external_scratch_elim_;
};

// 11. CpuStatePromotionPass
class CpuStatePromotionPass final : public ModulePass, public PassInfoMixin<CpuStatePromotionPass> {
public:
    explicit CpuStatePromotionPass(bool enable_entry_kill_pruning = true,
                                   bool enable_interproc_pruning = true)
        : enable_entry_kill_pruning_(enable_entry_kill_pruning),
          enable_interproc_pruning_(enable_interproc_pruning) {}
    static const char* static_name() { return "cpu-state-promotion-pass"; }
    const char* pass_name() const override { return "cpu-state-promotion-pass"; }
    bool run(Module& M) override;
private:
    bool enable_entry_kill_pruning_;
    bool enable_interproc_pruning_;
};

struct PassPipelineConfig {
    LoweringMode mode = LoweringMode::UserspaceBionicSo;
    bool enable_stack_alignment = true;
    bool enable_idiom_deconstruct = true;
    bool enable_fcmp_fusion = true;
    bool enable_nzcv_fusion = true;
    bool enable_nzcv_interblock_dse = true;
    bool enable_nzcv_dead_flag_elim = true;
    bool enable_link_reg_elision = true;
    bool enable_tail_call_devirt = true;
    bool enable_aapcs64_abi_contract = true;
    bool allow_external_scratch_elim = false;
    bool enable_cpu_state_promotion = true;
    bool enable_cpu_entry_kill_pruning = true;
    bool enable_cpu_interproc_pruning = true;
    bool enable_kernel_va_widening = false;
    bool enable_kernel_rodata_direct_map = false;
    bool enable_kernel_scs_and_heap_guard = false;
    bool enable_kernel_host_abi_unmarshal = false;

    static PassPipelineConfig forUserspaceSo() {
        PassPipelineConfig c;
        c.mode = LoweringMode::UserspaceBionicSo;
        c.enable_all_passes();
        return c;
    }

    static PassPipelineConfig forKernelKo() {
        PassPipelineConfig c;
        c.mode = LoweringMode::KernelModuleKo;
        c.enable_stack_alignment = true;
        c.enable_idiom_deconstruct = true;
        c.enable_fcmp_fusion = false; // Linux kernel compiled with -mgeneral-regs-only
        c.enable_nzcv_fusion = true;
        c.enable_nzcv_interblock_dse = true;
        c.enable_nzcv_dead_flag_elim = true;
        c.enable_link_reg_elision = false; // Preserve Shadow Call Stack (x18) integrity
        c.enable_tail_call_devirt = true;
        c.enable_aapcs64_abi_contract = true;
        c.enable_cpu_state_promotion = true;
        c.enable_kernel_va_widening = true;
        c.enable_kernel_rodata_direct_map = true;
        c.enable_kernel_scs_and_heap_guard = true;
        c.enable_kernel_host_abi_unmarshal = true;
        return c;
    }

    void disable_high_risk_passes();
    void enable_all_passes();
    void disable_all_opt_passes();
    bool set_pass_enabled(const std::string& name, bool en);
};

// Build the unified AArch64-to-LLVM pass pipeline
void build_aarch64_lowering_pipeline(ModulePassManager& MPM, const PassPipelineConfig& cfg = {});
inline void build_ultratears_pipeline(ModulePassManager& MPM, const PassPipelineConfig& cfg = {}) {
    build_aarch64_lowering_pipeline(MPM, cfg);
}

// Build the minimal structural lowering pipeline (ARMv8 sys/barriers + JumpTables only)
void build_raw_lowering_pipeline(ModulePassManager& MPM, const PassPipelineConfig& cfg = {});

// Produce a standalone, compilable unoptimized LLVM IR string from a freshly-lifted Module
std::string serialize_unlowered_reference_ir(const Module& M, const PassPipelineConfig& cfg = {});

} // namespace ultrs::passes
