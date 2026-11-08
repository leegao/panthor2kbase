// Implementation of external C ABI entry points for AArch64 FP/SIMD/Crypto/CRC32 helpers used by LLVM IR modules.
#include "simd_rt.h"
#include "simd.h"

void rt_fpcr_write_ext(Cpu *c, uint64_t v) { rt_fpcr_write(c, v); }

void rt_sl_gen(Cpu *c, int op, int sz, int ne, int d, int szd, int offd, int keep,
               int n, int szn, int offn, int m, int szm, int offm, int bidx, uint64_t bimm) {
    sl_gen(c, op, sz, ne, d, szd, offd, keep, n, szn, offn, m, szm, offm, bidx, bimm);
}

void rt_sl_addlp(Cpu *c, int u, int acc, int Q, int sz, int d, int n) { sl_addlp(c, u, acc, Q, sz, d, n); }
void rt_sl_across(Cpu *c, int op, int u, int Q, int sz, int d, int n) { sl_across(c, op, u, Q, sz, d, n); }
void rt_sl_pmull64(Cpu *c, int part, int d, int n, int m) { sl_pmull64(c, part, d, n, m); }

void rt_sv_tbl(Cpu *c, int Q, int len, int tbx, int d, int n, int m) { sv_tbl(c, Q, len, tbx, d, n, m); }
void rt_sl_ldm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea) { sl_ldm(c, Q, sz, selem, rpt, t, ea); }
void rt_sl_stm(Cpu *c, int Q, int sz, int selem, int rpt, int t, uint64_t ea) { sl_stm(c, Q, sz, selem, rpt, t, ea); }
void rt_sl_ldr(Cpu *c, int Q, int sz, int selem, int t, uint64_t ea) { sl_ldr(c, Q, sz, selem, t, ea); }
void rt_sl_lds(Cpu *c, int scale, int selem, int idx, int t, uint64_t ea) { sl_lds(c, scale, selem, idx, t, ea); }
void rt_sl_sts(Cpu *c, int scale, int selem, int idx, int t, uint64_t ea) { sl_sts(c, scale, selem, idx, t, ea); }

void rt_sv_aes(Cpu *c, int op, int d, int n) { sv_aes(c, op, d, n); }
void rt_sv_sha1hash(Cpu *c, int op, int d, int n, int m) { sv_sha1hash(c, op, d, n, m); }
void rt_sv_sha256hash(Cpu *c, int part1, int d, int n, int m) { sv_sha256hash(c, part1, d, n, m); }
void rt_sv_sha_misc(Cpu *c, int op, int d, int n, int m) { sv_sha_misc(c, op, d, n, m); }

uint32_t rt_crc32(uint32_t acc, uint64_t v, int bytes, int is_crc32c) {
    return is_crc32c ? CRC32C(acc, v, bytes) : CRC32(acc, v, bytes);
}

void rt_fp_op_s(Cpu *c, int op, int d, int n, int m) { fp_op_s(c, op, d, n, m); }
void rt_fp_op_d(Cpu *c, int op, int d, int n, int m) { fp_op_d(c, op, d, n, m); }
void rt_fp_fma_s(Cpu *c, int na, int nn, int d, int n, int m, int a) { fp_fma_s(c, na, nn, d, n, m, a); }
void rt_fp_fma_d(Cpu *c, int na, int nn, int d, int n, int m, int a) { fp_fma_d(c, na, nn, d, n, m, a); }

void rt_fp_dp1(Cpu *c, int t, int opc, int d, int n) {
    if (t == 3) {
        if (opc == 4) FP_SETS(d, simd_h2s(c, VS16(n)));
        else if (opc == 5) FP_SETD(d, simd_h2d(c, VS16(n)));
        return;
    }
    if (opc == 0) { c->v[d] = t ? VS64(n) : VS32(n); return; }
    if (opc == 1) { c->v[d] = t ? (VS64(n) & ~0x8000000000000000ull) : (VS32(n) & ~0x80000000u); return; }
    if (opc == 2) { c->v[d] = t ? (VS64(n) ^ 0x8000000000000000ull) : (VS32(n) ^ 0x80000000u); return; }
    if (opc == 3) {
        if (t) fp_op_d(c, FOP_SQRT, d, n, n);
        else fp_op_s(c, FOP_SQRT, d, n, n);
        return;
    }
    if (opc == 4 || opc == 5 || opc == 7) {
        int to = opc == 4 ? 0 : opc == 5 ? 1 : 3;
        if (to == 3) c->v[d] = t ? simd_dd2h(c, VSF64(n)) : simd_s2h(c, VSF32(n));
        else if (to == 1) FP_SETD(d, simd_s2d(c, VSF32(n)));
        else FP_SETS(d, simd_d2s(c, VSF64(n)));
        return;
    }
    int rm = opc == 8 ? 0 : opc == 9 ? 1 : opc == 10 ? 2 : opc == 11 ? 3 : opc == 12 ? 4 : opc == 14 ? 5 : 6;
    if (t) FP_SETD(d, frint_d(c, VSF64(n), rm));
    else FP_SETS(d, frint_s(c, VSF32(n), rm));
}

uint64_t rt_fp_cmp_s(Cpu *c, int n, int m, int zero) {
    return cmp_s(c, VSF32(n), zero ? 0.0f : VSF32(m));
}

uint64_t rt_fp_cmp_d(Cpu *c, int n, int m, int zero) {
    return cmp_d(c, VSF64(n), zero ? 0.0 : VSF64(m));
}

uint64_t rt_fp_toint(Cpu *c, int is_double, int n, int rm, int fbits, int ib, int uns) {
    return is_double ? toint_d(c, VSF64(n), rm, fbits, ib, uns)
                     : toint_s(c, VSF32(n), rm, fbits, ib, uns);
}

void rt_fp_fromint(Cpu *c, int is_double, int d, uint64_t v, int ib, int uns, int fbits) {
    if (is_double) FP_SETD(d, fromint_d(c, v, ib, uns, fbits));
    else FP_SETS(d, fromint_s(c, v, ib, uns, fbits));
}

void rt_fp_recpe(Cpu *c, int is_double, int d, int n, int is_rsqrte) {
    if (is_double) FP_SETD(d, is_rsqrte ? rsqrte_d(c, VSF64(n)) : recpe_d(c, VSF64(n)));
    else FP_SETS(d, is_rsqrte ? rsqrte_s(c, VSF32(n)) : recpe_s(c, VSF32(n)));
}

void rt_fp_recpx(Cpu *c, int is_double, int d, int n) {
    if (is_double) FP_SETD(d, recpx_d(c, VSF64(n)));
    else FP_SETS(d, recpx_s(c, VSF32(n)));
}

void rt_vec_fop(Cpu *c, int is_double, int op, int Q, int d, int n, int m, int a, int neg_n) {
    if (is_double) {
        vf64 vn = VQ(vf64, Q, n), vm = VQ(vf64, Q, m), va = a >= 0 ? VQ(vf64, Q, a) : (vf64){};
        if (neg_n) vn = vneg_d(vn);
        if (op == FOP_MADD) SV_FOP_D(FOP_MADD, Q, d, va, vn, vm);
        else SV_FOP_D(op, Q, d, vn, vm, va);
    } else {
        vf32 vn = VQ(vf32, Q, n), vm = VQ(vf32, Q, m), va = a >= 0 ? VQ(vf32, Q, a) : (vf32){};
        if (neg_n) vn = vneg_s(vn);
        if (op == FOP_MADD) SV_FOP_S(FOP_MADD, Q, d, va, vn, vm);
        else SV_FOP_S(op, Q, d, vn, vm, va);
    }
}

void rt_vec_fpair(Cpu *c, int is_double, int op, int Q, int d, int n, int m) {
    if (is_double) {
        vf64 A = VQ(vf64, Q, n), B = VQ(vf64, Q, m);
        vf64 L = (vf64){A[0], B[0]}, R = (vf64){A[1], B[1]};
        SV_FOP_D(op, Q, d, L, R, (vf64){});
    } else {
        vf32 A = VQ(vf32, Q, n), B = VQ(vf32, Q, m);
        vf32 L = Q ? (vf32){A[0], A[2], B[0], B[2]} : (vf32){A[0], B[0], A[2], A[2]};
        vf32 R = Q ? (vf32){A[1], A[3], B[1], B[3]} : (vf32){A[1], B[1], A[2], A[2]};
        SV_FOP_S(op, Q, d, L, R, (vf32){});
    }
}

void rt_vec_fcmp(Cpu *c, int is_double, int cop, int Q, int scalar, int d, int n, int m, int zero, int swap) {
    if (is_double) {
        vf64 a = scalar ? (vf64){VSF64(n), 0} : VQ(vf64, Q, n);
        vf64 b = zero ? (vf64){} : (scalar ? (vf64){VSF64(m), 0} : VQ(vf64, Q, m));
        if (swap) { vf64 t = a; a = b; b = t; }
        vu64 r = vcmp_d(c, cop, a, b);
        if (scalar) c->v[d] = (uint64_t)r[0];
        else VW(vu64, Q, d, r);
    } else {
        vf32 a = scalar ? (vf32){VSF32(n), 0, 0, 0} : VQ(vf32, Q, n);
        vf32 b = zero ? (vf32){} : (scalar ? (vf32){VSF32(m), 0, 0, 0} : VQ(vf32, Q, m));
        if (swap) { vf32 t = a; a = b; b = t; }
        vu32 r = vcmp_s(c, cop, a, b);
        if (scalar) c->v[d] = (uint32_t)r[0];
        else VW(vu32, Q, d, r);
    }
}

void rt_vec_frint(Cpu *c, int is_double, int Q, int rm, int d, int n) {
    if (is_double) VW(vf64, Q, d, vfrint_d(c, VQ(vf64, Q, n), rm));
    else VW(vf32, Q, d, vfrint_s(c, VQ(vf32, Q, n), rm));
}

void rt_vec_fcvt_int(Cpu *c, int is_double, int to_int, int Q, int scalar, int rm, int fbits, int uns, int d, int n) {
    if (to_int) {
        if (scalar) {
            if (is_double) c->v[d] = (uint64_t)toint_d(c, VSF64(n), rm, fbits, 64, uns);
            else c->v[d] = (uint32_t)toint_s(c, VSF32(n), rm, fbits, 32, uns);
        } else {
            if (is_double) VW(vu64, Q, d, vtoint_d(c, VQ(vf64, Q, n), rm, fbits, uns));
            else VW(vu32, Q, d, vtoint_s(c, VQ(vf32, Q, n), rm, fbits, uns));
        }
    } else {
        if (scalar) {
            if (is_double) FP_SETD(d, fromint_d(c, VS64(n), 64, uns, fbits));
            else FP_SETS(d, fromint_s(c, VS32(n), 32, uns, fbits));
        } else {
            if (is_double) VW(vf64, Q, d, vfromint_d(c, VREG(vu64, n), fbits, uns));
            else VW(vf32, Q, d, vfromint_s(c, VREG(vu32, n), fbits, uns));
        }
    }
}

void rt_vec_frecpe(Cpu *c, int is_double, int Q, int is_rsqrte, int d, int n) {
    if (is_double) VW(vf64, Q, d, is_rsqrte ? vrsqrte_d(c, VQ(vf64, Q, n)) : vrecpe_d(c, VQ(vf64, Q, n)));
    else VW(vf32, Q, d, is_rsqrte ? vrsqrte_s(c, VQ(vf32, Q, n)) : vrecpe_s(c, VQ(vf32, Q, n)));
}

void rt_vec_fcvtn(Cpu *c, int sz, int part, int odd, int d, int n) {
    if (odd && sz == 1 && part == -1) { FP_SETS(d, simd_d2s_odd(c, VSF64(n))); return; }
    sv_fcvtn(c, sz, part, odd, d, n);
}

void rt_vec_fcvtl(Cpu *c, int sz, int part, int d, int n) { sv_fcvtl(c, sz, part, d, n); }
void rt_sv_fred_s(Cpu *c, int op, int d, int n) { sv_fred_s(c, op, d, n); }

void rt_scalar_fpair(Cpu *c, int is_double, int op, int d, int n) {
    if (is_double) FP_SETD(d, pair_d(c, op, VREG(vf64, n)[0], VREG(vf64, n)[1]));
    else FP_SETS(d, pair_s(c, op, VREG(vf32, n)[0], VREG(vf32, n)[1]));
}

void rt_indexed_fp(Cpu *c, int is_double, int Q, int scalar, int opc, int U, int d, int n, int m, int idx) {
    if (is_double) {
        if (scalar) {
            double el = VREG(vf64, m)[idx];
            if (opc == 9) FP_SETD(d, op_d(c, U ? FOP_MULX : FOP_MUL, VSF64(n), el, 0));
            else FP_SETD(d, op_d(c, FOP_MADD, VSF64(d), opc == 1 ? VSF64(n) : neg_d(VSF64(n)), el));
        } else {
            vf64 el = vsplat_d(VREG(vf64, m), idx, Q);
            if (opc == 9) SV_FOP_D(U ? FOP_MULX : FOP_MUL, Q, d, VQ(vf64, Q, n), el, (vf64){});
            else SV_FOP_D(FOP_MADD, Q, d, VQ(vf64, Q, d), opc == 1 ? VQ(vf64, Q, n) : vneg_d(VQ(vf64, Q, n)), el);
        }
    } else {
        if (scalar) {
            float el = VREG(vf32, m)[idx];
            if (opc == 9) FP_SETS(d, op_s(c, U ? FOP_MULX : FOP_MUL, VSF32(n), el, 0));
            else FP_SETS(d, op_s(c, FOP_MADD, VSF32(d), opc == 1 ? VSF32(n) : neg_s(VSF32(n)), el));
        } else {
            vf32 el = vsplat_s(VREG(vf32, m), idx, Q);
            if (opc == 9) SV_FOP_S(U ? FOP_MULX : FOP_MUL, Q, d, VQ(vf32, Q, n), el, (vf32){});
            else SV_FOP_S(FOP_MADD, Q, d, VQ(vf32, Q, d), opc == 1 ? VQ(vf32, Q, n) : vneg_s(VQ(vf32, Q, n)), el);
        }
    }
}
