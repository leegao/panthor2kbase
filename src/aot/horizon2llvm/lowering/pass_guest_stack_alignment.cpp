// UltraTears LLVM Pass Implementation: Pass 11 - GuestStackAndAlignmentPass (src/horizon2llvm/lowering/pass_guest_stack_alignment.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include "passes.hpp"
#include "passes_llvm_utils.hpp"

namespace ultrs::passes {

namespace {

class GuestStackAndAlignmentPassImpl {
public:
    explicit GuestStackAndAlignmentPassImpl(bool enabled = true) : enabled_(enabled) {}

    static const char* static_name() { return "guest-stack-alignment-pass"; }
    const char* pass_name() const { return "guest-stack-alignment-pass"; }

    bool run(Module& M) {
        if (!enabled_) return false;
        bool changed = false;
        for (auto& fn : M.functions) {
            if (!fn->hle_symbol.empty() || fn->blocks.empty()) continue;

            auto const_align = [](uint64_t v) -> int {
                if (v == 0) return 16;
                uint64_t lsb = v & (~v + 1ULL);
                return static_cast<int>(std::min<uint64_t>(16, lsb));
            };

            size_t n_blocks = fn->blocks.size();
            std::unordered_map<std::string, size_t> block_idx;
            block_idx.reserve(n_blocks * 2);
            for (size_t i = 0; i < n_blocks; ++i) {
                block_idx[fn->blocks[i]->name] = i;
            }

            std::vector<std::vector<size_t>> preds(n_blocks);
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                const BasicBlock& bb = *fn->blocks[bi];
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    auto add_edge = [&](const std::string& lbl) {
                        auto sit = block_idx.find(lbl);
                        if (sit != block_idx.end()) {
                            preds[sit->second].push_back(bi);
                        }
                    };
                    if (it->tag == OpTag::UncondBranch && !it->target_label.empty()) {
                        add_edge(it->target_label);
                    } else if (it->tag == OpTag::CondBranch) {
                        if (!it->target_label.empty()) add_edge(it->target_label);
                        if (!it->false_label.empty()) add_edge(it->false_label);
                    } else if (it->text.rfind("switch i64 ", 0) == 0) {
                        size_t pos = 0;
                        while ((pos = it->text.find("label %", pos)) != std::string::npos) {
                            pos += 7;
                            size_t end = pos;
                            while (end < it->text.size()) {
                                char c = it->text[end];
                                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '_' || c == '.') {
                                    ++end;
                                } else {
                                    break;
                                }
                            }
                            add_edge(it->text.substr(pos, end - pos));
                            pos = end;
                        }
                    }
                    break;
                }
            }

            // 1. Forward dataflow propagation of stack provenance (`gpr_stack`) and alignment (`gpr_align`)
            //    across basic blocks so stack-derived pointers (`x24 = sp`, `x16 = sp`, `x9 = x16 + 120`,
            //    `x25..x27 = x16 + 144..176`, etc.) are recognized in all dominated blocks.
            std::vector<bool> visited(n_blocks, false);
            std::vector<std::unordered_map<int, int>> in_align(n_blocks), out_align(n_blocks);
            std::vector<std::unordered_set<int>> in_stack(n_blocks), out_stack(n_blocks);

            in_align[0][31] = 16;
            in_stack[0].insert(31);

            auto simulate_block = [&](size_t bi,
                                      const std::unordered_map<int, int>& start_align,
                                      const std::unordered_set<int>& start_stack,
                                      std::unordered_map<int, int>& end_align,
                                      std::unordered_set<int>& end_stack,
                                      bool rewrite_insts) {
                end_align = start_align;
                end_stack = start_stack;
                end_align[31] = 16;
                end_stack.insert(31);

                std::unordered_map<std::string, int> ssa_align;
                std::unordered_set<std::string> ssa_stack;

                auto get_align = [&](const Value& v) -> int {
                    if (v.is_const) return const_align(v.const_u64);
                    auto it = ssa_align.find(v.repr);
                    return (it != ssa_align.end()) ? it->second : 1;
                };

                BasicBlock& bb = *fn->blocks[bi];
                for (auto& inst : bb.insts) {
                    if (inst.dead) continue;

                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                        inst.tag == OpTag::SvcTrap) {
                        // Calls preserve AAPCS64 callee-saved registers (X19..X29) and SP (31)
                        for (auto it = end_align.begin(); it != end_align.end(); ) {
                            int r = it->first;
                            if ((r >= 19 && r <= 29) || r == 31) ++it;
                            else it = end_align.erase(it);
                        }
                        for (auto it = end_stack.begin(); it != end_stack.end(); ) {
                            int r = *it;
                            if ((r >= 19 && r <= 29) || r == 31) ++it;
                            else it = end_stack.erase(it);
                        }
                        end_align[31] = 16;
                        end_stack.insert(31);
                        continue;
                    }

                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0 && inst.arch_reg <= 31) {
                        auto it = end_align.find(inst.arch_reg);
                        if (it != end_align.end()) ssa_align[inst.dst_reg] = it->second;
                        if (end_stack.count(inst.arch_reg)) ssa_stack.insert(inst.dst_reg);
                        continue;
                    }

                    if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0 && inst.arch_reg <= 31) {
                        int a = get_align(inst.op0);
                        if (inst.arch_reg == 31) a = std::max(a, 16);
                        end_align[inst.arch_reg] = a;
                        if (ssa_stack.count(inst.op0.repr) || inst.arch_reg == 31) {
                            end_stack.insert(inst.arch_reg);
                        } else {
                            end_stack.erase(inst.arch_reg);
                        }
                        continue;
                    }

                    if (inst.tag == OpTag::RawIR && !inst.dst_reg.empty()) {
                        if (inst.text.find(" = add ") != std::string::npos ||
                            inst.text.find(" = sub ") != std::string::npos ||
                            inst.text.find(" = or ") != std::string::npos) {
                            int a0 = get_align(inst.op0);
                            int a1 = get_align(inst.op1);
                            ssa_align[inst.dst_reg] = std::gcd(a0, a1);
                            if (ssa_stack.count(inst.op0.repr) || ssa_stack.count(inst.op1.repr)) {
                                ssa_stack.insert(inst.dst_reg);
                            }
                        } else if (inst.text.find(" = shl ") != std::string::npos && inst.op1.is_const && inst.op1.const_u64 < 64) {
                            int shifted_align = const_align(1ULL << inst.op1.const_u64);
                            ssa_align[inst.dst_reg] = std::min(16, get_align(inst.op0) * shifted_align);
                        }
                        continue;
                    }

                    if (rewrite_insts && (inst.tag == OpTag::GuestMemLoad || inst.tag == OpTag::GuestMemStore)) {
                        TypeKind mt = (inst.tag == OpTag::GuestMemLoad) ? inst.dst_type : inst.op1.type;
                        int natural_align = (mt == TypeKind::Int8)   ? 1
                                          : (mt == TypeKind::Int16)  ? 2
                                          : (mt == TypeKind::Int32)  ? 4
                                          : (mt == TypeKind::Int64)  ? 8
                                          : (mt == TypeKind::Int128) ? 16 : 1;
                        int proven = std::min(natural_align, get_align(inst.op0));
                        bool on_stack = ssa_stack.count(inst.op0.repr) > 0;
                        inst.is_stack_access = on_stack;

                        if (proven > 1) {
                            inst.align_bytes = proven;
                            auto pos = inst.text.find(", align 1");
                            if (pos != std::string::npos) {
                                inst.text.replace(pos, 9, ", align " + std::to_string(proven));
                                changed = true;
                            }
                        }
                        if (on_stack && n_blocks <= 32) {
                            auto scope_pos = inst.text.find("!alias.scope !4");
                            if (scope_pos != std::string::npos) {
                                inst.text.replace(scope_pos, 15, "!alias.scope !6");
                                changed = true;
                            }
                        }
                    }
                }
            };

            bool df_changed = true;
            int iter = 0;
            while (df_changed && iter++ < 16) {
                df_changed = false;
                for (size_t bi = 0; bi < n_blocks; ++bi) {
                    std::unordered_map<int, int> merged_align;
                    std::unordered_set<int> merged_stack;
                    if (bi == 0) {
                        merged_align[31] = 16;
                        merged_stack.insert(31);
                    } else {
                        bool first_pred = true;
                        for (size_t p : preds[bi]) {
                            if (!visited[p]) continue;
                            if (first_pred) {
                                merged_align = out_align[p];
                                merged_stack = out_stack[p];
                                first_pred = false;
                            } else {
                                for (auto it = merged_align.begin(); it != merged_align.end(); ) {
                                    auto pit = out_align[p].find(it->first);
                                    if (pit == out_align[p].end()) {
                                        it = merged_align.erase(it);
                                    } else {
                                        it->second = std::gcd(it->second, pit->second);
                                        if (it->second <= 1) it = merged_align.erase(it);
                                        else ++it;
                                    }
                                }
                                for (auto it = merged_stack.begin(); it != merged_stack.end(); ) {
                                    if (!out_stack[p].count(*it)) it = merged_stack.erase(it);
                                    else ++it;
                                }
                            }
                        }
                        if (first_pred) {
                            merged_align[31] = 16;
                            merged_stack.insert(31);
                        }
                    }

                    std::unordered_map<int, int> new_out_align;
                    std::unordered_set<int> new_out_stack;
                    simulate_block(bi, merged_align, merged_stack, new_out_align, new_out_stack, false);

                    if (!visited[bi] || in_align[bi] != merged_align || in_stack[bi] != merged_stack ||
                        out_align[bi] != new_out_align || out_stack[bi] != new_out_stack) {
                        visited[bi] = true;
                        in_align[bi] = std::move(merged_align);
                        in_stack[bi] = std::move(merged_stack);
                        out_align[bi] = std::move(new_out_align);
                        out_stack[bi] = std::move(new_out_stack);
                        df_changed = true;
                    }
                }
            }

            // 2. Apply proven alignment & stack alias scope to each basic block
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                std::unordered_map<int, int> dummy_align;
                std::unordered_set<int> dummy_stack;
                simulate_block(bi, in_align[bi], in_stack[bi], dummy_align, dummy_stack, true);
            }
        }
        return changed;
    }

private:
    bool enabled_;
};


} // namespace

bool GuestStackAndAlignmentPass::run(Module& M) {
    GuestStackAndAlignmentPassImpl impl(enabled_);
    return impl.run(M);
}

} // namespace ultrs::passes
