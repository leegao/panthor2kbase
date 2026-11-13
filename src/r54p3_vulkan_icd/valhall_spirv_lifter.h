#pragma once
// ============================================================================
// valhall_spirv_lifter.h
// Valhall (v9/v10/v11) 64-bit ISA & Bifrost (v7) 128-bit Clause ISA Disassembler
// and SPIR-V 1.3 Translator (Compute, Vertex, and Fragment Pipelines)
// ============================================================================
#include "mali_csf_uapi_types.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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

    static uint64_t encodeLoadI128(uint8_t sr_base, uint8_t ptr_src0, int16_t offset = 0, uint8_t flow = 1);
    static uint64_t encodeStoreI128(uint8_t sr_base, uint8_t ptr_src0, int16_t offset = 0, uint8_t flow = 0);
    static uint64_t encodeStCvt(uint8_t sr_base, uint8_t rt_index = 0, uint8_t flow = 0);
    static uint64_t encodeMovI32(uint8_t dst, uint8_t src0, uint8_t flow = 0);
    static uint64_t encodeFmaF32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t src2, uint8_t flow = 0);
    static uint64_t encodeFmulF32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0);
    static uint64_t encodeFaddF32(uint8_t dst, uint8_t src0, uint8_t src1, bool negate_src1 = false, uint8_t flow = 0);
    static uint64_t encodeIaddS32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0);
    static uint64_t encodeIaddImmI32(uint8_t dst, uint8_t src0, uint32_t imm32, uint8_t flow = 0);
    static uint64_t encodeIcmpOrS32(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t cmpf = 2 /*lt*/, uint8_t flow = 0);
    static uint64_t encodeBranchz(uint8_t cond_src0, int32_t offset_words, bool branch_if_equal_zero = true, uint8_t flow = 10 /*reconverge*/);
    static uint64_t encodeNopEnd();

    static uint64_t encodeFminFmaxF32(uint8_t dst, uint8_t src0, uint8_t src1, bool is_fmax = false, uint8_t flow = 0);
    static uint64_t encodeFmaV2F16(uint8_t dst, uint8_t src0, uint8_t src1, uint8_t src2, uint8_t flow = 0);
    static uint64_t encodeSfuF32(uint16_t sfu_opcode, uint8_t dst, uint8_t src0, uint8_t flow = 0);
    static uint64_t encodeCvtOp(uint16_t cvt_opcode, uint8_t dst, uint8_t src0, uint8_t flow = 0);
    static uint64_t encodeIntBitBinOp(uint16_t int_opcode, uint8_t dst, uint8_t src0, uint8_t src1, uint8_t flow = 0);
    static uint64_t encodeCselF32(uint8_t dst, uint8_t src0_true, uint8_t src1_false, uint8_t cond_src2, uint8_t flow = 0);
    static uint64_t encodeTexSingle2D(uint8_t dst_sr, uint8_t uv_sr, uint8_t tex_index = 0, uint8_t flow = 1);
    static uint64_t encodeLdAttrImmV4F32(uint8_t dst_sr, uint8_t vertex_id_reg = 60, uint8_t attr_idx = 0, uint8_t flow = 1);
    static uint64_t encodeLdVarImmV4F32(uint8_t dst_sr, uint8_t var_offset = 0, uint8_t flow = 1);
    static uint64_t encodeAtomI32(uint8_t dst_sr, uint8_t ptr_src0, uint8_t aop = 2 /*IADD*/, uint8_t flow = 1);

    static std::string formatSrc(uint8_t src);
    static std::string formatFlow(uint8_t flow);
    static std::vector<ValhallDecodedInstr> disassembleObjc(const uint64_t* words, size_t count);

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

    static ValhallStructuredCfg buildAndStructurizeCfg(const std::vector<ValhallDecodedInstr>& instrs);
    static std::vector<uint32_t> translateObjcToSpirv(const uint64_t* objc_words, size_t num_words,
                                                      std::vector<ValhallDecodedInstr>* out_disasm = nullptr);
};

enum BifrostFlow : uint8_t {
    BIFROST_FLOW_END                = 0,
    BIFROST_FLOW_NBTB_PC            = 1,
    BIFROST_FLOW_NBTB_UNCONDITIONAL = 2,
    BIFROST_FLOW_NBTB               = 3,
    BIFROST_FLOW_BTB_UNCONDITIONAL  = 4,
    BIFROST_FLOW_BTB_NONE           = 5,
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
        BifrostAddOp add_op, uint8_t add_reg, uint32_t imm32 = 0);
    static std::vector<BifrostDecodedClause> disassembleClauses(const uint64_t* words, size_t num_words);
    static std::vector<uint32_t> translateObjcToSpirv(const uint64_t* objc_words, size_t num_words,
                                                      std::vector<BifrostDecodedClause>* out_disasm = nullptr);
};
