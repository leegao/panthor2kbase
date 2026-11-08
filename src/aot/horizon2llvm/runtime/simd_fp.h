// FP helpers for one precision; included twice by simd.h (F = s/float, d/double). No include guard.
// Scalar entry points take/return T, vector ones VT (all lanes; callers zero the unused half for Q=0).

#define FN(x) SIMD_CAT(x, F)
#define SGN ((U)1 << (NB - 1))
#define EXPM ((((U)1 << EB) - 1) << MB)
#define QB ((U)1 << (MB - 1))
#define DNAN (EXPM | QB)
#define MINN ((U)1 << MB) // smallest normal
#define VK(v) ((VT){} + (T)(v))
#define K(v) ((T)(v))
#if defined(__clang__)
#define EL_ABS(x)        __builtin_elementwise_abs(x)
#define EL_SQRT(x)       __builtin_elementwise_sqrt(x)
#define EL_FMA(a, b, c)  __builtin_elementwise_fma(a, b, c)
#define EL_RINT(x)       __builtin_elementwise_rint(x)
#define EL_CEIL(x)       __builtin_elementwise_ceil(x)
#define EL_FLOOR(x)      __builtin_elementwise_floor(x)
#define EL_TRUNC(x)      __builtin_elementwise_trunc(x)
#define EL_ROUND(x)      __builtin_elementwise_round(x)
#define VEL_ABS(x)       __builtin_elementwise_abs(x)
#define VEL_SQRT(x)      __builtin_elementwise_sqrt(x)
#define VEL_FMA(a, b, c) __builtin_elementwise_fma(a, b, c)
#define VEL_RINT(x)      __builtin_elementwise_rint(x)
#define VEL_CEIL(x)      __builtin_elementwise_ceil(x)
#define VEL_FLOOR(x)     __builtin_elementwise_floor(x)
#define VEL_TRUNC(x)     __builtin_elementwise_trunc(x)
#define VEL_ROUND(x)     __builtin_elementwise_round(x)
#define VRED_OR(v)       __builtin_reduce_or(v)
#else
SI T FN(el_abs)(T x) { return sizeof(T) == 4 ? __builtin_fabsf((float)x) : __builtin_fabs((double)x); }
SI T FN(el_sqrt)(T x) { return sizeof(T) == 4 ? __builtin_sqrtf((float)x) : __builtin_sqrt((double)x); }
SI T FN(el_fma)(T a, T b, T c) { return sizeof(T) == 4 ? __builtin_fmaf((float)a, (float)b, (float)c) : __builtin_fma((double)a, (double)b, (double)c); }
SI T FN(el_rint)(T x) { return sizeof(T) == 4 ? __builtin_rintf((float)x) : __builtin_rint((double)x); }
SI T FN(el_ceil)(T x) { return sizeof(T) == 4 ? __builtin_ceilf((float)x) : __builtin_ceil((double)x); }
SI T FN(el_floor)(T x) { return sizeof(T) == 4 ? __builtin_floorf((float)x) : __builtin_floor((double)x); }
SI T FN(el_trunc)(T x) { return sizeof(T) == 4 ? __builtin_truncf((float)x) : __builtin_trunc((double)x); }
SI T FN(el_round)(T x) { return sizeof(T) == 4 ? __builtin_roundf((float)x) : __builtin_round((double)x); }
SI VT FN(vel_abs)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_abs)(x[i]); return r; }
SI VT FN(vel_sqrt)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_sqrt)(x[i]); return r; }
SI VT FN(vel_fma)(VT a, VT b, VT c) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_fma)(a[i], b[i], c[i]); return r; }
SI VT FN(vel_rint)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_rint)(x[i]); return r; }
SI VT FN(vel_ceil)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_ceil)(x[i]); return r; }
SI VT FN(vel_floor)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_floor)(x[i]); return r; }
SI VT FN(vel_trunc)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_trunc)(x[i]); return r; }
SI VT FN(vel_round)(VT x) { VT r; for (int i = 0; i < NL; ++i) r[i] = FN(el_round)(x[i]); return r; }
SI int FN(vred_or)(VTS v) { U acc = 0; for (int i = 0; i < NL; ++i) acc |= (U)v[i]; return acc != 0; }
#undef EL_ABS
#undef EL_SQRT
#undef EL_FMA
#undef EL_RINT
#undef EL_CEIL
#undef EL_FLOOR
#undef EL_TRUNC
#undef EL_ROUND
#undef VEL_ABS
#undef VEL_SQRT
#undef VEL_FMA
#undef VEL_RINT
#undef VEL_CEIL
#undef VEL_FLOOR
#undef VEL_TRUNC
#undef VEL_ROUND
#undef VRED_OR
#define EL_ABS(x)        FN(el_abs)(x)
#define EL_SQRT(x)       FN(el_sqrt)(x)
#define EL_FMA(a, b, c)  FN(el_fma)(a, b, c)
#define EL_RINT(x)       FN(el_rint)(x)
#define EL_CEIL(x)       FN(el_ceil)(x)
#define EL_FLOOR(x)      FN(el_floor)(x)
#define EL_TRUNC(x)      FN(el_trunc)(x)
#define EL_ROUND(x)      FN(el_round)(x)
#define VEL_ABS(x)       FN(vel_abs)(x)
#define VEL_SQRT(x)      FN(vel_sqrt)(x)
#define VEL_FMA(a, b, c) FN(vel_fma)(a, b, c)
#define VEL_RINT(x)      FN(vel_rint)(x)
#define VEL_CEIL(x)      FN(vel_ceil)(x)
#define VEL_FLOOR(x)     FN(vel_floor)(x)
#define VEL_TRUNC(x)     FN(vel_trunc)(x)
#define VEL_ROUND(x)     FN(vel_round)(x)
#define VRED_OR(v)       FN(vred_or)(v)
#endif

SI U FN(fb)(T x) { U u; memcpy(&u, &x, sizeof u); return u; }
SI T FN(fv)(U u) { T x; memcpy(&x, &u, sizeof x); return x; }
SI int FN(isnan)(U u) { return (u & ~SGN) > EXPM; }
SI int FN(issnan)(U u) { return FN(isnan)(u) && !(u & QB); }
SI int FN(isqnan)(U u) { return FN(isnan)(u) && (u & QB); }
SI int FN(isinf)(U u) { return (u & ~SGN) == EXPM; }
SI int FN(iszero)(U u) { return !(u & ~SGN); }
SI int FN(isden)(U u) { return !(u & EXPM) && (u & ~SGN); }
SI T FN(flush)(T x) { U u = FN(fb)(x); return FN(isden)(u) ? FN(fv)(u & SGN) : x; }
SI T FN(fz)(Cpu *c, T x) { return __builtin_expect(c->fpcr & FPCR_FZ, 0) ? FN(flush)(x) : x; }
SI T FN(neg)(T x) { return FN(fv)(FN(fb)(x) ^ SGN); }

// FPProcessNaN(s): first signalling NaN (quietened), else first quiet NaN, else (invalid op) default NaN
SN T FN(nan1)(Cpu *c, T a) {
    U x = FN(fb)(a);
    if (!(c->fpcr & FPCR_DN) && FN(isnan)(x)) return FN(fv)(x | QB);
    return FN(fv)(DNAN);
}
SN T FN(nan2)(Cpu *c, T a, T b) {
    U x = FN(fb)(a), y = FN(fb)(b);
    if (!(c->fpcr & FPCR_DN)) {
        if (FN(issnan)(x)) return FN(fv)(x | QB);
        if (FN(issnan)(y)) return FN(fv)(y | QB);
        if (FN(isnan)(x)) return a;
        if (FN(isnan)(y)) return b;
    }
    return FN(fv)(DNAN);
}
SN T FN(nan3)(Cpu *c, T a, T b, T d) {
    U x = FN(fb)(a), y = FN(fb)(b), z = FN(fb)(d);
    if (!(c->fpcr & FPCR_DN)) {
        if (FN(issnan)(x)) return FN(fv)(x | QB);
        if (FN(issnan)(y)) return FN(fv)(y | QB);
        if (FN(issnan)(z)) return FN(fv)(z | QB);
        if (FN(isnan)(x)) return a;
        if (FN(isnan)(y)) return b;
        if (FN(isnan)(z)) return d;
    }
    return FN(fv)(DNAN);
}

// The arithmetic itself, for scalars (KK = K) and vectors (KK = VK). MADD: a + b*d (a = addend).
// RECPS/RSQRTS get their first operand already negated (FPNeg happens before NaN processing).
#define FCALC(op, a, b, d, KK)                                                                       \
    ((op) == FOP_ADD ? (a) + (b) : (op) == FOP_SUB ? (a) - (b)                                     \
   : (op) == FOP_MUL || (op) == FOP_MULX ? (a) * (b) : (op) == FOP_NMUL ? -((a) * (b))              \
   : (op) == FOP_DIV ? (a) / (b) : (op) == FOP_ABD ? EL_ABS((a) - (b))                             \
   : (op) == FOP_SQRT ? EL_SQRT(a)                                                                 \
   : (op) == FOP_MADD ? EL_FMA(b, d, a)                                                            \
   : (op) == FOP_RECPS ? EL_FMA(a, b, KK(2))                                                       \
   : (op) == FOP_RSQRTS ? EL_FMA(a, b, KK(3)) * KK(0.5) : (a))

SI VT FN(vfcalc)(int op, VT a, VT b, VT d) {
    switch (op) {
    case FOP_ADD:    return a + b;
    case FOP_SUB:    return a - b;
    case FOP_MUL:
    case FOP_MULX:   return a * b;
    case FOP_NMUL:   return -(a * b);
    case FOP_DIV:    return a / b;
    case FOP_ABD:    return VEL_ABS(a - b);
    case FOP_SQRT:   return VEL_SQRT(a);
    case FOP_MADD:   return VEL_FMA(b, d, a);
    case FOP_RECPS:  return VEL_FMA(a, b, VK(2));
    case FOP_RSQRTS: return VEL_FMA(a, b, VK(3)) * VK(0.5);
    default:         return a;
    }
}

// Lanes whose fast result needs fixing: NaN results (NaN operand or invalid op), RSQRTS overflow.
#define FBAD(op, r) ((op) == FOP_RSQRTS ? ((r) != (r)) | (EL_ABS(r) == K(INFINITY)) : (r) != (r))
SI VTS FN(vfbad)(int op, VT r) {
    if (op == FOP_RSQRTS) return (r != r) | (VEL_ABS(r) == VK(INFINITY));
    return r != r;
}
#define VFCALC(op, a, b, d, KK) FN(vfcalc)((op), (a), (b), (d))
#define VFBAD(op, r) FN(vfbad)((op), (r))

// (3 + a*b) / 2 with one rounding (a already negated): halve a normal operand first, so a product beyond
// the format's range (which the fast path rounds to inf, or to the largest normal when rounding down) is exact.
SN T FN(rsqrts)(T a, T b) {
    if (EL_ABS(a) >= FN(fv)(2 * MINN)) return EL_FMA(a * K(0.5), b, K(1.5));
    if (EL_ABS(b) >= FN(fv)(2 * MINN)) return EL_FMA(a, b * K(0.5), K(1.5));
    return EL_FMA(a, b, K(3)) * K(0.5);
}

// ARM result for a lane flagged by FBAD (operands already flushed).
SN T FN(fix)(Cpu *c, int op, T a, T b, T d) {
    U x = FN(fb)(a), y = FN(fb)(b), z = FN(fb)(d);
    int nan = FN(isnan)(x) || FN(isnan)(y);
    switch (op) {
    case FOP_SQRT: return FN(nan1)(c, a); // NaN in, or negative -> default NaN
    case FOP_MADD:
        if (FN(isnan)(x) || FN(isnan)(y) || FN(isnan)(z)) {
            if (FN(isqnan)(x) && ((FN(isinf)(y) && FN(iszero)(z)) || (FN(iszero)(y) && FN(isinf)(z))))
                return FN(fv)(DNAN);
            return FN(nan3)(c, a, b, d);
        }
        return FN(fv)(DNAN);
    case FOP_MULX:
        if (!nan) return FN(fv)(((x ^ y) & SGN) | FN(fb)(K(2)));
        break;
    case FOP_RECPS:
        if (!nan) return K(2);
        break;
    case FOP_RSQRTS:
        if (!nan) {
            if ((FN(isinf)(x) && FN(iszero)(y)) || (FN(iszero)(x) && FN(isinf)(y))) return K(1.5);
            return FN(rsqrts)(a, b);
        }
        break;
    case FOP_NMUL: return FN(neg)(FN(nan2)(c, a, b));
    case FOP_ABD: return FN(fv)(FN(fb)(FN(nan2)(c, a, b)) & ~SGN);
    }
    return FN(nan2)(c, a, b);
}

// FMAX/FMIN/FMAXNM/FMINNM (operands already flushed)
SI T FN(minmax)(Cpu *c, int op, T a, T b) {
    U x = FN(fb)(a), y = FN(fb)(b);
    if (__builtin_expect(FN(isnan)(x) || FN(isnan)(y), 0)) {
        if (op == FOP_MAXNM || op == FOP_MINNM) {
            if (FN(isqnan)(x) && !FN(isnan)(y)) return b;
            if (FN(isqnan)(y) && !FN(isnan)(x)) return a;
        }
        return FN(nan2)(c, a, b);
    }
    if (op == FOP_MAX || op == FOP_MAXNM) return a == b ? FN(fv)(x & y) : a > b ? a : b;
    return a == b ? FN(fv)(x | y) : a < b ? a : b;
}
#define FMINMAX(op) ((op) >= FOP_MAX && (op) <= FOP_MINNM)

// FPCR.FZ / RMode path: flush inputs, round in the FPCR mode, flush tiny results (tininess is detected
// before rounding on ARM: a result rounded up to the smallest normal is recomputed toward zero).
SN T FN(slow)(Cpu *c, int op, T a, T b, T d) {
#pragma STDC FENV_ACCESS ON
    uint64_t fpcr = c->fpcr;
    if (fpcr & FPCR_FZ) a = FN(flush)(a), b = FN(flush)(b), d = FN(flush)(d);
    if (FMINMAX(op)) return FN(minmax)(c, op, a, b);
    // [TOTK] The game runs with FPCR.FZ set, so every FP op takes this path. The host thread is always in
    // round-to-nearest (only this code changes the mode, and it puts nearest back): with RMode nearest no switch is
    // needed. fegetround / fesetround cost ~70% of the game's CPU time otherwise (mingw sets both x87 and SSE state).
    int mode = simd_fe_mode(fpcr), changed = mode != FE_TONEAREST;
    if (changed) fesetround(mode);
    volatile T va = a, vb = b, vd = d;
    volatile T vr = op == FOP_RSQRTS ? FN(rsqrts)(va, vb) : FCALC(op, va, vb, vd, K);
    T r = vr;
    if (FBAD(op, r)) vr = FN(fix)(c, op, a, b, d), r = vr;
    if ((fpcr & FPCR_FZ) && (FN(fb)(r) & ~SGN) <= MINN && !FN(iszero)(FN(fb)(r))) {
        if ((FN(fb)(r) & ~SGN) == MINN) {
            fesetround(FE_TOWARDZERO);
            changed = 1;
            volatile T vz = FCALC(op, va, vb, vd, K);
            if ((FN(fb)(vz) & ~SGN) < MINN) r = FN(fv)(FN(fb)(r) & SGN);
        } else {
            r = FN(fv)(FN(fb)(r) & SGN);
        }
    }
    if (changed) fesetround(FE_TONEAREST);
    return r;
}

// Scalar FP op (constant op folds the selection). FZ alone is the host's own mode (rt_fpcr_write: MXCSR DAZ / FTZ):
// only another rounding mode takes the slow path.
SI T FN(op)(Cpu *c, int op, T a, T b, T d) {
    if (__builtin_expect(c->fpcr & FPCR_RM, 0)) return FN(slow)(c, op, a, b, d);
    if (FMINMAX(op)) return FN(minmax)(c, op, FN(fz)(c, a), FN(fz)(c, b));
    T r = FCALC(op, a, b, d, K);
    if (__builtin_expect(FBAD(op, r), 0)) r = FN(fix)(c, op, FN(fz)(c, a), FN(fz)(c, b), FN(fz)(c, d));
    return r;
}

SN VT FN(vslow)(Cpu *c, int op, VT a, VT b, VT d) {
    VT r;
    for (int i = 0; i < NL; i++) r[i] = FN(op)(c, op, a[i], b[i], d[i]);
    return r;
}

SI VTU FN(vsel)(VTS m, VTU x, VTU y) { return (x & (VTU)m) | (y & ~(VTU)m); }

// Vector FP op: native lanes, any NaN (or FPCR.FZ/RMode) reruns all lanes through the scalar path.
SI VT FN(vfz)(Cpu *c, VT a);
SI VT FN(vop)(Cpu *c, int op, VT a, VT b, VT d) {
    if (__builtin_expect(c->fpcr & FPCR_RM, 0)) return FN(vslow)(c, op, a, b, d);
    VT r;
    if (FMINMAX(op)) {
        a = FN(vfz)(c, a), b = FN(vfz)(c, b);   // the result is an operand's bits: flushed as ARM's FZ would
        if (__builtin_expect(VRED_OR((a != a) | (b != b)) != 0, 0)) return FN(vslow)(c, op, a, b, d);
        VTU x = (VTU)a, y = (VTU)b;
        if (op == FOP_MAX || op == FOP_MAXNM)
            r = (VT)FN(vsel)(a > b, x, FN(vsel)(a == b, x & y, y));
        else
            r = (VT)FN(vsel)(a < b, x, FN(vsel)(a == b, x | y, y));
        return r;
    }
    r = VFCALC(op, a, b, d, VK);
    if (__builtin_expect(VRED_OR(VFBAD(op, r)) != 0, 0)) return FN(vslow)(c, op, a, b, d);
    return r;
}

SI VT FN(vfz)(Cpu *c, VT a) {
    if (__builtin_expect(c->fpcr & FPCR_FZ, 0)) {
        VTU u = (VTU)a, mag = u & ~((VTU){} + SGN);
        VTS den = (mag != 0) & ((u & EXPM) == 0);
        a = (VT)FN(vsel)(den, u & SGN, u);
    }
    return a;
}

// ---- compares
SI uint64_t FN(cmp)(Cpu *c, T a, T b) { // FCMP NZCV
    a = FN(fz)(c, a), b = FN(fz)(c, b);
    return a == b ? 0x60000000ull : a < b ? 0x80000000ull : a > b ? 0x20000000ull : 0x30000000ull;
}
// FCMEQ/FCMGE/FCMGT/FACGE/FACGT (op 0..4)
SI VTU FN(vcmp)(Cpu *c, int op, VT a, VT b) {
    a = FN(vfz)(c, a), b = FN(vfz)(c, b);
    if (op >= 3) { a = VEL_ABS(a); b = VEL_ABS(b); }
    switch (op) {
    case 0: return (VTU)(a == b);
    case 1: case 3: return (VTU)(a >= b);
    default: return (VTU)(a > b);
    }
}

// ---- rounding to integral (FRINT*): rm 0 N, 1 P, 2 M, 3 Z, 4 A; 5 X / 6 I = FPCR mode
// (rint = ties-to-even: the host stays in round-to-nearest outside the slow paths; no roundeven in MinGW's CRT)
SI T FN(rnd)(T x, int rm) {
    switch (rm) {
    case 0: return EL_RINT(x);
    case 1: return EL_CEIL(x);
    case 2: return EL_FLOOR(x);
    case 3: return EL_TRUNC(x);
    default: return EL_ROUND(x);
    }
}
SI T FN(frint)(Cpu *c, T x, int rm) {
    x = FN(fz)(c, x);
    if (rm >= 5) rm = (int)((c->fpcr >> 22) & 3);
    T r = FN(rnd)(x, rm);
    if (__builtin_expect(r != r, 0)) r = FN(nan1)(c, x);
    return r;
}
SI VT FN(vfrint)(Cpu *c, VT x, int rm) {
    x = FN(vfz)(c, x);
    if (rm >= 5) rm = (int)((c->fpcr >> 22) & 3);
    VT r;
    switch (rm) {
    case 0: r = VEL_RINT(x); break;
    case 1: r = VEL_CEIL(x); break;
    case 2: r = VEL_FLOOR(x); break;
    case 3: r = VEL_TRUNC(x); break;
    default: r = VEL_ROUND(x); break;
    }
    if (__builtin_expect(VRED_OR(r != r) != 0, 0))
        for (int i = 0; i < NL; i++)
            if (r[i] != r[i]) r[i] = FN(nan1)(c, x[i]);
    return r;
}

// ---- FP -> integer (FCVT[NPMZA][SU], fixed point): saturating, NaN -> 0. ib = 32/64.
SI uint64_t FN(toint)(Cpu *c, T x, int rm, int fbits, int ib, int uns) {
    x = FN(fz)(c, x);
    if (fbits) x = x * (T)(1ull << (fbits - 1)) * K(2);
    T r = FN(rnd)(x, rm);
    if (r != r) return 0;
    if (uns) {
        if (r <= K(0)) return 0;
        if (ib == 32) return r >= K(4294967296.0) ? 0xffffffffull : (uint32_t)r;
        return r >= K(18446744073709551616.0) ? ~0ull : (uint64_t)r;
    }
    if (ib == 32) return r >= K(2147483648.0) ? 0x7fffffffull : r < K(-2147483648.0) ? 0x80000000ull : (uint32_t)(int32_t)r;
    return r >= K(9223372036854775808.0) ? 0x7fffffffffffffffull : r < K(-9223372036854775808.0) ? 0x8000000000000000ull
                                                                                              : (uint64_t)(int64_t)r;
}

// ---- integer -> FP (SCVTF/UCVTF, fixed point), rounded in the FPCR mode
SN T FN(fromint_slow)(Cpu *c, uint64_t v, int ib, int uns, int fbits) {
#pragma STDC FENV_ACCESS ON
    int old = fegetround();
    fesetround(simd_fe_mode(c->fpcr));
    volatile uint64_t vv = v;
    volatile T r;
    if (uns) r = ib == 32 ? (T)(uint32_t)vv : (T)vv;
    else r = ib == 32 ? (T)(int32_t)vv : (T)(int64_t)vv;
    fesetround(old);
    T x = r;
    if (fbits) x = x / (T)(1ull << (fbits - 1)) / K(2);
    return x;
}
SI T FN(fromint)(Cpu *c, uint64_t v, int ib, int uns, int fbits) {
    if (__builtin_expect(c->fpcr & FPCR_RM, 0)) return FN(fromint_slow)(c, v, ib, uns, fbits);
    T r;
    if (uns) r = ib == 32 ? (T)(uint32_t)v : (T)v;
    else r = ib == 32 ? (T)(int32_t)v : (T)(int64_t)v;
    if (fbits) r = r / (T)(1ull << (fbits - 1)) / K(2);
    return r;
}

// ---- reciprocal estimates (bit-exact ARMv8.0 pseudocode)
SI T FN(recpe)(Cpu *c, T a) {
    U x = FN(fb)(FN(fz)(c, a)), s = x & SGN;
    if (FN(isnan)(x)) return FN(nan1)(c, a);
    if (FN(isinf)(x)) return FN(fv)(s);
    if (FN(iszero)(x)) return FN(fv)(s | EXPM);
    U mag = x & ~SGN;
    if (mag < ((U)1 << (MB - 2))) { // |x| < 2^(emin - 2): result overflows
        int rm = (c->fpcr >> 22) & 3;
        int inf = rm == 0 || (rm == 1 && !s) || (rm == 2 && s);
        return FN(fv)(s | (inf ? EXPM : EXPM - 1));
    }
    int64_t e = (int64_t)(mag >> MB);
    U frac = mag & (((U)1 << MB) - 1);
    if ((c->fpcr & FPCR_FZ) && e >= (int64_t)((1 << EB) - 3)) return FN(fv)(s); // result would be tiny
    if (e == 0) {
        if (!(frac >> (MB - 1))) e = -1, frac = (frac << 2) & (((U)1 << MB) - 1);
        else frac = (frac << 1) & (((U)1 << MB) - 1);
    }
    int scaled = 256 | (int)(frac >> (MB - 8));
    int64_t re = (int64_t)((1 << EB) - 3) - e;
    U est = (U)simd_recip_tab[scaled - 256]; // 8 fraction bits
    U f = est << (MB - 8);
    if (re == 0) f = (f >> 1) | ((U)1 << (MB - 1));
    else if (re == -1) f = (f >> 2) | ((U)1 << (MB - 2)), re = 0;
    return FN(fv)(s | ((U)re << MB) | f);
}
SI T FN(rsqrte)(Cpu *c, T a) {
    U x = FN(fb)(FN(fz)(c, a)), s = x & SGN;
    if (FN(isnan)(x)) return FN(nan1)(c, a);
    if (FN(iszero)(x)) return FN(fv)(s | EXPM);
    if (s) return FN(fv)(DNAN);
    if (FN(isinf)(x)) return K(0);
    int64_t e = (int64_t)(x >> MB);
    U frac = x & (((U)1 << MB) - 1);
    if (e == 0) {
        while (!(frac >> (MB - 1))) frac <<= 1, e--;
        frac = (frac << 1) & (((U)1 << MB) - 1);
    }
    int scaled = (e & 1) ? 128 | (int)(frac >> (MB - 7)) : 256 | (int)(frac >> (MB - 8));
    int64_t re = ((int64_t)(3 * ((1 << (EB - 1)) - 1) - 1) - e) / 2; // (380 - e) / 2, (3068 - e) / 2
    U est = (U)simd_rsqrt_tab[scaled - 128];
    return FN(fv)(((U)re << MB) | (est << (MB - 8)));
}
SI T FN(recpx)(Cpu *c, T a) {
    U x = FN(fb)(FN(fz)(c, a)), s = x & SGN;
    if (FN(isnan)(x)) return FN(nan1)(c, a);
    if (!(x & EXPM)) return FN(fv)(s | (EXPM - MINN));
    return FN(fv)(s | (~x & EXPM));
}

#undef FN
#undef SGN
#undef EXPM
#undef QB
#undef DNAN
#undef MINN
#undef VK
#undef K
