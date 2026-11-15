// Shared AArch64 ISA Bit-Decoding, Immediate Expansion & Atomic/SIMD Classifiers
// (src/aot/horizon2llvm/aarch64/aarch64_isa_common.hpp)
//
// Shared across:
//   - `src/aot/horizon2llvm/aarch64/aarch64_lifter.hpp` / `aarch64_lifter.cpp`
//   - `src/aot/src/mali_llvm_passes.cpp`
#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace ultrs::aarch64_isa {

inline constexpr uint32_t bits(uint32_t w, unsigned lo, unsigned n) {
    return (w >> lo) & ((1u << n) - 1u);
}

inline constexpr int64_t sext(uint64_t v, unsigned n) {
    uint64_t sign_bit = 1ULL << (n - 1);
    return (v & sign_bit) ? static_cast<int64_t>(v - (1ULL << n)) : static_cast<int64_t>(v);
}

inline constexpr uint64_t ones(unsigned n) {
    return n >= 64 ? ~0ULL : ((1ULL << n) - 1ULL);
}

inline constexpr uint64_t ror_val(uint64_t v, unsigned r, unsigned size) {
    r %= size;
    if (!r) return v & ones(size);
    return ((v >> r) | (v << (size - r))) & ones(size);
}

inline constexpr uint64_t replicate_val(uint64_t v, unsigned esize, unsigned size) {
    uint64_t out = 0;
    for (unsigned i = 0; i < size; i += esize) out |= (v << i);
    return out;
}

inline bool decode_bit_masks(unsigned N, unsigned imms, unsigned immr, unsigned size, bool immediate,
                             uint64_t& wmask, uint64_t& tmask) {
    unsigned x = (N << 6) | (~imms & 0x3Fu);
    if (x == 0) return false;
    unsigned length = 31u - __builtin_clz(x);
    if (length < 1) return false;
    unsigned levels = (1u << length) - 1u;
    if (immediate && (imms & levels) == levels) return false;
    unsigned S = imms & levels;
    unsigned R = immr & levels;
    unsigned diff = (S - R) & 0x3Fu;
    unsigned esize = 1u << length;
    unsigned d = diff & levels;
    uint64_t welem = ones(S + 1);
    uint64_t telem = ones(d + 1);
    wmask = replicate_val(ror_val(welem, R, esize), esize, size);
    tmask = replicate_val(telem, esize, size);
    return true;
}

inline std::pair<uint64_t, uint64_t> decode_bit_masks_pair(unsigned sf, unsigned immN, unsigned imms, unsigned immr) {
    uint64_t wmask = 0, tmask = 0;
    decode_bit_masks(immN, imms, immr, sf ? 64u : 32u, false, wmask, tmask);
    return {wmask, tmask};
}

inline constexpr uint64_t vfp_expand_imm(uint32_t imm8, unsigned N) {
    unsigned E = (N == 32) ? 8 : 11;
    unsigned F = N - E - 1;
    uint64_t sign = (imm8 >> 7) & 1;
    uint64_t b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << (E - 1)) | ((b6 ? ((1ULL << (E - 3)) - 1ULL) : 0ULL) << 2) | ((imm8 >> 4) & 3);
    uint64_t frac = static_cast<uint64_t>(imm8 & 15) << (F - 4);
    return (sign << (N - 1)) | (exp << F) | frac;
}

inline constexpr uint64_t simd_expand_imm(unsigned op, unsigned cmode, uint32_t imm8) {
    unsigned c = cmode >> 1;
    uint64_t u8 = imm8 & 0xFFu;
    if (c == 0) return (u8 << 32) | u8;
    if (c == 1) return ((u8 << 8) << 32) | (u8 << 8);
    if (c == 2) return ((u8 << 16) << 32) | (u8 << 16);
    if (c == 3) return ((u8 << 24) << 32) | (u8 << 24);
    if (c == 4) {
        uint64_t r = 0;
        for (unsigned s = 0; s < 64; s += 16) r |= (u8 << s);
        return r;
    }
    if (c == 5) {
        uint64_t r = 0;
        for (unsigned s = 0; s < 64; s += 16) r |= ((u8 << 8) << s);
        return r;
    }
    if (c == 6) {
        uint64_t v = !(cmode & 1) ? ((u8 << 8) | 0xFFULL) : ((u8 << 16) | 0xFFFFULL);
        return (v << 32) | v;
    }
    if (!(cmode & 1) && !op) {
        uint64_t r = 0;
        for (unsigned s = 0; s < 64; s += 8) r |= (u8 << s);
        return r;
    }
    if (!(cmode & 1) && op) {
        uint64_t r = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if ((u8 >> i) & 1) r |= (0xFFULL << (8 * i));
        }
        return r;
    }
    if ((cmode & 1) && !op) {
        uint64_t v = vfp_expand_imm(imm8, 32);
        return (v << 32) | v;
    }
    return vfp_expand_imm(imm8, 64);
}

inline constexpr bool decode_imm5_lane(unsigned imm5, unsigned& sz, unsigned& idx) {
    for (unsigned s = 0; s < 4; ++s) {
        if (imm5 & (1u << s)) {
            sz = s;
            idx = imm5 >> (s + 1);
            return true;
        }
    }
    return false;
}

// ARMv8.1 LSE Compare-and-Swap (CAS, CASA, CASL, CASAL, CASB, CASH)
struct CasInsn {
    unsigned size = 0;
    bool L = false;
    bool o0 = false;
    unsigned rs = 0;
    unsigned rn = 0;
    unsigned rt = 0;
    const char* succ_ordering() const {
        return (L && o0) ? "seq_cst" : (L ? "acquire" : (o0 ? "release" : "monotonic"));
    }
    const char* fail_ordering() const {
        return L ? "acquire" : "monotonic";
    }
};

inline constexpr bool decode_cas(uint32_t w, CasInsn& out) {
    if ((w & 0x3FA07C00u) != 0x08A07C00u) return false;
    out.size = bits(w, 30, 2);
    out.L    = bits(w, 22, 1) != 0;
    out.rs   = bits(w, 16, 5);
    out.o0   = bits(w, 15, 1) != 0;
    out.rn   = bits(w, 5, 5);
    out.rt   = bits(w, 0, 5);
    return true;
}

// ARMv8.1 LSE Atomic memory operations (LDADD*, LDCLR*, LDEOR*, LDSET*, LDSMAX*, LDSMIN*, LDUMAX*, LDUMIN*, SWP*)
struct LseAtomicInsn {
    unsigned size = 0;
    bool V = false;
    bool A = false;
    bool R = false;
    unsigned rs = 0;
    unsigned opc = 0;
    unsigned rn = 0;
    unsigned rt = 0;
    const char* llvm_rmw_op = nullptr;
    bool invert_src = false;
};

inline constexpr bool decode_lse_atomic(uint32_t w, LseAtomicInsn& out) {
    if ((w & 0x3B200C00u) != 0x38200000u) return false;
    unsigned V = bits(w, 26, 1);
    if (V != 0) return false;
    unsigned o3 = bits(w, 15, 1);
    unsigned opc = bits(w, 12, 3);
    const char* rmw = nullptr;
    bool inv = false;
    if (o3 == 1 && opc == 0) {
        rmw = "xchg";
    } else if (o3 == 0) {
        switch (opc) {
            case 0: rmw = "add"; break;
            case 1: rmw = "and"; inv = true; break;
            case 2: rmw = "xor"; break;
            case 3: rmw = "or";  break;
            case 4: rmw = "max"; break;
            case 5: rmw = "min"; break;
            case 6: rmw = "umax"; break;
            case 7: rmw = "umin"; break;
            default: return false;
        }
    } else {
        return false;
    }
    out.size = bits(w, 30, 2);
    out.V = false;
    out.A = bits(w, 23, 1) != 0;
    out.R = bits(w, 22, 1) != 0;
    out.rs = bits(w, 16, 5);
    out.opc = (o3 << 3) | opc;
    out.rn = bits(w, 5, 5);
    out.rt = bits(w, 0, 5);
    out.llvm_rmw_op = rmw;
    out.invert_src = inv;
    return true;
}

// ARMv8.4 RCPC Unscaled Acquire/Release (STLUR, LDAPUR, LDAPURSW, etc.)
struct RcpcUnscaledInsn {
    unsigned size = 0;
    unsigned opc = 0;
    int64_t imm9 = 0;
    unsigned rn = 0;
    unsigned rt = 0;
};

inline constexpr bool decode_rcpc_unscaled(uint32_t w, RcpcUnscaledInsn& out) {
    if ((w & 0x3F200C00u) != 0x19000000u) return false;
    out.size = bits(w, 30, 2);
    out.opc  = bits(w, 22, 2);
    out.imm9 = sext(bits(w, 12, 9), 9);
    out.rn   = bits(w, 5, 5);
    out.rt   = bits(w, 0, 5);
    return true;
}

} // namespace ultrs::aarch64_isa
