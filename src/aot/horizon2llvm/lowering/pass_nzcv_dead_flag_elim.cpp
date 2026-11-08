// UltraTears LLVM Pass Implementation: Pass 17 - NZCVDeadFlagElimPass (src/horizon2llvm/lowering/pass_nzcv_dead_flag_elim.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include <sstream>
#include <unordered_set>
#include "passes.hpp"
#include "passes_llvm_utils.hpp"

namespace ultrs::passes {

namespace {

class NZCVDeadFlagElimPassImpl {
public:
    explicit NZCVDeadFlagElimPassImpl(bool enabled = true) : enabled_(enabled) {}

    static const char* static_name() { return "nzcv-dead-flag-elim-pass"; }
    const char* pass_name() const { return "nzcv-dead-flag-elim-pass"; }

    bool run(Module& M) {
        if (!enabled_) return false;
        bool changed = false;
        for (auto& fn : M.functions) {
            if (!fn->hle_symbol.empty() || fn->blocks.empty()) continue;

            const size_t n_blocks = fn->blocks.size();
            std::unordered_map<std::string, size_t> block_idx;
            block_idx.reserve(n_blocks);
            for (size_t i = 0; i < n_blocks; ++i) {
                block_idx[fn->blocks[i]->name] = i;
            }

            // 1. Compute block-level transfer functions and CFG successors for NZCV (`arch_reg == 32`):
            //    Under AAPCS64, NZCV is caller-saved scratch state that is killed across function calls
            //    (`DirectCall`, `IndirectCall`, `SvcTrap`) and function returns (`Return`, `BrkTrap`, `UdfTrap`).
            std::vector<bool> gen(n_blocks, false);  // NZCV is upward-exposed read in block
            std::vector<bool> kill(n_blocks, false); // NZCV is overwritten or killed in block before any read
            std::vector<std::vector<size_t>> succs(n_blocks);
            std::vector<bool> has_unresolved_indirect_br(n_blocks, false);

            for (size_t bi = 0; bi < n_blocks; ++bi) {
                const BasicBlock& bb = *fn->blocks[bi];
                bool live = false;
                bool killed_all = false;
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead) continue;
                    if (it->tag == OpTag::FlagWrite ||
                        (it->tag == OpTag::CpuRegWrite && it->arch_reg == 32)) {
                        live = false;
                        killed_all = true;
                    } else if (it->tag == OpTag::CpuRegRead && it->arch_reg == 32) {
                        live = true;
                    } else if (it->tag == OpTag::DirectCall || it->tag == OpTag::IndirectCall ||
                               it->tag == OpTag::SvcTrap || it->tag == OpTag::Return ||
                               it->tag == OpTag::BrkTrap || it->tag == OpTag::UdfTrap ||
                               it->text.find("musttail call") != std::string::npos ||
                               it->text == "ret void") {
                        live = false;
                        killed_all = true;
                    }
                }
                gen[bi] = live;
                kill[bi] = killed_all && !live;

                // Collect CFG successors from block terminator
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    if (it->tag == OpTag::UncondBranch && !it->target_label.empty()) {
                        auto sit = block_idx.find(it->target_label);
                        if (sit != block_idx.end()) succs[bi].push_back(sit->second);
                    } else if (it->tag == OpTag::CondBranch) {
                        if (!it->target_label.empty()) {
                            auto sit = block_idx.find(it->target_label);
                            if (sit != block_idx.end()) succs[bi].push_back(sit->second);
                        }
                        if (!it->false_label.empty()) {
                            auto sit = block_idx.find(it->false_label);
                            if (sit != block_idx.end()) succs[bi].push_back(sit->second);
                        }
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
                            auto sit = block_idx.find(it->text.substr(pos, end - pos));
                            if (sit != block_idx.end()) succs[bi].push_back(sit->second);
                            pos = end;
                        }
                    } else if (it->tag == OpTag::IndirectJump) {
                        has_unresolved_indirect_br[bi] = true;
                    }
                    break;
                }
            }

            // 2. Solve backward dataflow equations:
            //    live_out[b] = OR_{s in succs[b]} live_in[s]
            //    live_in[b]  = gen[b] || (!kill[b] && live_out[b])
            std::vector<bool> live_in = gen;
            std::vector<bool> live_out(n_blocks, false);
            bool df_changed = true;
            while (df_changed) {
                df_changed = false;
                for (int bi = static_cast<int>(n_blocks) - 1; bi >= 0; --bi) {
                    size_t u = static_cast<size_t>(bi);
                    bool out_val = has_unresolved_indirect_br[u];
                    if (!out_val) {
                        for (size_t s : succs[u]) {
                            if (live_in[s]) {
                                out_val = true;
                                break;
                            }
                        }
                    }
                    bool in_val = gen[u] || (!kill[u] && out_val);
                    if (out_val != live_out[u] || in_val != live_in[u]) {
                        live_out[u] = out_val;
                        live_in[u] = in_val;
                        df_changed = true;
                    }
                }
            }

            // 3. Backward intra-block pass seeded with exact `live_out[bi]`
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                BasicBlock& bb = *fn->blocks[bi];
                bool nzcv_live = live_out[bi];
                for (int i = static_cast<int>(bb.insts.size()) - 1; i >= 0; --i) {
                    Instruction& inst = bb.insts[static_cast<size_t>(i)];
                    if (inst.dead) continue;
                    if (inst.tag == OpTag::FlagWrite ||
                        (inst.tag == OpTag::CpuRegWrite && inst.arch_reg == 32)) {
                        if (!nzcv_live) {
                            inst.dead = true;
                            changed = true;
                            // If this was a CCMP/CCMN FlagWrite (`cmp_kind == 0`), also mark its preceding
                            // single-use flag-packing RawIR instructions dead up to the GuestInsnMarker,
                            // preserving any SSA definitions whose values are used downstream.
                            if (inst.cmp_kind == 0) {
                                std::unordered_set<std::string> live_ssa;
                                auto add_used_tokens = [&](const std::string& txt) {
                                    std::istringstream iss(txt);
                                    std::string line;
                                    while (std::getline(iss, line)) {
                                        size_t semi = line.find(';');
                                        if (semi != std::string::npos) line.erase(semi);
                                        if (line.empty()) continue;

                                        size_t eq_pos = line.find('=');
                                        size_t pos = (eq_pos != std::string::npos) ? (eq_pos + 1) : 0;

                                        while ((pos = line.find('%', pos)) != std::string::npos) {
                                            size_t end = pos + 1;
                                            while (end < line.size()) {
                                                char ch = line[end];
                                                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '.') {
                                                    ++end;
                                                } else {
                                                    break;
                                                }
                                            }
                                            if (end > pos + 1) {
                                                live_ssa.insert(line.substr(pos, end - pos));
                                            }
                                            pos = end;
                                        }
                                    }
                                };

                                for (const auto& other_bb : fn->blocks) {
                                    if (other_bb.get() == &bb) continue;
                                    for (const auto& other_inst : other_bb->insts) {
                                        if (other_inst.dead) continue;
                                        add_used_tokens(other_inst.text);
                                        if (!other_inst.prefix_ir.empty()) add_used_tokens(other_inst.prefix_ir);
                                    }
                                }
                                for (size_t m = static_cast<size_t>(i) + 1; m < bb.insts.size(); ++m) {
                                    const auto& after_inst = bb.insts[m];
                                    if (after_inst.dead) continue;
                                    add_used_tokens(after_inst.text);
                                    if (!after_inst.prefix_ir.empty()) add_used_tokens(after_inst.prefix_ir);
                                }

                                for (int k = i - 1; k >= 0; --k) {
                                    Instruction& prev = bb.insts[static_cast<size_t>(k)];
                                    if (prev.dead) continue;
                                    if (prev.tag == OpTag::GuestInsnMarker ||
                                        prev.tag == OpTag::CpuRegWrite ||
                                        prev.tag == OpTag::FlagWrite) {
                                        break;
                                    }
                                    if (prev.tag == OpTag::RawIR && prev.cond_code < 0 && !prev.dst_reg.empty() &&
                                        prev.text.find("call ") == std::string::npos &&
                                        prev.text.find("store ") == std::string::npos) {
                                        if (!live_ssa.count(prev.dst_reg)) {
                                            prev.dead = true;
                                            continue;
                                        }
                                    }
                                    add_used_tokens(prev.text);
                                    if (!prev.prefix_ir.empty()) add_used_tokens(prev.prefix_ir);
                                }
                            }
                        } else {
                            nzcv_live = false;
                        }
                    } else if (inst.tag == OpTag::CpuRegRead && inst.arch_reg == 32) {
                        nzcv_live = true;
                    } else if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                               inst.tag == OpTag::SvcTrap || inst.tag == OpTag::Return ||
                               inst.tag == OpTag::BrkTrap || inst.tag == OpTag::UdfTrap ||
                               inst.text.find("musttail call") != std::string::npos ||
                               inst.text == "ret void") {
                        nzcv_live = false;
                    }
                }
            }
        }
        return changed;
    }

private:
    bool enabled_;
};

} // namespace

bool NZCVDeadFlagElimPass::run(Module& M) {
    NZCVDeadFlagElimPassImpl impl(enabled_);
    return impl.run(M);
}

} // namespace ultrs::passes
