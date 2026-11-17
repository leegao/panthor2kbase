// UltraTears LLVM Pass Implementation: Pass 20 - AAPCS64ABIContractPass (src/horizon2llvm/pass_aapcs64_abi_contract.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include <bitset>
#include "passes.hpp"
#include "passes_llvm_utils.hpp"
#include "aarch64/abi_aapcs64.hpp"

namespace ultrs::passes {

namespace {

class AAPCS64ABIContractPassImpl {
public:
    explicit AAPCS64ABIContractPassImpl(bool enabled = true, bool allow_external_scratch_elim = true)
        : enabled_(enabled), allow_external_scratch_elim_(allow_external_scratch_elim) {}

    static const char* static_name() { return "aapcs64-abi-contract-pass"; }
    const char* pass_name() const { return "aapcs64-abi-contract-pass"; }

    bool run(Module& M) {
        if (!enabled_) return false;
        bool changed = false;
        using RegSet = ultrs::aarch64::RegSet;
        constexpr size_t MAX_ARCH_REGS = ultrs::aarch64::MAX_ARCH_REGS;

        // Specification-derived AAPCS64 contracts from AArch64CallingConv.td:
        const RegSet aapcs_ret_live = ultrs::aarch64::abi::AAPCSReturnLive;
        const RegSet aapcs_call_reads = ultrs::aarch64::abi::AAPCSCallReads;

        // 1. Compute upward-exposed register read sets (`summary_reads`) and write sets (`summary_writes`) per function
        for (auto& fn : M.functions) {
            fn->summary_reads.clear();
            fn->summary_writes.clear();
            fn->has_unknown_calls = !fn->hle_symbol.empty();

            if (fn->blocks.empty()) continue;
            size_t n_blocks = fn->blocks.size();
            std::unordered_map<std::string, size_t> block_idx;
            for (size_t i = 0; i < n_blocks; ++i) block_idx[fn->blocks[i]->name] = i;

            std::vector<std::vector<size_t>> succs(n_blocks);
            std::vector<RegSet> block_gen(n_blocks);
            std::vector<RegSet> block_kill(n_blocks);

            for (size_t bi = 0; bi < n_blocks; ++bi) {
                const BasicBlock& bb = *fn->blocks[bi];
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    auto add_edge = [&](const std::string& lbl) {
                        auto sit = block_idx.find(lbl);
                        if (sit != block_idx.end()) succs[bi].push_back(sit->second);
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
                                    (c >= '0' && c <= '9') || c == '_' || c == '.') ++end;
                                else break;
                            }
                            add_edge(it->text.substr(pos, end - pos));
                            pos = end;
                        }
                    }
                    break;
                }

                RegSet gen_b, kill_b;
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead) continue;
                    if (it->tag == OpTag::CpuRegRead && it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                        gen_b.set(static_cast<size_t>(it->arch_reg));
                    } else if ((it->tag == OpTag::CpuRegWrite || it->tag == OpTag::FlagWrite) &&
                               it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                        fn->summary_writes.insert(it->arch_reg);
                        gen_b.reset(static_cast<size_t>(it->arch_reg));
                        kill_b.set(static_cast<size_t>(it->arch_reg));
                    } else if (it->tag == OpTag::IndirectCall || it->tag == OpTag::SvcTrap) {
                        fn->has_unknown_calls = true;
                        gen_b.set();
                    } else if (it->tag == OpTag::DirectCall || it->text.find("musttail call") != std::string::npos) {
                        if (it->branch_target != 0) {
                            fn->direct_callee_pcs.insert(it->branch_target);
                            std::string cname = fn_symbol(it->branch_target);
                            if (!M.fn_map.count(cname)) {
                                fn->has_unknown_calls = true;
                                gen_b.set();
                            }
                        } else {
                            fn->has_unknown_calls = true;
                            gen_b.set();
                        }
                    } else if (it->text.find("call ") != std::string::npos &&
                               it->text.find("(ptr %c") != std::string::npos) {
                        fn->summary_reads.insert(32);
                        fn->summary_writes.insert(32);
                        gen_b.set(32);
                        for (int vr = 64; vr <= 95; ++vr) {
                            fn->summary_reads.insert(vr);
                            fn->summary_writes.insert(vr);
                            gen_b.set(static_cast<size_t>(vr));
                        }
                    }
                }
                block_gen[bi] = gen_b;
                block_kill[bi] = kill_b;
            }

            std::vector<RegSet> live_in = block_gen;
            std::vector<RegSet> live_out(n_blocks);
            bool df = true;
            while (df) {
                df = false;
                for (int bi = static_cast<int>(n_blocks) - 1; bi >= 0; --bi) {
                    size_t u = static_cast<size_t>(bi);
                    RegSet out_u;
                    for (size_t s : succs[u]) out_u |= live_in[s];
                    RegSet in_u = block_gen[u] | (out_u & ~block_kill[u]);
                    if (out_u != live_out[u] || in_u != live_in[u]) {
                        live_out[u] = out_u;
                        live_in[u] = in_u;
                        df = true;
                    }
                }
            }
            for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                if (live_in[0].test(r)) {
                    fn->summary_reads.insert(static_cast<int>(r));
                }
            }
        }

        // 2. Propagate summaries across intra-module DirectCalls to fixpoint
        bool prop = true;
        int iter = 0;
        while (prop && iter++ < 1024) {
            prop = false;
            for (auto& fn : M.functions) {
                for (uint64_t callee_pc : fn->direct_callee_pcs) {
                    auto it = M.fn_map.find(fn_symbol(callee_pc));
                    if (it == M.fn_map.end()) {
                        if (!fn->has_unknown_calls) { fn->has_unknown_calls = true; prop = true; }
                        continue;
                    }
                    Function* callee = it->second;
                    if (callee->has_unknown_calls && !fn->has_unknown_calls) {
                        fn->has_unknown_calls = true;
                        prop = true;
                    }
                    for (int r : callee->summary_reads) {
                        if (fn->summary_reads.insert(r).second) prop = true;
                    }
                    for (int r : callee->summary_writes) {
                        if (fn->summary_writes.insert(r).second) prop = true;
                    }
                }
            }
        }

        // 3. Eliminate dead writes to registers before a DirectCall or Return when the callee/return
        //    does not read them and the caller overwrites them on all post-call paths.
        for (auto& fn : M.functions) {
            if (fn->blocks.empty()) continue;
            size_t n_blocks = fn->blocks.size();
            std::unordered_map<std::string, size_t> block_idx;
            for (size_t i = 0; i < n_blocks; ++i) block_idx[fn->blocks[i]->name] = i;

            bool has_calls = false;
            for (const auto& bb : fn->blocks) {
                for (const auto& inst : bb->insts) {
                    if (!inst.dead && (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall)) {
                        has_calls = true;
                        break;
                    }
                }
                if (has_calls) break;
            }

            std::vector<std::vector<size_t>> succs(n_blocks);
            std::vector<RegSet> block_gen(n_blocks);
            std::vector<RegSet> block_kill(n_blocks);
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                const BasicBlock& bb = *fn->blocks[bi];
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    auto add_edge = [&](const std::string& lbl) {
                        auto sit = block_idx.find(lbl);
                        if (sit != block_idx.end()) succs[bi].push_back(sit->second);
                    };
                    if (it->tag == OpTag::UncondBranch && !it->target_label.empty()) add_edge(it->target_label);
                    else if (it->tag == OpTag::CondBranch) {
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
                                    (c >= '0' && c <= '9') || c == '_' || c == '.') ++end;
                                else break;
                            }
                            add_edge(it->text.substr(pos, end - pos));
                            pos = end;
                        }
                    }
                    break;
                }
                RegSet gen_b, kill_b;
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead) continue;
                    if (it->tag == OpTag::Return || it->text == "ret void") {
                        if (has_calls && allow_external_scratch_elim_) {
                            gen_b |= aapcs_ret_live;
                        } else {
                            gen_b.set();
                        }
                        continue;
                    }
                    if (it->tag == OpTag::DirectCall || it->tag == OpTag::IndirectCall ||
                        it->tag == OpTag::SvcTrap || it->text.find("(ptr %c") != std::string::npos) {
                        if (it->tag == OpTag::DirectCall && allow_external_scratch_elim_) {
                            gen_b |= aapcs_call_reads;
                        } else {
                            gen_b.set();
                            kill_b.reset();
                        }
                        continue;
                    }
                    if ((it->tag == OpTag::CpuRegWrite || it->tag == OpTag::FlagWrite) &&
                        it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                        gen_b.reset(static_cast<size_t>(it->arch_reg));
                        kill_b.set(static_cast<size_t>(it->arch_reg));
                    } else if (it->tag == OpTag::CpuRegRead &&
                               it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                        gen_b.set(static_cast<size_t>(it->arch_reg));
                    }
                }
                block_gen[bi] = gen_b;
                block_kill[bi] = kill_b;
            }

            std::vector<RegSet> live_in = block_gen;
            std::vector<RegSet> live_out(n_blocks);
            bool df = true;
            while (df) {
                df = false;
                for (int bi = static_cast<int>(n_blocks) - 1; bi >= 0; --bi) {
                    size_t u = static_cast<size_t>(bi);
                    RegSet out_u;
                    if (succs[u].empty()) {
                        out_u = (has_calls && allow_external_scratch_elim_) ? aapcs_ret_live : RegSet().set();
                    } else {
                        for (size_t s : succs[u]) out_u |= live_in[s];
                    }
                    RegSet in_u = block_gen[u] | (out_u & ~block_kill[u]);
                    if (out_u != live_out[u] || in_u != live_in[u]) {
                        live_out[u] = out_u;
                        live_in[u] = in_u;
                        df = true;
                    }
                }
            }

            for (size_t bi = 0; bi < n_blocks; ++bi) {
                BasicBlock& bb = *fn->blocks[bi];
                for (size_t i = 0; i < bb.insts.size(); ++i) {
                    Instruction& inst = bb.insts[i];
                    if (inst.dead || inst.tag != OpTag::DirectCall) continue;

                    const Function* callee = nullptr;
                    auto cit = M.fn_map.find(fn_symbol(inst.branch_target));
                    if (cit != M.fn_map.end() && !cit->second->has_unknown_calls) {
                        callee = cit->second;
                    }

                    // Compute registers live immediately after this DirectCall (including across block exit)
                    RegSet post_live = live_out[bi];
                    for (int j = static_cast<int>(bb.insts.size()) - 1; j > static_cast<int>(i); --j) {
                        const Instruction& post = bb.insts[static_cast<size_t>(j)];
                        if (post.dead) continue;
                        if (post.tag == OpTag::Return || post.text == "ret void") {
                            if (has_calls && allow_external_scratch_elim_) post_live |= aapcs_ret_live;
                            else post_live.set();
                            continue;
                        }
                        if (post.tag == OpTag::DirectCall || post.tag == OpTag::IndirectCall ||
                            post.tag == OpTag::SvcTrap || post.text.find("(ptr %c") != std::string::npos) {
                            if (post.tag == OpTag::DirectCall && allow_external_scratch_elim_) post_live |= aapcs_call_reads;
                            else post_live.set();
                            continue;
                        }
                        if ((post.tag == OpTag::CpuRegWrite || post.tag == OpTag::FlagWrite) &&
                            post.arch_reg >= 0 && post.arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                            post_live.reset(static_cast<size_t>(post.arch_reg));
                        } else if (post.tag == OpTag::CpuRegRead &&
                                   post.arch_reg >= 0 && post.arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                            post_live.set(static_cast<size_t>(post.arch_reg));
                        }
                    }

                    std::unordered_set<int> read_before_call;
                    for (int k = static_cast<int>(i) - 1; k >= 0; --k) {
                        Instruction& pre = bb.insts[static_cast<size_t>(k)];
                        if (pre.dead) continue;
                        if (pre.tag == OpTag::DirectCall || pre.tag == OpTag::IndirectCall ||
                            pre.tag == OpTag::SvcTrap || pre.text.find("(ptr %c") != std::string::npos) {
                            break;
                        }
                        if (pre.tag == OpTag::CpuRegRead && pre.arch_reg >= 0) {
                            read_before_call.insert(pre.arch_reg);
                        } else if ((pre.tag == OpTag::CpuRegWrite || pre.tag == OpTag::FlagWrite) && pre.arch_reg >= 0) {
                            int r = pre.arch_reg;
                            bool is_aapcs_scratch = (r >= 9 && r <= 15) || (r == 32) || (r >= 80 && r <= 95);
                            bool callee_ignores = callee ? (!callee->summary_reads.count(r))
                                                         : (allow_external_scratch_elim_ && is_aapcs_scratch);
                            bool killed_post = (r >= 0 && r < static_cast<int>(MAX_ARCH_REGS)) && !post_live.test(static_cast<size_t>(r));
                            if (!read_before_call.count(r) && callee_ignores && killed_post) {
                                pre.dead = true;
                                changed = true;
                            } else {
                                read_before_call.insert(r);
                            }
                        }
                    }
                }
            }
        }
        return changed;
    }

private:
    bool enabled_;
    bool allow_external_scratch_elim_;
};


} // namespace

bool AAPCS64ABIContractPass::run(Module& M) {
    AAPCS64ABIContractPassImpl impl(enabled_, allow_external_scratch_elim_);
    return impl.run(M);
}

} // namespace ultrs::passes
