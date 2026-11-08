# Scoped-Down AArch64 -> LLVM 21 IR Lifter & Core Lowering Pipeline (`src/aot/horizon2llvm`)

Lightweight, self-contained C++17 AArch64-to-LLVM-IR static instruction lifter and 11-pass AArch64 lowering pipeline built on **LLVM 21** (`llvm::IRBuilder<llvm::NoFolder>`, `llvm::LLVMContext`, `llvm::Module`, `llvm::Function`, `llvm::BasicBlock`, and New Pass Manager `default<O2>`), scoped specifically for lifting Android/Linux AArch64 shared libraries (`libGLES_mali.so` and `libvulkan_panfrost.so`).

## Directory Structure

```
src/aot/horizon2llvm/
├── aarch64/
│   ├── abi_aapcs64.hpp                      # AAPCS64 calling convention & register definitions
│   ├── aarch64_lifter.hpp                   # ARMv8-A instruction bitfield decoders & lifter interface
│   └── aarch64_lifter.cpp                   # AArch64 integer, branch, load/store, atomic & AdvSIMD lifter
├── lowering/
│   ├── pass_guest_stack_alignment.cpp       # Guest stack frame & alignment lowering
│   ├── pass_aarch64_idiom_deconstruct.cpp   # AArch64 bitfield/idiom deconstruction
│   ├── pass_jump_table_lowering.cpp         # Indirect jump table switch lowering
│   ├── pass_fcmp_predicate_fusion.cpp       # Floating-point compare & predicate fusion
│   ├── pass_nzcv_predicate_fusion.cpp       # NZCV condition flag predicate fusion
│   ├── pass_nzcv_dead_flag_elim.cpp         # Intra/inter-block dead NZCV flag elimination
│   ├── pass_link_register_elision.cpp       # Leaf/non-escaping X30 link register elision
│   ├── pass_tail_call_devirt.cpp            # Tail-call devirtualization
│   ├── pass_aapcs64_abi_contract.cpp        # AAPCS64 caller/callee-saved register contract pruning
│   └── pass_cpu_state_promotion.cpp         # Inter-block Cpu* state register promotion
├── runtime/
│   ├── cpu.h                                # AArch64 Cpu state struct definition
│   ├── rt.h                                 # Minimal runtime helper declarations
│   ├── simd.h / simd_fp.h / simd_rt.*       # AdvSIMD / FP runtime helpers
├── tests/
│   ├── CMakeLists.txt
│   └── test_main_snippets.cpp               # Self-contained AArch64 lift + LLVM 21 O2 verification test
├── llvm_ir.hpp                              # SSA IR builder & Module/Function/BasicBlock representation
├── llvm_backend.hpp / llvm_backend.cpp      # Direct libLLVM verification, O2 optimization & object emission
├── passes.hpp / passes.cpp                  # 11-pass AArch64 lowering pipeline orchestration
├── passes_llvm_utils.hpp                    # PassLLVMBuilder helper utilities
├── main.cpp                                 # CLI driver (--compile-ir and --lift-raw-bin)
├── Makefile                                 # Standalone incremental Makefile
└── CMakeLists.txt                           # Standalone CMake build configuration
```

## Building & Running Tests

```bash
make -C src/aot/horizon2llvm -j$(nproc) all tests
```
