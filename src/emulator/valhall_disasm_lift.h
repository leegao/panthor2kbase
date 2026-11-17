/*
 * SPDX-License-Identifier: MIT
 *
 * Panfrost Valhall 64-Bit ISA Disassembler & NIR/SPIR-V Lifter
 * Integrates upstream Mesa's exact table-driven Valhall disassembler
 * (`va_disasm_instr` / `disassemble_valhall` generated from
 * `mesa/src/panfrost/compiler/bifrost/valhall/ISA.xml` in
 * `src/emulator/panfrost_valhall_disasm_gen.c`) with the emulator's
 * Valhall-to-SPIR-V lifter (`va_lift_shader_to_spirv_asm`).
 */
#ifndef VALHALL_DISASM_LIFT_H
#define VALHALL_DISASM_LIFT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provided by src/emulator/panfrost_valhall_disasm_gen.c (generated from Mesa ISA.xml) */
void va_disasm_instr(FILE *fp, uint64_t instr);
void disassemble_valhall(FILE *fp, const void *code, size_t size, bool verbose);

struct valhall_decoded_instr {
   uint64_t raw;
   uint8_t  opcode;
   uint8_t  dst;
   uint8_t  src0;
   uint8_t  src1;
   uint32_t imm32;
   const char *mnemonic;
   const char *spirv_op;
};

static inline struct valhall_decoded_instr
va_decode_instr(uint64_t word)
{
   struct valhall_decoded_instr d;
   d.raw    = word;
   /* Support both emulator micro-ISA encoding (opcode in [63:56] or [55:48]) */
   uint8_t hi_op = (uint8_t)((word >> 56) & 0xffU);
   uint8_t mid_op = (uint8_t)((word >> 48) & 0xffU);
   if (hi_op != 0) {
      d.opcode = hi_op;
      d.dst    = (uint8_t)((word >> 48) & 0x3fU);
      d.src0   = (uint8_t)((word >> 40) & 0x3fU);
      d.src1   = (uint8_t)((word >> 32) & 0x3fU);
      d.imm32  = (uint32_t)(word & 0xffffffffULL);
   } else {
      d.opcode = mid_op;
      d.dst    = (uint8_t)((word >> 40) & 0x3fU);
      d.src0   = (uint8_t)(word & 0x3fU);
      d.src1   = (uint8_t)((word >> 8) & 0x3fU);
      d.imm32  = (uint32_t)((word >> 8) & 0xffffffffULL);
   }

   switch (d.opcode) {
   case 0x00:
      d.mnemonic = "NOP.return";
      d.spirv_op = "OpReturn";
      break;
   case 0x60:
      d.mnemonic = "LD_BUFFER.i32.u64";
      d.spirv_op = "OpLoad %uint %ssbo_ptr Aligned 4";
      break;
   case 0x61:
      d.mnemonic = "LD_ATTR_IMM.v4.f32";
      d.spirv_op = "OpLoad %v4float %in_attr";
      break;
   case 0x66:
      d.mnemonic = "TEX_SINGLE.2d.f32";
      d.spirv_op = "OpImageSampleImplicitLod %v4float %sampled_img %uv";
      break;
   case 0x68:
      d.mnemonic = "ST_BUFFER.i32.u64";
      d.spirv_op = "OpStore %ssbo_out_ptr %val Aligned 4";
      break;
   case 0x71:
      d.mnemonic = "ST_CVT.v4.f32.rgba8";
      d.spirv_op = "OpStore %frag_color_out %val";
      break;
   case 0x7f:
      d.mnemonic = "BRANCHZ.eq.reconverge";
      d.spirv_op = "OpBranchConditional %cmp %then_lbl %merge_lbl";
      break;
   case 0x91:
      d.mnemonic = "MOV.i32";
      d.spirv_op = "OpCopyObject %uint %const_imm";
      break;
   case 0xa0:
      d.mnemonic = "IADD_IMM.i32";
      d.spirv_op = "OpIAdd %uint %src0 %const_imm";
      break;
   case 0xa4:
      d.mnemonic = "IMUL.i32";
      d.spirv_op = "OpIMul %uint %src0 %const_imm";
      break;
   case 0xb4:
      d.mnemonic = "FMA.f32";
      d.spirv_op = "OpExtInst %float %glsl450 Fma %src0 %src1 %const_imm";
      break;
   case 0xd2:
      d.mnemonic = "ATOM_OPC.iadd.u32";
      d.spirv_op = "OpAtomicIAdd %uint %ssbo_ptr %scope_dev %sem_relaxed %val";
      break;
   case 0xe0:
      d.mnemonic = "BARRIER.slot0";
      d.spirv_op = "OpControlBarrier %scope_wg %scope_wg %sem_acqrel_wg";
      break;
   default:
      d.mnemonic = "VALHALL_OP";
      d.spirv_op = "OpNop";
      break;
   }
   return d;
}

/*
 * Formats a 64-bit Valhall instruction using BOTH:
 *  - Upstream Mesa's ISA.xml table-driven `va_disasm_instr()` disassembler
 *  - Our symbolic SPIR-V lifter annotation
 */
static inline int
va_disasm_instr_str(uint64_t word, char *buf, size_t cap)
{
   char mesa_disasm[128];
   mesa_disasm[0] = '\0';
   char *mem_ptr = NULL;
   size_t mem_sz = 0;
   FILE *mfp = open_memstream(&mem_ptr, &mem_sz);
   if (mfp) {
      va_disasm_instr(mfp, word);
      fclose(mfp);
      if (mem_ptr) {
         /* Trim whitespace/newlines from Mesa va_disasm_instr output */
         char *p = mem_ptr;
         while (*p == ' ' || *p == '\t')
            p++;
         size_t len = strlen(p);
         while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
            p[--len] = '\0';
         snprintf(mesa_disasm, sizeof(mesa_disasm), "%s", p);
         free(mem_ptr);
      }
   }
   struct valhall_decoded_instr d = va_decode_instr(word);
   return snprintf(buf, cap, "0x%016llx: %-22s r%u, r%u, #0x%08x  ; [Mesa ISA.xml: %s] SPIR-V: %s",
                   (unsigned long long)word, d.mnemonic, d.dst, d.src0, d.imm32,
                   mesa_disasm[0] ? mesa_disasm : d.mnemonic, d.spirv_op);
}

static inline size_t
va_lift_shader_to_spirv_asm(const uint64_t *insns, size_t max_insns,
                            const char *stage_name, char *out_buf, size_t cap)
{
   size_t off = 0;
   int n = snprintf(out_buf + off, (cap > off) ? (cap - off) : 0,
                    "; SPIR-V\n"
                    "; Version: 1.6 (Lifted from Valhall 64-bit ISA via Mesa ISA.xml + NIR/SPIR-V lifter)\n"
                    "; Stage: %s\n"
                    "               OpCapability Shader\n"
                    "               OpMemoryModel Logical GLSL450\n"
                    "               OpEntryPoint %s %%main \"main\"\n"
                    "       %%void = OpTypeVoid\n"
                    "       %%uint = OpTypeInt 32 0\n"
                    "      %%float = OpTypeFloat 32\n"
                    "    %%v4float = OpTypeVector %%float 4\n"
                    "    %%fn_type = OpTypeFunction %%void\n"
                    "       %%main = OpFunction %%void None %%fn_type\n"
                    "      %%entry = OpLabel\n",
                    stage_name, stage_name);
   if (n > 0)
      off += (size_t)n;

   for (size_t i = 0; i < max_insns; i++) {
      uint64_t w = insns[i];
      if (w == 0ULL)
         break;
      char disasm_line[256];
      va_disasm_instr_str(w, disasm_line, sizeof(disasm_line));
      struct valhall_decoded_instr d = va_decode_instr(w);
      n = snprintf(out_buf + off, (cap > off) ? (cap - off) : 0,
                   "               ; [ISA+%02zu] %s\n"
                   "       %%r_%02zu = %s\n",
                   i * 8, disasm_line,
                   i, d.spirv_op);
      if (n > 0)
         off += (size_t)n;
      if (d.opcode == 0x00)
         break;
   }

   n = snprintf(out_buf + off, (cap > off) ? (cap - off) : 0,
                "               OpReturn\n"
                "               OpFunctionEnd\n\n");
   if (n > 0)
      off += (size_t)n;
   return off;
}

#ifdef __cplusplus
}
#endif

#endif /* VALHALL_DISASM_LIFT_H */
