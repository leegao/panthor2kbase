// External ABI wrappers for complex AArch64 FP/SIMD/Crypto/CRC32 helpers used by LLVM IR modules.
// Simple vector/integer/atomic/bitfield ops are emitted as native LLVM IR instructions and intrinsics;
// operations that depend on FPCR rounding/FZ slow paths, lookup tables, or multi-register structure
// loads/stores call these entry points (which can be inlined via LTO or linked directly).
#pragma once
#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

void rt_fpcr_write_ext(Cpu *c, uint64_t v);

// Generic lane-wise integer SIMD helper
void rt_sl_gen(Cpu *c, int op, int sz, int ne, int d, int szd, int offd, int keep,
               int n, int szn, int offn, int m, int szm, int offm, int bidx, uint64_t bimm);
void rt_sl_addlp(Cpu *c, int u, int acc, int Q, int sz, int d, int n);
void rt_sl_across(Cpu *c, int op, int u, int Q, int sz, int d, int n);
void rt_sl_pmull64(Cpu *c, int part, int d, int n, int m);

// Table lookup & structure loads/stores
void rt_sv_tbl(Cpu *c, int Q, int len, int tbx, int d, int n, int m);
void rt_sl_ldm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea);
void rt_sl_stm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea);
void rt_sl_ldr(Cpu *c, int Q, int sz, int selem, int t, uint64_t ea);
void rt_sl_lds(Cpu *c, int scale, int selem, int idx, int t, uint64_t ea);
void rt_sl_sts(Cpu *c, int scale, int selem, int idx, int t, uint64_t ea);

// Crypto & CRC32
void rt_sv_aes(Cpu *c, int op, int d, int n);
void rt_sv_sha1hash(Cpu *c, int op, int d, int n, int m);
void rt_sv_sha256hash(Cpu *c, int part1, int d, int n, int m);
void rt_sv_sha_misc(Cpu *c, int op, int d, int n, int m);
uint32_t rt_crc32(uint32_t acc, uint64_t v, int bytes, int is_crc32c);

// Scalar FP & conversions
void rt_fp_op_s(Cpu *c, int op, int d, int n, int m);
void rt_fp_op_d(Cpu *c, int op, int d, int n, int m);
void rt_fp_fma_s(Cpu *c, int na, int nn, int d, int n, int m, int a);
void rt_fp_fma_d(Cpu *c, int na, int nn, int d, int n, int m, int a);
void rt_fp_dp1(Cpu *c, int t, int opc, int d, int n);
uint64_t rt_fp_cmp_s(Cpu *c, int n, int m, int zero);
uint64_t rt_fp_cmp_d(Cpu *c, int n, int m, int zero);
uint64_t rt_fp_toint(Cpu *c, int is_double, int n, int rm, int fbits, int ib, int uns);
void rt_fp_fromint(Cpu *c, int is_double, int d, uint64_t v, int ib, int uns, int fbits);
void rt_fp_recpe(Cpu *c, int is_double, int d, int n, int is_rsqrte);
void rt_fp_recpx(Cpu *c, int is_double, int d, int n);

// Vector FP operations
void rt_vec_fop(Cpu *c, int is_double, int op, int Q, int d, int n, int m, int a, int neg_n);
void rt_vec_fpair(Cpu *c, int is_double, int op, int Q, int d, int n, int m);
void rt_vec_fcmp(Cpu *c, int is_double, int cop, int Q, int scalar, int d, int n, int m, int zero, int swap);
void rt_vec_frint(Cpu *c, int is_double, int Q, int rm, int d, int n);
void rt_vec_fcvt_int(Cpu *c, int is_double, int to_int, int Q, int scalar, int rm, int fbits, int uns, int d, int n);
void rt_vec_frecpe(Cpu *c, int is_double, int Q, int is_rsqrte, int d, int n);
void rt_vec_fcvtn(Cpu *c, int sz, int part, int odd, int d, int n);
void rt_vec_fcvtl(Cpu *c, int sz, int part, int d, int n);
void rt_sv_fred_s(Cpu *c, int op, int d, int n);
void rt_scalar_fpair(Cpu *c, int is_double, int op, int d, int n);
void rt_indexed_fp(Cpu *c, int is_double, int Q, int scalar, int opc, int U, int d, int n, int m, int idx);

#ifdef __cplusplus
}
#endif
