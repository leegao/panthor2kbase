// Unified Horizon2LLVM AArch64 -> LLVM IR & Native Object Recompiler CLI (src/aot/horizon2llvm/main.cpp)
//
// Unifies AArch64 lifting and mode-parameterized pass-pipeline lowering across:
//   1. User-space ELF64 ET_DYN shared libraries (`libGLES_mali.so` -> `LoweringMode::UserspaceBionicSo`)
//   2. Linux kernel ELF64 ET_REL modules (`mali_kbase*.ko` -> `LoweringMode::KernelModuleKo`,
//      generically recovering `.rodata` tables and jump tables from ELF relocations & symbol boundaries
//      with zero hardcoded `kRodataTableMap` entries, and lowering all target kernel functions directly
//      to SSA LLVM IR `lifted_kbase_ko.ll` and compiling to `.o` at `-O2`)
//   3. Standalone single-`.ko` generic lifting (`--lift-ko-single`)
//   4. Standalone LLVM IR compilation (`--compile-ir`)
//
// Usage:
//   horizon2llvm --lift-ko <r44_ko> <husky_ko> <out.h> <out_runtime.c> [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--record-stackframes|--profile-functions|--no-instrument-calls]
//   horizon2llvm --lift-ko-dual <r44_ko> <husky_ko> [<out.h> <out_runtime.c>] [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--record-stackframes|--profile-functions|--no-instrument-calls]
//   horizon2llvm --lift-ko-single <ko_path> <label> <out.ll> [--out-obj <out.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--no-instrument-calls]
//   horizon2llvm --lift-so <libGLES_mali.so> <out_dir> [--hook-Vulkan-calls] [--instrument-calls|--record-stackframes|--profile-functions|--no-instrument-calls]
//   horizon2llvm --compile-ir <in.ll> <out.o> [-O0|-O2] [--target-triple TRIPLE]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ko_lifter.hpp"
#include "llvm_backend.hpp"

using namespace ultrs;

int main(int argc, char** argv) {
    if (argc >= 4 && (std::string(argv[1]) == "--lift-ko" || std::string(argv[1]) == "--lift-ko-dual")) {
        std::string r44_ko = argv[2];
        std::string husky_ko = argv[3];
        std::string out_hdr = "src/emulator/lifted_kbase_ko.h";
        std::string out_c = "src/emulator/lifted_kbase_ko.c";
        std::string out_ll = "src/emulator/lifted_kbase_ko.ll";
        std::string out_obj_x86 = "src/emulator/lifted_kbase_ko_x86_64.o";
        std::string out_obj_arm64 = "src/emulator/lifted_kbase_ko_arm64.o";
        bool full_indirect_dispatch = false;
        bool count_insns = true;
        bool instrument_calls = true;
        if (const char* env_full = std::getenv("HORIZON2LLVM_FULL_INDIRECT_DISPATCH")) {
            if (std::string(env_full) == "1" || std::string(env_full) == "true") {
                full_indirect_dispatch = true;
            }
        }
        if (const char* env_cnt = std::getenv("HORIZON2LLVM_COUNT_INSNS")) {
            if (std::string(env_cnt) == "0" || std::string(env_cnt) == "false") {
                count_insns = false;
            } else if (std::string(env_cnt) == "1" || std::string(env_cnt) == "true") {
                count_insns = true;
            }
        }
        if (const char* env_inst = std::getenv("HORIZON2LLVM_INSTRUMENT_CALLS")) {
            if (std::string(env_inst) == "0" || std::string(env_inst) == "false") {
                instrument_calls = false;
            } else if (std::string(env_inst) == "1" || std::string(env_inst) == "true") {
                instrument_calls = true;
            }
        }
        int opt_start = 4;
        if (argc >= 6 && argv[4][0] != '-' && argv[5][0] != '-') {
            out_hdr = argv[4];
            out_c = argv[5];
            opt_start = 6;
        }
        for (int i = opt_start; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--out-ir" && i + 1 < argc) out_ll = argv[++i];
            else if (a == "--out-c" && i + 1 < argc) out_c = argv[++i];
            else if (a == "--out-hdr" && i + 1 < argc) out_hdr = argv[++i];
            else if ((a == "--out-obj-x86" || a == "--out-obj") && i + 1 < argc) out_obj_x86 = argv[++i];
            else if (a == "--out-obj-arm64" && i + 1 < argc) out_obj_arm64 = argv[++i];
            else if (a == "--full-indirect-dispatch") full_indirect_dispatch = true;
            else if (a == "--count-insns") count_insns = true;
            else if (a == "--no-count-insns") count_insns = false;
            else if (a == "--instrument-calls" || a == "--record-stackframes" ||
                     a == "--profile-functions" || a == "--debug-trace") instrument_calls = true;
            else if (a == "--no-instrument-calls" || a == "--no-record-stackframes" ||
                     a == "--no-profile-functions") instrument_calls = false;
        }
        std::string err;
        if (!ko_lifter::lift_ko_modules_to_c_and_ir(
                r44_ko, husky_ko, out_hdr, out_c, out_ll, err,
                out_obj_x86, out_obj_arm64, full_indirect_dispatch, count_insns, instrument_calls)) {
            std::fprintf(stderr, "[horizon2llvm --lift-ko] ERROR: %s\n", err.c_str());
            return 1;
        }
        std::printf("[horizon2llvm --lift-ko] Successfully lowered %s and %s (+ discovered kernel/*.ko variants) -> %s, %s, %s (objs: %s, %s, full_indirect_dispatch=%d, count_insns=%d, instrument_calls=%d)\n",
                    r44_ko.c_str(), husky_ko.c_str(), out_hdr.c_str(), out_c.c_str(),
                    out_ll.c_str(), out_obj_x86.c_str(), out_obj_arm64.c_str(),
                    full_indirect_dispatch ? 1 : 0, count_insns ? 1 : 0, instrument_calls ? 1 : 0);
        return 0;
    }

    if (argc >= 5 && std::string(argv[1]) == "--lift-ko-single") {
        std::string ko_path = argv[2];
        std::string label = argv[3];
        std::string out_ll = argv[4];
        std::string out_obj;
        bool full_indirect_dispatch = false;
        bool count_insns = true;
        bool instrument_calls = true;
        for (int i = 5; i < argc; ++i) {
            std::string a = argv[i];
            if ((a == "--out-obj" || a == "--out-obj-x86") && i + 1 < argc) out_obj = argv[++i];
            else if (a == "--full-indirect-dispatch") full_indirect_dispatch = true;
            else if (a == "--count-insns") count_insns = true;
            else if (a == "--no-count-insns") count_insns = false;
            else if (a == "--instrument-calls" || a == "--record-stackframes" ||
                     a == "--profile-functions" || a == "--debug-trace") instrument_calls = true;
            else if (a == "--no-instrument-calls") instrument_calls = false;
        }
        std::string err;
        if (!ko_lifter::lift_single_ko_module_to_ir(
                ko_path, label, out_ll, err, out_obj,
                full_indirect_dispatch, count_insns, instrument_calls)) {
            std::fprintf(stderr, "[horizon2llvm --lift-ko-single] ERROR: %s\n", err.c_str());
            return 1;
        }
        std::printf("[horizon2llvm --lift-ko-single] Successfully lowered %s (label=%s) -> %s (obj=%s, instrument_calls=%d)\n",
                    ko_path.c_str(), label.c_str(), out_ll.c_str(),
                    out_obj.empty() ? "none" : out_obj.c_str(), instrument_calls ? 1 : 0);
        return 0;
    }

    if (argc >= 4 && std::string(argv[1]) == "--lift-so") {
        std::string so_path = argv[2];
        std::string out_dir = argv[3];
        std::string cmd = "build-x86-64/aot/mali_llvm_pass_driver --lift-so \"" + so_path + "\" \"" + out_dir + "\"";
        for (int i = 4; i < argc; ++i) {
            cmd += " \"" + std::string(argv[i]) + "\"";
        }
        int rc = std::system(cmd.c_str());
        return (rc == 0) ? 0 : 1;
    }

    if (argc >= 4 && std::string(argv[1]) == "--compile-ir") {
        std::string in_ll = argv[2];
        std::string out_obj = argv[3];
        std::string opt_pipe = "default<O2>";
        std::string triple = llvm_backend::default_host_triple();
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-O0") opt_pipe = "mem2reg";
            else if (a == "-O1") opt_pipe = "default<O1>";
            else if (a == "-O2" || a == "-O3") opt_pipe = "default<O2>";
            else if (a == "--target-triple" && i + 1 < argc) triple = argv[++i];
        }
        std::ifstream f(in_ll, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "error: cannot open %s\n", in_ll.c_str());
            return 1;
        }
        std::string ir_text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto res = llvm_backend::process_module(ir_text, triple, true, opt_pipe, "", "", out_obj);
        if (!res.ok) {
            std::fprintf(stderr, "error compiling %s: %s\n", in_ll.c_str(), res.error_message.c_str());
            return 1;
        }
        return 0;
    }

    std::printf("horizon2llvm (unified AArch64-to-LLVM-IR & Native Object Lifter for libGLES_mali.so and mali_kbase*.ko)\n"
                "Usage:\n"
                "  %s --lift-ko <r44_ko> <husky_ko> <out.h> <out_runtime.c> [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--no-instrument-calls]\n"
                "  %s --lift-ko-dual <r44_ko> <husky_ko> [<out.h> <out_runtime.c>] [--out-ir <out.ll>] [--out-obj <x86.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--no-instrument-calls]\n"
                "  %s --lift-ko-single <ko_path> <label> <out.ll> [--out-obj <out.o>] [--full-indirect-dispatch] [--count-insns|--no-count-insns] [--instrument-calls|--no-instrument-calls]\n"
                "  %s --lift-so <libGLES_mali.so> <out_dir> [--hook-Vulkan-calls] [--instrument-calls|--no-instrument-calls]\n"
                "  %s --compile-ir <in.ll> <out.o> [-O0|-O2] [--target-triple TRIPLE]\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 0;
}
