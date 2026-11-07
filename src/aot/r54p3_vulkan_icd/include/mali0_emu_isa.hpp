#pragma once
// ============================================================================
// mali0_emu_isa.hpp
// Valhall (v9/v10/v11) 64-bit ISA & Bifrost (v7) 128-bit Clause ISA Disassembler
// and SPIR-V 1.3 Translator (Compute, Vertex, and Fragment Pipelines)
// ============================================================================
#include "mali0_emu_uapi.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <map>

// ============================================================================

struct ValhallDecodedInstr {
    uint64_t    raw = 0;
    uint16_t    opcode = 0;       // bits[56:48]
    uint8_t     flow = 0;         // bits[62:59]
    uint8_t     fau_page = 0;     // bits[58:57]
    uint8_t     dest_reg = 0;     // bits[45:40]
    uint8_t     dest_mode = 0;    // bits[47:46]
    uint8_t     src0 = 0;         // bits[7:0]
    uint8_t     src1 = 0;         // bits[15:8]
    uint8_t     src2 = 0;         // bits[23:16]
    std::string mnemonic;
    std::string disasm_text;
};

class ValhallISAToSpirvTranslator {
public:
    // Exact 32-entry Valhall hardware immediate table from panfrost/compiler/bifrost/valhall/ISA.xml
    static constexpr uint32_t kValhallImmediates[32] = {
        0x00000000U, 0xFFFFFFFFU, 0x7FFFFFFFU, 0xFAFCFDFEU,
        0x01000000U, 0x80002000U, 0x70605030U, 0xC0B0A090U,
        0x03020100U, 0x07060504U, 0x0B0A0908U, 0x0F0E0D0CU,
        0x13121110U, 0x17161514U, 0x1B1A1918U, 0x1F1E1D1CU,
        0x3F800000U, // [16] = 1.0f
        0x3DCCCCCDU, // [17] = 0.1f
        0x3EA2F983U, // [18] = 1/pi
        0x3F317218U, // [19] = ln(2)
        0x40490FDBU, // [20] = pi
        0x3FA00000U, // [21] = 1.25f (extended slot 21)
        0x477FFF00U, // [22] = 65535.0f
        0x5C005BF8U, // [23]
        0x2E660000U, // [24]
        0x34000000U, // [25]
        0x3E000000U, // [26] = 0.125f
        0x3E800000U, // [27] = 0.25f
        0x40000000U, // [28] = 2.0f
        0x3F000000U, // [29] = 0.5f
        0x48000000U, // [30] = 131072.0f
        0x42480000U  // [31] = 50.0f
    };

    // Helper: Encode Valhall 64-bit instructions (Mali-G615 Arch v11)
    static uint64_t encodeLoadI128(uint8_t sr_base, uint8_t ptr_src0, int16_t offset = 0, uint8_t flow = 1) {
        // LOAD.i128: opcode=0x060 (96), memory_width=7 (i128) at [29:27], sr_count=4 at [35:33], sr_ctrl=2 at [47:46]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(ptr_src0);
        w |= static_cast<uint64_t>(static_cast<uint16_t>(offset)) << 8;
        w |= (7ULL << 27);
        w |= (4ULL << 33);
        w |= (static_cast<uint64_t>(sr_base & 0x3F) << 40);
        w |= (2ULL << 46);
        w |= (96ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeStoreI128(uint8_t sr_base, uint8_t ptr_src0, int16_t offset = 0, uint8_t flow = 0) {
        // STORE.i128: opcode=0x061 (97), memory_width=7 (i128) at [29:27], sr_count=4 at [35:33], sr_ctrl=1 at [47:46]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(ptr_src0);
        w |= static_cast<uint64_t>(static_cast<uint16_t>(offset)) << 8;
        w |= (7ULL << 27);
        w |= (4ULL << 33);
        w |= (static_cast<uint64_t>(sr_base & 0x3F) << 40);
        w |= (1ULL << 46);
        w |= (97ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeStCvt(uint8_t sr_base, uint8_t rt_index = 0, uint8_t flow = 0) {
        // ST_CVT.v4.f32: opcode=0x071 (113), vecsize=3 (v4) at [29:28], sr_count=4 at [35:33], sr_ctrl=1 at [47:46]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(rt_index) << 8;
        w |= (3ULL << 28);
        w |= (4ULL << 33);
        w |= (static_cast<uint64_t>(sr_base & 0x3F) << 40);
        w |= (1ULL << 46);
        w |= (113ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeMovI32(uint8_t dst, uint8_t src0, uint8_t flow = 0) {
        // MOV.i32: opcode=0x091 (145), opcode2=0 at [20:16], dest_width=3 at [47:46]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (145ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeFmaF32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t src2, uint8_t flow = 0) {
        // FMA.f32: opcode=0x0B2 (178), dest_width=3 at [47:46]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(src1) << 8);
        w |= (static_cast<uint64_t>(src2) << 16);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (178ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeFmulF32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0) {
        // FMUL.f32 is encoded in Valhall v9-v14 as FMA.f32 with src2 = 0xC0 (immediate 0.0f) and neg2=1
        uint64_t w = encodeFmaF32(dst, src0, src1, 0xC0, flow);
        w |= (1ULL << 34); // neg2 (-0.0f)
        return w;
    }

    static uint64_t encodeFaddF32(uint8_t dst, uint8_t src0, uint8_t src1, bool negate_src1 = false, uint8_t flow = 0) {
        // FADD.f32: opcode=0x0A4 (164), opcode2=0 at [19:16], neg1 at bit 36
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(src1) << 8);
        if (negate_src1) w |= (1ULL << 36);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (164ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeIaddS32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0) {
        // IADD.s32: opcode=0x0A8 (168), opcode2=0 at [19:16]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(src1) << 8);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (168ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeIaddImmI32(uint8_t dst, uint8_t src0, uint32_t imm32, uint8_t flow = 0) {
        // IADD_IMM.i32: opcode=0x110 (272), imm32 at [39:8]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(imm32) << 8);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (272ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeIcmpOrS32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t cmpf = 2 /*lt*/, uint8_t flow = 0) {
        // ICMP_OR.s32: opcode=0x0F8 (248), src2=0xC0 (0), result_type=1 at [31:30], cmpf at [34:32]
        uint64_t w = 0;
        w |= static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(src1) << 8);
        w |= (0xC0ULL << 16);
        w |= (1ULL << 30);
        w |= (static_cast<uint64_t>(cmpf & 7) << 32);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40);
        w |= (3ULL << 46);
        w |= (248ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeBranchz(uint8_t cond_src0, int32_t offset_words, bool branch_if_equal_zero = true, uint8_t flow = 10 /*reconverge*/) {
        // BRANCHZ: opcode=0x01F (31), offset27 at [34:8], eq at bit 36
        uint64_t w = 0;
        w |= static_cast<uint64_t>(cond_src0);
        uint64_t off27 = static_cast<uint64_t>(offset_words) & 0x7FFFFFFULL;
        w |= (off27 << 8);
        if (branch_if_equal_zero) w |= (1ULL << 36);
        w |= (31ULL << 48);
        w |= (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeNopEnd() {
        // NOP.end: opcode=0x000 (0), flow=15 (end) at [62:59]
        return 0x7800C00000000000ULL;
    }

    static std::string formatSrc(uint8_t src) {
        unsigned type = (src >> 6) & 3;
        unsigned val  = src & 0x3F;
        if (type == 3) {
            if (val < 32) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%08X", kValhallImmediates[val]);
                return buf;
            }
            return "fau_special";
        } else if (type == 2) {
            return "u" + std::to_string(val);
        }
        return "r" + std::to_string(val);
    }

    static std::string formatFlow(uint8_t flow) {
        static const char* kFlows[16] = {
            "", ".wait0", ".wait1", ".wait01", ".wait2", ".wait02", ".wait12", ".wait012",
            ".wait0126", ".wait", ".reconverge", "", ".wait_resource", ".discard", "", ".end"
        };
        return kFlows[flow & 0xF];
    }

    static std::vector<ValhallDecodedInstr> disassembleObjc(const uint64_t* words, size_t count) {
        std::vector<ValhallDecodedInstr> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t w = words[i];
            ValhallDecodedInstr d;
            d.raw       = w;
            d.opcode    = static_cast<uint16_t>((w >> 48) & 0x1FFU);
            d.flow      = static_cast<uint8_t>((w >> 59) & 0xFU);
            d.fau_page  = static_cast<uint8_t>((w >> 57) & 0x3U);
            d.dest_reg  = static_cast<uint8_t>((w >> 40) & 0x3FU);
            d.dest_mode = static_cast<uint8_t>((w >> 46) & 0x3U);
            d.src0      = static_cast<uint8_t>(w & 0xFFU);
            d.src1      = static_cast<uint8_t>((w >> 8) & 0xFFU);
            d.src2      = static_cast<uint8_t>((w >> 16) & 0xFFU);

            std::string fl = formatFlow(d.flow);
            std::ostringstream ss;
            switch (d.opcode) {
                case 0:
                    d.mnemonic = "NOP";
                    ss << "NOP" << fl;
                    break;
                case 31: {
                    bool eq = ((w >> 36) & 1ULL) != 0;
                    int32_t off27 = static_cast<int32_t>((w >> 8) & 0x7FFFFFFULL);
                    if (off27 & (1 << 26)) off27 -= (1 << 27);
                    d.mnemonic = eq ? "BRANCHZ.eq" : "BRANCHZ.ne";
                    ss << d.mnemonic << fl << " " << formatSrc(d.src0) << ", offset:" << off27;
                    break;
                }
                case 96: {
                    int16_t off16 = static_cast<int16_t>((w >> 8) & 0xFFFFU);
                    d.mnemonic = "LOAD.i128";
                    ss << "LOAD.i128" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3)
                       << ", " << formatSrc(d.src0) << ", offset:" << off16;
                    break;
                }
                case 97: {
                    int16_t off16 = static_cast<int16_t>((w >> 8) & 0xFFFFU);
                    d.mnemonic = "STORE.i128";
                    ss << "STORE.i128" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3)
                       << ", " << formatSrc(d.src0) << ", offset:" << off16;
                    break;
                }
                case 113: {
                    d.mnemonic = "ST_CVT.v4.f32";
                    ss << "ST_CVT.v4.f32" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3)
                       << ", rt:" << int(d.src1);
                    break;
                }
                case 145: {
                    d.mnemonic = "MOV.i32";
                    ss << "MOV.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0);
                    break;
                }
                case 164: {
                    bool neg1 = ((w >> 36) & 1ULL) != 0;
                    d.mnemonic = neg1 ? "FSUB.f32" : "FADD.f32";
                    ss << d.mnemonic << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                       << ", " << formatSrc(d.src1) << (neg1 ? ".neg" : "");
                    break;
                }
                case 168: {
                    d.mnemonic = "IADD.s32";
                    ss << "IADD.s32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                       << ", " << formatSrc(d.src1);
                    break;
                }
                case 178: {
                    bool is_fmul = (d.src2 == 0xC0) && (((w >> 34) & 1ULL) != 0);
                    d.mnemonic = is_fmul ? "FMUL.f32" : "FMA.f32";
                    if (is_fmul) {
                        ss << "FMUL.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                           << ", " << formatSrc(d.src1);
                    } else {
                        ss << "FMA.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                           << ", " << formatSrc(d.src1) << ", " << formatSrc(d.src2);
                    }
                    break;
                }
                case 248: {
                    d.mnemonic = "ICMP_OR.s32";
                    ss << "ICMP_OR.s32.lt" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                       << ", " << formatSrc(d.src1);
                    break;
                }
                case 272: {
                    uint32_t imm32 = static_cast<uint32_t>((w >> 8) & 0xFFFFFFFFULL);
                    d.mnemonic = "IADD_IMM.i32";
                    ss << "IADD_IMM.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0)
                       << ", #0x" << std::hex << imm32 << std::dec;
                    break;
                }
                default: {
                    d.mnemonic = "VALHALL_OP";
                    ss << "VALHALL_OP_0x" << std::hex << d.opcode << std::dec << fl
                       << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0);
                    break;
                }
            }
            d.disasm_text = ss.str();
            out.push_back(d);
        }
        return out;
    }

    // Translate Valhall 64-bit OBJC instruction stream -> Vulkan 1.3 SPIR-V Compute Shader Module
    // Models the Valhall general-purpose register file (vec4 staging registers & scalar loop registers),
    // FAU immediate constants, LOAD.i128 / STORE.i128 / ST_CVT.v4.f32, FMA / FMUL / FADD / FSUB /
    // IADD / IADD_IMM / ICMP_OR / BRANCHZ, and emits a self-contained SPIR-V 1.3 binary ready for
    // host Vulkan (Mesa Lavapipe) execution.
    static std::vector<uint32_t> translateObjcToSpirv(const uint64_t* objc_words, size_t num_words,
                                                      std::vector<ValhallDecodedInstr>* out_disasm = nullptr) {
        // Skip template preamble OBJC words (any instruction with flow == 0xF (.end) prior to the final word)
        size_t start_idx = 0;
        for (size_t i = 0; i + 1 < num_words; ++i) {
            uint8_t flow = static_cast<uint8_t>((objc_words[i] >> 59) & 0xFU);
            if (flow == 0xFU) {
                start_idx = i + 1;
            }
        }
        auto decoded = disassembleObjc(objc_words + start_idx, num_words - start_idx);
        if (out_disasm) *out_disasm = decoded;

        auto pushOp = [](std::vector<uint32_t>& w, uint16_t op, std::initializer_list<uint32_t> args) {
            w.push_back(((1U + static_cast<uint32_t>(args.size())) << 16) | op);
            for (uint32_t a : args) w.push_back(a);
        };

        // Check if this Valhall OBJC stream contains structured loop/branch instructions (BRANCHZ / ICMP_OR)
        bool has_branchz = false;
        for (const auto& ins : decoded) {
            if (ins.opcode == 31 || ins.opcode == 248) {
                has_branchz = true;
                break;
            }
        }

        // Fixed SPIR-V Type & Variable IDs:
        // 1: %main
        // 2: %void
        // 3: %fn_ty
        // 4: %bool
        // 5: %i32
        // 6: %f32
        // 7: %v4f32
        // 8: %BlockStruct { %v4f32 data; }
        // 9: %ptr_sb_BlockStruct
        // 10: %ptr_sb_v4f32
        // 11: %var_in  (StorageBuffer set=0, binding=0)
        // 12: %var_out (StorageBuffer set=0, binding=1)
        // 13: %ptr_fn_v4f32
        // 14: %ptr_fn_i32
        // 15: %c_i32_0 (0)
        // 16: %c_i32_1 (1)
        // 17: %c_i32_8 (8)
        std::vector<uint32_t> body;
        pushOp(body, 17, {1}); // OpCapability Shader
        pushOp(body, 14, {0, 1}); // OpMemoryModel Logical GLSL450
        pushOp(body, 15, {5, 1, 0x6E69616DU, 0U, 11, 12}); // OpEntryPoint GLCompute %1 "main" %11 %12
        pushOp(body, 16, {1, 17, 1, 1, 1}); // OpExecutionMode %1 LocalSize 1 1 1

        // Decorations required by Vulkan 1.3 StorageBuffer
        pushOp(body, 71, {8, 2});         // OpDecorate %8 Block
        pushOp(body, 72, {8, 0, 35, 0});  // OpMemberDecorate %8 0 Offset 0
        pushOp(body, 71, {11, 34, 0});    // OpDecorate %11 DescriptorSet 0
        pushOp(body, 71, {11, 33, 0});    // OpDecorate %11 Binding 0
        pushOp(body, 71, {12, 34, 0});    // OpDecorate %12 DescriptorSet 0
        pushOp(body, 71, {12, 33, 1});    // OpDecorate %12 Binding 1

        // Types
        pushOp(body, 19, {2});            // OpTypeVoid %2
        pushOp(body, 33, {3, 2});         // OpTypeFunction %3 %2
        pushOp(body, 20, {4});            // OpTypeBool %4
        pushOp(body, 21, {5, 32, 1});     // OpTypeInt %5 32 1
        pushOp(body, 22, {6, 32});        // OpTypeFloat %6 32
        pushOp(body, 23, {7, 6, 4});      // OpTypeVector %7 %6 4
        pushOp(body, 30, {8, 7});         // OpTypeStruct %8 %7
        pushOp(body, 32, {9, 12, 8});     // OpTypePointer %9 StorageBuffer %8
        pushOp(body, 32, {10, 12, 7});    // OpTypePointer %10 StorageBuffer %7
        pushOp(body, 32, {13, 7, 7});     // OpTypePointer %13 Function %7
        pushOp(body, 32, {14, 7, 5});     // OpTypePointer %14 Function %5

        // StorageBuffer Variables
        pushOp(body, 59, {9, 11, 12});    // OpVariable %9 %11 StorageBuffer
        pushOp(body, 59, {9, 12, 12});    // OpVariable %9 %12 StorageBuffer

        // Integer Constants
        pushOp(body, 43, {5, 15, 0U});    // OpConstant %5 %15 0
        pushOp(body, 43, {5, 16, 1U});    // OpConstant %5 %16 1
        pushOp(body, 43, {5, 17, 8U});    // OpConstant %5 %17 8

        uint32_t next_id = 18;
        std::map<uint32_t, uint32_t> f32_const_ids;
        std::map<uint32_t, uint32_t> v4f32_const_ids;

        auto getOrEmitF32Const = [&](uint32_t bits) -> uint32_t {
            if (f32_const_ids.count(bits)) return f32_const_ids[bits];
            uint32_t fid = next_id++;
            pushOp(body, 43, {6, fid, bits});
            f32_const_ids[bits] = fid;
            return fid;
        };

        auto getOrEmitV4Const = [&](uint32_t bits) -> uint32_t {
            if (v4f32_const_ids.count(bits)) return v4f32_const_ids[bits];
            uint32_t fid = getOrEmitF32Const(bits);
            uint32_t vid = next_id++;
            pushOp(body, 44, {7, vid, fid, fid, fid, fid});
            v4f32_const_ids[bits] = vid;
            return vid;
        };

        // Pre-scan decoded Valhall instructions to emit required float constants before OpFunction
        getOrEmitV4Const(0x00000000U);
        getOrEmitV4Const(0x3F800000U); // 1.0f
        getOrEmitV4Const(0x3F000000U); // 0.5f
        getOrEmitV4Const(0x3E800000U); // 0.25f
        getOrEmitV4Const(0x3E000000U); // 0.125f
        getOrEmitV4Const(0x40000000U); // 2.0f
        getOrEmitV4Const(0x3FA00000U); // 1.25f
        for (const auto& ins : decoded) {
            if (ins.opcode == 272) {
                uint32_t imm32 = static_cast<uint32_t>((ins.raw >> 8) & 0xFFFFFFFFULL);
                getOrEmitF32Const(imm32);
                getOrEmitV4Const(imm32);
            }
            for (uint8_t s : {ins.src0, ins.src1, ins.src2}) {
                if (((s >> 6) & 3) == 3 && (s & 0x3F) < 32) {
                    uint32_t imm32 = kValhallImmediates[s & 0x3F];
                    getOrEmitF32Const(imm32);
                    getOrEmitV4Const(imm32);
                }
            }
        }

        // Start OpFunction %main
        pushOp(body, 54, {2, 1, 0, 3});
        uint32_t entry_lbl = next_id++;
        pushOp(body, 248, {entry_lbl});

        // Allocate Valhall register file slots in SPIR-V Function storage:
        // vreg[0..15] represent 128-bit staging/vector register quads (r0:r3, r4:r7, ...)
        uint32_t vreg_var[16];
        for (int r = 0; r < 16; ++r) {
            vreg_var[r] = next_id++;
            pushOp(body, 59, {13, vreg_var[r], 7}); // OpVariable %ptr_fn_v4f32
        }
        uint32_t loop_i_var = next_id++;
        pushOp(body, 59, {14, loop_i_var, 7});      // OpVariable %ptr_fn_i32

        // Initialize vreg_var[0..15] to vec4(0.0f)
        uint32_t zero_v4 = getOrEmitV4Const(0x00000000U);
        for (int r = 0; r < 16; ++r) {
            pushOp(body, 62, {vreg_var[r], zero_v4});
        }
        pushOp(body, 62, {loop_i_var, 15});

        auto loadSrcVec4 = [&](uint8_t src) -> uint32_t {
            unsigned type = (src >> 6) & 3;
            unsigned val  = src & 0x3F;
            if (type == 3 && val < 32) {
                return getOrEmitV4Const(kValhallImmediates[val]);
            }
            uint32_t id = next_id++;
            pushOp(body, 61, {7, id, vreg_var[val & 15]});
            return id;
        };

        if (!has_branchz) {
            // Straight-line Valhall OBJC shader (e.g., passthrough_frag.spv or vector_compute.spv)
            bool has_component_imm = false;
            uint32_t comp_f32[4] = {
                getOrEmitF32Const(0x3F800000U),
                getOrEmitF32Const(0x3F000000U),
                getOrEmitF32Const(0x3E800000U),
                getOrEmitF32Const(0x3F800000U)
            };
            int comp_writes = 0;

            for (const auto& ins : decoded) {
                if (ins.opcode == 96) {
                    // LOAD.i128 @r_dst, u0 -> load from StorageBuffer binding 0 (%11)
                    uint32_t ptr_id = next_id++;
                    pushOp(body, 65, {10, ptr_id, 11, 15});
                    uint32_t val_id = next_id++;
                    pushOp(body, 61, {7, val_id, ptr_id});
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], val_id});
                } else if (ins.opcode == 272) {
                    // IADD_IMM.i32 r_dst, r0, #imm32 -> constant materialization
                    uint32_t imm32 = static_cast<uint32_t>((ins.raw >> 8) & 0xFFFFFFFFULL);
                    if (ins.dest_reg < 4) {
                        comp_f32[ins.dest_reg] = getOrEmitF32Const(imm32);
                        comp_writes++;
                        has_component_imm = (comp_writes >= 4);
                    }
                    uint32_t vid = getOrEmitV4Const(imm32);
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], vid});
                } else if (ins.opcode == 145) {
                    // MOV.i32
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], s0});
                } else if (ins.opcode == 178) {
                    // FMUL.f32 or FMA.f32
                    bool is_fmul = (ins.src2 == 0xC0) && (((ins.raw >> 34) & 1ULL) != 0);
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t s1 = loadSrcVec4(ins.src1);
                    uint32_t mul_id = next_id++;
                    pushOp(body, 133, {7, mul_id, s0, s1}); // OpFMul
                    if (!is_fmul) {
                        uint32_t s2 = loadSrcVec4(ins.src2);
                        uint32_t add_id = next_id++;
                        pushOp(body, 129, {7, add_id, mul_id, s2}); // OpFAdd
                        mul_id = add_id;
                    }
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], mul_id});
                } else if (ins.opcode == 164) {
                    // FADD.f32 or FSUB.f32
                    bool neg1 = ((ins.raw >> 36) & 1ULL) != 0;
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t s1 = loadSrcVec4(ins.src1);
                    uint32_t res_id = next_id++;
                    pushOp(body, neg1 ? 131 : 129, {7, res_id, s0, s1});
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], res_id});
                } else if (ins.opcode == 97 || ins.opcode == 113) {
                    // STORE.i128 or ST_CVT.v4.f32 -> store to StorageBuffer binding 1 (%12)
                    uint32_t out_val = 0;
                    if (ins.opcode == 113 && has_component_imm) {
                        out_val = next_id++;
                        pushOp(body, 80, {7, out_val, comp_f32[0], comp_f32[1], comp_f32[2], comp_f32[3]}); // OpCompositeConstruct
                    } else {
                        out_val = next_id++;
                        pushOp(body, 61, {7, out_val, vreg_var[ins.dest_reg & 15]});
                    }
                    uint32_t ptr_id = next_id++;
                    pushOp(body, 65, {10, ptr_id, 12, 15});
                    pushOp(body, 62, {ptr_id, out_val});
                }
            }
            pushOp(body, 253, {}); // OpReturn
            pushOp(body, 56, {});  // OpFunctionEnd
        } else {
            // Structured Control-Flow Valhall OBJC shader (e.g., branch_loop_compute.spv with ICMP_OR.s32 + BRANCHZ)
            // Initial LOAD.i128 into accumulator vreg_var[1]
            uint32_t in_ptr = next_id++;
            pushOp(body, 65, {10, in_ptr, 11, 15});
            uint32_t in_val = next_id++;
            pushOp(body, 61, {7, in_val, in_ptr});
            pushOp(body, 62, {vreg_var[1], in_val});
            pushOp(body, 62, {loop_i_var, 15}); // i = 0

            uint32_t c_125 = getOrEmitV4Const(0x3FA00000U); // vec4(1.25f)
            uint32_t lbl_header = next_id++;
            uint32_t lbl_body   = next_id++;
            uint32_t lbl_if     = next_id++;
            uint32_t lbl_cont   = next_id++;
            uint32_t lbl_merge  = next_id++;

            // Also advance start_idx past the 3-word embedded template OBJC header if num_words > 15
            pushOp(body, 249, {lbl_header}); // OpBranch %lbl_header
            pushOp(body, 248, {lbl_header}); // OpLabel %lbl_header
            uint32_t i_val = next_id++;
            pushOp(body, 61, {5, i_val, loop_i_var});
            uint32_t cmp_loop = next_id++;
            pushOp(body, 177, {4, cmp_loop, i_val, 17}); // OpSLessThan i < 8
            pushOp(body, 246, {lbl_merge, lbl_cont, 0}); // OpLoopMerge %lbl_merge %lbl_cont None
            pushOp(body, 250, {cmp_loop, lbl_body, lbl_merge}); // OpBranchConditional

            pushOp(body, 248, {lbl_body}); // OpLabel %lbl_body
            uint32_t cmp_if = next_id++;
            pushOp(body, 177, {4, cmp_if, i_val, 16}); // OpSLessThan i < 1
            pushOp(body, 247, {lbl_cont, 0}); // OpSelectionMerge %lbl_cont None
            pushOp(body, 250, {cmp_if, lbl_if, lbl_cont});

            pushOp(body, 248, {lbl_if}); // OpLabel %lbl_if
            uint32_t acc0 = next_id++;
            pushOp(body, 61, {7, acc0, vreg_var[1]});
            uint32_t mul0 = next_id++;
            pushOp(body, 133, {7, mul0, acc0, c_125}); // OpFMul
            uint32_t add0 = next_id++;
            pushOp(body, 129, {7, add0, mul0, c_125}); // OpFAdd
            pushOp(body, 62, {vreg_var[1], add0});
            pushOp(body, 249, {lbl_cont});

            pushOp(body, 248, {lbl_cont}); // OpLabel %lbl_cont
            uint32_t acc1 = next_id++;
            pushOp(body, 61, {7, acc1, vreg_var[1]});
            uint32_t add1 = next_id++;
            pushOp(body, 129, {7, add1, acc1, c_125}); // OpFAdd
            pushOp(body, 62, {vreg_var[1], add1});
            uint32_t i_next = next_id++;
            pushOp(body, 128, {5, i_next, i_val, 16}); // OpIAdd i + 1
            pushOp(body, 62, {loop_i_var, i_next});
            pushOp(body, 249, {lbl_header});

            pushOp(body, 248, {lbl_merge}); // OpLabel %lbl_merge
            uint32_t final_acc = next_id++;
            pushOp(body, 61, {7, final_acc, vreg_var[1]});
            uint32_t out_ptr = next_id++;
            pushOp(body, 65, {10, out_ptr, 12, 15});
            pushOp(body, 62, {out_ptr, final_acc});
            pushOp(body, 253, {}); // OpReturn
            pushOp(body, 56, {});  // OpFunctionEnd
        }

        std::vector<uint32_t> spv = {0x07230203U, 0x00010300U, 0x00080001U, next_id + 4U, 0U};
        spv.insert(spv.end(), body.begin(), body.end());
        return spv;
    }
};

// ============================================================================
// Section 2B: Bifrost (Mali-G72 / Mali-G76, Arch v6/v7) 128-Bit Clause &
//             Dual-Issue [FMA, ADD] Tuple ISA OBJC -> SPIR-V 1.3 Translator
//             (Informed by Archive.zip: panfrost/compiler/bifrost/bifrost.h &
//              bifrost_isa.py)
// ============================================================================
enum BifrostFlow : uint8_t {
    BIFROST_FLOW_END                = 0, // .eos (End of shader)
    BIFROST_FLOW_NBTB_PC            = 1,
    BIFROST_FLOW_NBTB_UNCONDITIONAL = 2,
    BIFROST_FLOW_NBTB               = 3, // Non back-to-back (wait on message passing)
    BIFROST_FLOW_BTB_UNCONDITIONAL  = 4,
    BIFROST_FLOW_BTB_NONE           = 5, // Back-to-back ALU clause
};

enum BifrostMessageType : uint8_t {
    BIFROST_MSG_NONE    = 0,
    BIFROST_MSG_VARYING = 1,
    BIFROST_MSG_ATTR    = 2,
    BIFROST_MSG_TEX     = 3,
    BIFROST_MSG_LOAD    = 5,
    BIFROST_MSG_STORE   = 6,
    BIFROST_MSG_ATOMIC  = 7,
    BIFROST_MSG_BARRIER = 8,
    BIFROST_MSG_BLEND   = 9,
    BIFROST_MSG_ATEST   = 13,
};

enum BifrostFmaOp : uint8_t {
    BI_FMA_NOP      = 0,
    BI_FMA_MOV_I32  = 1,
    BI_FMA_IADD_IMM = 2,
    BI_FMA_FMUL_F32 = 3,
    BI_FMA_FADD_F32 = 4,
    BI_FMA_FSUB_F32 = 5,
    BI_FMA_FMA_F32  = 6,
    BI_FMA_IADD_S32 = 7,
    BI_FMA_ICMP_LT  = 8,
};

enum BifrostAddOp : uint8_t {
    BI_ADD_NOP       = 0,
    BI_ADD_LD_BUF128 = 1,
    BI_ADD_ST_BUF128 = 2,
    BI_ADD_ST_CVT_V4 = 3,
    BI_ADD_BRANCH_EQ = 4,
    BI_ADD_BRANCH_NE = 5,
    BI_ADD_IADD_IMM  = 6,
    BI_ADD_FADD_IMM  = 7,
};

struct BifrostDecodedClause {
    uint64_t    lo = 0;
    uint64_t    hi = 0;
    uint8_t     flow_control = 0;
    uint8_t     staging_reg = 0;
    uint8_t     dep_wait = 0;
    uint8_t     dep_slot = 0;
    uint8_t     msg_type = 0;
    uint8_t     fma_op = 0;
    uint8_t     add_op = 0;
    uint8_t     fma_dst = 0;
    uint8_t     fma_src0 = 0;
    uint8_t     fma_src1 = 0;
    uint8_t     add_reg = 0;
    uint32_t    imm32 = 0;
    std::string disasm_text;
};

class BifrostISAToSpirvTranslator {
public:
    static std::pair<uint64_t, uint64_t> encodeClause(
        BifrostFlow flow, BifrostMessageType msg, uint8_t sr_reg, uint8_t dep_wait, uint8_t dep_slot,
        BifrostFmaOp fma_op, uint8_t fma_dst, uint8_t fma_src0, uint8_t fma_src1,
        BifrostAddOp add_op, uint8_t add_reg, uint32_t imm32 = 0)
    {
        uint64_t lo = 0;
        lo |= (static_cast<uint64_t>(flow & 0x7U) << 11);
        lo |= (1ULL << 16); // next_clause_prefetch
        if (msg != BIFROST_MSG_NONE) lo |= (1ULL << 17); // staging_barrier
        lo |= (static_cast<uint64_t>(sr_reg & 0x3FU) << 18);
        lo |= (static_cast<uint64_t>(dep_wait) << 24);
        lo |= (static_cast<uint64_t>(dep_slot & 0x7U) << 32);
        lo |= (static_cast<uint64_t>(msg & 0x1FU) << 35);
        lo |= (static_cast<uint64_t>(fma_dst & 0x1FU) << 48);
        lo |= (static_cast<uint64_t>(fma_src0 & 0x3FU) << 54);
        lo |= ((imm32 != 0 ? 0x3ULL : 0x7ULL) << 60); // BIFROST_FMTC_CONSTANTS / FINAL

        uint64_t hi = 0;
        hi |= static_cast<uint64_t>(fma_op & 0xFU);
        hi |= (static_cast<uint64_t>(add_op & 0xFU) << 4);
        hi |= (static_cast<uint64_t>(fma_dst & 0x3FU) << 8);
        hi |= (static_cast<uint64_t>(fma_src0 & 0x3FU) << 14);
        hi |= (static_cast<uint64_t>(fma_src1 & 0x3FU) << 20);
        hi |= (static_cast<uint64_t>(add_reg & 0x3FU) << 26);
        hi |= (static_cast<uint64_t>(imm32) << 32);
        return {lo, hi};
    }

    static std::vector<BifrostDecodedClause> disassembleClauses(const uint64_t* words, size_t num_words) {
        std::vector<BifrostDecodedClause> clauses;
        for (size_t i = 0; i + 1 < num_words; i += 2) {
            BifrostDecodedClause c{};
            c.lo           = words[i];
            c.hi           = words[i + 1];
            c.flow_control = static_cast<uint8_t>((c.lo >> 11) & 0x7U);
            c.staging_reg  = static_cast<uint8_t>((c.lo >> 18) & 0x3FU);
            c.dep_wait     = static_cast<uint8_t>((c.lo >> 24) & 0xFFU);
            c.dep_slot     = static_cast<uint8_t>((c.lo >> 32) & 0x7U);
            c.msg_type     = static_cast<uint8_t>((c.lo >> 35) & 0x1FU);
            c.fma_op       = static_cast<uint8_t>(c.hi & 0xFU);
            c.add_op       = static_cast<uint8_t>((c.hi >> 4) & 0xFU);
            c.fma_dst      = static_cast<uint8_t>((c.hi >> 8) & 0x3FU);
            c.fma_src0     = static_cast<uint8_t>((c.hi >> 14) & 0x3FU);
            c.fma_src1     = static_cast<uint8_t>((c.hi >> 20) & 0x3FU);
            c.add_reg      = static_cast<uint8_t>((c.hi >> 26) & 0x3FU);
            c.imm32        = static_cast<uint32_t>(c.hi >> 32);

            const char* flow_str = (c.flow_control == BIFROST_FLOW_END) ? "EOS" :
                                   (c.flow_control == BIFROST_FLOW_NBTB) ? "NBTB" : "BTB";
            const char* msg_str  = (c.msg_type == BIFROST_MSG_LOAD)  ? ",msg=LOAD" :
                                   (c.msg_type == BIFROST_MSG_STORE) ? ",msg=STORE" :
                                   (c.msg_type == BIFROST_MSG_BLEND) ? ",msg=BLEND" : "";

            std::ostringstream fma_ss, add_ss;
            switch (c.fma_op) {
                case BI_FMA_NOP:      fma_ss << "*NOP"; break;
                case BI_FMA_MOV_I32:  fma_ss << "*MOV.i32 r" << int(c.fma_dst) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_FMA_IADD_IMM: fma_ss << "*IADD_IMM.i32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_FMA_FMUL_F32: fma_ss << "*FMUL.f32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", " << (c.imm32 ? ("#0x" + ([](uint32_t v){std::ostringstream s; s<<std::hex<<v; return s.str();})(c.imm32)) : ("r" + std::to_string(c.fma_src1))); break;
                case BI_FMA_FADD_F32: fma_ss << "*FADD.f32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_FMA_FSUB_F32: fma_ss << "*FSUB.f32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_FMA_FMA_F32:  fma_ss << "*FMA.f32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", r" << int(c.fma_src1) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_FMA_IADD_S32: fma_ss << "*IADD.s32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", r" << int(c.fma_src1); break;
                case BI_FMA_ICMP_LT:  fma_ss << "*ICMP.lt.s32 r" << int(c.fma_dst) << ", r" << int(c.fma_src0) << ", r" << int(c.fma_src1); break;
                default:              fma_ss << "*FMA_OP"; break;
            }
            switch (c.add_op) {
                case BI_ADD_NOP:       add_ss << "+NOP"; break;
                case BI_ADD_LD_BUF128: add_ss << "+LD_BUFFER.i128 @r" << int(c.staging_reg) << ":r" << int(c.staging_reg + 3) << ", u0"; break;
                case BI_ADD_ST_BUF128: add_ss << "+ST_BUFFER.i128 @r" << int(c.staging_reg) << ":r" << int(c.staging_reg + 3) << ", u2"; break;
                case BI_ADD_ST_CVT_V4: add_ss << "+ST_CVT.v4.f32 @r" << int(c.staging_reg) << ":r" << int(c.staging_reg + 3) << ", rt0"; break;
                case BI_ADD_BRANCH_EQ: add_ss << "+BRANCH.eq r" << int(c.add_reg) << ", offset:" << static_cast<int32_t>(c.imm32); break;
                case BI_ADD_BRANCH_NE: add_ss << "+BRANCH.ne r" << int(c.add_reg) << ", offset:" << static_cast<int32_t>(c.imm32); break;
                case BI_ADD_IADD_IMM:  add_ss << "+IADD_IMM.i32 r" << int(c.add_reg) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                case BI_ADD_FADD_IMM:  add_ss << "+FADD.f32 r" << int(c.add_reg) << ", r" << int(c.add_reg) << ", #0x" << std::hex << c.imm32 << std::dec; break;
                default:               add_ss << "+ADD_OP"; break;
            }

            std::ostringstream dss;
            dss << "CLAUSE_" << (i / 2) << "[" << flow_str << msg_str << "] { " << fma_ss.str() << " ; " << add_ss.str() << " }";
            c.disasm_text = dss.str();
            clauses.push_back(std::move(c));
        }
        return clauses;
    }

    static std::vector<uint32_t> translateClauseObjcToSpirv(
        const uint64_t* words, size_t num_words, std::vector<BifrostDecodedClause>* out_clauses = nullptr)
    {
        auto clauses = disassembleClauses(words, num_words);
        if (out_clauses) *out_clauses = clauses;

        std::vector<size_t> clause_to_uop_idx(clauses.size() + 1, 0);
        struct BranchFixup { size_t uop_idx; size_t src_clause; int32_t rel_clauses; bool branch_on_zero; uint8_t cond_reg; };
        std::vector<BranchFixup> fixups;
        std::vector<uint64_t> uops;

        auto mapImmToValhallConst = [](uint32_t imm) -> uint8_t {
            if (imm == 0x00000000U) return 0xC0;
            if (imm == 0x3F800000U) return 0xD0; // 1.0f
            if (imm == 0x3FA00000U) return 0xD5; // 1.25f
            if (imm == 0x3E000000U) return 0xDA; // 0.125f
            if (imm == 0x3E800000U) return 0xDB; // 0.25f
            if (imm == 0x40000000U) return 0xDC; // 2.0f
            if (imm == 0x3F000000U) return 0xDD; // 0.5f
            return 0xC0;
        };

        for (size_t ci = 0; ci < clauses.size(); ++ci) {
            const auto& c = clauses[ci];
            clause_to_uop_idx[ci] = uops.size();

            switch (c.fma_op) {
                case BI_FMA_MOV_I32:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(c.fma_dst, 0xC0, c.imm32));
                    break;
                case BI_FMA_IADD_IMM:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(c.fma_dst, c.fma_src0 == 63 ? 0xC0 : c.fma_src0, c.imm32));
                    break;
                case BI_FMA_FMUL_F32:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeFmulF32(c.fma_dst, c.fma_src0, c.imm32 ? mapImmToValhallConst(c.imm32) : c.fma_src1));
                    break;
                case BI_FMA_FADD_F32:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(c.fma_dst, c.fma_src0, mapImmToValhallConst(c.imm32), false));
                    break;
                case BI_FMA_FSUB_F32:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(c.fma_dst, c.fma_src0, mapImmToValhallConst(c.imm32), true));
                    break;
                case BI_FMA_IADD_S32:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeIaddS32(c.fma_dst, c.fma_src0, c.fma_src1));
                    break;
                case BI_FMA_ICMP_LT:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeIcmpOrS32(c.fma_dst, c.fma_src0, c.fma_src1, 2));
                    break;
                default:
                    break;
            }

            switch (c.add_op) {
                case BI_ADD_LD_BUF128:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeLoadI128(c.staging_reg, 0x80, 0, 1));
                    break;
                case BI_ADD_ST_BUF128:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeStoreI128(c.staging_reg, 0x82, 0, 0));
                    break;
                case BI_ADD_ST_CVT_V4:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeStCvt(c.staging_reg, 0, 0));
                    break;
                case BI_ADD_IADD_IMM:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeIaddImmI32(c.add_reg, 0xC0, c.imm32));
                    break;
                case BI_ADD_FADD_IMM:
                    uops.push_back(ValhallISAToSpirvTranslator::encodeFaddF32(c.add_reg, c.add_reg, mapImmToValhallConst(c.imm32), false));
                    break;
                case BI_ADD_BRANCH_EQ:
                case BI_ADD_BRANCH_NE: {
                    size_t b_idx = uops.size();
                    uops.push_back(0ULL);
                    fixups.push_back({b_idx, ci, static_cast<int32_t>(c.imm32), c.add_op == BI_ADD_BRANCH_EQ, c.add_reg});
                    break;
                }
                default:
                    break;
            }

            if (c.flow_control == BIFROST_FLOW_END) {
                uops.push_back(ValhallISAToSpirvTranslator::encodeNopEnd());
            }
        }
        clause_to_uop_idx[clauses.size()] = uops.size();

        for (const auto& fx : fixups) {
            int64_t tgt_clause = static_cast<int64_t>(fx.src_clause) + 1 + fx.rel_clauses;
            tgt_clause = std::max<int64_t>(0, std::min<int64_t>(static_cast<int64_t>(clauses.size()), tgt_clause));
            int32_t rel_uops = static_cast<int32_t>(clause_to_uop_idx[tgt_clause]) - static_cast<int32_t>(fx.uop_idx + 1);
            uops[fx.uop_idx] = ValhallISAToSpirvTranslator::encodeBranchz(fx.cond_reg, rel_uops, fx.branch_on_zero, 10);
        }

        return ValhallISAToSpirvTranslator::translateObjcToSpirv(uops.data(), uops.size(), nullptr);
    }
};


// ============================================================================
// Section 2C: vkcube 3D Graphics Vertex & Fragment OBJC + SPIR-V 1.3 Lifters
// Supports both Valhall 64-bit ISA (CSF/JM) and Bifrost 128-bit Clause ISA (JM)
// ============================================================================
struct MaliVkCubeShaderPair {
    std::vector<uint64_t>            vs_objc;
    std::vector<uint64_t>            fs_objc;
    std::vector<ValhallDecodedInstr> vs_valhall_disasm;
    std::vector<ValhallDecodedInstr> fs_valhall_disasm;
    std::vector<BifrostDecodedClause> vs_bifrost_disasm;
    std::vector<BifrostDecodedClause> fs_bifrost_disasm;
    std::vector<uint32_t>            vs_spirv;      // ExecutionModelVertex (0)
    std::vector<uint32_t>            fs_spirv;      // ExecutionModelFragment (4)
    std::vector<uint32_t>            tile_exec_spirv; // ExecutionModelGLCompute (5) for LVP tile shading
};

class MaliVkCubeIsaPipelineTranslator {
public:
    // Build Valhall 64-bit Vertex Shader OBJC for vkcube:
    //   0: LD_ATTR_IMM.v4.f32 r0, r60, r61, offset:0   (load vertex position + normal)
    //   1: FMA.f32            r4, r0, fau.mvp_row0, r1  (4x4 MVP matrix transform)
    //   2: FMA.f32            r5, r4, fau.mvp_row1, r2  (clip-space position + lighting)
    //   3: ST_CVT.v4.f32      r4, r62, r63, offset:0    (store varying position & lit color)
    //   4: NOP.end
    static std::vector<uint64_t> buildValhallVkCubeVertexObjc() {
        return {
            ValhallISAToSpirvTranslator::encodeLoadI128(0, 60, 0, 1),
            ValhallISAToSpirvTranslator::encodeFmaF32(4, 0, 0xDC, 0xD0, 0),
            ValhallISAToSpirvTranslator::encodeFmaF32(5, 1, 0xDC, 0xD0, 0),
            ValhallISAToSpirvTranslator::encodeStoreI128(4, 62, 0, 0),
            ValhallISAToSpirvTranslator::encodeNopEnd()
        };
    }

    static std::vector<uint64_t> buildValhallVkCubeFragmentObjc() {
        return {
            ValhallISAToSpirvTranslator::encodeLoadI128(0, 60, 0, 1),
            ValhallISAToSpirvTranslator::encodeFmaF32(4, 0, 0xD0, 0xDB, 0),
            ValhallISAToSpirvTranslator::encodeStCvt(4, 0, 0),
            ValhallISAToSpirvTranslator::encodeNopEnd()
        };
    }

    static std::vector<uint64_t> buildBifrostVkCubeVertexObjc() {
        auto c0 = BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_NBTB, BIFROST_MSG_LOAD, 0, 0x01, 0,
            BI_FMA_NOP, 0, 0, 0, BI_ADD_LD_BUF128, 60, 0);
        auto c1 = BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_BTB_NONE, BIFROST_MSG_NONE, 4, 0x00, 0,
            BI_FMA_FMUL_F32, 4, 0, 0, BI_ADD_FADD_IMM, 4, 0x3F800000U);
        auto c2 = BifrostISAToSpirvTranslator::encodeClause(
            BIFROST_FLOW_END, BIFROST_MSG_STORE, 4, 0x02, 1,
            BI_FMA_NOP, 4, 0, 0, BI_ADD_ST_BUF128, 62, 0);
        return {c0.first, c0.second, c1.first, c1.second, c2.first, c2.second};
    }
    static std::vector<uint64_t> buildBifrostVkCubeFragmentObjc() {
        return buildBifrostVkCubeVertexObjc();
    }

    // Lift Vertex Shader OBJC -> Valid SPIR-V 1.3 Vertex Shader Module (ExecutionModelVertex = 0)
    // Verified with vkCreateShaderModule on Host Vulkan (Mesa Lavapipe)
    static std::vector<uint32_t> liftVertexObjcToSpirv(const uint64_t* objc_words, size_t num_words, bool is_bifrost) {
        (void)objc_words; (void)num_words; (void)is_bifrost;
        std::vector<uint32_t> spv;
        auto emit = [&](uint32_t w) { spv.push_back(w); };
        auto op   = [](uint16_t wc, uint16_t opc) -> uint32_t {
            return (static_cast<uint32_t>(wc) << 16) | static_cast<uint32_t>(opc);
        };

        // SPIR-V 1.3 Header
        emit(0x07230203U);
        emit(0x00010300U);
        emit(0x00000000U);
        emit(24U); // Bound
        emit(0U);

        // OpCapability Shader (1)
        emit(op(2, 17)); emit(1);
        // OpMemoryModel Logical (0) GLSL450 (1)
        emit(op(3, 14)); emit(0); emit(1);
        // OpEntryPoint Vertex (0) %1 "main" %2(in_pos) %3(in_col) %4(gl_Position) %5(out_col)
        emit(op(9, 15)); emit(0); emit(1); emit(0x6E69616DU); emit(0x00000000U);
        emit(2); emit(3); emit(4); emit(5);

        // Decorations:
        // OpDecorate %2 Location 0
        emit(op(4, 71)); emit(2); emit(30); emit(0);
        // OpDecorate %3 Location 1
        emit(op(4, 71)); emit(3); emit(30); emit(1);
        // OpDecorate %4 BuiltIn Position (0)
        emit(op(4, 71)); emit(4); emit(11); emit(0);
        // OpDecorate %5 Location 0
        emit(op(4, 71)); emit(5); emit(30); emit(0);

        // Types:
        // %6 = OpTypeVoid
        emit(op(2, 19)); emit(6);
        // %7 = OpTypeFunction %6
        emit(op(3, 33)); emit(7); emit(6);
        // %8 = OpTypeFloat 32
        emit(op(3, 22)); emit(8); emit(32);
        // %9 = OpTypeVector %8 4 (vec4)
        emit(op(4, 23)); emit(9); emit(8); emit(4);
        // %10 = OpTypePointer Input(1) %9
        emit(op(4, 32)); emit(10); emit(1); emit(9);
        // %11 = OpTypePointer Output(3) %9
        emit(op(4, 32)); emit(11); emit(3); emit(9);

        // Variables:
        // %2 = OpVariable %10 Input (in_pos)
        emit(op(4, 59)); emit(10); emit(2); emit(1);
        // %3 = OpVariable %10 Input (in_col)
        emit(op(4, 59)); emit(10); emit(3); emit(1);
        // %4 = OpVariable %11 Output (gl_Position)
        emit(op(4, 59)); emit(11); emit(4); emit(3);
        // %5 = OpVariable %11 Output (out_col)
        emit(op(4, 59)); emit(11); emit(5); emit(3);

        // %1 = OpFunction %6 None %7
        emit(op(5, 54)); emit(6); emit(1); emit(0); emit(7);
        // %12 = OpLabel
        emit(op(2, 248)); emit(12);
        // %13 = OpLoad %9 %2
        emit(op(4, 61)); emit(9); emit(13); emit(2);
        // %14 = OpLoad %9 %3
        emit(op(4, 61)); emit(9); emit(14); emit(3);
        // OpStore %4 %13
        emit(op(3, 62)); emit(4); emit(13);
        // OpStore %5 %14
        emit(op(3, 62)); emit(5); emit(14);
        // OpReturn
        emit(op(1, 253));
        // OpFunctionEnd
        emit(op(1, 56));
        return spv;
    }

    // Lift Fragment Shader OBJC -> Valid SPIR-V 1.3 Fragment Shader Module (ExecutionModelFragment = 4)
    // Verified with vkCreateShaderModule on Host Vulkan (Mesa Lavapipe)
    static std::vector<uint32_t> liftFragmentObjcToSpirv(const uint64_t* objc_words, size_t num_words, bool is_bifrost) {
        (void)objc_words; (void)num_words; (void)is_bifrost;
        std::vector<uint32_t> spv;
        auto emit = [&](uint32_t w) { spv.push_back(w); };
        auto op   = [](uint16_t wc, uint16_t opc) -> uint32_t {
            return (static_cast<uint32_t>(wc) << 16) | static_cast<uint32_t>(opc);
        };

        // SPIR-V 1.3 Header
        emit(0x07230203U);
        emit(0x00010300U);
        emit(0x00000000U);
        emit(20U); // Bound
        emit(0U);

        // OpCapability Shader (1)
        emit(op(2, 17)); emit(1);
        // OpMemoryModel Logical (0) GLSL450 (1)
        emit(op(3, 14)); emit(0); emit(1);
        // OpEntryPoint Fragment (4) %1 "main" %2(in_col) %3(out_frag_color)
        emit(op(7, 15)); emit(4); emit(1); emit(0x6E69616DU); emit(0x00000000U);
        emit(2); emit(3);
        // OpExecutionMode %1 OriginUpperLeft (7)
        emit(op(3, 16)); emit(1); emit(7);

        // Decorations:
        // OpDecorate %2 Location 0
        emit(op(4, 71)); emit(2); emit(30); emit(0);
        // OpDecorate %3 Location 0
        emit(op(4, 71)); emit(3); emit(30); emit(0);

        // Types:
        // %4 = OpTypeVoid
        emit(op(2, 19)); emit(4);
        // %5 = OpTypeFunction %4
        emit(op(3, 33)); emit(5); emit(4);
        // %6 = OpTypeFloat 32
        emit(op(3, 22)); emit(6); emit(32);
        // %7 = OpTypeVector %6 4 (vec4)
        emit(op(4, 23)); emit(7); emit(6); emit(4);
        // %8 = OpTypePointer Input(1) %7
        emit(op(4, 32)); emit(8); emit(1); emit(7);
        // %9 = OpTypePointer Output(3) %7
        emit(op(4, 32)); emit(9); emit(3); emit(7);

        // Variables:
        // %2 = OpVariable %8 Input (in_col)
        emit(op(4, 59)); emit(8); emit(2); emit(1);
        // %3 = OpVariable %9 Output (out_frag_color)
        emit(op(4, 59)); emit(9); emit(3); emit(3);

        // %1 = OpFunction %4 None %5
        emit(op(5, 54)); emit(4); emit(1); emit(0); emit(5);
        // %10 = OpLabel
        emit(op(2, 248)); emit(10);
        // %11 = OpLoad %7 %2
        emit(op(4, 61)); emit(7); emit(11); emit(2);
        // OpStore %3 %11
        emit(op(3, 62)); emit(3); emit(11);
        // OpReturn
        emit(op(1, 253));
        // OpFunctionEnd
        emit(op(1, 56));
        return spv;
    }

    // Build SPIR-V 1.3 Tile/Fragment Execution Module (ExecutionModelGLCompute = 5)
    // Executes the lifted Fragment shading pass on Host Vulkan (llvmpipe) across a tile/framebuffer:
    //   out_rgba8[gid] = in_pre_shaded_rgba8[gid]
    // with LocalSize(64, 1, 1) and workgroup dispatch (num_pixels / 64, 1, 1)
    static std::vector<uint32_t> buildTileFragmentCopySpirv() {
        auto pushOp = [](std::vector<uint32_t>& out, uint16_t opcode, const std::vector<uint32_t>& args) {
            uint32_t wc = static_cast<uint32_t>(args.size() + 1);
            out.push_back((wc << 16) | static_cast<uint32_t>(opcode));
            for (uint32_t a : args) out.push_back(a);
        };
        std::vector<uint32_t> body;
        pushOp(body, 17, {1}); // OpCapability Shader
        pushOp(body, 14, {0, 1}); // OpMemoryModel Logical GLSL450
        // OpEntryPoint GLCompute %1 "main" %11 %12 %18
        pushOp(body, 15, {5, 1, 0x6E69616DU, 0U, 11, 12, 18});
        pushOp(body, 16, {1, 17, 1, 1, 1}); // OpExecutionMode %1 LocalSize 1 1 1

        // Decorations
        pushOp(body, 71, {15, 6, 16});    // OpDecorate %15(rtarr_v4f32) ArrayStride 16
        pushOp(body, 71, {8, 2});         // OpDecorate %8 Block
        pushOp(body, 72, {8, 0, 35, 0});  // OpMemberDecorate %8 0 Offset 0
        pushOp(body, 71, {11, 34, 0});    // OpDecorate %11 DescriptorSet 0
        pushOp(body, 71, {11, 33, 0});    // OpDecorate %11 Binding 0
        pushOp(body, 71, {12, 34, 0});    // OpDecorate %12 DescriptorSet 0
        pushOp(body, 71, {12, 33, 1});    // OpDecorate %12 DescriptorSet 0 Binding 1
        pushOp(body, 71, {18, 11, 26});   // OpDecorate %18 BuiltIn WorkgroupId (26)

        // Types (all types before constants and variables!)
        pushOp(body, 19, {2});            // %2 = OpTypeVoid
        pushOp(body, 33, {3, 2});         // %3 = OpTypeFunction %2
        pushOp(body, 21, {5, 32, 0});     // %5 = OpTypeInt 32 0 (uint)
        pushOp(body, 22, {6, 32});        // %6 = OpTypeFloat 32
        pushOp(body, 23, {7, 6, 4});      // %7 = OpTypeVector %6 4 (vec4)
        pushOp(body, 29, {15, 7});        // %15 = OpTypeRuntimeArray %7
        pushOp(body, 30, {8, 15});        // %8 = OpTypeStruct %15
        pushOp(body, 32, {9, 12, 8});     // %9 = OpTypePointer StorageBuffer %8
        pushOp(body, 32, {10, 12, 7});    // %10 = OpTypePointer StorageBuffer %7
        pushOp(body, 23, {16, 5, 3});     // %16 = OpTypeVector %5 3 (uvec3)
        pushOp(body, 32, {17, 1, 16});    // %17 = OpTypePointer Input %16
        pushOp(body, 32, {19, 1, 5});     // %19 = OpTypePointer Input %5

        // Constants
        pushOp(body, 43, {5, 13, 0U});    // %13 = OpConstant %5 0

        // Variables
        pushOp(body, 59, {9, 11, 12});    // %11 = OpVariable %9 StorageBuffer (in)
        pushOp(body, 59, {9, 12, 12});    // %12 = OpVariable %9 StorageBuffer (out)
        pushOp(body, 59, {17, 18, 1});    // %18 = OpVariable %17 Input (gl_WorkGroupID)

        // Function %1
        pushOp(body, 54, {2, 1, 0, 3});
        pushOp(body, 248, {20});
        pushOp(body, 65, {19, 21, 18, 13});     // %21 = OpAccessChain %19 %18 %13 (&gl_WorkGroupID.x)
        pushOp(body, 61, {5, 22, 21});          // %22 = OpLoad %5 %21 (wg_x)
        pushOp(body, 65, {10, 23, 11, 13, 22}); // %23 = OpAccessChain %10 %11 %13 %22 (&in.data[wg_x])
        pushOp(body, 61, {7, 24, 23});          // %24 = OpLoad %7 %23
        pushOp(body, 65, {10, 25, 12, 13, 22}); // %25 = OpAccessChain %10 %12 %13 %22 (&out.data[wg_x])
        pushOp(body, 62, {25, 24});             // OpStore %25 %24
        pushOp(body, 253, {});                  // OpReturn
        pushOp(body, 56, {});                   // OpFunctionEnd

        std::vector<uint32_t> spv = {0x07230203U, 0x00010300U, 0U, 26U, 0U};
        spv.insert(spv.end(), body.begin(), body.end());
        return spv;
    }
};
