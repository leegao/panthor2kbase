// Scoped-Down AArch64 -> LLVM IR Static Lifter & Compiler CLI (src/aot/horizon2llvm/main.cpp)
//
// Usage:
//   horizon2llvm --compile-ir <in.ll> <out.o> [-O0|-O2] [--target-triple TRIPLE]
//   horizon2llvm --lift-raw-bin <in.bin> <entry_pc_hex> <out.ll> [--emit-obj <out.o>]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "aarch64/aarch64_lifter.hpp"
#include "llvm_backend.hpp"
#include "llvm_ir.hpp"
#include "passes.hpp"

namespace fs = std::filesystem;
using namespace ultrs;

int main(int argc, char** argv) {
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

    if (argc >= 5 && std::string(argv[1]) == "--lift-raw-bin") {
        std::string in_bin = argv[2];
        uint64_t entry_pc = std::stoull(argv[3], nullptr, 16);
        std::string out_ll = argv[4];
        std::string out_obj;
        for (int i = 5; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--emit-obj" && i + 1 < argc) out_obj = argv[++i];
        }
        std::ifstream f(in_bin, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "error: cannot open %s\n", in_bin.c_str());
            return 1;
        }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t n_words = raw.size() / 4;
        std::vector<uint32_t> words(n_words);
        for (size_t i = 0; i < n_words; ++i) {
            words[i] = static_cast<uint32_t>(raw[i * 4 + 0]) |
                       (static_cast<uint32_t>(raw[i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(raw[i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(raw[i * 4 + 3]) << 24);
        }
        uint64_t end_pc = entry_pc + n_words * 4;
        llvm_ir::Module mod("lifted_aarch64", llvm_backend::default_host_triple());
        std::set<uint64_t> labels = {entry_pc};
        for (size_t i = 0; i < n_words; ++i) {
            uint64_t pc = entry_pc + i * 4;
            uint32_t w = words[i];
            if ((w & 0x7C000000u) == 0x14000000u) {
                uint64_t t = static_cast<uint64_t>(static_cast<int64_t>(pc) + lifter::sext(w & 0x03FFFFFFu, 26) * 4);
                if (t >= entry_pc && t < end_pc) labels.insert(t);
                if ((w & 0x80000000u) == 0 && i + 1 < n_words) labels.insert(pc + 4);
            } else if (((w >> 25) & 0x7Fu) == 0b0101010u || ((w >> 25) & 0x3Eu) == 0b011010u) {
                uint64_t t = static_cast<uint64_t>(static_cast<int64_t>(pc) + lifter::sext((w >> 5) & 0x7FFFFu, 19) * 4);
                if (t >= entry_pc && t < end_pc) labels.insert(t);
                if (i + 1 < n_words) labels.insert(pc + 4);
            }
        }
        auto* fn = mod.create_function(lifter::fn_symbol(entry_pc), entry_pc, end_pc);
        fn->label_pcs = labels;
        fn->raw_words = words;
        lifter::FunctionLifter fl(*fn, entry_pc, words, entry_pc, end_pc, labels, {});
        fl.lift();

        passes::PassPipelineConfig cfg;
        llvm_ir::ModulePassManager mpm;
        passes::build_aarch64_lowering_pipeline(mpm, cfg);
        mpm.run(mod);

        std::string ir_text = mod.serialize();
        {
            std::ofstream out(out_ll, std::ios::binary);
            out.write(ir_text.data(), static_cast<std::streamsize>(ir_text.size()));
        }
        if (!out_obj.empty()) {
            auto res = llvm_backend::process_module(ir_text, mod.target_triple, true, "default<O2>", "", "", out_obj);
            if (!res.ok) {
                std::fprintf(stderr, "error emitting object: %s\n", res.error_message.c_str());
                return 1;
            }
        }
        return 0;
    }

    std::printf("horizon2llvm (scoped-down AArch64-to-LLVM-IR lifter for Mali/PanVK AOT)\n"
                "Usage:\n"
                "  %s --compile-ir <in.ll> <out.o> [-O0|-O2] [--target-triple TRIPLE]\n"
                "  %s --lift-raw-bin <in.bin> <entry_pc_hex> <out.ll> [--emit-obj <out.o>]\n",
                argv[0], argv[0]);
    return 0;
}
