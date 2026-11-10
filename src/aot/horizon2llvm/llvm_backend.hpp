// Unified Horizon2LLVM Dynamic LLVM Backend Bridge (src/aot/horizon2llvm/llvm_backend.hpp)
//
// Dynamically loads host `libLLVM*.so` via `dlopen` / `dlsym` (requiring zero external
// llvm-dev C/C++ headers at build time) to invoke LLVM's in-process IR Parser, Verifier,
// New Pass Manager optimizer (`LLVMRunPasses`), Bitcode Writer, native TargetMachine
// Object File emitter, and AArch64 MCDisassembler.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ultrs::llvm_backend {

struct BackendResult {
    bool ok = false;
    std::string error_message;
    std::string optimized_ir;
};

std::string default_host_triple();

BackendResult process_module(
    const std::string& ir_text,
    const std::string& target_triple,
    bool verify,
    const std::string& opt_pipeline,
    const std::string& out_ll_path,
    const std::string& out_bc_path,
    const std::string& out_obj_path);

class LibLLVM {
public:
    static LibLLVM& instance();

    bool available() const { return handle_ != nullptr; }

    bool process_ir(const std::string& ir_text, const std::string& module_id,
                    bool verify, const std::string& opt_pipeline = "",
                    std::string* out_optimized_ll = nullptr,
                    const std::string& out_bc_path = "",
                    const std::string& out_obj_path = "",
                    const std::string& target_triple = "x86_64-pc-linux-gnu",
                    const std::string& cpu_arch = "x86-64-v3") const;

    std::string disassemble_aarch64_inst(uint64_t pc, uint32_t word) const;

    std::string disassemble_aarch64_function(const std::string& fn_name,
                                             uint64_t entry_pc, uint64_t end_pc,
                                             const std::vector<uint32_t>& words,
                                             uint64_t text_base) const;

private:
    LibLLVM();
    ~LibLLVM() = default;
    LibLLVM(const LibLLVM&) = delete;
    LibLLVM& operator=(const LibLLVM&) = delete;

    void* handle_ = nullptr;
};

} // namespace ultrs::llvm_backend
