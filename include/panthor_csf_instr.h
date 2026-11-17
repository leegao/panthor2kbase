/*
 * SPDX-License-Identifier: MIT
 *
 * Shared 64-bit ARM Mali CSF (Command Stream Frontend) Instruction Parser,
 * Builder, and Symbolic Lifting Header (Valhall v10 - v14).
 *
 * Shared between:
 *  - src/adapter/panthor_kbase_adapter.cpp (CSF stream normalizer & ring emitter)
 *  - src/emulator/mali_kbase_emu.c (64-bit CSF microcode interpreter)
 *  - tests/unit_tests_edge_cases.c & tests/run_vulkan_samples_and_equivalence_suite.c
 */
#ifndef PANTHOR_CSF_INSTR_H
#define PANTHOR_CSF_INSTR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "panthor_kbase_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

struct csf_decoded_insn {
   uint8_t  opcode;
   uint8_t  dst_reg;
   uint8_t  src0_reg;
   uint8_t  src1_reg;
   uint8_t  scope;
   uint8_t  heap_mode;
   bool     notif_bit;
   uint16_t sb_mask;
   uint32_t imm32;
   uint64_t imm48;
   uint64_t raw;
};

static inline struct csf_decoded_insn
csf_decode_insn(uint64_t raw)
{
   struct csf_decoded_insn d;
   d.raw       = raw;
   d.opcode    = (uint8_t)((raw >> 56) & 0xffULL);
   d.dst_reg   = (uint8_t)((raw >> 48) & 0xffULL);
   d.src0_reg  = (uint8_t)((raw >> 40) & 0xffULL);
   d.src1_reg  = (uint8_t)((raw >> 32) & 0xffULL);
   d.scope     = (uint8_t)((raw >> 28) & 0x3ULL);
   d.heap_mode = (uint8_t)((raw >> 28) & 0x3ULL);
   d.notif_bit = ((raw >> 24) & 0x1ULL) != 0;
   d.sb_mask   = (uint16_t)((raw >> 16) & 0xffffULL);
   d.imm32     = (uint32_t)(raw & 0xffffffffULL);
   d.imm48     = raw & 0xffffffffffffULL;
   return d;
}

static inline const char *
csf_opcode_name(uint8_t op)
{
   switch (op) {
   case CSF_OP_NOP:             return "NOP";
   case CSF_OP_MOVE48:          return "MOVE48";
   case CSF_OP_MOVE32:          return "MOVE32";
   case CSF_OP_WAIT:            return "WAIT";
   case CSF_OP_FLUSH_CACHE2:    return "FLUSH_CACHE2";
   case CSF_OP_SET_SB_ENTRY:    return "SET_SB_ENTRY";
   case CSF_OP_SET_STATE:       return "SET_STATE";
   case CSF_OP_HEAP_OPERATION:  return "HEAP_OPERATION";
   case CSF_OP_HEAP_SET:        return "HEAP_SET";
   case CSF_OP_CALL:            return "CALL";
   case CSF_OP_JUMP:            return "JUMP";
   case CSF_OP_REQ_RESOURCE:    return "REQ_RESOURCE";
   case CSF_OP_SYNC_SET32:      return "SYNC_SET32";
   case CSF_OP_SYNC_ADD32:      return "SYNC_ADD32";
   case CSF_OP_SYNC_WAIT32:     return "SYNC_WAIT32";
   case CSF_OP_RUN_COMPUTE:     return "RUN_COMPUTE";
   case CSF_OP_RUN_TILER:       return "RUN_TILER";
   case CSF_OP_RUN_IDVS:        return "RUN_IDVS";
   case CSF_OP_RUN_FRAGMENT:    return "RUN_FRAGMENT";
   case CSF_OP_FINISH_TILING:   return "FINISH_TILING";
   case CSF_OP_FINISH_FRAGMENT: return "FINISH_FRAGMENT";
   case CSF_OP_ERROR_BARRIER:   return "ERROR_BARRIER";
   case CSF_OP_SYNC_SET64:      return "SYNC_SET64";
   case CSF_OP_SYNC_ADD64:      return "SYNC_ADD64";
   case CSF_OP_SYNC_WAIT64:     return "SYNC_WAIT64";
   case CSF_OP_LOAD_MULTIPLE:   return "LOAD_MULTIPLE";
   case CSF_OP_STORE_MULTIPLE:  return "STORE_MULTIPLE";
   default:                     return "CSF_UNKNOWN";
   }
}

static inline uint64_t
csf_encode_nop(void)
{
   return 0ULL;
}

static inline uint64_t
csf_encode_move48(uint8_t dst_reg, uint64_t imm48)
{
   return (CSF_OP_MOVE48 << 56) | ((uint64_t)dst_reg << 48) | (imm48 & 0xffffffffffffULL);
}

static inline uint64_t
csf_encode_move32(uint8_t dst_reg, uint32_t imm32)
{
   return (CSF_OP_MOVE32 << 56) | ((uint64_t)dst_reg << 48) | (uint64_t)imm32;
}

static inline uint64_t
csf_encode_wait(uint16_t sb_mask)
{
   return (CSF_OP_WAIT << 56) | ((uint64_t)sb_mask << 16);
}

static inline uint64_t
csf_encode_call(uint8_t addr_reg_pair, uint8_t size_reg)
{
   return (CSF_OP_CALL << 56) | ((uint64_t)addr_reg_pair << 40) | ((uint64_t)size_reg << 32);
}

static inline uint64_t
csf_encode_req_resource(uint32_t res_mask)
{
   return (CSF_OP_REQ_RESOURCE << 56) | (uint64_t)(res_mask & 0xfU);
}

static inline uint64_t
csf_encode_heap_set(uint8_t addr_reg_pair)
{
   return (CSF_OP_HEAP_SET << 56) | ((uint64_t)addr_reg_pair << 40);
}

static inline uint64_t
csf_encode_sync_add64(uint8_t addr_reg, uint8_t val_reg, uint8_t scope, bool notify, uint8_t defer_sb)
{
   return (CSF_OP_SYNC_ADD64 << 56) |
          (1ULL << 52) |
          ((uint64_t)addr_reg << 40) |
          ((uint64_t)val_reg << 32) |
          ((uint64_t)(scope & 0x3U) << 28) |
          (notify ? (1ULL << 24) : 0ULL) |
          ((uint64_t)(defer_sb & 0xfU) << 16);
}

static inline uint64_t
csf_encode_error_barrier(void)
{
   return (CSF_OP_ERROR_BARRIER << 56);
}

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_CSF_INSTR_H */
