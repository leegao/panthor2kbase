// Scoped-Down Unit Test Suite for AArch64 -> LLVM IR Lifter & Core Lowering Passes
// (src/aot/horizon2llvm/tests/test_main_snippets.cpp)

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "aarch64/aarch64_lifter.hpp"
#include "llvm_backend.hpp"
#include "llvm_ir.hpp"
#include "passes.hpp"

using namespace ultrs;

int main() {
    std::printf("[horizon2llvm] Running scoped-down AArch64-to-LLVM-IR lifter & lowering unit tests...\n");

    // Lift a representative AArch64 function (prologue STP, arithmetic, conditional branch, epilogue LDP + RET)
    const uint64_t entry_pc = 0x1000ULL;
    const std::vector<uint32_t> words = {
        0xa9bf7bfdU, // stp x29, x30, [sp, #-16]!
        0x910003fdU, // mov x29, sp
        0x8b010000U, // add x0, x0, x1
        0xf100041fU, // cmp x0, #1
        0x54000041U, // b.ne +8 (0x1018)
        0xd2800540U, // mov x0, #42
        0xa8c17bfdU, // ldp x29, x30, [sp], #16
        0xd65f03c0U, // ret
    };
    const uint64_t end_pc = entry_pc + words.size() * 4;
    std::set<uint64_t> labels = {0x1000ULL, 0x1014ULL, 0x1018ULL};

    llvm_ir::Module mod("test_aarch64_mod", llvm_backend::default_host_triple());
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
    assert(!ir_text.empty());
    assert(ir_text.find("define void @f_1000") != std::string::npos);

    auto res = llvm_backend::process_module(ir_text, mod.target_triple, true, "default<O2>");
    assert(res.ok && "LLVM 21 verifier + O2 optimization pipeline must succeed on lifted AArch64 IR");

    std::printf("[horizon2llvm] PASS: Lifted AArch64 function verified and optimized cleanly via LLVM 21 (%zu bytes IR)\n",
                res.optimized_ir.size());
    return 0;
}