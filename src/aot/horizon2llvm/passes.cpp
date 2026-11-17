// Scoped-Down AArch64 -> LLVM Pass Pipeline Orchestration (src/aot/horizon2llvm/passes.cpp)

#include "passes.hpp"

namespace ultrs::passes {

namespace {

constexpr uint32_t pack_sys(unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2) {
    return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}

void lower_sysreg_read(Function& fn, BasicBlock& bb, size_t idx) {
    Instruction& inst = bb.insts[idx];
    int rt = inst.arch_reg;
    if (rt == 31) {
        inst.dead = true;
        return;
    }
    uint32_t k = inst.sys_key;
    std::string val_repr = "0";
    Instruction pre_inst;
    bool has_pre = false;

    if (k == pack_sys(3, 3, 13, 0, 2) || k == pack_sys(3, 3, 13, 0, 3)) { // TPIDR_EL0 / TPIDRRO_EL0
        std::string off_str = (k == pack_sys(3, 3, 13, 0, 2)) ? "280" : "288";
        std::string p = fn.next_reg("tp_p");
        std::string v = fn.next_reg("tp");
        pre_inst.tag = OpTag::RawIR;
        pre_inst.dst_reg = v;
        pre_inst.dst_type = TypeKind::Int64;
        pre_inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 " + off_str + "\n  " +
                        v + " = load i64, ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
        has_pre = true;
        val_repr = v;
    } else if (k == pack_sys(3, 3, 4, 2, 0)) { // NZCV -> arch_reg 32
        std::string v = fn.next_reg("nzcv_v");
        std::string p = v + ".p";
        pre_inst.tag = OpTag::CpuRegRead;
        pre_inst.arch_reg = 32;
        pre_inst.dst_reg = v;
        pre_inst.dst_type = TypeKind::Int64;
        pre_inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 272\n  " +
                        v + " = load i64, ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
        has_pre = true;
        val_repr = v;
    } else if (k == pack_sys(3, 3, 4, 4, 0) || k == pack_sys(3, 3, 4, 4, 1)) { // FPCR / FPSR
        std::string off_str = (k == pack_sys(3, 3, 4, 4, 0)) ? "320" : "328";
        std::string p = fn.next_reg("fp_p");
        std::string v = fn.next_reg("fp_v");
        pre_inst.tag = OpTag::RawIR;
        pre_inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 " + off_str + "\n  " +
                        v + " = load i64, ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
        has_pre = true;
        val_repr = v;
    } else if (k == pack_sys(3, 3, 0, 0, 7)) { // DCZID_EL0
        val_repr = "4";
    } else if (k == pack_sys(3, 3, 0, 0, 1)) { // CTR_EL0
        val_repr = "2219098116";
    }

    std::string wp = fn.next_reg("mrsw");
    inst.tag = OpTag::CpuRegWrite;
    inst.arch_reg = rt;
    inst.op0 = Value::reg(TypeKind::Int64, val_repr);
    inst.text = wp + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(rt * 8) + "\n  " +
                "store i64 " + val_repr + ", ptr " + wp + ", align 8, !alias.scope !3, !noalias !4";
    if (has_pre) {
        bb.insts.insert(bb.insts.begin() + static_cast<ptrdiff_t>(idx), std::move(pre_inst));
    }
}

void lower_sysreg_write(Function& fn, BasicBlock& bb, size_t idx) {
    Instruction& inst = bb.insts[idx];
    uint32_t k = inst.sys_key;
    std::string v = inst.op0.repr;
    if (k == pack_sys(3, 3, 13, 0, 2)) { // TPIDR_EL0
        std::string p = fn.next_reg("msrp");
        inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 280\n  "
                    "store i64 " + v + ", ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
        inst.tag = OpTag::RawIR;
    } else if (k == pack_sys(3, 3, 4, 2, 0)) { // NZCV
        std::string m = fn.next_reg("nzcv_m");
        std::string p = fn.next_reg("nzcv_p");
        std::string pre = m + " = and i64 " + v + ", 4026531840";
        inst.tag = OpTag::FlagWrite;
        inst.arch_reg = 32;
        inst.op0 = Value::reg(TypeKind::Int64, m);
        inst.prefix_ir = pre;
        inst.text = pre + "\n  " + p + " = getelementptr inbounds i8, ptr %c, i64 272\n  "
                    "store i64 " + m + ", ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
    } else if (k == pack_sys(3, 3, 4, 4, 0) || k == pack_sys(3, 3, 4, 4, 1)) { // FPCR / FPSR
        std::string off_str = (k == pack_sys(3, 3, 4, 4, 0)) ? "320" : "328";
        std::string p = fn.next_reg("fpp");
        inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 " + off_str + "\n  "
                    "store i64 " + v + ", ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
        inst.tag = OpTag::RawIR;
    } else {
        inst.dead = true;
    }
}

} // namespace

bool AArch64SysBarrierLoweringPass::run(Module& M) {
    bool changed = false;
    for (auto& fn : M.functions) {
        for (auto& bb : fn->blocks) {
            for (size_t i = 0; i < bb->insts.size(); ++i) {
                Instruction& inst = bb->insts[i];
                if (inst.dead) continue;
                switch (inst.tag) {
                    case OpTag::SvcTrap: {
                        inst.text = "fence seq_cst";
                        inst.tag = OpTag::RawIR;
                        changed = true;
                        break;
                    }
                    case OpTag::BrkTrap:
                    case OpTag::UdfTrap: {
                        inst.text = "unreachable";
                        changed = true;
                        break;
                    }
                    case OpTag::MemBarrier: {
                        if (inst.sys_key == 2) { // CLREX
                            std::string p = fn->next_reg("clrex");
                            inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 296\n  "
                                        "store i64 -1, ptr " + p + ", align 8, !alias.scope !3, !noalias !4";
                        } else {
                            inst.text = "fence seq_cst";
                        }
                        inst.tag = OpTag::RawIR;
                        changed = true;
                        break;
                    }
                    case OpTag::DcZva: {
                        std::string aligned = fn->next_reg("zva_a");
                        std::string ptr_r   = fn->next_reg("zva_p");
                        inst.text = aligned + " = and i64 " + inst.op0.repr + ", -64\n  " +
                                    ptr_r + " = inttoptr i64 " + aligned + " to ptr\n  " +
                                    "call void @llvm.memset.p0.i64(ptr align 64 " + ptr_r + ", i8 0, i64 64, i1 false)";
                        inst.tag = OpTag::RawIR;
                        changed = true;
                        break;
                    }
                    case OpTag::SysRegRead: {
                        lower_sysreg_read(*fn, *bb, i);
                        changed = true;
                        break;
                    }
                    case OpTag::SysRegWrite: {
                        lower_sysreg_write(*fn, *bb, i);
                        changed = true;
                        break;
                    }
                    case OpTag::GuestMemLoad:
                    case OpTag::GuestMemStore: {
                        if (inst.text.find("!noalias") == std::string::npos) {
                            inst.text += ", !alias.scope !4, !noalias !3";
                            changed = true;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
    return changed;
}

void PassPipelineConfig::disable_high_risk_passes() {
    enable_nzcv_interblock_dse = false;
    enable_link_reg_elision = false;
    enable_tail_call_devirt = false;
    enable_aapcs64_abi_contract = false;
    allow_external_scratch_elim = false;
    enable_cpu_interproc_pruning = false;
}

void PassPipelineConfig::enable_all_passes() {
    enable_stack_alignment = true;
    enable_idiom_deconstruct = true;
    enable_fcmp_fusion = true;
    enable_nzcv_fusion = true;
    enable_nzcv_interblock_dse = true;
    enable_nzcv_dead_flag_elim = true;
    enable_link_reg_elision = true;
    enable_tail_call_devirt = true;
    enable_aapcs64_abi_contract = true;
    allow_external_scratch_elim = true;
    enable_cpu_state_promotion = true;
    enable_cpu_entry_kill_pruning = true;
    enable_cpu_interproc_pruning = true;
}

void PassPipelineConfig::disable_all_opt_passes() {
    disable_high_risk_passes();
    enable_stack_alignment = false;
    enable_idiom_deconstruct = false;
    enable_fcmp_fusion = false;
    enable_nzcv_fusion = false;
    enable_nzcv_dead_flag_elim = false;
    enable_cpu_state_promotion = false;
    enable_cpu_entry_kill_pruning = false;
}

bool PassPipelineConfig::set_pass_enabled(const std::string& name, bool en) {
    if (name == "high-risk" || name == "high_risk") {
        if (!en) disable_high_risk_passes();
        else {
            enable_nzcv_interblock_dse = true;
            enable_link_reg_elision = true;
            enable_tail_call_devirt = true;
            enable_aapcs64_abi_contract = true;
            allow_external_scratch_elim = true;
            enable_cpu_interproc_pruning = true;
        }
        return true;
    }
    if (name == "all-opt" || name == "all_opt" || name == "opt") {
        if (en) enable_all_passes();
        else disable_all_opt_passes();
        return true;
    }
    if (name == "stack-alignment" || name == GuestStackAndAlignmentPass::static_name()) {
        enable_stack_alignment = en; return true;
    }
    if (name == "idiom-deconstruct" || name == AArch64IdiomDeconstructPass::static_name()) {
        enable_idiom_deconstruct = en; return true;
    }
    if (name == "fcmp-fusion" || name == FCMPPredicateFusionPass::static_name()) {
        enable_fcmp_fusion = en; return true;
    }
    if (name == "nzcv-fusion" || name == NZCVPredicateFusionPass::static_name()) {
        enable_nzcv_fusion = en; return true;
    }
    if (name == "nzcv-interblock-dse") {
        enable_nzcv_interblock_dse = en; return true;
    }
    if (name == "nzcv-dead-flag-elim" || name == NZCVDeadFlagElimPass::static_name()) {
        enable_nzcv_dead_flag_elim = en; return true;
    }
    if (name == "link-reg-elision" || name == LinkRegisterElisionPass::static_name()) {
        enable_link_reg_elision = en; return true;
    }
    if (name == "tail-call-devirt" || name == TailCallDevirtualizationPass::static_name()) {
        enable_tail_call_devirt = en; return true;
    }
    if (name == "aapcs64-abi" || name == AAPCS64ABIContractPass::static_name()) {
        enable_aapcs64_abi_contract = en;
        allow_external_scratch_elim = en;
        return true;
    }
    if (name == "cpu-state-promotion" || name == CpuStatePromotionPass::static_name()) {
        enable_cpu_state_promotion = en; return true;
    }
    if (name == "cpu-entry-kill-pruning") {
        enable_cpu_entry_kill_pruning = en; return true;
    }
    if (name == "cpu-interproc-pruning") {
        enable_cpu_interproc_pruning = en; return true;
    }
    return false;
}

void build_aarch64_lowering_pipeline(ModulePassManager& MPM, const PassPipelineConfig& cfg) {
    MPM.addPass(std::make_unique<AArch64SysBarrierLoweringPass>(cfg.mode));
    MPM.addPass(std::make_unique<GuestStackAndAlignmentPass>(cfg.enable_stack_alignment));
    MPM.addPass(std::make_unique<AArch64IdiomDeconstructPass>(cfg.enable_idiom_deconstruct));
    MPM.addPass(std::make_unique<JumpTableLoweringPass>());
    MPM.addPass(std::make_unique<FCMPPredicateFusionPass>(cfg.enable_fcmp_fusion));
    MPM.addPass(std::make_unique<NZCVPredicateFusionPass>(cfg.enable_nzcv_fusion, cfg.enable_nzcv_interblock_dse));
    MPM.addPass(std::make_unique<NZCVDeadFlagElimPass>(cfg.enable_nzcv_dead_flag_elim));
    MPM.addPass(std::make_unique<LinkRegisterElisionPass>(cfg.enable_link_reg_elision));
    MPM.addPass(std::make_unique<TailCallDevirtualizationPass>(cfg.enable_tail_call_devirt));
    MPM.addPass(std::make_unique<AAPCS64ABIContractPass>(
        cfg.enable_aapcs64_abi_contract,
        cfg.allow_external_scratch_elim || cfg.enable_aapcs64_abi_contract));
    if (cfg.enable_cpu_state_promotion) {
        MPM.addPass(std::make_unique<CpuStatePromotionPass>(cfg.enable_cpu_entry_kill_pruning, cfg.enable_cpu_interproc_pruning));
    }
}

void build_raw_lowering_pipeline(ModulePassManager& MPM, const PassPipelineConfig&) {
    MPM.addPass(std::make_unique<AArch64SysBarrierLoweringPass>());
    MPM.addPass(std::make_unique<JumpTableLoweringPass>());
}

std::string serialize_unlowered_reference_ir(const Module& M, const PassPipelineConfig& cfg) {
    Module copy_mod(M.name + "_raw", M.target_triple);
    copy_mod.external_guest_fns = M.external_guest_fns;
    copy_mod.external_hle_fns = M.external_hle_fns;
    copy_mod.global_defs = M.global_defs;
    copy_mod.chunk_meta = M.chunk_meta;
    copy_mod.needs_noalias_md = M.needs_noalias_md;
    for (const auto& fn : M.functions) {
        Function* cfn = copy_mod.create_function(fn->name, fn->entry_pc, fn->end_pc);
        cfn->linkage = fn->linkage;
        cfn->hle_symbol = fn->hle_symbol;
        cfn->has_all_labels = fn->has_all_labels;
        cfn->jump_tables = fn->jump_tables;
        cfn->label_pcs = fn->label_pcs;
        cfn->direct_callee_pcs = fn->direct_callee_pcs;
        cfn->raw_words = fn->raw_words;
        cfn->next_ssa_id = fn->next_ssa_id;
        for (const auto& bb : fn->blocks) {
            BasicBlock* cbb = cfn->create_block(bb->name, bb->start_pc);
            cbb->insts = bb->insts;
        }
    }
    ModulePassManager raw_mpm;
    build_raw_lowering_pipeline(raw_mpm, cfg);
    raw_mpm.run(copy_mod);
    return copy_mod.serialize();
}

} // namespace ultrs::passes
