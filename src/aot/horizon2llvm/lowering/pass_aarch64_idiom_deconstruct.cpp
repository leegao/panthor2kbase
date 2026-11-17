// UltraTears LLVM Pass Implementation: Pass 12 - AArch64IdiomDeconstructPass (src/horizon2llvm/pass_aarch64_idiom_deconstruct.cpp)
//
// Uses LLVM 21 C++ APIs (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::Module`, `llvm::Function`,
// `llvm::BasicBlock`, `llvm::Instruction`) via `PassLLVMBuilder`.

#include "passes.hpp"
#include "passes_llvm_utils.hpp"

namespace ultrs::passes {

namespace {

class AArch64IdiomDeconstructPassImpl {
public:
    explicit AArch64IdiomDeconstructPassImpl(bool enabled = true) : enabled_(enabled) {}

    static const char* static_name() { return "aarch64-idiom-deconstruct-pass"; }
    const char* pass_name() const { return "aarch64-idiom-deconstruct-pass"; }

    bool run(Module& M) {
        if (!enabled_) return false;
        bool changed = false;
        for (auto& fn : M.functions) {
            if (!fn->hle_symbol.empty()) continue;

            for (auto& bb : fn->blocks) {
                std::unordered_map<std::string, uint64_t> known_consts;
                std::unordered_set<std::string> proven_nonzero;
                std::unordered_map<int, uint64_t> gpr_consts;
                std::unordered_set<int> gpr_nonzero;

                // Track scalar V-register writes: V reg -> (TypeKind::Int32/Int64, ssa_repr)
                std::unordered_map<int, std::pair<TypeKind, std::string>> vreg_scalar;
                std::unordered_map<std::string, std::pair<int, size_t>> vreg_read_from; // dst_reg -> (vreg, inst_idx)
                std::unordered_map<std::string, std::pair<TypeKind, std::string>> zext_i128_src;

                auto get_const = [&](const Value& v, uint64_t& out) -> bool {
                    if (v.is_const) { out = v.const_u64; return true; }
                    auto it = known_consts.find(v.repr);
                    if (it != known_consts.end()) { out = it->second; return true; }
                    return false;
                };

                auto is_nonzero = [&](const Value& v) -> bool {
                    uint64_t c = 0;
                    if (get_const(v, c)) return c != 0;
                    return proven_nonzero.count(v.repr) > 0;
                };

                for (size_t i = 0; i < bb->insts.size(); ++i) {
                    Instruction& inst = bb->insts[i];
                    if (inst.dead) continue;

                    if (inst.tag == OpTag::DirectCall || inst.tag == OpTag::IndirectCall ||
                        inst.tag == OpTag::SvcTrap || inst.text.find("(ptr %c") != std::string::npos) {
                        gpr_consts.clear();
                        gpr_nonzero.clear();
                        vreg_scalar.clear();
                        continue;
                    }

                    // 1. Materialize constant-folded CpuRegWrite (`MOVZ`+`MOVK` / `ADRP`+`ADD` / `CRC32`) directly
                    if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 0 && inst.arch_reg < 31) {
                        uint64_t cval = 0;
                        if (get_const(inst.op0, cval)) {
                            gpr_consts[inst.arch_reg] = cval;
                            if (cval != 0) gpr_nonzero.insert(inst.arch_reg);
                            else gpr_nonzero.erase(inst.arch_reg);

                            std::string imm_s = std::to_string(cval);
                            if (inst.op0.repr != imm_s || inst.text.find("store i64 " + imm_s + ",") == std::string::npos) {
                                inst.op0 = Value::imm_u64(cval);
                                std::string wp = fn->next_reg("cwp");
                                inst.text = "; [AArch64IdiomDeconstructPass] collapsed constant chain -> immediate store\n  " +
                                            wp + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(inst.arch_reg * 8) + "\n  " +
                                            "store i64 " + imm_s + ", ptr " + wp + ", align 8, !alias.scope !3, !noalias !4";
                                changed = true;
                            }
                        } else {
                            gpr_consts.erase(inst.arch_reg);
                            if (is_nonzero(inst.op0)) gpr_nonzero.insert(inst.arch_reg);
                            else gpr_nonzero.erase(inst.arch_reg);
                        }
                        continue;
                    }

                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 0 && inst.arch_reg < 31) {
                        auto cit = gpr_consts.find(inst.arch_reg);
                        if (cit != gpr_consts.end()) {
                            // 32-bit Wn reads truncate the 64-bit architectural register to 32 bits
                            uint64_t cval = (inst.dst_type == TypeKind::Int32)
                                          ? static_cast<uint64_t>(static_cast<uint32_t>(cit->second))
                                          : cit->second;
                            known_consts[inst.dst_reg] = cval;
                            if (cval != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.dst_type != TypeKind::Int32 && gpr_nonzero.count(inst.arch_reg)) {
                            // Only 64-bit Xn reads inherit 64-bit non-zero status (lower 32 bits may be 0)
                            proven_nonzero.insert(inst.dst_reg);
                        }
                        continue;
                    }

                    // 4. Track V-register scalar writes & reads for intra-block zext/trunc forwarding
                    if (inst.tag == OpTag::CpuRegWrite && inst.arch_reg >= 64) {
                        auto zit = zext_i128_src.find(inst.op0.repr);
                        if (zit != zext_i128_src.end()) {
                            vreg_scalar[inst.arch_reg] = zit->second;
                        } else {
                            vreg_scalar.erase(inst.arch_reg);
                        }
                        continue;
                    }

                    if (inst.tag == OpTag::CpuRegRead && inst.arch_reg >= 64) {
                        vreg_read_from[inst.dst_reg] = {inst.arch_reg, i};
                        continue;
                    }

                    if (inst.tag == OpTag::RawIR && !inst.dst_reg.empty()) {
                        uint64_t a = 0, b = 0;
                        if (inst.text.find(" = call i32 @rt_crc32(") != std::string::npos) {
                            char dst[64] = {}, acc_s[64] = {}, val_s[64] = {};
                            int bytes = 0, is_c = 0;
                            if (std::sscanf(inst.text.c_str(),
                                            "%63s = call i32 @rt_crc32(i32 %63[^,], i64 %63[^,], i32 %d, i32 %d)",
                                            dst, acc_s, val_s, &bytes, &is_c) == 5 &&
                                (bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8)) {
                                auto resolve_tok = [&](const char* tok, uint64_t& out_v) -> bool {
                                    if (tok[0] == '%') {
                                        auto it = known_consts.find(tok);
                                        if (it != known_consts.end()) { out_v = it->second; return true; }
                                        return false;
                                    }
                                    out_v = static_cast<uint64_t>(std::strtoll(tok, nullptr, 10));
                                    return true;
                                };
                                uint64_t acc_v = 0, val_v = 0;
                                if (resolve_tok(acc_s, acc_v) && resolve_tok(val_s, val_v)) {
                                    uint32_t crc = static_cast<uint32_t>(acc_v);
                                    uint32_t poly = is_c ? 0x82F63B78u : 0xEDB88320u;
                                    for (int bi = 0; bi < bytes; ++bi) {
                                        crc ^= static_cast<uint32_t>((val_v >> (8 * bi)) & 0xFFu);
                                        for (int bit = 0; bit < 8; ++bit) {
                                            crc = (crc >> 1) ^ ((crc & 1u) ? poly : 0u);
                                        }
                                    }
                                    known_consts[inst.dst_reg] = crc;
                                    if (crc != 0) proven_nonzero.insert(inst.dst_reg);
                                    inst.op0 = Value::imm_i32(0);
                                    inst.op1 = Value::imm_i32(static_cast<int32_t>(crc));
                                    inst.text = "; [AArch64IdiomDeconstructPass] constant-folded CRC32\n  " +
                                                inst.dst_reg + " = add i32 0, " + std::to_string(static_cast<int32_t>(crc));
                                    changed = true;
                                }
                            }
                        } else if (inst.text.find(" = add ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b)) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a + b) : (a + b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = sub ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b)) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a - b) : (a - b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = and ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b)) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a & b) : (a & b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = or ") != std::string::npos) {
                            if (get_const(inst.op0, a) && get_const(inst.op1, b)) {
                                uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a | b) : (a | b);
                                known_consts[inst.dst_reg] = r;
                                if (r != 0) proven_nonzero.insert(inst.dst_reg);
                            } else if (inst.dst_type != TypeKind::Int32 && (is_nonzero(inst.op0) || is_nonzero(inst.op1))) {
                                proven_nonzero.insert(inst.dst_reg);
                            }
                        } else if (inst.text.find(" = xor ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b)) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a ^ b) : (a ^ b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = shl ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b) && b < 64) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? static_cast<uint32_t>(a << b) : (a << b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = lshr ") != std::string::npos && get_const(inst.op0, a) && get_const(inst.op1, b) && b < 64) {
                            uint64_t r = (inst.dst_type == TypeKind::Int32) ? (static_cast<uint32_t>(a) >> b) : (a >> b);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        } else if (inst.text.find(" = zext ") != std::string::npos) {
                            if (get_const(inst.op0, a)) {
                                known_consts[inst.dst_reg] = a;
                                if (a != 0) proven_nonzero.insert(inst.dst_reg);
                            } else if (is_nonzero(inst.op0)) {
                                proven_nonzero.insert(inst.dst_reg);
                            }
                            if (inst.dst_type == TypeKind::Int128 &&
                                (inst.op0.type == TypeKind::Int32 || inst.op0.type == TypeKind::Int64)) {
                                zext_i128_src[inst.dst_reg] = {inst.op0.type, inst.op0.repr};
                            }
                        } else if (inst.text.find(" = trunc i128 ") != std::string::npos &&
                                   (inst.dst_type == TypeKind::Int32 || inst.dst_type == TypeKind::Int64)) {
                            auto rit = vreg_read_from.find(inst.op0.repr);
                            if (rit != vreg_read_from.end()) {
                                auto sit = vreg_scalar.find(rit->second.first);
                                if (sit != vreg_scalar.end() && sit->second.first == inst.dst_type) {
                                    inst.text = "; [AArch64IdiomDeconstructPass] forwarded scalar V-register value\n  " +
                                                inst.dst_reg + " = add " + type_str(inst.dst_type) + " 0, " + sit->second.second;
                                    changed = true;
                                }
                            }
                        } else if (inst.text.find(" = trunc ") != std::string::npos && get_const(inst.op0, a)) {
                            uint64_t r = a;
                            if (inst.dst_type == TypeKind::Int32) r = static_cast<uint32_t>(a);
                            else if (inst.dst_type == TypeKind::Int16) r = static_cast<uint16_t>(a);
                            else if (inst.dst_type == TypeKind::Int8) r = static_cast<uint8_t>(a);
                            known_consts[inst.dst_reg] = r;
                            if (r != 0) proven_nonzero.insert(inst.dst_reg);
                        }
                    }

                    // 2a. Safe UDIV guard elimination:
                    //   [i]   %cmp = icmp eq <ty> %b, 0
                    //   [i+1] %sb  = select i1 %cmp, <ty> 1, <ty> %b
                    //   [i+2] %q   = udiv <ty> %a, %sb
                    //   [i+3] %res = select i1 %cmp, <ty> 0, <ty> %q
                    if (i + 3 < bb->insts.size() &&
                        inst.tag == OpTag::RawIR && inst.text.find(" = icmp eq ") != std::string::npos &&
                        inst.op1.is_const && inst.op1.const_u64 == 0 &&
                        bb->insts[i + 2].text.find(" = udiv ") != std::string::npos &&
                        bb->insts[i + 3].text.find(" = select i1 " + inst.dst_reg) != std::string::npos &&
                        is_nonzero(inst.op0)) {
                        TypeKind dt = bb->insts[i + 2].dst_type;
                        std::string a_repr = bb->insts[i + 2].op0.repr;
                        std::string b_repr = inst.op0.repr;
                        uint64_t bc = 0;
                        if (get_const(inst.op0, bc)) b_repr = std::to_string(bc);

                        inst.dead = true;
                        bb->insts[i + 1].dead = true;
                        bb->insts[i + 2].dead = true;
                        bb->insts[i + 3].text = "; [AArch64IdiomDeconstructPass] elided UDIV zero guard\n  " +
                                                bb->insts[i + 3].dst_reg + " = udiv " + type_str(dt) + " " + a_repr + ", " + b_repr;
                        i += 3;
                        changed = true;
                        continue;
                    }

                    // 2b. Safe SDIV guard elimination:
                    //   [i]   %is_zero = icmp eq <ty> %b, 0
                    //   [i+1] %is_neg1 = icmp eq <ty> %b, -1
                    //   ...
                    //   [i+6] %q       = sdiv <ty> %a, %safe_b
                    //   [i+7] %sel1    = select i1 %ovf, <ty> %a, <ty> %q
                    //   [i+8] %res     = select i1 %is_zero, <ty> 0, <ty> %sel1
                    if (i + 8 < bb->insts.size() &&
                        inst.tag == OpTag::RawIR && inst.text.find(" = icmp eq ") != std::string::npos &&
                        inst.op1.is_const && inst.op1.const_u64 == 0 &&
                        bb->insts[i + 6].text.find(" = sdiv ") != std::string::npos &&
                        bb->insts[i + 8].text.find(" = select i1 " + inst.dst_reg) != std::string::npos) {
                        uint64_t bc = 0;
                        TypeKind dt = bb->insts[i + 6].dst_type;
                        uint64_t neg1 = (dt == TypeKind::Int32) ? 0xFFFFFFFFULL : ~0ULL;
                        if (get_const(inst.op0, bc) && bc != 0 && bc != neg1) {
                            std::string a_repr = bb->insts[i + 6].op0.repr;
                            std::string b_repr = std::to_string(static_cast<int64_t>(bc));
                            for (size_t k = i; k < i + 8; ++k) bb->insts[k].dead = true;
                            bb->insts[i + 8].text = "; [AArch64IdiomDeconstructPass] elided SDIV zero/overflow guard\n  " +
                                                    bb->insts[i + 8].dst_reg + " = sdiv " + type_str(dt) + " " + a_repr + ", " + b_repr;
                            i += 8;
                            changed = true;
                            continue;
                        }
                    }

                    // 3. Adjacent paired store coalescing (STP Wzr, Wzr -> i64; STP Xzr, Xzr -> i128)
                    if (inst.tag == OpTag::GuestMemStore && i + 1 < bb->insts.size()) {
                        bool is_stp_layout = (i >= 1 && bb->insts[i + 1].tag == OpTag::GuestMemStore);
                        bool is_str_layout = (!is_stp_layout && i + 2 < bb->insts.size() && bb->insts[i + 2].tag == OpTag::GuestMemStore);
                        if (is_stp_layout || is_str_layout) {
                        const Instruction& ea2_inst = is_stp_layout ? bb->insts[i - 1] : bb->insts[i + 1];
                        Instruction& st2 = is_stp_layout ? bb->insts[i + 1] : bb->insts[i + 2];
                        uint64_t v0 = 0, v1 = 0;
                        if (!ea2_inst.dead && !st2.dead && st2.tag == OpTag::GuestMemStore &&
                            ea2_inst.text.find(" = add i64 ") != std::string::npos &&
                            inst.op1.type == st2.op1.type &&
                            ea2_inst.op0.repr == inst.op0.repr && st2.op0.repr == ea2_inst.dst_reg &&
                            ea2_inst.op1.is_const &&
                            get_const(inst.op1, v0) && get_const(st2.op1, v1)) {
                            int al = std::max(1, inst.align_bytes);
                            const char* scope_md = inst.is_stack_access ? "!alias.scope !6, !noalias !3"
                                                                        : "!alias.scope !4, !noalias !3";
                            if (inst.op1.type == TypeKind::Int32 && ea2_inst.op1.const_u64 == 4) {
                                uint64_t combined = (static_cast<uint64_t>( static_cast<uint32_t>(v1) ) << 32) |
                                                    static_cast<uint64_t>( static_cast<uint32_t>(v0) );
                                std::string p = fn->next_reg("stpp");
                                inst.op1 = Value::imm_u64(combined);
                                inst.text = "; [AArch64IdiomDeconstructPass] coalesced paired 32-bit constant stores -> i64\n  " +
                                            p + " = inttoptr i64 " + inst.op0.repr + " to ptr\n  " +
                                            "store i64 " + std::to_string(combined) + ", ptr " + p +
                                            ", align " + std::to_string(al) + ", " + scope_md;
                                st2.dead = true;
                                changed = true;
                            } else if (inst.op1.type == TypeKind::Int64 && ea2_inst.op1.const_u64 == 8 && v0 == 0 && v1 == 0) {
                                std::string p = fn->next_reg("stpp");
                                inst.op1 = Value::imm_i128(0, 0);
                                inst.text = "; [AArch64IdiomDeconstructPass] coalesced paired 64-bit zero stores -> i128\n  " +
                                            p + " = inttoptr i64 " + inst.op0.repr + " to ptr\n  " +
                                            "store i128 0, ptr " + p +
                                            ", align " + std::to_string(al) + ", " + scope_md;
                                st2.dead = true;
                                changed = true;
                            }
                        }
                        }
                    }

                    // 3b. Materialize known constant values directly into single GuestMemStore instructions
                    if (inst.tag == OpTag::GuestMemStore && !inst.op1.is_const &&
                        (inst.op1.type == TypeKind::Int8 || inst.op1.type == TypeKind::Int16 ||
                         inst.op1.type == TypeKind::Int32 || inst.op1.type == TypeKind::Int64)) {
                        uint64_t cval = 0;
                        if (get_const(inst.op1, cval)) {
                            std::string old_op = std::string(type_str(inst.op1.type)) + " " + inst.op1.repr + ",";
                            auto pos = inst.text.find(old_op);
                            if (pos != std::string::npos) {
                                std::string imm_s = (inst.op1.type == TypeKind::Int32)
                                                  ? std::to_string(static_cast<int32_t>(cval))
                                                  : std::to_string(cval);
                                inst.text.replace(pos, old_op.size(), std::string(type_str(inst.op1.type)) + " " + imm_s + ",");
                                inst.op1 = Value::imm(inst.op1.type, cval);
                                changed = true;
                            }
                        }
                    }
                }

                // 5. Clean up dead constant-folded intermediate SSA instructions (e.g. collapsed MOVZ/MOVK/CRC32 chains)
                if (!known_consts.empty()) {
                    std::unordered_set<std::string> live_ssa;
                    auto add_used_tokens = [&](const std::string& txt, const std::string& skip_def) {
                        size_t pos = 0;
                        while ((pos = txt.find('%', pos)) != std::string::npos) {
                            size_t end = pos + 1;
                            while (end < txt.size()) {
                                char ch = txt[end];
                                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '.') {
                                    ++end;
                                } else {
                                    break;
                                }
                            }
                            if (end > pos + 1) {
                                std::string tok = txt.substr(pos, end - pos);
                                if (tok != skip_def) live_ssa.insert(std::move(tok));
                            }
                            pos = end;
                        }
                    };
                    for (auto it = bb->insts.rbegin(); it != bb->insts.rend(); ++it) {
                        if (it->dead) continue;
                        if (it->tag == OpTag::RawIR && !it->dst_reg.empty() &&
                            known_consts.count(it->dst_reg) && !live_ssa.count(it->dst_reg) &&
                            it->text.find("call ") == std::string::npos) {
                            it->dead = true;
                            changed = true;
                            continue;
                        }
                        add_used_tokens(it->text, it->dst_reg);
                        if (!it->prefix_ir.empty()) add_used_tokens(it->prefix_ir, "");
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

bool AArch64IdiomDeconstructPass::run(Module& M) {
    AArch64IdiomDeconstructPassImpl impl(enabled_);
    return impl.run(M);
}

} // namespace ultrs::passes
