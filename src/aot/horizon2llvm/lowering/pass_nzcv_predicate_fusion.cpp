// UltraTears LLVM Pass Implementation: Pass 16 - NZCVPredicateFusionPass (src/horizon2llvm/lowering/pass_nzcv_predicate_fusion.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include "passes.hpp"
#include "passes_llvm_utils.hpp"

namespace ultrs::passes {

namespace {

struct CCMPFusionInfo {
    bool valid = false;
    int cmp_kind = 1; // 1 = SUB (CCMP), 2 = ADD (CCMN)
    unsigned cmp_sf = 1;
    std::string outer_cond_repr;
    std::string lhs_repr;
    std::string rhs_repr;
    std::string res_repr;
    unsigned nzcv_imm = 0; // 4-bit immediate flags
};

class NZCVPredicateFusionPassImpl {
public:
    explicit NZCVPredicateFusionPassImpl(bool enabled = true, bool enable_interblock_dse = true)
        : enabled_(enabled), enable_interblock_dse_(enable_interblock_dse) {}

    static const char* static_name() { return "nzcv-predicate-fusion-pass"; }
    const char* pass_name() const { return "nzcv-predicate-fusion-pass"; }

    bool run(Module& M) {
        if (!enabled_) return false;
        bool changed = false;
        for (auto& fn : M.functions) {
            if (!fn->hle_symbol.empty() || fn->blocks.empty()) continue;

            // Phase 1: Intra-block FlagWrite (including CCMP/CCMN chains) + CondEval -> direct `icmp` / `fcmp` fusion
            for (auto& bb : fn->blocks) {
                int active_fw_idx = -1;
                CCMPFusionInfo active_ccmp{};
                std::unordered_map<std::string, size_t> nzcv_read_inst_idx;

                for (size_t i = 0; i < bb->insts.size(); ++i) {
                    Instruction& inst = bb->insts[i];
                    if (inst.dead) continue;

                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                        inst.tag == OpTag::SvcTrap || inst.text.find("@rt_") != std::string::npos) {
                        active_fw_idx = -1;
                        active_ccmp = {};
                        continue;
                    }

                    if (inst.tag == OpTag::FlagWrite) {
                        if (inst.cmp_kind >= 1 && inst.cmp_kind <= 5) {
                            active_fw_idx = static_cast<int>(i);
                            active_ccmp = {};
                        } else {
                            active_fw_idx = -1;
                            active_ccmp = try_extract_ccmp_info(*bb, i);
                        }
                        continue;
                    }

                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg == 32) {
                        nzcv_read_inst_idx[inst.dst_reg] = i;
                        continue;
                    }

                    if (inst.cond_code >= 0 && inst.cond_code < 14 && !inst.dst_reg.empty()) {
                        if (active_fw_idx >= 0) {
                            const Instruction& fw = bb->insts[static_cast<size_t>(active_fw_idx)];
                            std::string fused_ir = try_fuse_condition(fw, inst.cond_code, inst.dst_reg);
                            if (!fused_ir.empty()) {
                                inst.text = std::string("; [NZCVPredicateFusionPass] fused NZCV condition -> direct ") +
                                            (fw.cmp_kind >= 4 ? "fcmp\n  " : "icmp\n  ") + fused_ir;
                                auto r_it = nzcv_read_inst_idx.find(inst.op0.repr);
                                if (r_it != nzcv_read_inst_idx.end()) {
                                    bb->insts[r_it->second].dead = true;
                                }
                                changed = true;
                            }
                        } else if (active_ccmp.valid) {
                            std::string fused_ir = try_fuse_ccmp_condition(*fn, active_ccmp, inst.cond_code, inst.dst_reg);
                            if (!fused_ir.empty()) {
                                inst.text = "; [NZCVPredicateFusionPass] fused CCMP/CCMN condition -> direct select+icmp\n  " + fused_ir;
                                auto r_it = nzcv_read_inst_idx.find(inst.op0.repr);
                                if (r_it != nzcv_read_inst_idx.end()) {
                                    bb->insts[r_it->second].dead = true;
                                }
                                changed = true;
                            }
                        }
                    }
                }
            }

            // Phase 2: Inter-block backward liveness for NZCV (`arch_reg == 32`)
            if (!enable_interblock_dse_) continue;
            std::unordered_map<std::string, bool> kills_nzcv_on_entry;
            for (const auto& bb : fn->blocks) {
                bool kills = false;
                for (const auto& inst : bb->insts) {
                    if (inst.dead) continue;
                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg == 32) {
                        kills = false;
                        break;
                    }
                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                        inst.tag == OpTag::SvcTrap || inst.tag == OpTag::Return) {
                        kills = true;
                        break;
                    }
                    if (inst.text.find("@rt_") != std::string::npos) {
                        kills = false;
                        break;
                    }
                    if (inst.tag == OpTag::FlagWrite) {
                        kills = true;
                        break;
                    }
                }
                kills_nzcv_on_entry[bb->name] = kills;
            }

            for (auto& bb : fn->blocks) {
                std::vector<std::string> succs;
                for (auto it = bb->insts.rbegin(); it != bb->insts.rend(); ++it) {
                    if (it->dead || it->tag == OpTag::Comment || it->tag == OpTag::GuestInsnMarker) continue;
                    if (it->tag == OpTag::UncondBranch && !it->target_label.empty()) {
                        succs.push_back(it->target_label);
                    } else if (it->tag == OpTag::CondBranch) {
                        if (!it->target_label.empty()) succs.push_back(it->target_label);
                        if (!it->false_label.empty()) succs.push_back(it->false_label);
                    }
                    break;
                }
                if (succs.empty()) continue;

                bool all_succs_kill = true;
                for (const auto& s : succs) {
                    auto it = kills_nzcv_on_entry.find(s);
                    if (it == kills_nzcv_on_entry.end() || !it->second) {
                        all_succs_kill = false;
                        break;
                    }
                }
                if (!all_succs_kill) continue;

                for (auto it = bb->insts.rbegin(); it != bb->insts.rend(); ++it) {
                    if (it->dead) continue;
                    if (it->tag == OpTag::CpuRegRead && it->arch_reg == 32) break;
                    if (it->tag == OpTag::DirectCall || it->tag == OpTag::IndirectCall ||
                        it->tag == OpTag::SvcTrap || it->text.find("@rt_") != std::string::npos) {
                        break;
                    }
                    if (it->tag == OpTag::FlagWrite) {
                        it->dead = true;
                        changed = true;
                        break;
                    }
                }
            }
        }
        return changed;
    }

private:
    bool enabled_;
    bool enable_interblock_dse_;

    // Recognize the SSA sequence emitted by `lift_dp_reg` (`op2 == 2`, CCMP / CCMN) immediately before a `FlagWrite`
    static CCMPFusionInfo try_extract_ccmp_info(const BasicBlock& bb, size_t fw_idx) {
        CCMPFusionInfo info{};
        if (fw_idx < 2) return info;
        const Instruction& sel_inst = bb.insts[fw_idx - 1];
        if (sel_inst.dead || sel_inst.tag != OpTag::RawIR) return info;

        // Match: `%sel84 = select i1 %cond58, i64 %t83, i64 1073741824`
        char sel_dst[64] = {}, cond_reg[64] = {}, packed_reg[64] = {};
        unsigned long long imm_u64 = 0;
        if (std::sscanf(sel_inst.text.c_str(),
                        "%63s = select i1 %63[^,], i64 %63[^,], i64 %llu",
                        sel_dst, cond_reg, packed_reg, &imm_u64) != 4) {
            return info;
        }
        if (bb.insts[fw_idx].op0.repr != sel_dst) return info;

        // Scan backwards in the same guest instruction (`guest_pc` or up to 24 instructions) for `%t64 = sub/add i32/i64 %a, %b`
        for (int k = static_cast<int>(fw_idx) - 2; k >= 0 && k >= static_cast<int>(fw_idx) - 24; --k) {
            const Instruction& prev = bb.insts[static_cast<size_t>(k)];
            if (prev.dead) continue;
            if (prev.tag == OpTag::GuestInsnMarker || prev.tag == OpTag::FlagWrite) break;
            if (prev.tag == OpTag::RawIR) {
                char r_dst[64] = {}, op_str[16] = {}, ty_str[16] = {}, lhs[64] = {}, rhs[64] = {};
                if (std::sscanf(prev.text.c_str(), "%63s = %15s %15s %63[^,], %63s",
                                r_dst, op_str, ty_str, lhs, rhs) == 5 &&
                    (std::strcmp(op_str, "sub") == 0 || std::strcmp(op_str, "add") == 0) &&
                    (std::strcmp(ty_str, "i64") == 0 || std::strcmp(ty_str, "i32") == 0)) {
                    info.valid = true;
                    info.cmp_kind = (std::strcmp(op_str, "sub") == 0) ? 1 : 2;
                    info.cmp_sf = (std::strcmp(ty_str, "i64") == 0) ? 1 : 0;
                    info.outer_cond_repr = cond_reg;
                    info.lhs_repr = lhs;
                    info.rhs_repr = rhs;
                    info.res_repr = r_dst;
                    info.nzcv_imm = static_cast<unsigned>((imm_u64 >> 28) & 0xFULL);
                    return info;
                }
            }
        }
        return info;
    }

    static bool eval_static_nzcv_cond(unsigned nzcv_imm, int cond) {
        bool N = ((nzcv_imm >> 3) & 1u) != 0;
        bool Z = ((nzcv_imm >> 2) & 1u) != 0;
        bool C = ((nzcv_imm >> 1) & 1u) != 0;
        bool V = (nzcv_imm & 1u) != 0;
        switch (cond) {
            case 0:  return Z;               // EQ
            case 1:  return !Z;              // NE
            case 2:  return C;               // CS / HS
            case 3:  return !C;              // CC / LO
            case 4:  return N;               // MI
            case 5:  return !N;              // PL
            case 6:  return V;               // VS
            case 7:  return !V;              // VC
            case 8:  return C && !Z;         // HI
            case 9:  return !C || Z;         // LS
            case 10: return N == V;          // GE
            case 11: return N != V;          // LT
            case 12: return !Z && (N == V);  // GT
            case 13: return Z || (N != V);   // LE
            default: return true;
        }
    }

    static std::string try_fuse_ccmp_condition(Function& fn, const CCMPFusionInfo& ccmp, int cond, const std::string& out_reg) {
        Instruction synth_fw;
        synth_fw.cmp_kind = ccmp.cmp_kind;
        synth_fw.cmp_sf = ccmp.cmp_sf;
        synth_fw.op1.repr = ccmp.lhs_repr;
        synth_fw.op2.repr = ccmp.rhs_repr;
        synth_fw.dst_reg = ccmp.res_repr;

        std::string inner_reg = fn.next_reg("ccmp_in");
        std::string inner_ir = try_fuse_condition(synth_fw, cond, inner_reg);
        if (inner_ir.empty()) return "";

        bool fb = eval_static_nzcv_cond(ccmp.nzcv_imm, cond);
        return inner_ir + "\n  " + out_reg + " = select i1 " + ccmp.outer_cond_repr +
               ", i1 " + inner_reg + ", i1 " + (fb ? "true" : "false");
    }

    static std::string try_fuse_condition(const Instruction& fw, int cond, const std::string& out_reg) {
        const char* ty = fw.cmp_sf ? "i64" : "i32";
        const std::string& a = fw.op1.repr;
        const std::string& b = fw.op2.repr;
        const std::string& r = fw.dst_reg;
        if (fw.cmp_kind == 1) { // SUBS / CMP (a - b)
            const char* pred = nullptr;
            switch (cond) {
                case 0:  pred = "eq";  break; // EQ
                case 1:  pred = "ne";  break; // NE
                case 2:  pred = "uge"; break; // CS / HS
                case 3:  pred = "ult"; break; // CC / LO
                case 4:  return out_reg + " = icmp slt " + ty + " " + r + ", 0"; // MI
                case 5:  return out_reg + " = icmp sge " + ty + " " + r + ", 0"; // PL
                case 8:  pred = "ugt"; break; // HI
                case 9:  pred = "ule"; break; // LS
                case 10: pred = "sge"; break; // GE
                case 11: pred = "slt"; break; // LT
                case 12: pred = "sgt"; break; // GT
                case 13: pred = "sle"; break; // LE
                default: return "";
            }
            if (pred) {
                return out_reg + " = icmp " + pred + " " + ty + " " + a + ", " + b;
            }
        } else if (fw.cmp_kind == 2) { // ADDS / CMN (r = a + b)
            switch (cond) {
                case 0: return out_reg + " = icmp eq "  + ty + " " + r + ", 0";
                case 1: return out_reg + " = icmp ne "  + ty + " " + r + ", 0";
                case 2: return out_reg + " = icmp ult " + ty + " " + r + ", " + a;
                case 3: return out_reg + " = icmp uge " + ty + " " + r + ", " + a;
                case 4: return out_reg + " = icmp slt " + ty + " " + r + ", 0";
                case 5: return out_reg + " = icmp sge " + ty + " " + r + ", 0";
                default: return "";
            }
        } else if (fw.cmp_kind == 3) { // ANDS / TST (r = a & b, C=0, V=0)
            switch (cond) {
                case 0:  return out_reg + " = icmp eq "  + ty + " " + r + ", 0";
                case 1:  return out_reg + " = icmp ne "  + ty + " " + r + ", 0";
                case 4:
                case 11: return out_reg + " = icmp slt " + ty + " " + r + ", 0";
                case 5:
                case 10: return out_reg + " = icmp sge " + ty + " " + r + ", 0";
                case 12: return out_reg + " = icmp sgt " + ty + " " + r + ", 0";
                case 13: return out_reg + " = icmp sle " + ty + " " + r + ", 0";
                default: return "";
            }
        } else if (fw.cmp_kind == 4 || fw.cmp_kind == 5) { // FCMP_S / FCMP_D
            const char* fty = (fw.cmp_kind == 5) ? "double" : "float";
            const char* fpred = nullptr;
            switch (cond) {
                case 0:  fpred = "oeq"; break; // EQ (Z==1)
                case 1:  fpred = "une"; break; // NE (Z==0)
                case 2:  fpred = "uge"; break; // CS/HS (C==1)
                case 3:  fpred = "olt"; break; // CC/LO (C==0)
                case 4:  fpred = "olt"; break; // MI (N==1)
                case 5:  fpred = "uge"; break; // PL (N==0)
                case 6:  fpred = "uno"; break; // VS (V==1)
                case 7:  fpred = "ord"; break; // VC (V==0)
                case 8:  fpred = "ugt"; break; // HI (C==1 && Z==0)
                case 9:  fpred = "ole"; break; // LS (C==0 || Z==1)
                case 10: fpred = "oge"; break; // GE (N==V)
                case 11: fpred = "ult"; break; // LT (N!=V)
                case 12: fpred = "ogt"; break; // GT (Z==0 && N==V)
                case 13: fpred = "ule"; break; // LE (Z==1 || N!=V)
                default: return "";
            }
            if (fpred) {
                return out_reg + " = fcmp " + fpred + " " + fty + " " + a + ", " + b;
            }
        }
        return "";
    }
};

} // namespace

bool NZCVPredicateFusionPass::run(Module& M) {
    NZCVPredicateFusionPassImpl impl(enabled_, enable_interblock_dse_);
    return impl.run(M);
}

} // namespace ultrs::passes
