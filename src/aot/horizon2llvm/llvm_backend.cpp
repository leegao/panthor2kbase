// Unified Horizon2LLVM Dynamic LLVM Backend Bridge Implementation (src/aot/horizon2llvm/llvm_backend.cpp)
//
// Dynamically loads host `libLLVM*.so` via `dlopen` / `dlsym` without requiring
// external `llvm-c/*` or `llvm/IR/*` headers at compile time.

#include "llvm_backend.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace ultrs::llvm_backend {

std::string default_host_triple() {
#if defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "x86_64-pc-linux-gnu";
#endif
}

LibLLVM& LibLLVM::instance() {
    static LibLLVM inst;
    return inst;
}

LibLLVM::LibLLVM() {
    const char* cands[] = {
        std::getenv("MALI_HOST_LIBLLVM"),
        "/usr/lib/x86_64-linux-gnu/libLLVM.so.22.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-21.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM.so.21.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-20.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM.so.20.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-19.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-18.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-17.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-16.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-15.so.1",
        "/usr/lib/x86_64-linux-gnu/libLLVM-14.so.1",
        "libLLVM-18.so.1",
        "libLLVM-17.so.1",
        "libLLVM-16.so.1",
        "libLLVM-15.so.1",
        "libLLVM-14.so.1",
        "libLLVM.so",
    };
    for (const char* c : cands) {
        if (!c || !c[0]) continue;
        handle_ = dlopen(c, RTLD_LAZY | RTLD_GLOBAL);
        if (handle_) break;
    }
    if (!handle_) return;

    using InitFn = void (*)();
    const char* init_syms[] = {
        "LLVMInitializeX86TargetInfo",
        "LLVMInitializeX86Target",
        "LLVMInitializeX86TargetMC",
        "LLVMInitializeX86AsmPrinter",
        "LLVMInitializeX86AsmParser",
        "LLVMInitializeX86Disassembler",
        "LLVMInitializeAArch64TargetInfo",
        "LLVMInitializeAArch64Target",
        "LLVMInitializeAArch64TargetMC",
        "LLVMInitializeAArch64AsmPrinter",
        "LLVMInitializeAArch64AsmParser",
        "LLVMInitializeAArch64Disassembler",
    };
    for (const char* s : init_syms) {
        auto fn = reinterpret_cast<InitFn>(dlsym(handle_, s));
        if (fn) fn();
    }
}

bool LibLLVM::process_ir(const std::string& ir_text, const std::string& module_id,
                         bool verify, const std::string& opt_pipeline,
                         std::string* out_optimized_ll,
                         const std::string& out_bc_path,
                         const std::string& out_obj_path,
                         const std::string& target_triple,
                         const std::string& cpu_arch) const {
    if (!handle_) {
        throw std::runtime_error("libLLVM shared library not loaded");
    }

    using ContextCreateFn = void* (*)();
    using ContextDisposeFn = void (*)(void*);
    using CreateMemBufFn = void* (*)(const char*, size_t, const char*);
    using ParseIRFn = int (*)(void*, void*, void**, char**);
    using DisposeModuleFn = void (*)(void*);
    using VerifyModuleFn = int (*)(void*, int, char**);
    using DisposeMsgFn = void (*)(char*);
    using GetTargetFn = int (*)(const char*, void**, char**);
    using CreateTMFn = void* (*)(void*, const char*, const char*, const char*, int, int, int);
    using DisposeTMFn = void (*)(void*);
    using CreatePBOptsFn = void* (*)();
    using DisposePBOptsFn = void (*)(void*);
    using RunPassesFn = void* (*)(void*, const char*, void*, void*);
    using GetErrMsgFn = char* (*)(void*);
    using DisposeErrMsgFn = void (*)(char*);
    using PrintModToStrFn = char* (*)(void*);
    using WriteBCFn = int (*)(void*, const char*);
    using EmitToFileFn = int (*)(void*, void*, char*, int, char**);

    auto LLVMContextCreate = reinterpret_cast<ContextCreateFn>(dlsym(handle_, "LLVMContextCreate"));
    auto LLVMContextDispose = reinterpret_cast<ContextDisposeFn>(dlsym(handle_, "LLVMContextDispose"));
    auto LLVMCreateMemoryBufferWithMemoryRangeCopy = reinterpret_cast<CreateMemBufFn>(dlsym(handle_, "LLVMCreateMemoryBufferWithMemoryRangeCopy"));
    auto LLVMParseIRInContext = reinterpret_cast<ParseIRFn>(dlsym(handle_, "LLVMParseIRInContext"));
    auto LLVMDisposeModule = reinterpret_cast<DisposeModuleFn>(dlsym(handle_, "LLVMDisposeModule"));
    auto LLVMVerifyModule = reinterpret_cast<VerifyModuleFn>(dlsym(handle_, "LLVMVerifyModule"));
    auto LLVMDisposeMessage = reinterpret_cast<DisposeMsgFn>(dlsym(handle_, "LLVMDisposeMessage"));
    auto LLVMGetTargetFromTriple = reinterpret_cast<GetTargetFn>(dlsym(handle_, "LLVMGetTargetFromTriple"));
    auto LLVMCreateTargetMachine = reinterpret_cast<CreateTMFn>(dlsym(handle_, "LLVMCreateTargetMachine"));
    auto LLVMDisposeTargetMachine = reinterpret_cast<DisposeTMFn>(dlsym(handle_, "LLVMDisposeTargetMachine"));
    auto LLVMCreatePassBuilderOptions = reinterpret_cast<CreatePBOptsFn>(dlsym(handle_, "LLVMCreatePassBuilderOptions"));
    auto LLVMDisposePassBuilderOptions = reinterpret_cast<DisposePBOptsFn>(dlsym(handle_, "LLVMDisposePassBuilderOptions"));
    auto LLVMRunPasses = reinterpret_cast<RunPassesFn>(dlsym(handle_, "LLVMRunPasses"));
    auto LLVMGetErrorMessage = reinterpret_cast<GetErrMsgFn>(dlsym(handle_, "LLVMGetErrorMessage"));
    auto LLVMDisposeErrorMessage = reinterpret_cast<DisposeErrMsgFn>(dlsym(handle_, "LLVMDisposeErrorMessage"));
    auto LLVMPrintModuleToString = reinterpret_cast<PrintModToStrFn>(dlsym(handle_, "LLVMPrintModuleToString"));
    auto LLVMWriteBitcodeToFile = reinterpret_cast<WriteBCFn>(dlsym(handle_, "LLVMWriteBitcodeToFile"));
    auto LLVMTargetMachineEmitToFile = reinterpret_cast<EmitToFileFn>(dlsym(handle_, "LLVMTargetMachineEmitToFile"));

    void* ctx = LLVMContextCreate();
    void* mem_buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(ir_text.data(), ir_text.size(), module_id.c_str());
    void* mod = nullptr;
    char* err_msg = nullptr;

    if (LLVMParseIRInContext(ctx, mem_buf, &mod, &err_msg) != 0) {
        std::string err = err_msg ? err_msg : "unknown LLVM IR parse error";
        if (err_msg) LLVMDisposeMessage(err_msg);
        LLVMContextDispose(ctx);
        throw std::runtime_error("LLVMParseIRInContext failed for " + module_id + ":\n" + err);
    }

    if (verify && LLVMVerifyModule) {
        char* v_err = nullptr;
        if (LLVMVerifyModule(mod, 2 /* LLVMReturnStatusAction */, &v_err) != 0) {
            std::string err = v_err ? v_err : "verifyModule failed";
            if (v_err) LLVMDisposeMessage(v_err);
            LLVMDisposeModule(mod);
            LLVMContextDispose(ctx);
            throw std::runtime_error("LLVMVerifyModule failed for " + module_id + ":\n" + err);
        }
        if (v_err) LLVMDisposeMessage(v_err);
    }

    void* tm = nullptr;
    if (!opt_pipeline.empty() || !out_obj_path.empty()) {
        void* target = nullptr;
        char* targ_err = nullptr;
        if (LLVMGetTargetFromTriple(target_triple.c_str(), &target, &targ_err) != 0) {
            if (targ_err) LLVMDisposeMessage(targ_err);
        } else {
            std::string arch = cpu_arch;
            if (target_triple.find("aarch64") != std::string::npos ||
                target_triple.find("arm64") != std::string::npos) {
                if (arch == "x86-64-v3") arch = "generic";
            }
            tm = LLVMCreateTargetMachine(
                target, target_triple.c_str(), arch.c_str(), "",
                2 /* LLVMCodeGenLevelDefault */, 2 /* LLVMRelocPIC */, 0 /* LLVMCodeModelDefault */);
        }
    }

    if (!opt_pipeline.empty() && LLVMRunPasses) {
        void* opts = LLVMCreatePassBuilderOptions();
        void* pass_err = LLVMRunPasses(mod, opt_pipeline.c_str(), tm, opts);
        LLVMDisposePassBuilderOptions(opts);
        if (pass_err) {
            char* msg = LLVMGetErrorMessage(pass_err);
            std::string s = msg ? msg : "LLVMRunPasses error";
            if (msg) LLVMDisposeErrorMessage(msg);
            if (tm) LLVMDisposeTargetMachine(tm);
            LLVMDisposeModule(mod);
            LLVMContextDispose(ctx);
            throw std::runtime_error("LLVMRunPasses(" + opt_pipeline + ") failed: " + s);
        }
    }

    if (out_optimized_ll && LLVMPrintModuleToString) {
        char* s = LLVMPrintModuleToString(mod);
        if (s) {
            *out_optimized_ll = s;
            LLVMDisposeMessage(s);
        }
    }

    if (!out_bc_path.empty() && LLVMWriteBitcodeToFile) {
        if (LLVMWriteBitcodeToFile(mod, out_bc_path.c_str()) != 0) {
            if (tm) LLVMDisposeTargetMachine(tm);
            LLVMDisposeModule(mod);
            LLVMContextDispose(ctx);
            throw std::runtime_error("LLVMWriteBitcodeToFile failed: " + out_bc_path);
        }
    }

    if (!out_obj_path.empty() && tm && LLVMTargetMachineEmitToFile) {
        char* emit_err = nullptr;
        if (LLVMTargetMachineEmitToFile(tm, mod, const_cast<char*>(out_obj_path.c_str()),
                                        1 /* LLVMObjectFile */, &emit_err) != 0) {
            std::string err = emit_err ? emit_err : "unknown codegen error";
            if (emit_err) LLVMDisposeMessage(emit_err);
            LLVMDisposeTargetMachine(tm);
            LLVMDisposeModule(mod);
            LLVMContextDispose(ctx);
            throw std::runtime_error("LLVMTargetMachineEmitToFile failed: " + err);
        }
    }

    if (tm) LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return true;
}

BackendResult process_module(
    const std::string& ir_text,
    const std::string& target_triple,
    bool verify,
    const std::string& opt_pipeline,
    const std::string& out_ll_path,
    const std::string& out_bc_path,
    const std::string& out_obj_path) {
    BackendResult res;
    try {
        std::string opt_ll;
        LibLLVM::instance().process_ir(
            ir_text, "horizon2llvm_module", verify, opt_pipeline,
            (!out_ll_path.empty() ? &opt_ll : nullptr),
            out_bc_path, out_obj_path, target_triple, "generic");
        if (!out_ll_path.empty() && !opt_ll.empty()) {
            std::ofstream f(out_ll_path, std::ios::binary);
            f.write(opt_ll.data(), static_cast<std::streamsize>(opt_ll.size()));
            res.optimized_ir = std::move(opt_ll);
        }
        res.ok = true;
    } catch (const std::exception& e) {
        res.ok = false;
        res.error_message = e.what();
    }
    return res;
}

std::string LibLLVM::disassemble_aarch64_inst(uint64_t pc, uint32_t word) const {
    if (!handle_) {
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), ".word 0x%08x", word);
        return fallback;
    }
    using CreateDisasmFn = void* (*)(const char*, void*, int, void*, void*);
    using DisasmInstFn = size_t (*)(void*, uint8_t*, uint64_t, uint64_t, char*, size_t);
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    static auto create_fn = reinterpret_cast<CreateDisasmFn>(dlsym(handle_, "LLVMCreateDisasm"));
    static auto disasm_fn = reinterpret_cast<DisasmInstFn>(dlsym(handle_, "LLVMDisasmInstruction"));
    static void* dcr = create_fn ? create_fn("aarch64-unknown-linux-gnu", nullptr, 0, nullptr, nullptr) : nullptr;
    if (!dcr || !disasm_fn) {
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), ".word 0x%08x", word);
        return fallback;
    }
    char buf[160] = {0};
    uint8_t bytes[4] = {
        static_cast<uint8_t>(word & 0xFFu),
        static_cast<uint8_t>((word >> 8) & 0xFFu),
        static_cast<uint8_t>((word >> 16) & 0xFFu),
        static_cast<uint8_t>((word >> 24) & 0xFFu),
    };
    size_t sz = disasm_fn(dcr, bytes, 4, pc, buf, sizeof(buf));
    if (sz == 0) {
        std::snprintf(buf, sizeof(buf), "\t.word\t0x%08x", word);
    }
    std::string s = buf;
    while (!s.empty() && (s.front() == '\t' || s.front() == ' ')) {
        s.erase(s.begin());
    }
    for (char& c : s) {
        if (c == '\t') c = ' ';
    }
    return s;
}

std::string LibLLVM::disassemble_aarch64_function(const std::string& fn_name,
                                                  uint64_t entry_pc, uint64_t end_pc,
                                                  const std::vector<uint32_t>& words,
                                                  uint64_t text_base) const {
    std::ostringstream os;
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr), "; Function %s [0x%09" PRIx64 "..0x%09" PRIx64 ")\n%s:\n",
                  fn_name.c_str(), entry_pc, end_pc, fn_name.c_str());
    os << hdr;
    for (uint64_t pc = entry_pc; pc < end_pc; pc += 4) {
        uint32_t w = words[(pc - text_base) / 4];
        std::string asm_str = disassemble_aarch64_inst(pc, w);
        char line[256];
        std::snprintf(line, sizeof(line), "  0x%09" PRIx64 ":  %08x    %s\n", pc, w, asm_str.c_str());
        os << line;
    }
    os << "\n";
    return os.str();
}

} // namespace ultrs::llvm_backend
