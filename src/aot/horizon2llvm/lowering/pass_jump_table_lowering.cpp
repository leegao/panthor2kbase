// UltraTears LLVM Pass Implementation: Pass 13 - JumpTableLoweringPass (src/horizon2llvm/lowering/pass_jump_table_lowering.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include "passes.hpp"
#include "passes_llvm_utils.hpp"

namespace ultrs::passes {

namespace {

class JumpTableLoweringPassImpl {
public:
    static const char* static_name() { return "jump-table-lowering-pass"; }
    const char* pass_name() const { return "jump-table-lowering-pass"; }

    bool run(Module& M) {
        bool changed = false;
        for (auto& fn : M.functions) {
            bool has_unreach = false;
            for (const auto& bb : fn->blocks) {
                if (bb->name.rfind("_unreach") != std::string::npos && !bb->insts.empty()) {
                    has_unreach = true;
                    break;
                }
            }
            bool needs_dispatch_bb = fn->has_all_labels;
            size_t orig_block_count = fn->blocks.size();

            for (size_t bi = 0; bi < orig_block_count; ++bi) {
                BasicBlock* bb = fn->blocks[bi].get();
                for (size_t ii = 0; ii < bb->insts.size(); ++ii) {
                    Instruction& inst = bb->insts[ii];
                    if (inst.dead || inst.tag != OpTag::IndirectJump) continue;

                    uint64_t br_pc = inst.guest_pc;
                    std::string target_repr = inst.op0.repr;
                    auto jt_it = fn->jump_tables.find(br_pc);

                    if (jt_it != fn->jump_tables.end() && jt_it->second.is_all_labels) {
                        std::string p = fn->next_reg("pcp");
                        inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 264\n  "
                                    "store i64 " + target_repr + ", ptr " + p + ", align 8, !alias.scope !3, !noalias !4\n  "
                                    "br label %L_dispatch";
                        needs_dispatch_bb = true;
                        changed = true;
                    } else if (jt_it != fn->jump_tables.end() && !jt_it->second.targets.empty()) {
                        std::string def_name = block_label(br_pc) + "_jt_def";
                        BasicBlock* def_bb = fn->create_block(def_name, br_pc);

                        PassLLVMBuilder PB(*fn, bb);
                        std::vector<std::pair<uint64_t, std::string>> cases;
                        cases.reserve(jt_it->second.targets.size());
                        for (uint64_t t : jt_it->second.targets) {
                            cases.emplace_back(t, block_label(t));
                        }
                        inst.text = PB.emit_switch(target_repr, def_bb->name, cases);

                        IRBuilder defB(fn.get(), def_bb);
                        defB.write_cpu_field_i64(264, inst.op0);
                        if (fn->has_all_labels || has_unreach) {
                            defB.emit_raw("br label %L_dispatch", OpTag::UncondBranch);
                            needs_dispatch_bb = true;
                        } else {
                            defB.emit_raw("musttail call void @rt_dispatch(ptr %c)\n  ret void", OpTag::Return);
                        }
                        changed = true;
                    } else {
                        std::string p = fn->next_reg("pcp");
                        inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 264\n  "
                                    "store i64 " + target_repr + ", ptr " + p + ", align 8, !alias.scope !3, !noalias !4\n  "
                                    "musttail call void @rt_dispatch(ptr %c)\n  ret void";
                        inst.tag = OpTag::Return;
                        changed = true;
                    }
                }
            }

            if (needs_dispatch_bb && !fn->get_block("L_dispatch")) {
                BasicBlock* disp_bb = fn->create_block("L_dispatch", fn->end_pc);
                BasicBlock* disp_def = fn->create_block("L_dispatch_ext", fn->end_pc);

                IRBuilder B(fn.get(), disp_bb);
                Value pc_val = B.read_cpu_field_i64(264);
                PassLLVMBuilder PB(*fn, disp_bb);
                std::vector<std::pair<uint64_t, std::string>> cases;
                if (fn->has_all_labels) {
                    for (uint64_t t = fn->entry_pc; t < fn->end_pc; t += 4) {
                        cases.emplace_back(t, block_label(t));
                    }
                } else {
                    std::set<uint64_t> seen_pcs;
                    for (const auto& block : fn->blocks) {
                        if (block->name == "entry" || block->name == "L_dispatch" ||
                            block->name == "L_dispatch_ext" ||
                            block->name.rfind("_jt_def") != std::string::npos) {
                            continue;
                        }
                        if (block->start_pc >= fn->entry_pc && block->start_pc < fn->end_pc) {
                            if (seen_pcs.insert(block->start_pc).second) {
                                cases.emplace_back(block->start_pc, block->name);
                            }
                        }
                    }
                    std::sort(cases.begin(), cases.end());
                }
                B.emit_raw(PB.emit_switch(pc_val.repr, disp_def->name, cases), OpTag::IndirectJump);

                B.set_insert_point(disp_def);
                B.emit_raw("musttail call void @rt_dispatch(ptr %c)\n  ret void", OpTag::Return);

                BasicBlock* entry_bb = fn->get_block("entry");
                if (entry_bb && !entry_bb->insts.empty()) {
                    Instruction& last_inst = entry_bb->insts.back();
                    if (last_inst.tag == OpTag::UncondBranch &&
                        last_inst.text == "br label %" + block_label(fn->entry_pc)) {
                        std::string p = fn->next_reg("pcp");
                        std::string cur_pc = fn->next_reg("cur_pc");
                        std::string cmp_reg = fn->next_reg("is_entry");
                        last_inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 264\n  " +
                                         cur_pc + " = load i64, ptr " + p + ", align 8, !alias.scope !3, !noalias !4\n  " +
                                         cmp_reg + " = icmp eq i64 " + cur_pc + ", " + std::to_string(fn->entry_pc) + "\n  " +
                                         "br i1 " + cmp_reg + ", label %" + block_label(fn->entry_pc) + ", label %L_dispatch";
                        last_inst.tag = OpTag::CondBranch;
                    }
                }

                changed = true;
            }
        }
        return changed;
    }
};


} // namespace

bool JumpTableLoweringPass::run(Module& M) {
    JumpTableLoweringPassImpl impl;
    return impl.run(M);
}

} // namespace ultrs::passes
