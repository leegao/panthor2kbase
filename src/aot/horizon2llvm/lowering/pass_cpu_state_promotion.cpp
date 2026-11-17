// UltraTears LLVM Pass Implementation: Pass 21 - CpuStatePromotionPass (src/horizon2llvm/lowering/pass_cpu_state_promotion.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include <bitset>
#include <cstdio>
#include <sstream>
#include "passes.hpp"
#include "passes_llvm_utils.hpp"
#include "aarch64/abi_aapcs64.hpp"
#include <iostream>

namespace ultrs::passes {

namespace {

class CpuStatePromotionPassImpl {
public:
    static constexpr size_t MAX_ARCH_REGS = 96;
    using RegSet = std::bitset<MAX_ARCH_REGS>;

    explicit CpuStatePromotionPassImpl(bool enable_entry_kill_pruning = true,
                                       bool enable_interproc_pruning = true)
        : enable_entry_kill_pruning_(enable_entry_kill_pruning),
          enable_interproc_pruning_(enable_interproc_pruning) {}

    static const char* static_name() { return "cpu-state-promotion-pass"; }
    const char* pass_name() const { return "cpu-state-promotion-pass"; }

    static bool classify_helper_reg_effects(const Instruction& inst, RegSet& reads, RegSet& writes) {
        if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
            inst.tag == OpTag::SvcTrap || inst.tag == OpTag::Return ||
            inst.tag == OpTag::BrkTrap || inst.tag == OpTag::UdfTrap) {
            return false;
        }
        if (inst.text.find("(ptr %c") == std::string::npos) return false;
        if (inst.text.find("@rt_dispatch(") != std::string::npos ||
            inst.text.find("@rt_svc(") != std::string::npos ||
            inst.text.find("@rt_brk(") != std::string::npos ||
            inst.text.find("@rt_udf(") != std::string::npos) {
            return false;
        }

        int a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        if (std::sscanf(inst.text.c_str(),
                        "call void @rt_vec_fop(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d)",
                        &a0, &a1, &a2, &a3, &a4, &a5, &a6, &a7) == 8) {
            if (a4 >= 0 && a4 < 32) reads.set(64 + static_cast<size_t>(a4));
            if (a5 >= 0 && a5 < 32) reads.set(64 + static_cast<size_t>(a5));
            if (a6 >= 0 && a6 < 32) reads.set(64 + static_cast<size_t>(a6));
            if (a3 >= 0 && a3 < 32) writes.set(64 + static_cast<size_t>(a3));
            return true;
        }
        if (std::sscanf(inst.text.c_str(),
                        "call void @rt_vec_frecpe(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d)",
                        &a0, &a1, &a2, &a3, &a4) == 5) {
            if (a4 >= 0 && a4 < 32) reads.set(64 + static_cast<size_t>(a4));
            if (a3 >= 0 && a3 < 32) writes.set(64 + static_cast<size_t>(a3));
            return true;
        }
        if (std::sscanf(inst.text.c_str(),
                        "call void @rt_fp_op_s(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d)",
                        &a0, &a1, &a2, &a3) == 4 ||
            std::sscanf(inst.text.c_str(),
                        "call void @rt_fp_op_d(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d)",
                        &a0, &a1, &a2, &a3) == 4) {
            if (a2 >= 0 && a2 < 32) reads.set(64 + static_cast<size_t>(a2));
            if (a3 >= 0 && a3 < 32) reads.set(64 + static_cast<size_t>(a3));
            if (a1 >= 0 && a1 < 32) writes.set(64 + static_cast<size_t>(a1));
            return true;
        }
        if (std::sscanf(inst.text.c_str(),
                        "call void @rt_sl_ldr(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d,",
                        &a0, &a1, &a2, &a3) == 4) {
            for (int k = 0; k < a2 && k < 4; ++k) {
                writes.set(64 + static_cast<size_t>((a3 + k) & 31));
            }
            return true;
        }
        if (std::sscanf(inst.text.c_str(),
                        "call void @rt_sl_ldm(ptr %%c, i32 %d, i32 %d, i32 %d, i32 %d, i32 %d,",
                        &a0, &a1, &a2, &a3, &a4) == 5) {
            int cnt = a2 * a3;
            for (int k = 0; k < cnt && k < 4; ++k) {
                writes.set(64 + static_cast<size_t>((a4 + k) & 31));
            }
            return true;
        }
        if (inst.text.find("@rt_sl_") != std::string::npos ||
            inst.text.find("@rt_sv_") != std::string::npos ||
            inst.text.find("@rt_vec_") != std::string::npos ||
            inst.text.find("@rt_fp_") != std::string::npos ||
            inst.text.find("@rt_scalar_") != std::string::npos ||
            inst.text.find("@rt_indexed_") != std::string::npos) {
            reads.set(32);
            writes.set(32);
            for (size_t r = 64; r < 96; ++r) {
                reads.set(r);
                writes.set(r);
            }
            return true;
        }
        return false;
    }

    // =========================================================================
    // Step 1b Helpers: Clean Stack Frame Detection & Callee-Save Elision
    // =========================================================================

    struct CleanStackFrameResult {
        bool has_clean_stack_frame = false;
        bool defer_sp_sync = false;
        std::set<int> elided_callee_regs;
        std::string entry_sp_ssa;
        std::string frame_sp_ssa;
        int64_t saved_frame_sp_rel = 0;
        size_t prolog_bi = 0;
    };

    struct PrologSave {
        int64_t sp_rel_off = 0;
        size_t read_idx = 0;
        size_t store_idx = 0;
    };

    struct EpilogRestore {
        size_t load_idx = 0;
        size_t write_idx = 0;
    };

    static size_t find_prologue_block_index(const Function& fn, const std::vector<std::vector<size_t>>& preds) {
        if (fn.blocks.size() >= 2 && fn.blocks[0]->name == "entry" && !fn.has_all_labels) {
            std::string first_lbl = block_label(fn.entry_pc);
            if (fn.blocks[1]->name == first_lbl && preds[1].size() == 1) {
                return 1;
            }
        }
        return 0;
    }

    static bool has_intermediate_sp_write(const Function& fn, size_t prolog_bi,
                                          const std::unordered_set<size_t>& ret_bi_set,
                                          const std::vector<bool>& is_jt_def_block) {
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (bi == prolog_bi || ret_bi_set.count(bi) || is_jt_def_block[bi]) continue;
            for (const auto& inst : fn.blocks[bi]->insts) {
                if (!inst.dead && inst.tag == OpTag::CpuRegWrite && inst.arch_reg == 31) {
                    return true;
                }
            }
        }
        return false;
    }

    static bool analyze_prologue_saves(
        const BasicBlock& pbb,
        std::unordered_map<int, PrologSave>& prolog_saves,
        int64_t& out_frame_sp_rel,
        std::string& out_entry_sp_ssa,
        std::string& out_frame_sp_ssa) {
        std::unordered_map<std::string, int64_t> ssa_sp_rel;
        std::unordered_map<std::string, std::pair<int, size_t>> ssa_unmod_reg;
        std::unordered_set<int> written_in_prolog;
        int64_t cur_sp_rel = 0;
        int sp_writes_in_prolog = 0;
        bool prolog_sp_valid = true;
        std::string cand_entry_sp_ssa;
        std::string cand_frame_sp_ssa;

        for (size_t idx = 0; idx < pbb.insts.size(); ++idx) {
            const Instruction& inst = pbb.insts[idx];
            if (inst.dead) continue;
            if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                inst.tag == OpTag::SvcTrap) {
                break;
            }
            if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0 && inst.arch_reg <= 31) {
                if (inst.arch_reg == 31) {
                    ssa_sp_rel[inst.dst_reg] = cur_sp_rel;
                    if (cur_sp_rel == 0 && cand_entry_sp_ssa.empty()) {
                        cand_entry_sp_ssa = inst.dst_reg;
                    }
                }
                if (inst.arch_reg >= 19 && inst.arch_reg <= 30 && !written_in_prolog.count(inst.arch_reg)) {
                    ssa_unmod_reg[inst.dst_reg] = {inst.arch_reg, idx};
                }
                continue;
            }
            if (inst.tag == OpTag::RawIR && !inst.dst_reg.empty()) {
                if (inst.op1.is_const) {
                    auto it = ssa_sp_rel.find(inst.op0.repr);
                    if (it != ssa_sp_rel.end()) {
                        if (inst.text.find(" = add i64 ") != std::string::npos) {
                            ssa_sp_rel[inst.dst_reg] = it->second + static_cast<int64_t>(inst.op1.const_u64);
                        } else if (inst.text.find(" = sub i64 ") != std::string::npos) {
                            ssa_sp_rel[inst.dst_reg] = it->second - static_cast<int64_t>(inst.op1.const_u64);
                        }
                    }
                }
                continue;
            }
            if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0 && inst.arch_reg <= 31) {
                written_in_prolog.insert(inst.arch_reg);
                if (inst.arch_reg == 31) {
                    auto it = ssa_sp_rel.find(inst.op0.repr);
                    if (it != ssa_sp_rel.end() && inst.prefix_ir.empty()) {
                        cur_sp_rel = it->second;
                        sp_writes_in_prolog++;
                        cand_frame_sp_ssa = inst.op0.repr;
                    } else {
                        prolog_sp_valid = false;
                    }
                }
                continue;
            }
            if (inst.tag == OpTag::GuestMemStore) {
                auto sit = ssa_sp_rel.find(inst.op0.repr);
                auto rit = ssa_unmod_reg.find(inst.op1.repr);
                if (sit != ssa_sp_rel.end() && rit != ssa_unmod_reg.end()) {
                    int r = rit->second.first;
                    int64_t off = sit->second;
                    if (off < 0 && off >= -65536 && !prolog_saves.count(r)) {
                        prolog_saves[r] = PrologSave{off, rit->second.second, idx};
                    }
                }
            }
        }

        if (prolog_sp_valid && sp_writes_in_prolog == 1 && cur_sp_rel < 0 && cur_sp_rel >= -65536 &&
            !cand_entry_sp_ssa.empty() && !cand_frame_sp_ssa.empty()) {
            out_frame_sp_rel = cur_sp_rel;
            out_entry_sp_ssa = cand_entry_sp_ssa;
            out_frame_sp_ssa = cand_frame_sp_ssa;
            return true;
        }
        return false;
    }

    static bool analyze_epilogue_restores(
        const Function& fn,
        const std::vector<size_t>& return_blocks,
        int64_t frame_sp_rel,
        const std::unordered_map<int, PrologSave>& prolog_saves,
        std::vector<size_t>& ret_sp_write_indices,
        std::vector<size_t>& ret_sp_read_indices,
        std::vector<std::unordered_map<int, EpilogRestore>>& epilog_restores) {
        for (size_t ri = 0; ri < return_blocks.size(); ++ri) {
            BasicBlock& rbb = *fn.blocks[return_blocks[ri]];
            int64_t r_sp_rel = frame_sp_rel;
            int r_sp_writes = 0;
            std::unordered_map<std::string, int64_t> r_ssa_sp;
            std::unordered_map<std::string, std::pair<int64_t, size_t>> r_loaded_slot;
            std::unordered_map<std::string, size_t> r_sp_read_def;

            for (size_t idx = 0; idx < rbb.insts.size(); ++idx) {
                const Instruction& inst = rbb.insts[idx];
                if (inst.dead) continue;
                if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                    inst.tag == OpTag::SvcTrap) {
                    return false;
                }
                if (inst.tag == OpTag::CpuRegRead && inst.arch_reg == 31) {
                    r_ssa_sp[inst.dst_reg] = r_sp_rel;
                    r_sp_read_def[inst.dst_reg] = idx;
                    continue;
                }
                if (inst.tag == OpTag::RawIR && !inst.dst_reg.empty() && inst.op1.is_const) {
                    auto it = r_ssa_sp.find(inst.op0.repr);
                    if (it != r_ssa_sp.end()) {
                        if (inst.text.find(" = add i64 ") != std::string::npos) {
                            r_ssa_sp[inst.dst_reg] = it->second + static_cast<int64_t>(inst.op1.const_u64);
                            if (r_sp_read_def.count(inst.op0.repr)) {
                                r_sp_read_def[inst.dst_reg] = r_sp_read_def[inst.op0.repr];
                            }
                        } else if (inst.text.find(" = sub i64 ") != std::string::npos) {
                            r_ssa_sp[inst.dst_reg] = it->second - static_cast<int64_t>(inst.op1.const_u64);
                        }
                    }
                    continue;
                }
                if (inst.tag == OpTag::GuestMemLoad && inst.dst_type == TypeKind::Int64) {
                    auto it = r_ssa_sp.find(inst.op0.repr);
                    if (it != r_ssa_sp.end()) {
                        r_loaded_slot[inst.dst_reg] = {it->second, idx};
                    }
                    continue;
                }
                if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0 && inst.arch_reg <= 31) {
                    if (inst.arch_reg == 31) {
                        auto it = r_ssa_sp.find(inst.op0.repr);
                        if (it != r_ssa_sp.end()) {
                            r_sp_rel = it->second;
                            r_sp_writes++;
                            ret_sp_write_indices[ri] = idx;
                            if (r_sp_read_def.count(inst.op0.repr)) {
                                ret_sp_read_indices[ri] = r_sp_read_def[inst.op0.repr];
                            }
                        } else {
                            return false;
                        }
                    } else if (inst.arch_reg >= 19 && inst.arch_reg <= 30) {
                        auto lit = r_loaded_slot.find(inst.op0.repr);
                        if (lit != r_loaded_slot.end() && prolog_saves.count(inst.arch_reg) &&
                            prolog_saves.at(inst.arch_reg).sp_rel_off == lit->second.first) {
                            epilog_restores[ri][inst.arch_reg] = EpilogRestore{lit->second.second, idx};
                        } else {
                            epilog_restores[ri].erase(inst.arch_reg);
                        }
                    }
                }
            }
            if (r_sp_writes != 1 || r_sp_rel != 0) {
                return false;
            }
        }
        return true;
    }

    static bool is_callee_reg_written_in_body(
        const Function& fn, int r,
        const std::vector<bool>& is_jt_def_block,
        const std::vector<size_t>& return_blocks,
        const std::vector<std::unordered_map<int, EpilogRestore>>& epilog_restores) {
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (is_jt_def_block[bi]) continue;
            const BasicBlock& bb = *fn.blocks[bi];
            for (size_t idx = 0; idx < bb.insts.size(); ++idx) {
                const Instruction& inst = bb.insts[idx];
                if (inst.dead) continue;
                bool is_epilog_write = false;
                for (size_t ri = 0; ri < return_blocks.size(); ++ri) {
                    if (bi == return_blocks[ri] && epilog_restores[ri].count(r) &&
                        idx == epilog_restores[ri].at(r).write_idx) {
                        is_epilog_write = true;
                        break;
                    }
                }
                if (is_epilog_write) continue;

                if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg == r) {
                    return true;
                }
                RegSet h_reads, h_writes;
                if (classify_helper_reg_effects(inst, h_reads, h_writes)) {
                    if (h_writes.test(static_cast<size_t>(r))) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    static bool is_reg_read_outside_prologue(
        const Function& fn, int r, size_t prolog_bi, size_t prolog_read_idx) {
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const BasicBlock& bb = *fn.blocks[bi];
            for (size_t idx = 0; idx < bb.insts.size(); ++idx) {
                const Instruction& inst = bb.insts[idx];
                if (inst.dead) continue;
                if (inst.tag == OpTag::CpuRegRead && inst.arch_reg == r) {
                    if (!(bi == prolog_bi && idx == prolog_read_idx)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    static std::set<int> elide_prologue_epilogue_callee_saves(
        Function& fn, size_t prolog_bi,
        const std::unordered_map<int, PrologSave>& prolog_saves,
        const std::vector<size_t>& return_blocks,
        const std::vector<std::unordered_map<int, EpilogRestore>>& epilog_restores,
        const std::vector<bool>& is_jt_def_block,
        bool& changed) {
        std::set<int> elided_callee_regs;
        BasicBlock& pbb = *fn.blocks[prolog_bi];

        for (const auto& [r, ps] : prolog_saves) {
            if (r == 30 && is_reg_read_outside_prologue(fn, 30, prolog_bi, ps.read_idx)) {
                continue;
            }
            bool restored_in_all = true;
            for (size_t ri = 0; ri < return_blocks.size(); ++ri) {
                if (!epilog_restores[ri].count(r)) {
                    restored_in_all = false;
                    break;
                }
            }
            if (!restored_in_all) continue;

            // Callee-saved registers can only be elided if they are unmodified throughout
            // the entire function body (outside of prologue save and epilogue restore);
            // if modified, the caller's incoming value must be restored at function exit!
            if (is_callee_reg_written_in_body(fn, r, is_jt_def_block, return_blocks, epilog_restores)) {
                continue;
            }

            // Elide prologue save store and epilogue restore load + write
            pbb.insts[ps.store_idx].dead = true;
            const std::string& rd_ssa = pbb.insts[ps.read_idx].dst_reg;
            bool rd_used_elsewhere = false;
            for (size_t idx = ps.read_idx + 1; idx < pbb.insts.size(); ++idx) {
                if (idx == ps.store_idx || pbb.insts[idx].dead) continue;
                if (pbb.insts[idx].text.find(rd_ssa) != std::string::npos) {
                    rd_used_elsewhere = true;
                    break;
                }
            }
            if (!rd_used_elsewhere) {
                pbb.insts[ps.read_idx].dead = true;
            }

            for (size_t ri = 0; ri < return_blocks.size(); ++ri) {
                BasicBlock& rbb = *fn.blocks[return_blocks[ri]];
                const auto& er = epilog_restores[ri].at(r);
                rbb.insts[er.load_idx].dead = true;
                rbb.insts[er.write_idx].dead = true;
            }

            elided_callee_regs.insert(r);
            changed = true;
        }
        return elided_callee_regs;
    }

    static bool elide_epilogue_sp_restores(
        Function& fn,
        const std::vector<size_t>& return_blocks,
        const std::vector<size_t>& ret_sp_write_indices,
        const std::vector<size_t>& ret_sp_read_indices,
        const std::vector<std::vector<size_t>>& preds,
        const std::vector<bool>& is_jt_def_block) {
        // Elide epilogue `add sp, sp, #frame_size` write and sync SP directly around calls
        // only when no unreachable orphan blocks exist that would block SP SSA dominance
        for (size_t b = 1; b < fn.blocks.size(); ++b) {
            if (!is_jt_def_block[b] && preds[b].empty()) {
                return false;
            }
        }

        for (size_t ri = 0; ri < return_blocks.size(); ++ri) {
            BasicBlock& rbb = *fn.blocks[return_blocks[ri]];
            size_t w_idx = ret_sp_write_indices[ri];
            size_t r_idx = ret_sp_read_indices[ri];
            rbb.insts[w_idx].dead = true;
            const std::string& sp_rd_ssa = rbb.insts[r_idx].dst_reg;
            bool sp_rd_used = false;
            for (size_t idx = r_idx + 1; idx < rbb.insts.size(); ++idx) {
                if (idx == w_idx || rbb.insts[idx].dead) continue;
                if (rbb.insts[idx].dst_reg == rbb.insts[w_idx].op0.repr) continue;
                if (rbb.insts[idx].text.find(sp_rd_ssa) != std::string::npos) {
                    sp_rd_used = true;
                    break;
                }
            }
            if (!sp_rd_used) {
                rbb.insts[r_idx].dead = true;
            }
        }
        return true;
    }

    static CleanStackFrameResult detect_clean_stack_frame_and_elide_callee_saves(
        Function& fn,
        bool enable_entry_kill_pruning,
        bool has_musttail_or_indjump,
        const std::vector<size_t>& return_blocks,
        const std::vector<std::vector<size_t>>& preds,
        const std::vector<bool>& is_jt_def_block,
        bool& changed) {
        CleanStackFrameResult res;
        size_t n_blocks = fn.blocks.size();
        if (!enable_entry_kill_pruning || has_musttail_or_indjump || return_blocks.empty() || n_blocks < 2) {
            return res;
        }

        size_t prolog_bi = find_prologue_block_index(fn, preds);
        std::unordered_set<size_t> ret_bi_set(return_blocks.begin(), return_blocks.end());
        if (ret_bi_set.count(prolog_bi)) return res;

        if (has_intermediate_sp_write(fn, prolog_bi, ret_bi_set, is_jt_def_block)) {
            return res;
        }

        std::unordered_map<int, PrologSave> prolog_saves;
        int64_t frame_sp_rel = 0;
        std::string cand_entry_sp_ssa, cand_frame_sp_ssa;
        if (!analyze_prologue_saves(*fn.blocks[prolog_bi], prolog_saves, frame_sp_rel,
                                    cand_entry_sp_ssa, cand_frame_sp_ssa)) {
            return res;
        }

        std::vector<size_t> ret_sp_write_indices(return_blocks.size(), 0);
        std::vector<size_t> ret_sp_read_indices(return_blocks.size(), 0);
        std::vector<std::unordered_map<int, EpilogRestore>> epilog_restores(return_blocks.size());

        if (!analyze_epilogue_restores(fn, return_blocks, frame_sp_rel, prolog_saves,
                                       ret_sp_write_indices, ret_sp_read_indices, epilog_restores)) {
            return res;
        }

        res.has_clean_stack_frame = true;
        res.entry_sp_ssa = cand_entry_sp_ssa;
        res.frame_sp_ssa = cand_frame_sp_ssa;
        res.saved_frame_sp_rel = frame_sp_rel;
        res.prolog_bi = prolog_bi;

        res.elided_callee_regs = elide_prologue_epilogue_callee_saves(
            fn, prolog_bi, prolog_saves, return_blocks, epilog_restores, is_jt_def_block, changed);

        // TODO(leegao): defer_sp_sync was eliding epilogue `add sp, sp, #frame` restores
        // in return blocks while retaining prologue `sub sp, sp, #frame` allocations, leaking
        // frames across calls and corrupting caller SP. It also injected premature `rest_sp`
        // restores (+saved_frame_sp_rel) after mid-function calls, prematurely resetting SP
        // to entry SP while the function frame was still live. To safely re-enable:
        // 1) Ensure epilogue SP restores are NEVER elided when callers depend on AAPCS64 SP balance.
        // 2) Keep SP pointing to the current function frame across all mid-function external calls
        //    instead of resetting it to entry SP.
        res.defer_sp_sync = false;

        return res;
    }

    bool run(Module& M) {
        bool changed = false;

        for (auto& fn : M.functions) {
            if (!fn->hle_symbol.empty() || fn->blocks.empty()) continue;

            // 1. Build block-level CFG for forward SSA propagation, backward DSE, and frame analysis
            size_t n_blocks = fn->blocks.size();
            std::unordered_map<std::string, size_t> block_idx;
            block_idx.reserve(n_blocks * 2);
            for (size_t i = 0; i < n_blocks; ++i) {
                block_idx[fn->blocks[i]->name] = i;
            }

            std::vector<std::vector<size_t>> succs(n_blocks);
            std::vector<std::vector<size_t>> preds(n_blocks);
            std::vector<size_t> return_blocks;
            std::vector<bool> is_jt_def_block(n_blocks, false);
            bool has_musttail_or_indjump = false;

            for (size_t bi = 0; bi < n_blocks; ++bi) {
                const BasicBlock& bb = *fn->blocks[bi];
                if (bb.name.find("_jt_def") != std::string::npos) {
                    is_jt_def_block[bi] = true;
                }
                if (!is_jt_def_block[bi]) {
                    for (const auto& inst : bb.insts) {
                        if (inst.dead) continue;
                        if ((inst.tag == OpTag::IndirectJump && inst.text.rfind("switch i64 ", 0) != 0) ||
                            inst.tag == OpTag::BrkTrap ||
                            inst.tag == OpTag::UdfTrap ||
                            inst.text.find("musttail call") != std::string::npos) {
                            has_musttail_or_indjump = true;
                        }
                    }
                }
                for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    auto add_edge = [&](const std::string& lbl) {
                        auto sit = block_idx.find(lbl);
                        if (sit != block_idx.end()) {
                            succs[bi].push_back(sit->second);
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
                    } else if ((it->tag == OpTag::Return || it->text == "ret void") && !is_jt_def_block[bi]) {
                        return_blocks.push_back(bi);
                    }
                    break;
                }
            }

            // 1b. Prologue/Epilogue Callee-Saved Register Save/Restore Elision & Clean Stack Frame Detection
            CleanStackFrameResult frame_info = detect_clean_stack_frame_and_elide_callee_saves(
                *fn, enable_entry_kill_pruning_, has_musttail_or_indjump, return_blocks,
                preds, is_jt_def_block, changed);
            const auto& elided_callee_regs = frame_info.elided_callee_regs;
            bool has_clean_stack_frame = frame_info.has_clean_stack_frame;
            bool defer_sp_sync = frame_info.defer_sp_sync;
            const auto& frame_sp_ssa = frame_info.frame_sp_ssa;
            int64_t saved_frame_sp_rel = frame_info.saved_frame_sp_rel;
            size_t prolog_bi = frame_info.prolog_bi;

            // Pre-count original architectural register reads/writes for candidate selection
            std::unordered_map<int, int> orig_reg_reads, orig_reg_writes;
            size_t initial_call_count = 0;
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                if (is_jt_def_block[bi]) continue;
                for (const auto& inst : fn->blocks[bi]->insts) {
                    if (inst.dead) continue;
                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0) orig_reg_reads[inst.arch_reg]++;
                    else if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0) orig_reg_writes[inst.arch_reg]++;
                    else if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall) initial_call_count++;
                }
            }

            // 2a. Intra-block AND Dominator-Intersection Cross-Block SSA register store-to-load & load-to-load forwarding:
            // Forward the SSA value of any `CpuRegWrite` or prior `CpuRegRead` directly to subsequent `CpuRegRead`s
            // within the same block and across dominated basic blocks where all incoming paths carry the identical
            // SSA definition (including across calls for `elided_callee_regs` and `SP`).
            {
                // Compute syntactic CFG dominators `dom[b][d]` so cross-block SSA forwarding strictly respects LLVM dominance
                std::vector<std::vector<bool>> dom(n_blocks, std::vector<bool>(n_blocks, true));
                dom[0].assign(n_blocks, false);
                dom[0][0] = true;
                for (size_t b = 1; b < n_blocks; ++b) {
                    if (preds[b].empty()) {
                        dom[b].assign(n_blocks, false);
                        dom[b][b] = true;
                    }
                }
                bool dom_changed = true;
                while (dom_changed) {
                    dom_changed = false;
                    for (size_t b = 1; b < n_blocks; ++b) {
                        if (preds[b].empty()) continue;
                        std::vector<bool> next_d(n_blocks, true);
                        for (size_t p : preds[b]) {
                            for (size_t d = 0; d < n_blocks; ++d) {
                                if (!dom[p][d]) next_d[d] = false;
                            }
                        }
                        next_d[b] = true;
                        if (next_d != dom[b]) {
                            dom[b] = std::move(next_d);
                            dom_changed = true;
                        }
                    }
                }

                std::unordered_map<std::string, size_t> ssa_def_block;
                for (size_t b = 0; b < n_blocks; ++b) {
                    for (const auto& inst : fn->blocks[b]->insts) {
                        if (inst.dead) continue;
                        if (!inst.dst_reg.empty()) {
                            ssa_def_block[inst.dst_reg] = b;
                        }
                        if ((inst.tag == OpTag::CpuRegWrite || inst.tag == OpTag::FlagWrite) &&
                            !inst.op0.repr.empty() && inst.op0.repr[0] == '%') {
                            ssa_def_block.emplace(inst.op0.repr, b);
                        }
                    }
                }

                std::vector<bool> avail_visited(n_blocks, false);
                std::vector<std::unordered_map<int, std::string>> avail_in(n_blocks), avail_out(n_blocks);

                auto simulate_avail = [&](size_t bi,
                                          const std::unordered_map<int, std::string>& start_map,
                                          std::unordered_map<int, std::string>& end_map,
                                          bool apply_rewrite) {
                    end_map = start_map;
                    BasicBlock& bb = *fn->blocks[bi];
                    for (auto& inst : bb.insts) {
                        if (inst.dead) continue;
                        RegSet h_reads, h_writes;
                        if (classify_helper_reg_effects(inst, h_reads, h_writes)) {
                            for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                                if (h_writes.test(r)) end_map.erase(static_cast<int>(r));
                            }
                            continue;
                        }
                        if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                            inst.tag == OpTag::SvcTrap || inst.tag == OpTag::Return ||
                            inst.tag == OpTag::BrkTrap || inst.tag == OpTag::UdfTrap ||
                            inst.text.find("(ptr %c") != std::string::npos ||
                            inst.text == "ret void") {
                            for (auto it = end_map.begin(); it != end_map.end(); ) {
                                int r = it->first;
                                if (elided_callee_regs.count(r) || (has_clean_stack_frame && r == 31)) ++it;
                                else it = end_map.erase(it);
                            }
                            continue;
                        }
                        if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0 &&
                            (inst.arch_reg <= 31 || (inst.arch_reg >= 64 && inst.arch_reg <= 95))) {
                            auto it = end_map.find(inst.arch_reg);
                            if (it != end_map.end() && !inst.dst_reg.empty()) {
                                if (apply_rewrite) {
                                    bool is_vec = (inst.arch_reg >= 64);
                                    inst.tag = OpTag::RawIR;
                                    inst.arch_reg = -1;
                                    inst.text = inst.dst_reg + " = add " + (is_vec ? "i128 0, " : "i64 0, ") + it->second;
                                    changed = true;
                                }
                            } else if (!inst.dst_reg.empty()) {
                                end_map[inst.arch_reg] = inst.dst_reg;
                            }
                            continue;
                        }
                        if ((inst.tag == OpTag::CpuRegWrite || inst.tag == OpTag::FlagWrite) &&
                            inst.arch_reg >= 0 &&
                            (inst.arch_reg <= 31 || (inst.arch_reg >= 64 && inst.arch_reg <= 95))) {
                            if (inst.prefix_ir.empty() && !inst.op0.repr.empty()) {
                                end_map[inst.arch_reg] = inst.op0.repr;
                            } else {
                                end_map.erase(inst.arch_reg);
                            }
                            continue;
                        }
                    }
                };

                bool fw_df = true;
                int fw_iter = 0;
                while (fw_df && fw_iter++ < 16) {
                    fw_df = false;
                    for (size_t bi = 0; bi < n_blocks; ++bi) {
                        std::unordered_map<int, std::string> merged;
                        if (bi > 0) {
                            if (preds[bi].empty()) continue;
                            bool first_p = true;
                            for (size_t p : preds[bi]) {
                                if (!avail_visited[p]) continue;
                                if (first_p) {
                                    merged = avail_out[p];
                                    first_p = false;
                                } else {
                                    for (auto it = merged.begin(); it != merged.end(); ) {
                                        auto pit = avail_out[p].find(it->first);
                                        if (pit == avail_out[p].end() || pit->second != it->second) {
                                            it = merged.erase(it);
                                        } else {
                                            ++it;
                                        }
                                    }
                                }
                            }
                            if (first_p) continue;
                            for (auto it = merged.begin(); it != merged.end(); ) {
                                int r = it->first;
                                bool is_invariant = elided_callee_regs.count(r) || (has_clean_stack_frame && r == 31);
                                if (!is_invariant) {
                                    it = merged.erase(it);
                                    continue;
                                }
                                auto dit = ssa_def_block.find(it->second);
                                if (dit == ssa_def_block.end() || !dom[bi][dit->second]) {
                                    it = merged.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }
                        std::unordered_map<int, std::string> next_out;
                        simulate_avail(bi, merged, next_out, false);
                        if (!avail_visited[bi] || avail_in[bi] != merged || avail_out[bi] != next_out) {
                            avail_visited[bi] = true;
                            avail_in[bi] = std::move(merged);
                            avail_out[bi] = std::move(next_out);
                            fw_df = true;
                        }
                    }
                }

                for (size_t bi = 0; bi < n_blocks; ++bi) {
                    if (has_clean_stack_frame && !frame_sp_ssa.empty() && bi != 0 && bi != prolog_bi && dom[bi][prolog_bi]) {
                        avail_in[bi][31] = frame_sp_ssa;
                    }
                    std::unordered_map<int, std::string> dummy_out;
                    simulate_avail(bi, avail_in[bi], dummy_out, true);
                }
            }

            // 2b. Inter-block backward dead-store elimination for registers overwritten on all successor paths
            {
                RegSet aapcs_ret_live;
                if (has_clean_stack_frame || initial_call_count >= 1) {
                    aapcs_ret_live = ultrs::aarch64::abi::AAPCSReturnLive;
                    for (int r : elided_callee_regs) {
                        if (r >= 0 && r < static_cast<int>(MAX_ARCH_REGS)) aapcs_ret_live.reset(r);
                    }
                    if (defer_sp_sync) aapcs_ret_live.reset(ultrs::aarch64::regs::SP);
                } else {
                    aapcs_ret_live.set();
                }

                RegSet aapcs_ext_call_reads = ultrs::aarch64::abi::AAPCSCallReads;
                for (int r : elided_callee_regs) {
                    if (r >= 0 && r < static_cast<int>(MAX_ARCH_REGS)) aapcs_ext_call_reads.reset(r);
                }
                if (defer_sp_sync) aapcs_ext_call_reads.reset(ultrs::aarch64::regs::SP);

                std::vector<RegSet> block_gen(n_blocks);
                std::vector<RegSet> block_kill(n_blocks);
                for (size_t bi = 0; bi < n_blocks; ++bi) {
                    if (is_jt_def_block[bi]) {
                        block_kill[bi].set();
                        continue;
                    }
                    const BasicBlock& bb = *fn->blocks[bi];
                    RegSet gen_b, kill_b;
                    for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                        if (it->dead) continue;
                        RegSet h_reads, h_writes;
                        if (classify_helper_reg_effects(*it, h_reads, h_writes)) {
                            gen_b = (gen_b & ~h_writes) | h_reads;
                            kill_b = (kill_b & ~h_reads) | h_writes;
                            continue;
                        }
                        if (it->text.find("musttail call") != std::string::npos) {
                            gen_b.set();
                            continue;
                        }
                        if (it->tag == OpTag::Return || it->text == "ret void") {
                            gen_b |= aapcs_ret_live;
                            for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                                if (!aapcs_ret_live.test(r)) kill_b.set(r);
                            }
                            continue;
                        }
                        if (it->tag == OpTag::DirectCall) {
                            gen_b.reset(0);
                            kill_b.set(0);
                            gen_b |= aapcs_ext_call_reads;
                            continue;
                        }
                        if (it->tag == OpTag::IndirectCall || it->tag == OpTag::SvcTrap ||
                            it->tag == OpTag::BrkTrap || it->tag == OpTag::UdfTrap ||
                            it->text.find("(ptr %c") != std::string::npos) {
                            gen_b.set();
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
                bool dse_df = true;
                while (dse_df) {
                    dse_df = false;
                    for (int bi = static_cast<int>(n_blocks) - 1; bi >= 0; --bi) {
                        size_t u = static_cast<size_t>(bi);
                        if (is_jt_def_block[u]) continue;
                        RegSet out_u;
                        if (succs[u].empty()) {
                            bool u_is_musttail = false;
                            for (auto rit = fn->blocks[u]->insts.rbegin(); rit != fn->blocks[u]->insts.rend(); ++rit) {
                                if (rit->dead || rit->tag == OpTag::Comment || rit->tag == OpTag::GuestInsnMarker) continue;
                                if (rit->text.find("musttail call") != std::string::npos) u_is_musttail = true;
                                break;
                            }
                            out_u = u_is_musttail ? RegSet().set() : aapcs_ret_live;
                        } else {
                            for (size_t s : succs[u]) {
                                if (!is_jt_def_block[s]) out_u |= live_in[s];
                            }
                        }
                        RegSet in_u = block_gen[u] | (out_u & ~block_kill[u]);
                        if (out_u != live_out[u] || in_u != live_in[u]) {
                            live_out[u] = out_u;
                            live_in[u] = in_u;
                            dse_df = true;
                        }
                    }
                }

                for (size_t bi = 0; bi < n_blocks; ++bi) {
                    if (is_jt_def_block[bi]) continue;
                    BasicBlock& bb = *fn->blocks[bi];
                    RegSet live = live_out[bi];
                    for (auto it = bb.insts.rbegin(); it != bb.insts.rend(); ++it) {
                        if (it->dead) continue;
                        RegSet h_reads, h_writes;
                        if (classify_helper_reg_effects(*it, h_reads, h_writes)) {
                            live = (live & ~h_writes) | h_reads;
                            continue;
                        }
                        if (it->text.find("musttail call") != std::string::npos) {
                            live.set();
                            continue;
                        }
                        if (it->tag == OpTag::Return || it->text == "ret void") {
                            live = aapcs_ret_live;
                            continue;
                        }
                        if (it->tag == OpTag::DirectCall) {
                            live.reset(0);
                            live |= aapcs_ext_call_reads;
                            continue;
                        }
                        if (it->tag == OpTag::IndirectCall || it->tag == OpTag::SvcTrap ||
                            it->tag == OpTag::BrkTrap || it->tag == OpTag::UdfTrap ||
                            it->text.find("(ptr %c") != std::string::npos) {
                            live.set();
                            continue;
                        }
                        if ((it->tag == OpTag::CpuRegWrite || it->tag == OpTag::FlagWrite) &&
                            it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                            size_t r = static_cast<size_t>(it->arch_reg);
                            if (!live.test(r)) {
                                it->dead = true;
                                changed = true;
                            } else {
                                live.reset(r);
                            }
                        } else if (it->tag == OpTag::CpuRegRead &&
                                   it->arch_reg >= 0 && it->arch_reg < static_cast<int>(MAX_ARCH_REGS)) {
                            live.set(static_cast<size_t>(it->arch_reg));
                        }
                    }
                }
            }

            std::set<int> read_regs, written_regs, all_regs;
            size_t full_call_count = 0;
            for (size_t bi = 0; bi < n_blocks; ++bi) {
                if (is_jt_def_block[bi]) continue;
                for (const auto& inst : fn->blocks[bi]->insts) {
                    if (inst.dead) continue;
                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0) {
                        read_regs.insert(inst.arch_reg);
                        all_regs.insert(inst.arch_reg);
                    } else if ((inst.tag == OpTag::CpuRegWrite || inst.tag == OpTag::FlagWrite) && inst.arch_reg >= 0) {
                        written_regs.insert(inst.arch_reg);
                        all_regs.insert(inst.arch_reg);
                    } else if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall) {
                        ++full_call_count;
                    }
                }
            }
            if (all_regs.empty() && !defer_sp_sync) {
                if (optimize_ssa_and_dce(*fn)) changed = true;
                continue;
            }

            // 3. Upward-exposed entry reload pruning
            std::unordered_set<int> killed_on_entry;
            std::unordered_set<int> read_on_entry;
            auto scan_entry_kills = [&](const BasicBlock* b) {
                for (const auto& inst : b->insts) {
                    if (inst.dead) continue;
                    RegSet h_reads, h_writes;
                    if (classify_helper_reg_effects(inst, h_reads, h_writes)) {
                        for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                            if (h_reads.test(r) && !killed_on_entry.count(static_cast<int>(r))) {
                                read_on_entry.insert(static_cast<int>(r));
                            }
                        }
                        for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                            if (h_writes.test(r) && !read_on_entry.count(static_cast<int>(r))) {
                                killed_on_entry.insert(static_cast<int>(r));
                            }
                        }
                        continue;
                    }
                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                        inst.tag == OpTag::SvcTrap ||
                        (inst.text.find("call ") != std::string::npos && inst.text.find("(ptr %c") != std::string::npos)) {
                        break;
                    }
                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0) {
                        if (!killed_on_entry.count(inst.arch_reg)) {
                            read_on_entry.insert(inst.arch_reg);
                        }
                    } else if ((inst.tag == OpTag::CpuRegWrite || inst.tag == OpTag::FlagWrite) && inst.arch_reg >= 0) {
                        if (!read_on_entry.count(inst.arch_reg)) {
                            killed_on_entry.insert(inst.arch_reg);
                        }
                    }
                }
            };

            BasicBlock* entry_bb = fn->blocks[0].get();
            if (enable_entry_kill_pruning_) {
                scan_entry_kills(entry_bb);
                if (fn->blocks.size() >= 2 && entry_bb->name == "entry" && !fn->has_all_labels) {
                    std::string first_lbl = block_label(fn->entry_pc);
                    if (fn->blocks[1]->name == first_lbl && preds[1].size() == 1) {
                        scan_entry_kills(fn->blocks[1].get());
                    }
                }
                for (int r : elided_callee_regs) {
                    killed_on_entry.insert(r);
                }
            }

            // 4. Adaptive Register-Pressure-Aware Promotion:
            if (full_call_count >= 1 && (all_regs.size() > 8 || defer_sp_sync || !elided_callee_regs.empty())) {
                std::unordered_map<int, int> post_dse_writes;
                for (const auto& bb : fn->blocks) {
                    for (const auto& inst : bb->insts) {
                        if (!inst.dead && inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0) {
                            post_dse_writes[inst.arch_reg]++;
                        }
                    }
                }
                std::vector<std::pair<int, int>> pure_cands;
                for (int r : elided_callee_regs) {
                    if (!all_regs.count(r)) continue;
                    int pwr = post_dse_writes[r];
                    int rd = orig_reg_reads[r];
                    if (n_blocks <= 32 && pwr <= 4 && rd >= 2) {
                        pure_cands.push_back({rd * 4 - pwr, r});
                    }
                }
                std::sort(pure_cands.begin(), pure_cands.end(), std::greater<std::pair<int, int>>());
                std::set<int> pure_local_regs;
                size_t max_callee_budget = defer_sp_sync ? 2 : 3;
                for (size_t ci = 0; ci < pure_cands.size() && pure_local_regs.size() < max_callee_budget; ++ci) {
                    pure_local_regs.insert(pure_cands[ci].second);
                }

                std::vector<std::pair<int, int>> cands;
                for (int r : {31, 29, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28}) {
                    if (elided_callee_regs.count(r)) continue;
                    if (defer_sp_sync && r == 31) continue;
                    int rd = orig_reg_reads[r], wr = orig_reg_writes[r];
                    int pwr = post_dse_writes[r];
                    if (full_call_count > 32) continue;
                    if (fn->blocks.size() > 260 && r != 31) continue;
                    if (r >= 19 && r <= 28 && pwr > 2 && !(r == 19 && full_call_count >= 20 && full_call_count <= 32)) continue;
                    int min_net = (r == 31 || r == 29) ? 2 : 12;
                    if (rd >= 3 && (rd - wr) >= min_net) {
                        int prio = (r == 31 || r == 29) ? (1000 + (rd - wr)) : (rd - wr);
                        cands.push_back({prio, r});
                    }
                }
                std::sort(cands.begin(), cands.end(), std::greater<std::pair<int, int>>());
                std::set<int> writethrough_regs;
                for (size_t ci = 0; ci < cands.size() && (pure_local_regs.size() + writethrough_regs.size()) < max_callee_budget; ++ci) {
                    writethrough_regs.insert(cands[ci].second);
                }

                std::set<int> promoted_regs = pure_local_regs;
                promoted_regs.insert(writethrough_regs.begin(), writethrough_regs.end());

                if (!promoted_regs.empty()) {
                    std::vector<Instruction> prolog;
                    PassLLVMBuilder entry_PB(*fn, entry_bb);
                    for (int r : promoted_regs) {
                        std::string loc = local_alloca_name(r);
                        Instruction al;
                        al.tag = OpTag::RawIR;
                        al.text = entry_PB.emit_alloca(TypeKind::Int64, 8, loc);
                        prolog.push_back(std::move(al));
                    }
                    for (int r : promoted_regs) {
                        if (enable_entry_kill_pruning_ && killed_on_entry.count(r)) continue;
                        prolog.push_back(make_reload_from_cpu(*fn, r));
                    }
                    entry_bb->insts.insert(entry_bb->insts.begin(), prolog.begin(), prolog.end());
                }

                if (!promoted_regs.empty() || defer_sp_sync) {
                    for (size_t bi = 0; bi < n_blocks; ++bi) {
                        if (is_jt_def_block[bi]) continue;
                        BasicBlock* bb = fn->blocks[bi].get();
                        std::vector<Instruction> new_insts;
                        new_insts.reserve(bb->insts.size() + 8);
                        for (auto& inst : bb->insts) {
                            if (inst.dead) continue;
                            if (defer_sp_sync &&
                                (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall || inst.tag == OpTag::SvcTrap)) {
                                std::string p1 = fn->next_reg("spf");
                                Instruction fl_sp;
                                fl_sp.tag = OpTag::RawIR;
                                fl_sp.text = p1 + " = getelementptr inbounds i8, ptr %c, i64 256\n  "
                                             "store i64 " + frame_sp_ssa + ", ptr " + p1 + ", align 8, !alias.scope !3, !noalias !4";
                                new_insts.push_back(std::move(fl_sp));
                                new_insts.push_back(std::move(inst));
                                std::string v2 = fn->next_reg("spv");
                                std::string p2 = fn->next_reg("spr");
                                Instruction rest_sp;
                                rest_sp.tag = OpTag::RawIR;
                                rest_sp.text = v2 + " = add i64 " + frame_sp_ssa + ", " + std::to_string(-saved_frame_sp_rel) + "\n  " +
                                               p2 + " = getelementptr inbounds i8, ptr %c, i64 256\n  "
                                               "store i64 " + v2 + ", ptr " + p2 + ", align 8, !alias.scope !3, !noalias !4";
                                new_insts.push_back(std::move(rest_sp));
                                changed = true;
                                continue;
                            }
                            if (inst.tag == OpTag::CpuRegRead && promoted_regs.count(inst.arch_reg)) {
                                std::string loc = local_alloca_name(inst.arch_reg);
                                inst.text = inst.dst_reg + " = load i64, ptr " + loc + ", align 8";
                                new_insts.push_back(std::move(inst));
                                changed = true;
                                continue;
                            }
                            if (inst.tag == OpTag::CpuRegWrite && promoted_regs.count(inst.arch_reg)) {
                                std::string loc = local_alloca_name(inst.arch_reg);
                                std::string loc_store = "store i64 " + inst.op0.repr + ", ptr " + loc + ", align 8";
                                if (pure_local_regs.count(inst.arch_reg)) {
                                    inst.text = inst.prefix_ir.empty() ? loc_store : (inst.prefix_ir + "\n  " + loc_store);
                                } else {
                                    inst.text = inst.text + "\n  " + loc_store;
                                }
                                new_insts.push_back(std::move(inst));
                                changed = true;
                                continue;
                            }
                            new_insts.push_back(std::move(inst));
                        }
                        bb->insts = std::move(new_insts);
                    }
                }
                if (optimize_ssa_and_dce(*fn)) changed = true;
                continue;
            }

            // 5. Full `%loc_*` alloca promotion for call-free or low-register-pressure functions
            std::vector<Instruction> prolog;
            PassLLVMBuilder entry_PB(*fn, entry_bb);
            for (int r : all_regs) {
                bool is_vec = (r >= 64);
                std::string loc = local_alloca_name(r);
                Instruction al;
                al.tag = OpTag::RawIR;
                al.text = entry_PB.emit_alloca(is_vec ? TypeKind::Int128 : TypeKind::Int64, is_vec ? 16 : 8, loc);
                prolog.push_back(std::move(al));
            }
            for (int r : all_regs) {
                if (enable_entry_kill_pruning_ && killed_on_entry.count(r)) continue;
                prolog.push_back(make_reload_from_cpu(*fn, r));
            }
            entry_bb->insts.insert(entry_bb->insts.begin(), prolog.begin(), prolog.end());

            for (auto& bb : fn->blocks) {
                std::vector<Instruction> new_insts;
                new_insts.reserve(bb->insts.size() * 2);
                std::unordered_set<int> synced_in_cpu;

                bool block_ends_in_return = false;
                for (auto it = bb->insts.rbegin(); it != bb->insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    if (it->tag == OpTag::Return || it->text == "ret void" ||
                        it->text.find("musttail call") != std::string::npos) {
                        block_ends_in_return = true;
                    }
                    break;
                }

                for (size_t idx = 0; idx < bb->insts.size(); ++idx) {
                    Instruction& inst = bb->insts[idx];
                    if (inst.dead) continue;

                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0) {
                        bool is_vec = (inst.arch_reg >= 64);
                        std::string loc = local_alloca_name(inst.arch_reg);
                        inst.text = inst.dst_reg + " = load " + (is_vec ? "i128" : "i64") + ", ptr " + loc +
                                    ", align " + (is_vec ? "16" : "8");
                        new_insts.push_back(std::move(inst));
                        changed = true;
                        continue;
                    }

                    if ((inst.tag == OpTag::CpuRegWrite || inst.tag == OpTag::FlagWrite) && inst.arch_reg >= 0) {
                        synced_in_cpu.erase(inst.arch_reg);
                        bool is_vec = (inst.arch_reg >= 64);
                        std::string loc = local_alloca_name(inst.arch_reg);
                        std::string store_stmt = "store " + std::string(is_vec ? "i128 " : "i64 ") + inst.op0.repr +
                                                 ", ptr " + loc + ", align " + (is_vec ? "16" : "8");
                        inst.text = inst.prefix_ir.empty() ? store_stmt : (inst.prefix_ir + "\n  " + store_stmt);
                        new_insts.push_back(std::move(inst));
                        changed = true;
                        continue;
                    }

                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall) {
                        const Function* known_callee = nullptr;
                        if (enable_interproc_pruning_ && inst.tag == OpTag::DirectCall) {
                            auto cit = M.fn_map.find(fn_symbol(inst.branch_target));
                            if (cit != M.fn_map.end() && !cit->second->has_unknown_calls &&
                                (!cit->second->summary_reads.empty() || !cit->second->summary_writes.empty())) {
                                known_callee = cit->second;
                            }
                        }

                        std::unordered_set<int> killed_after_call;
                        if (enable_entry_kill_pruning_) {
                            std::unordered_set<int> read_after_call;
                            for (size_t j = idx + 1; j < bb->insts.size(); ++j) {
                                const Instruction& post = bb->insts[j];
                                if (post.dead) continue;
                                RegSet h_reads, h_writes;
                                if (classify_helper_reg_effects(post, h_reads, h_writes)) {
                                    for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                                        if (h_reads.test(r) && !killed_after_call.count(static_cast<int>(r))) {
                                            read_after_call.insert(static_cast<int>(r));
                                        }
                                    }
                                    for (size_t r = 0; r < MAX_ARCH_REGS; ++r) {
                                        if (h_writes.test(r) && !read_after_call.count(static_cast<int>(r))) {
                                            killed_after_call.insert(static_cast<int>(r));
                                        }
                                    }
                                    continue;
                                }
                                if (post.tag == OpTag::DirectCall || post.tag == OpTag::IndirectCall ||
                                    post.tag == OpTag::SvcTrap ||
                                    (post.text.find("call ") != std::string::npos && post.text.find("(ptr %c") != std::string::npos)) {
                                    break;
                                }
                                if (post.tag == OpTag::CpuRegRead && post.arch_reg >= 0) {
                                    if (!killed_after_call.count(post.arch_reg)) read_after_call.insert(post.arch_reg);
                                } else if ((post.tag == OpTag::CpuRegWrite || post.tag == OpTag::FlagWrite) && post.arch_reg >= 0) {
                                    if (!read_after_call.count(post.arch_reg)) killed_after_call.insert(post.arch_reg);
                                }
                            }
                        }

                        for (int r : written_regs) {
                            if (elided_callee_regs.count(r)) continue;
                            if (synced_in_cpu.count(r)) continue;
                            if (known_callee && !known_callee->summary_reads.count(r)) {
                                if (!block_ends_in_return || known_callee->summary_writes.count(r)) {
                                    continue;
                                }
                            }
                            new_insts.push_back(make_flush_to_cpu(*fn, r));
                            synced_in_cpu.insert(r);
                        }
                        new_insts.push_back(std::move(inst));
                        for (int r : all_regs) {
                            if (elided_callee_regs.count(r)) continue;
                            if (known_callee && !known_callee->summary_writes.count(r)) {
                                continue;
                            }
                            if (is_aapcs64_callee_saved_reg(r)) {
                                continue;
                            }
                            if (enable_entry_kill_pruning_ && killed_after_call.count(r)) {
                                synced_in_cpu.erase(r);
                                continue;
                            }
                            new_insts.push_back(make_reload_from_cpu(*fn, r));
                            synced_in_cpu.insert(r);
                        }
                        continue;
                    }

                    bool is_musttail = (inst.text.find("musttail call") != std::string::npos);
                    if (inst.tag == OpTag::Return || inst.text == "ret void" ||
                        inst.tag == OpTag::BrkTrap || inst.tag == OpTag::UdfTrap || is_musttail) {
                        for (int r : written_regs) {
                            if (!is_musttail && elided_callee_regs.count(r)) continue;
                            if (!is_musttail && defer_sp_sync && r == 31 && full_call_count == 0) continue;
                            if (synced_in_cpu.count(r)) continue;
                            new_insts.push_back(make_flush_to_cpu(*fn, r));
                            synced_in_cpu.insert(r);
                        }
                        new_insts.push_back(std::move(inst));
                        if (is_musttail) {
                            while (idx + 1 < bb->insts.size()) {
                                ++idx;
                                if (!bb->insts[idx].dead) {
                                    new_insts.push_back(std::move(bb->insts[idx]));
                                }
                            }
                        }
                        continue;
                    }

                    if (inst.tag == OpTag::SvcTrap) {
                        for (int r : written_regs) {
                            if (elided_callee_regs.count(r)) continue;
                            if (synced_in_cpu.count(r)) continue;
                            new_insts.push_back(make_flush_to_cpu(*fn, r));
                            synced_in_cpu.insert(r);
                        }
                        new_insts.push_back(std::move(inst));
                        for (int r : all_regs) {
                            if (r == 0 || r == 1 || r == 2 || r == 3) {
                                new_insts.push_back(make_reload_from_cpu(*fn, r));
                                synced_in_cpu.insert(r);
                            }
                        }
                        continue;
                    }

                    RegSet h_reads, h_writes;
                    if (classify_helper_reg_effects(inst, h_reads, h_writes)) {
                        for (int r : written_regs) {
                            if (r >= 0 && r < static_cast<int>(MAX_ARCH_REGS) &&
                                h_reads.test(static_cast<size_t>(r)) && !synced_in_cpu.count(r)) {
                                new_insts.push_back(make_flush_to_cpu(*fn, r));
                                synced_in_cpu.insert(r);
                            }
                        }
                        new_insts.push_back(std::move(inst));
                        for (int r : all_regs) {
                            if (r >= 0 && r < static_cast<int>(MAX_ARCH_REGS) &&
                                h_writes.test(static_cast<size_t>(r))) {
                                new_insts.push_back(make_reload_from_cpu(*fn, r));
                                synced_in_cpu.insert(r);
                            }
                        }
                        continue;
                    }

                    if (inst.text.find("call ") != std::string::npos && inst.text.find("(ptr %c") != std::string::npos) {
                        for (int r : written_regs) {
                            if ((r == 32 || r >= 64) && !synced_in_cpu.count(r)) {
                                new_insts.push_back(make_flush_to_cpu(*fn, r));
                                synced_in_cpu.insert(r);
                            }
                        }
                        new_insts.push_back(std::move(inst));
                        for (int r : all_regs) {
                            if (r == 32 || r >= 64) {
                                new_insts.push_back(make_reload_from_cpu(*fn, r));
                                synced_in_cpu.insert(r);
                            }
                        }
                        continue;
                    }

                    new_insts.push_back(std::move(inst));
                }
                bb->insts = std::move(new_insts);
            }

            if (optimize_ssa_and_dce(*fn)) changed = true;
        }
        return changed;
    }

private:
    bool enable_entry_kill_pruning_;
    bool enable_interproc_pruning_;

    static bool is_aapcs64_callee_saved_reg(int r) {
        return (r >= 19 && r <= 29) || (r == 31);
    }

    static bool is_ident_char(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.';
    }

    static bool optimize_ssa_and_dce(Function& fn) {
        bool changed = false;

        // 1. Flatten multi-line RawIR instructions into single-line Instruction objects
        for (auto& bb : fn.blocks) {
            std::vector<Instruction> flat;
            flat.reserve(bb->insts.size() * 2);
            for (auto& inst : bb->insts) {
                if (inst.dead) continue;
                if (inst.text.find('\n') == std::string::npos) {
                    flat.push_back(std::move(inst));
                    continue;
                }
                std::stringstream ss(inst.text);
                std::string line;
                while (std::getline(ss, line)) {
                    size_t first = 0;
                    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
                    if (first >= line.size()) continue;
                    Instruction sub;
                    sub.tag = (line.rfind("; ", first) == first) ? OpTag::Comment : OpTag::RawIR;
                    sub.text = line.substr(first);
                    flat.push_back(std::move(sub));
                }
            }
            bb->insts = std::move(flat);
        }

        // 2. Count SSA definitions across the function
        std::unordered_map<std::string, int> def_count;
        for (const auto& bb : fn.blocks) {
            for (const auto& inst : bb->insts) {
                if (inst.dead || inst.text.empty() || inst.text[0] != '%') continue;
                size_t eq = inst.text.find(" = ");
                if (eq != std::string::npos) {
                    def_count[inst.text.substr(0, eq)]++;
                }
            }
        }

        std::unordered_map<std::string, std::string> ssa_subst;
        auto resolve_subst = [&](const std::string& tok) -> std::string {
            std::string cur = tok;
            int depth = 0;
            while (depth++ < 16) {
                auto it = ssa_subst.find(cur);
                if (it == ssa_subst.end()) break;
                cur = it->second;
            }
            return cur;
        };

        auto apply_subst_to_rhs = [&](std::string& text) -> bool {
            if (ssa_subst.empty() || text.empty() || text[0] == ';') return false;
            size_t start_pos = 0;
            if (text[0] == '%') {
                size_t eq = text.find(" = ");
                if (eq != std::string::npos) start_pos = eq + 3;
            }
            bool mod = false;
            std::string out;
            out.reserve(text.size());
            out.append(text, 0, start_pos);
            size_t i = start_pos;
            while (i < text.size()) {
                if (text[i] == '%') {
                    size_t j = i + 1;
                    while (j < text.size() && is_ident_char(text[j])) ++j;
                    if (j > i + 1) {
                        std::string tok = text.substr(i, j - i);
                        auto it = ssa_subst.find(tok);
                        if (it != ssa_subst.end()) {
                            std::string rep = resolve_subst(it->second);
                            out.append(rep);
                            mod = true;
                            i = j;
                            continue;
                        }
                    }
                }
                out.push_back(text[i]);
                ++i;
            }
            if (mod) text = std::move(out);
            return mod;
        };

        // 3. Per-block copy propagation & typed cast round-trip / CSE folding
        for (auto& bb : fn.blocks) {
            std::unordered_map<std::string, std::pair<std::string, std::string>> vec_to_i128;
            std::unordered_map<std::string, std::string> i128_to_vec_cse;
            std::unordered_map<std::string, std::pair<std::string, std::string>> int_to_i128;
            std::unordered_map<std::string, std::string> i128_to_int_cse;
            std::unordered_map<std::string, std::string> i32_to_i64;
            std::unordered_map<std::string, std::string> i64_to_i32_cse;
            std::unordered_map<std::string, std::pair<std::string, std::string>> fp_to_int;
            std::unordered_map<std::string, std::string> int_to_fp_cse;
            std::unordered_map<std::string, std::string> pure_rhs_cse;

            for (auto& inst : bb->insts) {
                if (inst.dead || inst.text.empty() || inst.text[0] == ';') continue;
                if (apply_subst_to_rhs(inst.text)) changed = true;

                if (inst.text[0] != '%') continue;
                size_t eq = inst.text.find(" = ");
                if (eq == std::string::npos) continue;
                std::string dst = inst.text.substr(0, eq);
                if (def_count[dst] != 1) continue;
                std::string rhs = inst.text.substr(eq + 3);

                // Identity copies: add/or i32/i64/i128 0, X
                for (const char* pfx : {"add i64 0, ", "add i128 0, ", "add i32 0, ", "or i64 0, ", "or i128 0, ", "or i32 0, "}) {
                    size_t plen = std::strlen(pfx);
                    if (rhs.rfind(pfx, 0) == 0) {
                        std::string src = rhs.substr(plen);
                        if (!src.empty() && src.find(' ') == std::string::npos) {
                            ssa_subst[dst] = resolve_subst(src);
                            inst.dead = true;
                            changed = true;
                            break;
                        }
                    }
                }
                if (inst.dead) continue;

                // Bitcast round-trips & CSE: bitcast <src_ty> <src_val> to <dst_ty>
                if (rhs.rfind("bitcast ", 0) == 0) {
                    size_t to_pos = rhs.rfind(" to ");
                    if (to_pos != std::string::npos) {
                        std::string dst_ty = rhs.substr(to_pos + 4);
                        std::string mid = rhs.substr(8, to_pos - 8);
                        size_t sp = mid.rfind(' ');
                        if (sp != std::string::npos) {
                            std::string src_ty = mid.substr(0, sp);
                            std::string src_val = mid.substr(sp + 1);
                            if (dst_ty == "i128" && !src_ty.empty() && src_ty[0] == '<') {
                                auto it = vec_to_i128.find(src_val);
                                if (it != vec_to_i128.end() && it->second.second == "i128") {
                                    ssa_subst[dst] = it->second.first;
                                    inst.dead = true;
                                    changed = true;
                                    continue;
                                }
                                vec_to_i128[dst] = {src_val, src_ty};
                                i128_to_vec_cse[dst + "|" + src_ty] = src_val;
                            } else if (src_ty == "i128" && !dst_ty.empty() && dst_ty[0] == '<') {
                                auto it = vec_to_i128.find(src_val);
                                if (it != vec_to_i128.end() && it->second.second == dst_ty) {
                                    ssa_subst[dst] = it->second.first;
                                    inst.dead = true;
                                    changed = true;
                                    continue;
                                }
                                std::string key = src_val + "|" + dst_ty;
                                auto cit = i128_to_vec_cse.find(key);
                                if (cit != i128_to_vec_cse.end()) {
                                    ssa_subst[dst] = cit->second;
                                    inst.dead = true;
                                    changed = true;
                                    continue;
                                }
                                i128_to_vec_cse[key] = dst;
                                vec_to_i128[dst] = {src_val, "i128"};
                            } else if ((src_ty == "float" && dst_ty == "i32") || (src_ty == "double" && dst_ty == "i64")) {
                                fp_to_int[dst] = {src_val, src_ty};
                                int_to_fp_cse[dst + "|" + src_ty] = src_val;
                            } else if ((src_ty == "i32" && dst_ty == "float") || (src_ty == "i64" && dst_ty == "double")) {
                                auto it = fp_to_int.find(src_val);
                                if (it != fp_to_int.end() && it->second.second == dst_ty) {
                                    ssa_subst[dst] = it->second.first;
                                    inst.dead = true;
                                    changed = true;
                                    continue;
                                }
                                std::string key = src_val + "|" + dst_ty;
                                auto cit = int_to_fp_cse.find(key);
                                if (cit != int_to_fp_cse.end()) {
                                    ssa_subst[dst] = cit->second;
                                    inst.dead = true;
                                    changed = true;
                                    continue;
                                }
                                int_to_fp_cse[key] = dst;
                            }
                        }
                    }
                    continue;
                }

                // zext i32 <src> to i64 and trunc i64 <src> to i32
                if (rhs.rfind("zext i32 ", 0) == 0 && rhs.size() > 16 && rhs.rfind(" to i64") == rhs.size() - 7) {
                    std::string src_val = rhs.substr(9, rhs.size() - 16);
                    i32_to_i64[dst] = src_val;
                    i64_to_i32_cse[dst] = src_val;
                    continue;
                }
                if (rhs.rfind("trunc i64 ", 0) == 0 && rhs.size() > 17 && rhs.rfind(" to i32") == rhs.size() - 7) {
                    std::string src_val = rhs.substr(10, rhs.size() - 17);
                    auto it = i32_to_i64.find(src_val);
                    if (it != i32_to_i64.end()) {
                        ssa_subst[dst] = it->second;
                        inst.dead = true;
                        changed = true;
                        continue;
                    }
                    auto cit = i64_to_i32_cse.find(src_val);
                    if (cit != i64_to_i32_cse.end()) {
                        ssa_subst[dst] = cit->second;
                        inst.dead = true;
                        changed = true;
                        continue;
                    }
                    i64_to_i32_cse[src_val] = dst;
                    continue;
                }

                // zext i32/i64 <src> to i128 and trunc i128 <src> to i32/i64
                if (rhs.rfind("zext i32 ", 0) == 0 || rhs.rfind("zext i64 ", 0) == 0) {
                    size_t to_pos = rhs.rfind(" to i128");
                    if (to_pos != std::string::npos && to_pos + 8 == rhs.size()) {
                        std::string int_ty = rhs.substr(5, 3);
                        std::string src_val = rhs.substr(9, to_pos - 9);
                        int_to_i128[dst] = {src_val, int_ty};
                        i128_to_int_cse[dst + "|" + int_ty] = src_val;
                    }
                    continue;
                }
                if (rhs.rfind("shufflevector ", 0) == 0 || rhs.rfind("extractelement ", 0) == 0) {
                    auto cit = pure_rhs_cse.find(rhs);
                    if (cit != pure_rhs_cse.end()) {
                        ssa_subst[dst] = cit->second;
                        inst.dead = true;
                        changed = true;
                        continue;
                    }
                    pure_rhs_cse[rhs] = dst;
                    continue;
                }
                if (rhs.rfind("trunc i128 ", 0) == 0) {
                    size_t to_pos = rhs.rfind(" to ");
                    if (to_pos != std::string::npos) {
                        std::string int_ty = rhs.substr(to_pos + 4);
                        if (int_ty == "i32" || int_ty == "i64") {
                            std::string src_val = rhs.substr(11, to_pos - 11);
                            auto it = int_to_i128.find(src_val);
                            if (it != int_to_i128.end() && it->second.second == int_ty) {
                                ssa_subst[dst] = it->second.first;
                                inst.dead = true;
                                changed = true;
                                continue;
                            }
                            std::string key = src_val + "|" + int_ty;
                            auto cit = i128_to_int_cse.find(key);
                            if (cit != i128_to_int_cse.end()) {
                                ssa_subst[dst] = cit->second;
                                inst.dead = true;
                                changed = true;
                                continue;
                            }
                            i128_to_int_cse[key] = dst;
                        }
                    }
                    continue;
                }
            }
        }

        // Apply any remaining cross-block substitutions
        if (!ssa_subst.empty()) {
            for (auto& bb : fn.blocks) {
                for (auto& inst : bb->insts) {
                    if (!inst.dead) {
                        if (apply_subst_to_rhs(inst.text)) changed = true;
                    }
                }
            }
        }

        // 4. Multi-pass dead pure SSA instruction elimination
        for (int iter = 0; iter < 8; ++iter) {
            std::unordered_set<std::string> used_regs;
            for (const auto& bb : fn.blocks) {
                for (const auto& inst : bb->insts) {
                    if (inst.dead || inst.text.empty() || inst.text[0] == ';') continue;
                    size_t start_pos = 0;
                    if (inst.text[0] == '%') {
                        size_t eq = inst.text.find(" = ");
                        if (eq != std::string::npos) start_pos = eq + 3;
                    }
                    size_t i = start_pos;
                    while (i < inst.text.size()) {
                        if (inst.text[i] == '%') {
                            size_t j = i + 1;
                            while (j < inst.text.size() && is_ident_char(inst.text[j])) ++j;
                            if (j > i + 1) {
                                used_regs.insert(inst.text.substr(i, j - i));
                            }
                            i = j;
                        } else {
                            ++i;
                        }
                    }
                }
            }

            bool killed_any = false;
            for (auto& bb : fn.blocks) {
                for (auto& inst : bb->insts) {
                    if (inst.dead || inst.text.empty() || inst.text[0] != '%') continue;
                    size_t eq = inst.text.find(" = ");
                    if (eq == std::string::npos) continue;
                    std::string dst = inst.text.substr(0, eq);
                    if (def_count[dst] != 1 || used_regs.count(dst) != 0) continue;
                    std::string rhs = inst.text.substr(eq + 3);
                    if (rhs.rfind("alloca ", 0) == 0) continue;
                    if (rhs.find("load volatile") != std::string::npos) continue;
                    if (rhs.rfind("call ", 0) == 0) continue;
                    if (rhs.rfind("fadd ", 0) == 0 || rhs.rfind("fsub ", 0) == 0 ||
                        rhs.rfind("fmul ", 0) == 0 || rhs.rfind("fdiv ", 0) == 0) continue;
                    inst.dead = true;
                    killed_any = true;
                    changed = true;
                }
            }
            if (!killed_any) break;
        }

        // Compact non-dead instructions
        for (auto& bb : fn.blocks) {
            std::vector<Instruction> compact;
            compact.reserve(bb->insts.size());
            for (auto& inst : bb->insts) {
                if (!inst.dead) compact.push_back(std::move(inst));
            }
            bb->insts = std::move(compact);
        }

        return changed;
    }

    static std::string local_alloca_name(int reg) {
        if (reg >= 0 && reg <= 30) return "%loc_x" + std::to_string(reg);
        if (reg == 31) return "%loc_sp";
        if (reg == 32) return "%loc_nzcv";
        if (reg >= 64 && reg <= 95) return "%loc_v" + std::to_string(reg - 64);
        return "%loc_r" + std::to_string(reg);
    }

    static int cpu_byte_offset(int reg) {
        if (reg >= 0 && reg <= 30) return reg * 8;
        if (reg == 31) return 256;
        if (reg == 32) return 272;
        if (reg >= 64 && reg <= 95) return 512 + (reg - 64) * 16;
        return 0;
    }

    static Instruction make_reload_from_cpu(Function& fn, int reg) {
        bool is_vec = (reg >= 64);
        int off = cpu_byte_offset(reg);
        std::string loc = local_alloca_name(reg);
        std::string p = fn.next_reg("ldp");
        std::string v = fn.next_reg("ldv");
        const char* ty = is_vec ? "i128" : "i64";
        const char* al = is_vec ? "16" : "8";
        Instruction inst;
        inst.tag = OpTag::RawIR;
        inst.text = p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(off) + "\n  " +
                    v + " = load " + ty + ", ptr " + p + ", align " + al + ", !alias.scope !3, !noalias !4\n  " +
                    "store " + ty + " " + v + ", ptr " + loc + ", align " + al;
        return inst;
    }

    static Instruction make_flush_to_cpu(Function& fn, int reg) {
        bool is_vec = (reg >= 64);
        int off = cpu_byte_offset(reg);
        std::string loc = local_alloca_name(reg);
        std::string v = fn.next_reg("flsv");
        std::string p = fn.next_reg("flsp");
        const char* ty = is_vec ? "i128" : "i64";
        const char* al = is_vec ? "16" : "8";
        Instruction inst;
        inst.tag = OpTag::RawIR;
        inst.text = v + " = load " + ty + ", ptr " + loc + ", align " + al + "\n  " +
                    p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(off) + "\n  " +
                    "store " + ty + " " + v + ", ptr " + p + ", align " + al + ", !alias.scope !3, !noalias !4";
        return inst;
    }
};


} // namespace

bool CpuStatePromotionPass::run(Module& M) {
    CpuStatePromotionPassImpl impl(enable_entry_kill_pruning_, enable_interproc_pruning_);
    return impl.run(M);
}

} // namespace ultrs::passes
