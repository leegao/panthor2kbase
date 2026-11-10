// Unified Horizon2LLVM AArch64 -> LLVM IR & Native Object Recompiler CLI (src/aot/horizon2llvm/main.cpp)
//
// Unifies AArch64 lifting and mode-parameterized pass-pipeline lowering across:
//   1. User-space ELF64 ET_DYN shared libraries (`libGLES_mali.so` -> `LoweringMode::UserspaceBionicSo`)
//   2. Linux kernel ELF64 ET_REL modules (`mali_kbase.ko` & `mali_kbase_husky.ko` -> `LoweringMode::KernelModuleKo`,
//      lowering all 68 kernel functions directly to SSA LLVM IR `lifted_kbase_ko.ll` and compiling to `.o`)
//   3. Standalone LLVM IR compilation (`--compile-ir`)
//
// Usage:
//   horizon2llvm --lift-ko <r44_ko> <husky_ko> <out.h> <out_runtime.c> [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch]
//   horizon2llvm --lift-ko-dual <r44_ko> <husky_ko> [<out.h> <out_runtime.c>] [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch]
//   horizon2llvm --lift-so <libGLES_mali.so> <out_dir> [--hook-Vulkan-calls]
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
        if (const char* env_full = std::getenv("HORIZON2LLVM_FULL_INDIRECT_DISPATCH")) {
            if (std::string(env_full) == "1" || std::string(env_full) == "true") {
                full_indirect_dispatch = true;
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
        }
        std::string err;
        if (!ko_lifter::lift_ko_modules_to_c_and_ir(
                r44_ko, husky_ko, out_hdr, out_c, out_ll, err,
                out_obj_x86, out_obj_arm64, full_indirect_dispatch)) {
            std::fprintf(stderr, "[horizon2llvm --lift-ko] ERROR: %s\n", err.c_str());
            return 1;
        }
        std::printf("[horizon2llvm --lift-ko] Successfully lowered %s and %s -> %s, %s, %s (objs: %s, %s, full_indirect_dispatch=%d)\n",
                    r44_ko.c_str(), husky_ko.c_str(), out_hdr.c_str(), out_c.c_str(),
                    out_ll.c_str(), out_obj_x86.c_str(), out_obj_arm64.c_str(),
                    full_indirect_dispatch ? 1 : 0);
        return 0;
    }

    if (argc >= 4 && std::string(argv[1]) == "--lift-so") {
        std::string so_path = argv[2];
        std::string out_dir = argv[3];
        std::string cmd = "src/aot/bin/mali_llvm_pass_driver --lift-so \"" + so_path + "\" \"" + out_dir + "\"";
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
        auto res = llvm_backend::process_module(ir_text, triple, false, opt_pipe, "", "", out_obj);
        if (!res.ok) {
            std::fprintf(stderr, "error compiling %s: %s\n", in_ll.c_str(), res.error_message.c_str());
            return 1;
        }
        return 0;
    }

    std::printf("horizon2llvm (unified AArch64-to-LLVM-IR & Native Object Lifter for libGLES_mali.so and mali_kbase*.ko)\n"
                "Usage:\n"
                "  %s --lift-ko <r44_ko> <husky_ko> <out.h> <out_runtime.c> [--out-ir <out.ll>] [--out-obj-x86 <x86.o>] [--out-obj-arm64 <arm64.o>] [--full-indirect-dispatch]\n"
                "  %s --lift-ko-dual <r44_ko> <husky_ko> [<out.h> <out_runtime.c>] [--out-ir <out.ll>] [--out-obj <x86.o>] [--full-indirect-dispatch]\n"
                "  %s --lift-so <libGLES_mali.so> <out_dir> [--hook-Vulkan-calls]\n"
                "  %s --compile-ir <in.ll> <out.o> [-O0|-O2] [--target-triple TRIPLE]\n",
                argv[0], argv[0], argv[0], argv[0]);
    return 0;
}
