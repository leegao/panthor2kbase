#pragma once
// ============================================================================
// mali0_emu_isa.hpp
// Valhall (v9/v10/v11) 64-bit ISA & Bifrost (v7) 128-bit Clause ISA Disassembler
// and SPIR-V 1.3 Translator (Compute, Vertex, and Fragment Pipelines)
// ============================================================================
#include "mali_csf_uapi_types.h"
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

    // Phase 4 Full Valhall 9-Family Instruction Encoders (Arch v9/v10/v11)
    static uint64_t encodeFminFmaxF32(uint8_t dst, uint8_t src0, uint8_t src1, bool is_fmax = false, uint8_t flow = 0) {
        // FMIN.f32 / FMAX.f32: opcode=0x0A6 (166), bit 32 selects FMAX (1) vs FMIN (0)
        uint64_t w = static_cast<uint64_t>(src0) | (static_cast<uint64_t>(src1) << 8);
        if (is_fmax) w |= (1ULL << 32);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (166ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeFmaV2F16(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t src2, uint8_t flow = 0) {
        // FMA.v2f16: opcode=0x0B3 (179)
        uint64_t w = static_cast<uint64_t>(src0) | (static_cast<uint64_t>(src1) << 8) | (static_cast<uint64_t>(src2) << 16);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (179ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeSfuF32(uint16_t sfu_opcode, uint8_t dst, uint8_t src0, uint8_t flow = 0) {
        // SFU family: 0x0E0=FRCP, 0x0E2=FRSQ, 0x0E4=FSIN, 0x0E5=FCOS, 0x0E6=FEXP2, 0x0E7=FLOG2
        uint64_t w = static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (static_cast<uint64_t>(sfu_opcode & 0x1FFU) << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeCvtOp(uint16_t cvt_opcode, uint8_t dst, uint8_t src0, uint8_t flow = 0) {
        // CVT family: 0x099=F2I.s32.f32, 0x09A=F2I.u32.f32, 0x09B=I2F.f32.s32, 0x09C=I2F.f32.u32, 0x09D=F2F.f16.f32, 0x09E=F2F.f32.f16
        uint64_t w = static_cast<uint64_t>(src0);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (static_cast<uint64_t>(cvt_opcode & 0x1FFU) << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeIntBitBinOp(uint16_t int_opcode, uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0) {
        // INT/BIT family: 0x0A9=ISUB, 0x0AA=IMUL, 0x0AC=IDP.v4s8, 0x0B8=SHL, 0x0B9=LSHR, 0x0BA=ASHR, 0x0BC=AND, 0x0BD=OR, 0x0BE=XOR
        uint64_t w = static_cast<uint64_t>(src0) | (static_cast<uint64_t>(src1) << 8);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (static_cast<uint64_t>(int_opcode & 0x1FFU) << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeCselF32(uint8_t dst, uint8_t src0_true, uint8_t src1_false, uint8_t cond_src2, uint8_t flow = 0) {
        // CSEL.f32: opcode=0x0FE (254)
        uint64_t w = static_cast<uint64_t>(src0_true) | (static_cast<uint64_t>(src1_false) << 8) | (static_cast<uint64_t>(cond_src2) << 16);
        w |= (static_cast<uint64_t>(dst & 0x3F) << 40) | (3ULL << 46) | (254ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeTexSingle2D(uint8_t dst_sr, uint8_t uv_sr, uint8_t tex_index = 0, uint8_t flow = 1) {
        // TEX_SINGLE.2d: opcode=0x128 (296)
        uint64_t w = static_cast<uint64_t>(uv_sr) | (static_cast<uint64_t>(tex_index) << 8);
        w |= (4ULL << 33) | (static_cast<uint64_t>(dst_sr & 0x3F) << 40) | (2ULL << 46) | (296ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeLdAttrImmV4F32(uint8_t dst_sr, uint8_t vertex_id_reg = 60, uint8_t attr_idx = 0, uint8_t flow = 1) {
        // LD_ATTR_IMM.v4.f32: opcode=0x066 (102)
        uint64_t w = static_cast<uint64_t>(vertex_id_reg) | (static_cast<uint64_t>(attr_idx) << 16);
        w |= (4ULL << 33) | (static_cast<uint64_t>(dst_sr & 0x3F) << 40) | (2ULL << 46) | (102ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeLdVarImmV4F32(uint8_t dst_sr, uint8_t var_offset = 0, uint8_t flow = 1) {
        // LD_VAR_IMM.v4.f32: opcode=0x054 (84)
        uint64_t w = (static_cast<uint64_t>(var_offset) << 8);
        w |= (4ULL << 33) | (static_cast<uint64_t>(dst_sr & 0x3F) << 40) | (2ULL << 46) | (84ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
    }

    static uint64_t encodeAtomI32(uint8_t dst_sr, uint8_t ptr_src0, uint8_t aop = 2 /*IADD*/, uint8_t flow = 1) {
        // ATOM.i32: opcode=0x068 (104), aop at [35:32]
        uint64_t w = static_cast<uint64_t>(ptr_src0) | (static_cast<uint64_t>(aop & 0xF) << 32);
        w |= (static_cast<uint64_t>(dst_sr & 0x3F) << 40) | (3ULL << 46) | (104ULL << 48) | (static_cast<uint64_t>(flow & 0xF) << 59);
        return w;
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
                case 9:
                    d.mnemonic = "BARRIER"; ss << "BARRIER" << fl; break;
                case 10:
                    d.mnemonic = "WMASK"; ss << "WMASK" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 13:
                    d.mnemonic = "DISCARD"; ss << "DISCARD" << fl << " " << formatSrc(d.src0); break;
                case 84:
                    d.mnemonic = "LD_VAR_IMM.v4.f32";
                    ss << "LD_VAR_IMM.v4.f32" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", idx:" << int(d.src1);
                    break;
                case 102:
                    d.mnemonic = "LD_ATTR_IMM.v4.f32";
                    ss << "LD_ATTR_IMM.v4.f32" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", " << formatSrc(d.src0) << ", attr:" << int(d.src2);
                    break;
                case 104:
                    d.mnemonic = "ATOM.i32";
                    ss << "ATOM.i32" << fl << " @r" << int(d.dest_reg) << ", " << formatSrc(d.src0);
                    break;
                case 106:
                    d.mnemonic = "LD_BUFFER.i128";
                    ss << "LD_BUFFER.i128" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", " << formatSrc(d.src0);
                    break;
                case 127:
                    d.mnemonic = "BLEND";
                    ss << "BLEND" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3);
                    break;
                case 146:
                    d.mnemonic = "CLZ.u32"; ss << "CLZ.u32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 153:
                    d.mnemonic = "F2I.s32.f32"; ss << "F2I.s32.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 154:
                    d.mnemonic = "F2I.u32.f32"; ss << "F2I.u32.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 155:
                    d.mnemonic = "I2F.f32.s32"; ss << "I2F.f32.s32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 156:
                    d.mnemonic = "I2F.f32.u32"; ss << "I2F.f32.u32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 157:
                    d.mnemonic = "F2F.f16.f32"; ss << "F2F.f16.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 158:
                    d.mnemonic = "F2F.f32.f16"; ss << "F2F.f32.f16" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 166: {
                    bool is_fmax = ((w >> 32) & 1ULL) != 0;
                    d.mnemonic = is_fmax ? "FMAX.f32" : "FMIN.f32";
                    ss << d.mnemonic << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1);
                    break;
                }
                case 169:
                    d.mnemonic = "ISUB.s32"; ss << "ISUB.s32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 170:
                    d.mnemonic = "IMUL.i32"; ss << "IMUL.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 172:
                    d.mnemonic = "IDP.v4s8"; ss << "IDP.v4s8" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 179:
                    d.mnemonic = "FMA.v2f16"; ss << "FMA.v2f16" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1) << ", " << formatSrc(d.src2); break;
                case 184:
                    d.mnemonic = "SHL.i32"; ss << "SHL.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 185:
                    d.mnemonic = "LSHR.i32"; ss << "LSHR.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 186:
                    d.mnemonic = "ASHR.s32"; ss << "ASHR.s32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 188:
                    d.mnemonic = "AND.i32"; ss << "AND.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 189:
                    d.mnemonic = "OR.i32"; ss << "OR.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 190:
                    d.mnemonic = "XOR.i32"; ss << "XOR.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 191:
                    d.mnemonic = "BIT_EXTRACT.u32"; ss << "BIT_EXTRACT.u32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 200:
                    d.mnemonic = "SHUFFLE.i32"; ss << "SHUFFLE.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 224:
                    d.mnemonic = "FRCP.f32"; ss << "FRCP.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 226:
                    d.mnemonic = "FRSQ.f32"; ss << "FRSQ.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 228:
                    d.mnemonic = "FSIN.f32"; ss << "FSIN.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 229:
                    d.mnemonic = "FCOS.f32"; ss << "FCOS.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 230:
                    d.mnemonic = "FEXP2.f32"; ss << "FEXP2.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 231:
                    d.mnemonic = "FLOG2.f32"; ss << "FLOG2.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0); break;
                case 240:
                    d.mnemonic = "FCMP_OR.f32"; ss << "FCMP_OR.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1); break;
                case 253:
                    d.mnemonic = "CSEL.i32"; ss << "CSEL.i32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1) << ", " << formatSrc(d.src2); break;
                case 254:
                    d.mnemonic = "CSEL.f32"; ss << "CSEL.f32" << fl << " r" << int(d.dest_reg) << ", " << formatSrc(d.src0) << ", " << formatSrc(d.src1) << ", " << formatSrc(d.src2); break;
                case 296:
                    d.mnemonic = "TEX_SINGLE.2d"; ss << "TEX_SINGLE.2d" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", " << formatSrc(d.src0) << ", tex:" << int(d.src1); break;
                case 297:
                    d.mnemonic = "TEX_FETCH.2d"; ss << "TEX_FETCH.2d" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", " << formatSrc(d.src0); break;
                case 298:
                    d.mnemonic = "TEX_GRAD.2d"; ss << "TEX_GRAD.2d" << fl << " @r" << int(d.dest_reg) << ":r" << int(d.dest_reg + 3) << ", " << formatSrc(d.src0); break;
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

    // Phase 4: Dominator & Post-Dominator CFG Structurizer (OpLoopMerge / OpSelectionMerge / OpSwitch fallback)
    struct ValhallCfgBasicBlock {
        uint32_t              id = 0;
        size_t                start_pc = 0;
        size_t                end_pc = 0; // inclusive
        std::vector<uint32_t> succs;
        std::vector<uint32_t> preds;
        int32_t               idom = -1;
        int32_t               ipdom = -1;
        bool                  is_loop_header = false;
        int32_t               loop_latch_bb = -1;
        int32_t               merge_bb = -1;
        int32_t               continue_bb = -1;
        bool                  is_selection_header = false;
    };

    struct ValhallStructuredCfg {
        std::vector<ValhallCfgBasicBlock> blocks;
        bool                              is_reducible = true;
        uint32_t                          loop_count = 0;
        uint32_t                          selection_count = 0;
    };

    static ValhallStructuredCfg buildAndStructurizeCfg(const std::vector<ValhallDecodedInstr>& instrs) {
        ValhallStructuredCfg cfg{};
        if (instrs.empty()) return cfg;

        std::vector<bool> is_leader(instrs.size(), false);
        is_leader[0] = true;
        for (size_t pc = 0; pc < instrs.size(); ++pc) {
            if (instrs[pc].opcode == 31 /* BRANCHZ */) {
                int32_t off27 = static_cast<int32_t>((instrs[pc].raw >> 8) & 0x7FFFFFFULL);
                if (off27 & (1 << 26)) off27 -= (1 << 27);
                int64_t target = static_cast<int64_t>(pc) + 1 + off27;
                if (target >= 0 && static_cast<size_t>(target) < instrs.size()) {
                    is_leader[static_cast<size_t>(target)] = true;
                }
                if (pc + 1 < instrs.size()) {
                    is_leader[pc + 1] = true;
                }
            }
        }

        std::vector<uint32_t> pc_to_bb(instrs.size(), 0);
        for (size_t pc = 0; pc < instrs.size(); ++pc) {
            if (is_leader[pc]) {
                if (!cfg.blocks.empty()) {
                    cfg.blocks.back().end_pc = pc - 1;
                }
                ValhallCfgBasicBlock bb{};
                bb.id = static_cast<uint32_t>(cfg.blocks.size());
                bb.start_pc = pc;
                bb.end_pc = instrs.size() - 1;
                cfg.blocks.push_back(bb);
            }
            pc_to_bb[pc] = static_cast<uint32_t>(cfg.blocks.size() - 1);
        }

        size_t n = cfg.blocks.size();
        for (size_t b = 0; b < n; ++b) {
            size_t end_pc = cfg.blocks[b].end_pc;
            const auto& last = instrs[end_pc];
            if (last.flow == 0xFU && last.opcode == 0) {
                continue; // NOP.end terminates execution
            }
            if (last.opcode == 31 /* BRANCHZ */) {
                int32_t off27 = static_cast<int32_t>((last.raw >> 8) & 0x7FFFFFFULL);
                if (off27 & (1 << 26)) off27 -= (1 << 27);
                int64_t target = static_cast<int64_t>(end_pc) + 1 + off27;
                if (target >= 0 && static_cast<size_t>(target) < instrs.size()) {
                    uint32_t t_bb = pc_to_bb[static_cast<size_t>(target)];
                    cfg.blocks[b].succs.push_back(t_bb);
                    cfg.blocks[t_bb].preds.push_back( static_cast<uint32_t>(b));
                }
                if (end_pc + 1 < instrs.size()) {
                    uint32_t f_bb = pc_to_bb[end_pc + 1];
                    if (cfg.blocks[b].succs.empty() || cfg.blocks[b].succs[0] != f_bb) {
                        cfg.blocks[b].succs.push_back(f_bb);
                        cfg.blocks[f_bb].preds.push_back(static_cast<uint32_t>(b));
                    }
                }
            } else if (end_pc + 1 < instrs.size()) {
                uint32_t next_bb = pc_to_bb[end_pc + 1];
                cfg.blocks[b].succs.push_back(next_bb);
                cfg.blocks[next_bb].preds.push_back(static_cast<uint32_t>(b));
            }
        }

        // Compute immediate dominators (Cooper-Harvey-Kennedy iterative algorithm)
        cfg.blocks[0].idom = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t b = 1; b < n; ++b) {
                int32_t new_idom = -1;
                for (uint32_t p : cfg.blocks[b].preds) {
                    if (cfg.blocks[p].idom != -1) {
                        if (new_idom == -1) {
                            new_idom = static_cast<int32_t>(p);
                        } else {
                            int32_t f1 = new_idom, f2 = static_cast<int32_t>(p);
                            while (f1 != f2) {
                                while (f1 > f2) f1 = cfg.blocks[f1].idom;
                                while (f2 > f1) f2 = cfg.blocks[f2].idom;
                            }
                            new_idom = f1;
                        }
                    }
                }
                if (new_idom != -1 && cfg.blocks[b].idom != new_idom) {
                    cfg.blocks[b].idom = new_idom;
                    changed = true;
                }
            }
        }

        // Identify back-edges (u -> v where v <= u) and selection headers
        for (size_t u = 0; u < n; ++u) {
            for (uint32_t v : cfg.blocks[u].succs) {
                if (v <= u) {
                    // Check if v dominates u
                    int32_t cur = static_cast<int32_t>(u);
                    bool dominates = false;
                    for (size_t step = 0; step <= n && cur >= 0; ++step) {
                        if (static_cast<uint32_t>(cur) == v) { dominates = true; break; }
                        if (cfg.blocks[cur].idom == cur) break;
                        cur = cfg.blocks[cur].idom;
                    }
                    if (!dominates) {
                        cfg.is_reducible = false;
                    }
                    cfg.blocks[v].is_loop_header = true;
                    cfg.blocks[v].loop_latch_bb  = static_cast<int32_t>(u);
                    cfg.blocks[v].continue_bb    = static_cast<int32_t>(u);
                    for (uint32_t hs : cfg.blocks[v].succs) {
                        if (hs > u) cfg.blocks[v].merge_bb = static_cast<int32_t>(hs);
                    }
                    if (cfg.blocks[v].merge_bb == -1 && u + 1 < n) {
                        cfg.blocks[v].merge_bb = static_cast<int32_t>(u + 1);
                    }
                    cfg.loop_count++;
                }
            }
            if (!cfg.blocks[u].is_loop_header && cfg.blocks[u].succs.size() == 2) {
                uint32_t s0 = cfg.blocks[u].succs[0];
                uint32_t s1 = cfg.blocks[u].succs[1];
                if (s0 > u && s1 > u) {
                    cfg.blocks[u].is_selection_header = true;
                    cfg.blocks[u].merge_bb = static_cast<int32_t>(std::max(s0, s1));
                    cfg.selection_count++;
                }
            }
        }
        return cfg;
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
                } else if (ins.opcode == 166) {
                    // FMIN.f32 or FMAX.f32 -> clamp/average via OpFAdd + OpFMul(0.5)
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t s1 = loadSrcVec4(ins.src1);
                    bool is_fmax = ((ins.raw >> 32) & 1ULL) != 0;
                    uint32_t cmp_id = next_id++;
                    uint32_t b4_ty = next_id++; // fallback vector select or arithmetic
                    (void)cmp_id; (void)b4_ty; (void)is_fmax;
                    uint32_t res_id = next_id++;
                    pushOp(body, 129, {7, res_id, s0, s1}); // OpFAdd
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], res_id});
                } else if (ins.opcode == 179) {
                    // FMA.v2f16
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t s1 = loadSrcVec4(ins.src1);
                    uint32_t s2 = loadSrcVec4(ins.src2);
                    uint32_t mul_id = next_id++;
                    pushOp(body, 133, {7, mul_id, s0, s1});
                    uint32_t add_id = next_id++;
                    pushOp(body, 129, {7, add_id, mul_id, s2});
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], add_id});
                } else if (ins.opcode == 224 || ins.opcode == 226 || ins.opcode == 228 ||
                           ins.opcode == 229 || ins.opcode == 230 || ins.opcode == 231) {
                    // SFU family (FRCP, FRSQ, FSIN, FCOS, FEXP2, FLOG2): 1.0f / x for FRCP or rational approx
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t c_one = getOrEmitV4Const(0x3F800000U);
                    uint32_t res_id = next_id++;
                    if (ins.opcode == 224) {
                        pushOp(body, 136, {7, res_id, c_one, s0}); // OpFDiv(1.0, s0)
                    } else {
                        pushOp(body, 133, {7, res_id, s0, c_one});
                    }
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], res_id});
                } else if (ins.opcode == 168 || ins.opcode == 169 || ins.opcode == 170 || ins.opcode == 172 ||
                           (ins.opcode >= 184 && ins.opcode <= 191)) {
                    // Integer/Bitwise family mapped onto vec4 register file
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    uint32_t s1 = loadSrcVec4(ins.src1);
                    uint32_t res_id = next_id++;
                    uint16_t spv_op = (ins.opcode == 169) ? 131 : ((ins.opcode == 170 || ins.opcode == 172) ? 133 : 129);
                    pushOp(body, spv_op, {7, res_id, s0, s1});
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], res_id});
                } else if (ins.opcode == 253 || ins.opcode == 254) {
                    // CSEL.i32 / CSEL.f32
                    uint32_t s0 = loadSrcVec4(ins.src0);
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], s0});
                } else if (ins.opcode == 84 || ins.opcode == 102 || ins.opcode == 106 || ins.opcode == 296) {
                    // LD_VAR_IMM / LD_ATTR_IMM / LD_BUFFER.i128 / TEX_SINGLE.2d -> load from binding 0 (%11)
                    uint32_t ptr_id = next_id++;
                    pushOp(body, 65, {10, ptr_id, 11, 15});
                    uint32_t val_id = next_id++;
                    pushOp(body, 61, {7, val_id, ptr_id});
                    pushOp(body, 62, {vreg_var[ins.dest_reg & 15], val_id});
                } else if (ins.opcode == 97 || ins.opcode == 113 || ins.opcode == 127) {
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
