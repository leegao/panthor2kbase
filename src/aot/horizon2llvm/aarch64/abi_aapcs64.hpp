// Auto-generated from AArch64CallingConv.td by gen_aapcs64.py. DO NOT EDIT DIRECTLY.
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

namespace ultrs::aarch64 {

constexpr size_t MAX_ARCH_REGS = 96;
using RegSet = std::bitset<MAX_ARCH_REGS>;

// Architectural register index mappings (matching Cpu struct / llvm_ir arch_reg):
//   0..30 : X0..X30
//   31    : SP
//   32    : NZCV
//   64..95: V0..V31
namespace regs {
    constexpr int X0 = 0;
    constexpr int X1 = 1;
    constexpr int X2 = 2;
    constexpr int X3 = 3;
    constexpr int X4 = 4;
    constexpr int X5 = 5;
    constexpr int X6 = 6;
    constexpr int X7 = 7;
    constexpr int X8 = 8;
    constexpr int X9 = 9;
    constexpr int X10 = 10;
    constexpr int X11 = 11;
    constexpr int X12 = 12;
    constexpr int X13 = 13;
    constexpr int X14 = 14;
    constexpr int X15 = 15;
    constexpr int X16 = 16;
    constexpr int X17 = 17;
    constexpr int X18 = 18;
    constexpr int X19 = 19;
    constexpr int X20 = 20;
    constexpr int X21 = 21;
    constexpr int X22 = 22;
    constexpr int X23 = 23;
    constexpr int X24 = 24;
    constexpr int X25 = 25;
    constexpr int X26 = 26;
    constexpr int X27 = 27;
    constexpr int X28 = 28;
    constexpr int X29 = 29;
    constexpr int X30 = 30;
    constexpr int FP = 29;
    constexpr int LR = 30;
    constexpr int SP = 31;
    constexpr int NZCV = 32;
    constexpr int V0 = 64;
    constexpr int V1 = 65;
    constexpr int V2 = 66;
    constexpr int V3 = 67;
    constexpr int V4 = 68;
    constexpr int V5 = 69;
    constexpr int V6 = 70;
    constexpr int V7 = 71;
    constexpr int V8 = 72;
    constexpr int V9 = 73;
    constexpr int V10 = 74;
    constexpr int V11 = 75;
    constexpr int V12 = 76;
    constexpr int V13 = 77;
    constexpr int V14 = 78;
    constexpr int V15 = 79;
    constexpr int V16 = 80;
    constexpr int V17 = 81;
    constexpr int V18 = 82;
    constexpr int V19 = 83;
    constexpr int V20 = 84;
    constexpr int V21 = 85;
    constexpr int V22 = 86;
    constexpr int V23 = 87;
    constexpr int V24 = 88;
    constexpr int V25 = 89;
    constexpr int V26 = 90;
    constexpr int V27 = 91;
    constexpr int V28 = 92;
    constexpr int V29 = 93;
    constexpr int V30 = 94;
    constexpr int V31 = 95;
} // namespace regs

// Specification-derived register bitsets under AAPCS64:
namespace abi {

    // Argument GPRs (X0..X8, including X8 sret)
    inline RegSet make_ArgGPRs() {
        RegSet s; s.set(0); s.set(1); s.set(2); s.set(3); s.set(4); s.set(5); s.set(6); s.set(7); s.set(8); return s;
    }
    const RegSet ArgGPRs = make_ArgGPRs();

    // Argument FPRs (V0..V7)
    inline RegSet make_ArgFPRs() {
        RegSet s; s.set(64); s.set(65); s.set(66); s.set(67); s.set(68); s.set(69); s.set(70); s.set(71); return s;
    }
    const RegSet ArgFPRs = make_ArgFPRs();

    // Return GPRs (X0..X7)
    inline RegSet make_RetGPRs() {
        RegSet s; s.set(0); s.set(1); s.set(2); s.set(3); s.set(4); s.set(5); s.set(6); s.set(7); return s;
    }
    const RegSet RetGPRs = make_RetGPRs();

    // Return FPRs (V0..V7)
    inline RegSet make_RetFPRs() {
        RegSet s; s.set(64); s.set(65); s.set(66); s.set(67); s.set(68); s.set(69); s.set(70); s.set(71); return s;
    }
    const RegSet RetFPRs = make_RetFPRs();

    // Callee-saved GPRs (X19..X29, SP)
    inline RegSet make_CalleeSavedGPRs() {
        RegSet s; s.set(19); s.set(20); s.set(21); s.set(22); s.set(23); s.set(24); s.set(25); s.set(26); s.set(27); s.set(28); s.set(29); s.set(31); return s;
    }
    const RegSet CalleeSavedGPRs = make_CalleeSavedGPRs();

    // Callee-saved FPRs (V8..V15 lower 64 bits)
    inline RegSet make_CalleeSavedFPRs() {
        RegSet s; s.set(72); s.set(73); s.set(74); s.set(75); s.set(76); s.set(77); s.set(78); s.set(79); return s;
    }
    const RegSet CalleeSavedFPRs = make_CalleeSavedFPRs();

    // Caller-saved temporary GPRs (X9..X18, X30/LR)
    inline RegSet make_CallerSavedScratchGPRs() {
        RegSet s; s.set(9); s.set(10); s.set(11); s.set(12); s.set(13); s.set(14); s.set(15); s.set(16); s.set(17); s.set(18); s.set(30); return s;
    }
    const RegSet CallerSavedScratchGPRs = make_CallerSavedScratchGPRs();

    // Caller-saved temporary FPRs (V16..V31)
    inline RegSet make_CallerSavedScratchFPRs() {
        RegSet s; s.set(80); s.set(81); s.set(82); s.set(83); s.set(84); s.set(85); s.set(86); s.set(87); s.set(88); s.set(89); s.set(90); s.set(91); s.set(92); s.set(93); s.set(94); s.set(95); return s;
    }
    const RegSet CallerSavedScratchFPRs = make_CallerSavedScratchFPRs();

    // Caller-saved flags (NZCV)
    inline RegSet make_ScratchFlags() {
        RegSet s; s.set(32); return s;
    }
    const RegSet ScratchFlags = make_ScratchFlags();

    // Compound AAPCS64 contracts:
    // Live across function return under standard AAPCS64:
    inline RegSet make_AAPCSReturnLive() {
        return RetGPRs | RetFPRs | CalleeSavedGPRs | CalleeSavedFPRs;
    }
    const RegSet AAPCSReturnLive = make_AAPCSReturnLive();

    // Upward-exposed reads at external call boundaries:
    inline RegSet make_AAPCSCallReads() {
        RegSet s = ArgGPRs | ArgFPRs | CalleeSavedGPRs | CalleeSavedFPRs;
        s.set(regs::LR);
        return s;
    }
    const RegSet AAPCSCallReads = make_AAPCSCallReads();

    // Registers clobbered by external calls (caller-saved scratch):
    inline RegSet make_AAPCSClobberedByCall() {
        return CallerSavedScratchGPRs | CallerSavedScratchFPRs | ScratchFlags | ArgGPRs | ArgFPRs | RetGPRs | RetFPRs;
    }
    const RegSet AAPCSClobberedByCall = make_AAPCSClobberedByCall();

    // Query helper predicates:
    constexpr bool is_gpr(int r) { return r >= 0 && r <= 31; }
    constexpr bool is_fpr(int r) { return r >= 64 && r <= 95; }
    constexpr bool is_callee_saved_gpr(int r) { return (r >= 19 && r <= 29) || r == 31; }
    constexpr bool is_callee_saved_fpr(int r) { return r >= 72 && r <= 79; }
    constexpr bool is_callee_saved(int r) { return is_callee_saved_gpr(r) || is_callee_saved_fpr(r); }
    constexpr bool is_caller_saved(int r) { return !is_callee_saved(r); }
    constexpr bool is_arg_reg(int r) { return (r >= 0 && r <= 8) || (r >= 64 && r <= 71); }
    constexpr bool is_ret_reg(int r) { return (r >= 0 && r <= 7) || (r >= 64 && r <= 71); }

} // namespace abi
} // namespace ultrs::aarch64
