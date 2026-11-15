// ============================================================================
// mali_llvm_passes.cpp
// ====================
// Unified C++ LLVM 15 Lifting & Lowering Pass Framework for 64-Bit ARM64-Bionic
// libGLES_mali.so (r26p0-01eac0, r49p1-03bet0 v4/v5, r54p3-00eac0) targeting
// native x86-64 Linux (glibc), covering both the Mali Shader Compiler (CMPBE
// MBS2 / ISA OBJC) and the Vulkan ICD + /dev/mali0 (kbase) Kernel UAPI.
//
// Pass Pipeline Architecture:
//   Pass 1: ELF64LoaderAndCallGraphPass
//           - Maps PT_LOAD segments by p_vaddr, applies .relr.dyn and APS2
//             .rela.dyn relocations, parses .eh_frame DWARF FDEs & .dynsym,
//             discovers CMPBE + Vulkan ICD (HMI, vk_hal_open, vkGetInstanceProcAddr,
//             vkEnumerate*, vkCreateShaderModule, vkDestroyShaderModule) +
//             /dev/mali0 kbase UAPI (mali_gpu_props_get) entry points, computes
//             their transitive closure, and auto-detects ctx allocator offsets.
//   Pass 2: CFGRecoveryAndPicJumpTablePass
//           - Discovers basic block leaders and Clang AArch64 PIC jump tables
//             (cmp / b.hi / adrp / add / adr / ldrb / add / br).
//   Pass 3: AArch64ToLLVMIRLiftingPass
//           - Lifts 64-bit AArch64 machine code into an LLVMModuleRef using
//             Horizon aarch64_lifter.hpp idioms (@llvm.fshr, @llvm.bitreverse,
//             @llvm.bswap, @llvm.ctlz, guarded UDIV/SDIV, LSLV/LSRV/ASRV/RORV,
//             LDAR/STLR atomics, AdvSIMD EXT/UZP1/USHLL/CNT/CMEQ/ADDV/UMAXV/
//             TBL/DUP/INS/REV32, and ARMv8 Crypto SHA1C/SHA1P/SHA1M/SHA1H/
//             SHA1SU0/SHA1SU1) and semantic Bionic/ARM64 intrinsic markers.
//   Pass 4: Arm64BionicIdiosyncrasyLoweringPass (LLVM IR Transformation Pass)
//           - Strips @llvm.mali.arm64.hint (PACIASP, AUTIASP, BTI, NOP)
//           - Strips @llvm.mali.arm64.retaa (ARMv8.3-A authenticated return)
//           - Lowers @llvm.mali.arm64.mrs (TPIDR_EL0 stack canary +0x28 &
//             OpenGL TLS +0x18, DCZID_EL0)
//           - Lowers @llvm.mali.arm64.dc_zva and @llvm.mali.bionic.tlsdesc
//   Pass 5: BionicToGlibcLibcLoweringPass (LLVM IR Transformation Pass)
//           - Rewrites @llvm.mali.bionic.plt64.<sym> calls (both standard and
//             FORTIFY_SOURCE *_chk variants) to LLVM intrinsics, glibc calls,
//             unavailable vendor/Android library stubs (liblog, libcutils,
//             MediaTek libgpud), and /dev/mali0 kbase UAPI ioctl shims.
//   Pass 6: LLVMO2OptimizationAndX86CodegenPass (LLVM PassBuilder & Codegen)
//           - Runs LLVMVerifyModule, LLVMRunPasses("default<O2>"), and emits
//             native x86-64-v3 ELF64 object code via LLVMTargetMachineEmitToFile.
// ============================================================================

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <dlfcn.h>
#include "../horizon2llvm/aarch64/aarch64_isa_common.hpp"
#include "../horizon2llvm/llvm_backend.hpp"
#include "../horizon2llvm/llvm_ir.hpp"
#include "../horizon2llvm/passes.hpp"
#include <glob.h>
#include <sys/wait.h>
#include <unistd.h>

// Dynamic runtime loader for ANY host libLLVM (LLVM 14..22+ on Arch, Ubuntu, Debian, Fedora)
// Eliminates hardcoded DT_NEEDED on libLLVM-15.so.1 so prebuilt binaries work across distros.
extern "C" {
typedef struct LLVMOpaqueContext *LLVMContextRef;
typedef struct LLVMOpaqueModule *LLVMModuleRef;
typedef struct LLVMOpaqueType *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;
typedef struct LLVMOpaqueBasicBlock *LLVMBasicBlockRef;
typedef struct LLVMOpaqueBuilder *LLVMBuilderRef;
typedef struct LLVMOpaqueMemoryBuffer *LLVMMemoryBufferRef;
typedef struct LLVMOpaqueTarget *LLVMTargetRef;
typedef struct LLVMOpaqueTargetMachine *LLVMTargetMachineRef;
typedef struct LLVMOpaquePassBuilderOptions *LLVMPassBuilderOptionsRef;
typedef struct LLVMOpaqueError *LLVMErrorRef;
typedef int LLVMBool;

typedef enum {
    LLVMVerifierFailureActionAbortProcess = 0,
    LLVMVerifierFailureActionPrintMessage = 1,
    LLVMVerifierFailureActionReturnStatus = 2
} LLVMVerifierFailureAction;
}

#define MALI_LLVM_API_LIST(X) \
    X(void, LLVMInitializeX86TargetInfo, (void), ()) \
    X(void, LLVMInitializeX86Target, (void), ()) \
    X(void, LLVMInitializeX86TargetMC, (void), ()) \
    X(void, LLVMInitializeX86AsmPrinter, (void), ()) \
    X(void, LLVMInitializeX86AsmParser, (void), ()) \
    X(LLVMContextRef, LLVMContextCreate, (void), ()) \
    X(void, LLVMContextDispose, (LLVMContextRef C), (C)) \
    X(LLVMBool, LLVMCreateMemoryBufferWithContentsOfFile, (const char *Path, LLVMMemoryBufferRef *OutMemBuf, char **OutMessage), (Path, OutMemBuf, OutMessage)) \
    X(LLVMMemoryBufferRef, LLVMCreateMemoryBufferWithMemoryRangeCopy, (const char *InputData, size_t InputDataLength, const char *BufferName), (InputData, InputDataLength, BufferName)) \
    X(LLVMBool, LLVMParseIRInContext, (LLVMContextRef ContextRef, LLVMMemoryBufferRef MemBuf, LLVMModuleRef *OutM, char **OutMessage), (ContextRef, MemBuf, OutM, OutMessage)) \
    X(LLVMBool, LLVMVerifyModule, (LLVMModuleRef M, LLVMVerifierFailureAction Action, char **OutMessage), (M, Action, OutMessage)) \
    X(LLVMBool, LLVMPrintModuleToFile, (LLVMModuleRef M, const char *Filename, char **ErrorMessage), (M, Filename, ErrorMessage)) \
    X(void, LLVMDisposeMessage, (char *Message), (Message)) \
    X(void, LLVMDisposeModule, (LLVMModuleRef M), (M)) \
    X(LLVMContextRef, LLVMGetModuleContext, (LLVMModuleRef M), (M)) \
    X(LLVMTypeRef, LLVMVoidTypeInContext, (LLVMContextRef C), (C)) \
    X(LLVMTypeRef, LLVMInt1TypeInContext, (LLVMContextRef C), (C)) \
    X(LLVMTypeRef, LLVMInt8TypeInContext, (LLVMContextRef C), (C)) \
    X(LLVMTypeRef, LLVMInt32TypeInContext, (LLVMContextRef C), (C)) \
    X(LLVMTypeRef, LLVMInt64TypeInContext, (LLVMContextRef C), (C)) \
    X(LLVMTypeRef, LLVMPointerType, (LLVMTypeRef ElementType, unsigned AddressSpace), (ElementType, AddressSpace)) \
    X(LLVMTypeRef, LLVMFunctionType, (LLVMTypeRef ReturnType, LLVMTypeRef *ParamTypes, unsigned ParamCount, LLVMBool IsVarArg), (ReturnType, ParamTypes, ParamCount, IsVarArg)) \
    X(LLVMValueRef, LLVMConstInt, (LLVMTypeRef IntTy, unsigned long long N, LLVMBool SignExtend), (IntTy, N, SignExtend)) \
    X(unsigned long long, LLVMConstIntGetZExtValue, (LLVMValueRef ConstantVal), (ConstantVal)) \
    X(LLVMValueRef, LLVMGetNamedFunction, (LLVMModuleRef M, const char *Name), (M, Name)) \
    X(LLVMValueRef, LLVMAddFunction, (LLVMModuleRef M, const char *Name, LLVMTypeRef FunctionTy), (M, Name, FunctionTy)) \
    X(LLVMValueRef, LLVMGetFirstFunction, (LLVMModuleRef M), (M)) \
    X(LLVMValueRef, LLVMGetNextFunction, (LLVMValueRef Fn), (Fn)) \
    X(LLVMBasicBlockRef, LLVMGetFirstBasicBlock, (LLVMValueRef Fn), (Fn)) \
    X(LLVMBasicBlockRef, LLVMGetNextBasicBlock, (LLVMBasicBlockRef BB), (BB)) \
    X(LLVMValueRef, LLVMGetFirstInstruction, (LLVMBasicBlockRef BB), (BB)) \
    X(LLVMValueRef, LLVMGetNextInstruction, (LLVMValueRef Inst), (Inst)) \
    X(LLVMValueRef, LLVMIsACallInst, (LLVMValueRef Val), (Val)) \
    X(unsigned, LLVMGetNumArgOperands, (LLVMValueRef Instr), (Instr)) \
    X(LLVMValueRef, LLVMGetArgOperand, (LLVMValueRef Inst, unsigned i), (Inst, i)) \
    X(LLVMValueRef, LLVMGetCalledValue, (LLVMValueRef Instr), (Instr)) \
    X(const char *, LLVMGetValueName2, (LLVMValueRef Val, size_t *Length), (Val, Length)) \
    X(void, LLVMReplaceAllUsesWith, (LLVMValueRef OldVal, LLVMValueRef NewVal), (OldVal, NewVal)) \
    X(void, LLVMInstructionEraseFromParent, (LLVMValueRef Inst), (Inst)) \
    X(LLVMBuilderRef, LLVMCreateBuilderInContext, (LLVMContextRef C), (C)) \
    X(void, LLVMPositionBuilderBefore, (LLVMBuilderRef Builder, LLVMValueRef Instr), (Builder, Instr)) \
    X(void, LLVMDisposeBuilder, (LLVMBuilderRef Builder), (Builder)) \
    X(LLVMValueRef, LLVMBuildAdd, (LLVMBuilderRef B, LLVMValueRef LHS, LLVMValueRef RHS, const char *Name), (B, LHS, RHS, Name)) \
    X(LLVMValueRef, LLVMBuildAnd, (LLVMBuilderRef B, LLVMValueRef LHS, LLVMValueRef RHS, const char *Name), (B, LHS, RHS, Name)) \
    X(LLVMValueRef, LLVMBuildZExt, (LLVMBuilderRef B, LLVMValueRef Val, LLVMTypeRef DestTy, const char *Name), (B, Val, DestTy, Name)) \
    X(LLVMValueRef, LLVMBuildTrunc, (LLVMBuilderRef B, LLVMValueRef Val, LLVMTypeRef DestTy, const char *Name), (B, Val, DestTy, Name)) \
    X(LLVMValueRef, LLVMBuildIntToPtr, (LLVMBuilderRef B, LLVMValueRef Val, LLVMTypeRef DestTy, const char *Name), (B, Val, DestTy, Name)) \
    X(LLVMValueRef, LLVMBuildPtrToInt, (LLVMBuilderRef B, LLVMValueRef Val, LLVMTypeRef DestTy, const char *Name), (B, Val, DestTy, Name)) \
    X(LLVMValueRef, LLVMBuildGEP2, (LLVMBuilderRef B, LLVMTypeRef Ty, LLVMValueRef Pointer, LLVMValueRef *Indices, unsigned NumIndices, const char *Name), (B, Ty, Pointer, Indices, NumIndices, Name)) \
    X(LLVMValueRef, LLVMBuildLoad2, (LLVMBuilderRef B, LLVMTypeRef Ty, LLVMValueRef PointerVal, const char *Name), (B, Ty, PointerVal, Name)) \
    X(LLVMValueRef, LLVMBuildStore, (LLVMBuilderRef B, LLVMValueRef Val, LLVMValueRef Ptr), (B, Val, Ptr)) \
    X(LLVMValueRef, LLVMBuildCall2, (LLVMBuilderRef B, LLVMTypeRef Ty, LLVMValueRef Fn, LLVMValueRef *Args, unsigned NumArgs, const char *Name), (B, Ty, Fn, Args, NumArgs, Name)) \
    X(LLVMBool, LLVMGetTargetFromTriple, (const char *Triple, LLVMTargetRef *T, char **ErrorMessage), (Triple, T, ErrorMessage)) \
    X(LLVMTargetMachineRef, LLVMCreateTargetMachine, (LLVMTargetRef T, const char *Triple, const char *CPU, const char *Features, int Level, int Reloc, int CodeModel), (T, Triple, CPU, Features, Level, Reloc, CodeModel)) \
    X(void, LLVMDisposeTargetMachine, (LLVMTargetMachineRef T), (T)) \
    X(LLVMPassBuilderOptionsRef, LLVMCreatePassBuilderOptions, (void), ()) \
    X(void, LLVMDisposePassBuilderOptions, (LLVMPassBuilderOptionsRef Options), (Options)) \
    X(LLVMErrorRef, LLVMRunPasses, (LLVMModuleRef M, const char *Passes, LLVMTargetMachineRef TM, LLVMPassBuilderOptionsRef Options), (M, Passes, TM, Options)) \
    X(char *, LLVMGetErrorMessage, (LLVMErrorRef Err), (Err)) \
    X(void, LLVMDisposeErrorMessage, (char *ErrMsg), (ErrMsg)) \
    X(LLVMBool, LLVMTargetMachineEmitToFile, (LLVMTargetMachineRef T, LLVMModuleRef M, char *Filename, int codegen, char **ErrorMessage), (T, M, Filename, codegen, ErrorMessage))

namespace {
struct DynamicLlvmTable {
    void *handle = nullptr;
    std::string loaded_path;
    LLVMTypeRef (*p_LLVMPointerTypeInContext)(LLVMContextRef, unsigned) = nullptr;
    void (*p_LLVMContextSetOpaquePointers)(LLVMContextRef, LLVMBool) = nullptr;
    void (*p_cxx_setOpaquePointers)(LLVMContextRef, bool) = nullptr;
    void (*p_cxx_enableOpaquePointers)(LLVMContextRef) = nullptr;
    void (*p_LLVMParseCommandLineOptions)(int, const char *const *, const char *) = nullptr;
#define DECL_PTR(ret, name, params, args) ret (*p_##name) params = nullptr;
    MALI_LLVM_API_LIST(DECL_PTR)
#undef DECL_PTR

    bool tryOpen(const char *path) {
        if (!path || !path[0]) return false;
        void *h = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
        if (!h) return false;
        if (!tryOpenHandle(h, path)) {
            dlclose(h);
            return false;
        }
        return true;
    }

    bool tryOpenHandle(void *h, const char *path) {
        void *lookup_h = h ? h : RTLD_DEFAULT;
        if (!dlsym(lookup_h, "LLVMContextCreate") || !dlsym(lookup_h, "LLVMRunPasses")) {
            return false;
        }
        handle = lookup_h;
        loaded_path = path ? path : "RTLD_DEFAULT";
        p_LLVMPointerTypeInContext = reinterpret_cast<LLVMTypeRef (*)(LLVMContextRef, unsigned)>(
            dlsym(lookup_h, "LLVMPointerTypeInContext"));
        p_LLVMContextSetOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, LLVMBool)>(
            dlsym(lookup_h, "LLVMContextSetOpaquePointers"));
        p_cxx_setOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, bool)>(
            dlsym(lookup_h, "_ZNK4llvm11LLVMContext17setOpaquePointersEb"));
        if (!p_cxx_setOpaquePointers) {
            p_cxx_setOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, bool)>(
                dlsym(lookup_h, "_ZN4llvm11LLVMContext17setOpaquePointersEb"));
        }
        p_cxx_enableOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef)>(
            dlsym(lookup_h, "_ZN4llvm11LLVMContext20enableOpaquePointersEv"));
        if (!p_cxx_enableOpaquePointers) {
            p_cxx_enableOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef)>(
                dlsym(lookup_h, "_ZNK4llvm11LLVMContext20enableOpaquePointersEv"));
        }
        p_LLVMParseCommandLineOptions = reinterpret_cast<void (*)(int, const char *const *, const char *)>(
            dlsym(lookup_h, "LLVMParseCommandLineOptions"));
#define LOAD_SYM(ret, name, params, args) \
        p_##name = reinterpret_cast<ret (*) params>(dlsym(lookup_h, #name)); \
        if (!p_##name) { std::fprintf(stderr, "[LLVM_LOADER] Missing symbol %s in %s\n", #name, path); return false; }
        MALI_LLVM_API_LIST(LOAD_SYM)
#undef LOAD_SYM
        return true;
    }

    void ensureLoaded() {
        if (handle) return;
        auto& shared_llvm = ultrs::llvm_backend::LibLLVM::instance();
        if (shared_llvm.available()) {
            void* sh = shared_llvm.raw_handle();
            if (tryOpenHandle(sh, shared_llvm.loaded_path().c_str())) return;
        }
        const char *env_lib = getenv("MALI_HOST_LIBLLVM");
        if (env_lib && tryOpen(env_lib)) return;

        // First check if already linked into process
        if (dlsym(RTLD_DEFAULT, "LLVMContextCreate") && tryOpen(nullptr)) return;

        static const char *const kDirectCandidates[] = {
            "libLLVM.so",
            "libLLVM-22.so", "libLLVM-21.so", "libLLVM-20.so", "libLLVM-19.so",
            "libLLVM-18.so", "libLLVM-17.so", "libLLVM-16.so", "libLLVM-15.so",
            "libLLVM-21.so.1", "libLLVM-20.so.1", "libLLVM-19.so.1", "libLLVM-18.so.1",
            "libLLVM-17.so.1", "libLLVM-16.so.1", "libLLVM-15.so.1", "libLLVM-14.so.1",
        };
        for (const char *c : kDirectCandidates) {
            if (tryOpen(c)) return;
        }

        static const char *const kGlobPatterns[] = {
            "/usr/lib/libLLVM*.so*",
            "/usr/lib64/libLLVM*.so*",
            "/usr/lib/x86_64-linux-gnu/libLLVM*.so*",
            "/usr/lib/llvm-*/lib/libLLVM*.so*",
            "/usr/local/lib/libLLVM*.so*",
        };
        for (const char *pat : kGlobPatterns) {
            glob_t g{};
            if (glob(pat, 0, nullptr, &g) == 0) {
                // Prefer highest version (reverse order)
                for (size_t i = g.gl_pathc; i > 0; --i) {
                    if (tryOpen(g.gl_pathv[i - 1])) {
                        globfree(&g);
                        return;
                    }
                }
                globfree(&g);
            }
        }
        std::fprintf(stderr, "[ERROR] Could not dynamically load any host libLLVM shared library (checked LLVM 14..22 in /usr/lib, /usr/lib/x86_64-linux-gnu, /usr/lib/llvm-*/lib). Set MALI_HOST_LIBLLVM=/path/to/libLLVM.so\n");
        std::exit(1);
    }
};

static DynamicLlvmTable g_llvm;
} // namespace

#define DEF_WRAPPER(ret, name, params, args) \
    static inline ret raw_##name params { g_llvm.ensureLoaded(); return g_llvm.p_##name args; }
MALI_LLVM_API_LIST(DEF_WRAPPER)
#undef DEF_WRAPPER

#define LLVMInitializeX86TargetInfo raw_LLVMInitializeX86TargetInfo
#define LLVMInitializeX86Target raw_LLVMInitializeX86Target
#define LLVMInitializeX86TargetMC raw_LLVMInitializeX86TargetMC
#define LLVMInitializeX86AsmPrinter raw_LLVMInitializeX86AsmPrinter
#define LLVMInitializeX86AsmParser raw_LLVMInitializeX86AsmParser
#define LLVMContextDispose raw_LLVMContextDispose
#define LLVMCreateMemoryBufferWithContentsOfFile raw_LLVMCreateMemoryBufferWithContentsOfFile
#define LLVMCreateMemoryBufferWithMemoryRangeCopy raw_LLVMCreateMemoryBufferWithMemoryRangeCopy
#define LLVMParseIRInContext raw_LLVMParseIRInContext
#define LLVMVerifyModule raw_LLVMVerifyModule
#define LLVMPrintModuleToFile raw_LLVMPrintModuleToFile
#define LLVMDisposeMessage raw_LLVMDisposeMessage
#define LLVMDisposeModule raw_LLVMDisposeModule
#define LLVMGetModuleContext raw_LLVMGetModuleContext
#define LLVMVoidTypeInContext raw_LLVMVoidTypeInContext
#define LLVMInt1TypeInContext raw_LLVMInt1TypeInContext
#define LLVMInt8TypeInContext raw_LLVMInt8TypeInContext
#define LLVMInt32TypeInContext raw_LLVMInt32TypeInContext
#define LLVMInt64TypeInContext raw_LLVMInt64TypeInContext
#define LLVMPointerType raw_LLVMPointerType
#define LLVMFunctionType raw_LLVMFunctionType
#define LLVMConstInt raw_LLVMConstInt
#define LLVMConstIntGetZExtValue raw_LLVMConstIntGetZExtValue
#define LLVMGetNamedFunction raw_LLVMGetNamedFunction
#define LLVMAddFunction raw_LLVMAddFunction
#define LLVMGetFirstFunction raw_LLVMGetFirstFunction
#define LLVMGetNextFunction raw_LLVMGetNextFunction
#define LLVMGetFirstBasicBlock raw_LLVMGetFirstBasicBlock
#define LLVMGetNextBasicBlock raw_LLVMGetNextBasicBlock
#define LLVMGetFirstInstruction raw_LLVMGetFirstInstruction
#define LLVMGetNextInstruction raw_LLVMGetNextInstruction
#define LLVMIsACallInst raw_LLVMIsACallInst
#define LLVMGetNumArgOperands raw_LLVMGetNumArgOperands
#define LLVMGetArgOperand raw_LLVMGetArgOperand
#define LLVMGetCalledValue raw_LLVMGetCalledValue
#define LLVMGetValueName2 raw_LLVMGetValueName2
#define LLVMReplaceAllUsesWith raw_LLVMReplaceAllUsesWith
#define LLVMInstructionEraseFromParent raw_LLVMInstructionEraseFromParent
#define LLVMCreateBuilderInContext raw_LLVMCreateBuilderInContext
#define LLVMPositionBuilderBefore raw_LLVMPositionBuilderBefore
#define LLVMDisposeBuilder raw_LLVMDisposeBuilder
#define LLVMBuildAdd raw_LLVMBuildAdd
#define LLVMBuildAnd raw_LLVMBuildAnd
#define LLVMBuildZExt raw_LLVMBuildZExt
#define LLVMBuildTrunc raw_LLVMBuildTrunc
#define LLVMBuildIntToPtr raw_LLVMBuildIntToPtr
#define LLVMBuildPtrToInt raw_LLVMBuildPtrToInt
#define LLVMBuildGEP2 raw_LLVMBuildGEP2
#define LLVMBuildLoad2 raw_LLVMBuildLoad2
#define LLVMBuildStore raw_LLVMBuildStore
#define LLVMBuildCall2 raw_LLVMBuildCall2
#define LLVMGetTargetFromTriple raw_LLVMGetTargetFromTriple
#define LLVMCreateTargetMachine raw_LLVMCreateTargetMachine
#define LLVMDisposeTargetMachine raw_LLVMDisposeTargetMachine
#define LLVMCreatePassBuilderOptions raw_LLVMCreatePassBuilderOptions
#define LLVMDisposePassBuilderOptions raw_LLVMDisposePassBuilderOptions
#define LLVMRunPasses raw_LLVMRunPasses
#define LLVMGetErrorMessage raw_LLVMGetErrorMessage
#define LLVMDisposeErrorMessage raw_LLVMDisposeErrorMessage
#define LLVMTargetMachineEmitToFile raw_LLVMTargetMachineEmitToFile

static inline LLVMContextRef LLVMContextCreate(void) {
    g_llvm.ensureLoaded();
    static bool s_cl_parsed = false;
    if (!s_cl_parsed) {
        s_cl_parsed = true;
        if (g_llvm.p_LLVMParseCommandLineOptions &&
            (g_llvm.loaded_path.find("14") != std::string::npos ||
             g_llvm.loaded_path.find("15") != std::string::npos)) {
            const char *argv[] = {"horizon2llvm", "-opaque-pointers"};
            g_llvm.p_LLVMParseCommandLineOptions(2, argv, "");
        }
    }
    LLVMContextRef C = g_llvm.p_LLVMContextCreate();
    if (C) {
        if (g_llvm.p_LLVMContextSetOpaquePointers) {
            g_llvm.p_LLVMContextSetOpaquePointers(C, 1);
        } else if (g_llvm.p_cxx_setOpaquePointers) {
            g_llvm.p_cxx_setOpaquePointers(C, true);
        } else if (g_llvm.p_cxx_enableOpaquePointers) {
            g_llvm.p_cxx_enableOpaquePointers(C);
        }
    }
    return C;
}

static inline LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef C, unsigned AddressSpace) {
    g_llvm.ensureLoaded();
    if (g_llvm.p_LLVMPointerTypeInContext)
        return g_llvm.p_LLVMPointerTypeInContext(C, AddressSpace);
    return g_llvm.p_LLVMPointerType(g_llvm.p_LLVMInt8TypeInContext(C), AddressSpace);
}

struct LifterConfig {
    bool is_libcpp = false;
    std::string prefix = "aot";
    std::string plt_table_name = "g_aot_plt_table";
    uint64_t image_base = 0x400000000ULL;
    bool seed_all_dynsyms = false;
};

static inline bool isHotInlinePltSym(const std::string &sym) {
    return sym == "memcpy" || sym == "__memcpy_chk" ||
           sym == "memmove" || sym == "__memmove_chk" ||
           sym == "memset" || sym == "__memset_chk" ||
           sym == "strlen" || sym == "__strlen_chk" ||
           sym == "malloc" || sym == "calloc" || sym == "realloc" || sym == "free" ||
           sym == "_Znwm" || sym == "_ZnwmRKSt9nothrow_t" ||
           sym == "_Znam" || sym == "_ZnamRKSt9nothrow_t" ||
           sym == "_ZdlPv" || sym == "_ZdaPv" || sym == "_ZdlPvm" || sym == "_ZdaPvm" ||
           sym == "__cxa_guard_acquire" || sym == "__cxa_guard_release" || sym == "__cxa_guard_abort";
}

static constexpr uint64_t HLE_MALLOC_MAGIC   = 0xF000FFF0ULL;
static constexpr uint64_t HLE_TLSDESC_MAGIC  = 0xF000FFF4ULL;
static constexpr uint64_t HLE_VK_FREE_MAGIC  = 0xF000FFF6ULL;
static constexpr uint64_t HLE_FREE_MAGIC     = 0xF000FFF8ULL;
static constexpr uint64_t HLE_VK_ALLOC_MAGIC = 0xF000FFFCULL;

static inline int64_t sext64(uint64_t val, unsigned bits) {
    uint64_t m = 1ULL << (bits - 1);
    val &= (bits == 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
    return static_cast<int64_t>((val ^ m) - m);
}

static inline uint32_t readU32(const std::vector<uint8_t> &buf, uint64_t off) {
    uint32_t v = 0;
    std::memcpy(&v, &buf[off], sizeof(v));
    return v;
}

static inline int32_t readS32(const std::vector<uint8_t> &buf, uint64_t off) {
    int32_t v = 0;
    std::memcpy(&v, &buf[off], sizeof(v));
    return v;
}

static inline uint64_t readU64(const std::vector<uint8_t> &buf, uint64_t off) {
    uint64_t v = 0;
    std::memcpy(&v, &buf[off], sizeof(v));
    return v;
}

static inline void writeU64(std::vector<uint8_t> &buf, uint64_t off, uint64_t val) {
    std::memcpy(&buf[off], &val, sizeof(val));
}

static int64_t readSLEB128(const std::vector<uint8_t> &buf, size_t &pos) {
    int64_t res = 0;
    unsigned shift = 0;
    while (true) {
        uint8_t b = buf[pos++];
        res |= static_cast<int64_t>(b & 0x7f) << shift;
        shift += 7;
        if ((b & 0x80) == 0) {
            if (shift < 64 && (b & 0x40)) res |= -(1LL << shift);
            return res;
        }
    }
}

// Unified with horizon2llvm::aarch64_isa (src/aot/horizon2llvm/aarch64/aarch64_isa_common.hpp)
static inline std::pair<uint64_t, uint64_t> decodeBitMasks(unsigned sf, unsigned immN, unsigned imms, unsigned immr) {
    return ultrs::aarch64_isa::decode_bit_masks_pair(sf, immN, imms, immr);
}

static inline uint64_t expandAdvSimdImm(unsigned op, unsigned cmode, unsigned imm8) {
    return ultrs::aarch64_isa::simd_expand_imm(op, cmode, imm8);
}

static inline uint32_t expandVfpFloat32Imm(unsigned imm8) {
    return static_cast<uint32_t>(ultrs::aarch64_isa::vfp_expand_imm(imm8, 32));
}

static inline uint64_t expandVfpFloat64Imm(unsigned imm8) {
    return ultrs::aarch64_isa::vfp_expand_imm(imm8, 64);
}

// ============================================================================
// Pass 1: ELF64LoaderAndCallGraphPass
// ============================================================================
struct SectionInfo {
    uint64_t addr = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t entsize = 0;
};

struct DynSymEntry {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint16_t shndx = 0;
};

class ELF64LoaderAndCallGraphPass {
public:
    std::vector<uint8_t> raw;
    std::vector<uint8_t> elf;
    std::string driver_version = "unknown-64bit";
    std::string build_id_short = "";
    std::vector<std::pair<uint64_t, uint64_t>> text_ranges;
    std::map<std::string, SectionInfo> sections;
    std::map<uint64_t, std::pair<std::string, uint64_t>> func_by_addr;
    std::map<std::string, uint64_t> addr_by_name;
    std::vector<DynSymEntry> dynsyms;
    std::map<uint64_t, std::string> plt_map;
    std::vector<uint64_t> sorted_fstarts;
    std::set<uint64_t> relocated_data_slots;
    std::vector<uint64_t> closure_funcs;
    std::map<uint64_t, std::string> closure_plts;
    uint64_t ctx_malloc_offset = 0x78;
    uint64_t hmi_rva = 0;
    uint64_t kbase_page_shift_rva = 0;
    uint64_t vk_dispatch_table_rva = 0;
    uint64_t vk_dispatch_table_count = 0;
    uint64_t vk_dispatch_table_stride = 0;
    std::vector<std::pair<std::string, uint64_t>> vk_dispatch_entries;
    std::vector<uint64_t> init_array_funcs;
    struct GlobDatReloc {
        uint64_t r_offset;
        uint32_t r_type;
        std::string sym_name;
    };
    std::vector<GlobDatReloc> glob_dat_relocs;
    std::vector<std::pair<std::string, uint64_t>> exported_funcs;
    std::vector<std::pair<std::string, uint64_t>> exported_data;
    uint64_t image_base = 0x400000000ULL;
    bool seed_all_dynsyms = false;

    bool run(const std::string &so_path, uint64_t img_base = 0x400000000ULL, bool seed_all = false) {
        image_base = img_base;
        seed_all_dynsyms = seed_all;
        std::ifstream in(so_path, std::ios::binary);
        if (!in) return false;
        raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (raw.size() < 64 || raw[0] != 0x7f || raw[1] != 'E' || raw[2] != 'L' || raw[3] != 'F' || raw[4] != 2) {
            return false;
        }
        uint16_t machine = *reinterpret_cast<const uint16_t *>(&raw[18]);
        if (machine != 183) return false;

        // Detect version string
        for (size_t i = 0; i + 16 < raw.size(); ++i) {
            if (raw[i] == 'r' && raw[i + 1] >= '0' && raw[i + 1] <= '9') {
                size_t j = i + 1;
                while (j < i + 20 && ((raw[j] >= '0' && raw[j] <= '9') || (raw[j] >= 'a' && raw[j] <= 'z') || raw[j] == '-')) j++;
                std::string cand(reinterpret_cast<const char *>(&raw[i]), j - i);
                if (cand.find('p') != std::string::npos && cand.find("-0") != std::string::npos && cand.size() >= 10) {
                    driver_version = cand;
                    break;
                }
            }
        }

        // Map PT_LOAD segments by p_vaddr
        uint64_t e_phoff = readU64(raw, 32);
        uint16_t e_phentsize = *reinterpret_cast<const uint16_t *>(&raw[54]);
        uint16_t e_phnum = *reinterpret_cast<const uint16_t *>(&raw[56]);
        uint64_t max_vaddr = 0;
        struct PtLoad { uint64_t vaddr, offset, filesz, memsz; uint32_t flags; };
        std::vector<PtLoad> loads;
        for (uint16_t i = 0; i < e_phnum; ++i) {
            uint64_t off = e_phoff + i * e_phentsize;
            uint32_t p_type = readU32(raw, off);
            uint32_t p_flags = readU32(raw, off + 4);
            uint64_t p_offset = readU64(raw, off + 8);
            uint64_t p_vaddr = readU64(raw, off + 16);
            uint64_t p_filesz = readU64(raw, off + 32);
            uint64_t p_memsz = readU64(raw, off + 40);
            if (p_type == 1) {
                loads.push_back({p_vaddr, p_offset, p_filesz, p_memsz, p_flags});
                if (p_vaddr + p_memsz > max_vaddr) max_vaddr = p_vaddr + p_memsz;
            }
        }
        uint64_t img_sz = (max_vaddr + 0xFFFFULL) & ~0xFFFFULL;
        elf.assign(img_sz, 0);
        for (const auto &ld : loads) {
            if (ld.filesz > 0 && ld.offset + ld.filesz <= raw.size()) {
                std::memcpy(&elf[ld.vaddr], &raw[ld.offset], ld.filesz);
            }
            if (ld.flags & 1) text_ranges.push_back({ld.vaddr, ld.vaddr + ld.filesz});
        }

        // Parse ELF64 Section Headers
        uint64_t e_shoff = readU64(raw, 40);
        uint16_t e_shentsize = *reinterpret_cast<const uint16_t *>(&raw[58]);
        uint16_t e_shnum = *reinterpret_cast<const uint16_t *>(&raw[60]);
        uint16_t e_shstrndx = *reinterpret_cast<const uint16_t *>(&raw[62]);
        uint64_t shstr_off = readU64(raw, e_shoff + e_shstrndx * e_shentsize + 24);
        for (uint16_t i = 0; i < e_shnum; ++i) {
            uint64_t off = e_shoff + i * e_shentsize;
            uint32_t sh_name = readU32(raw, off);
            uint64_t sh_addr = readU64(raw, off + 16);
            uint64_t sh_offset = readU64(raw, off + 24);
            uint64_t sh_size = readU64(raw, off + 32);
            uint64_t sh_entsize = readU64(raw, off + 56);
            const char *sname = reinterpret_cast<const char *>(&raw[shstr_off + sh_name]);
            sections[sname] = {sh_addr, sh_offset, sh_size, sh_entsize};
        }

        if (sections.count(".note.gnu.build-id")) {
            auto sec = sections[".note.gnu.build-id"];
            if (sec.offset + 22 <= raw.size()) {
                char hexbuf[16];
                std::snprintf(hexbuf, sizeof(hexbuf), "%02x%02x%02x",
                              raw[sec.offset + 16], raw[sec.offset + 17], raw[sec.offset + 18]);
                build_id_short = hexbuf;
            }
        }

        parseEhFrame();
        parseDynsym();
        applyRelocations();
        parsePlt();
        for (const auto &kv : func_by_addr) sorted_fstarts.push_back(kv.first);

        discoverVulkanAndKbaseEntries();
        computeClosure();
        detectCtxMallocOffset();
        return true;
    }

private:
    bool isExecAddr(uint64_t addr) const {
        for (const auto &r : text_ranges) {
            if (addr >= r.first && addr < r.second) return true;
        }
        return false;
    }

    void parseEhFrame() {
        if (!sections.count(".eh_frame")) return;
        auto sec = sections[".eh_frame"];
        uint64_t pos = sec.offset;
        uint64_t end = sec.offset + sec.size;
        while (pos + 4 <= end) {
            uint32_t len = readU32(raw, pos);
            if (len == 0 || len == 0xFFFFFFFFU) break;
            uint64_t entry_start = pos + 4;
            uint64_t entry_end = entry_start + len;
            if (entry_end > end) break;
            uint32_t cie_id = readU32(raw, entry_start);
            if (cie_id != 0 && entry_start + 12 <= entry_end) {
                int32_t pc_rel = readS32(raw, entry_start + 4);
                uint32_t fn_len = readU32(raw, entry_start + 8);
                uint64_t fn_start = static_cast<uint64_t>(static_cast<int64_t>(sec.addr + (entry_start + 4 - sec.offset)) + pc_rel);
                if (isExecAddr(fn_start) && fn_len > 0 && fn_len < 0x200000) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "sub_%08" PRIx64, fn_start);
                    func_by_addr[fn_start] = {buf, fn_len};
                }
            }
            pos = entry_end;
        }
    }

    void parseDynsym() {
        if (!sections.count(".dynsym") || !sections.count(".dynstr")) return;
        auto dsym = sections[".dynsym"];
        auto dstr = sections[".dynstr"];
        size_t count = dsym.size / 24;
        dynsyms.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t off = dsym.offset + i * 24;
            uint32_t st_name = readU32(raw, off);
            uint8_t st_info = raw[off + 4];
            uint16_t st_shndx = *reinterpret_cast<const uint16_t *>(&raw[off + 6]);
            uint64_t st_value = readU64(raw, off + 8);
            uint64_t st_size = readU64(raw, off + 16);
            std::string name(reinterpret_cast<const char *>(&raw[dstr.offset + st_name]));
            dynsyms[i] = {name, st_value, st_size, st_shndx};
            if (name == "HMI" && st_value != 0) {
                hmi_rva = st_value;
            }
            if (st_value != 0 && (st_info & 0xF) == 2) {
                uint64_t sz = st_size;
                if (sz == 0 && func_by_addr.count(st_value)) sz = func_by_addr[st_value].second;
                func_by_addr[st_value] = {name, sz};
                addr_by_name[name] = st_value;
                if (st_shndx != 0) exported_funcs.push_back({name, st_value});
            } else if (st_value != 0 && st_shndx != 0 && !name.empty()) {
                exported_data.push_back({name, st_value});
            }
        }
    }

    void applyRelocations() {
        if (sections.count(".relr.dyn")) {
            auto rsec = sections[".relr.dyn"];
            uint64_t where = 0;
            for (uint64_t pos = rsec.offset; pos + 8 <= rsec.offset + rsec.size; pos += 8) {
                uint64_t entry = readU64(raw, pos);
                if ((entry & 1ULL) == 0) {
                    where = entry;
                    if (where + 8 <= elf.size()) {
                        uint64_t val = readU64(elf, where) & 0xFFFFFFFFFFFFULL;
                        writeU64(elf, where, val);
                        relocated_data_slots.insert(where);
                    }
                    where += 8;
                } else {
                    uint64_t bitmap = entry >> 1;
                    for (unsigned b = 0; b < 63; ++b) {
                        if ((bitmap >> b) & 1ULL) {
                            uint64_t slot = where + b * 8ULL;
                            if (slot + 8 <= elf.size()) {
                                uint64_t val = readU64(elf, slot) & 0xFFFFFFFFFFFFULL;
                                writeU64(elf, slot, val);
                                relocated_data_slots.insert(slot);
                            }
                        }
                    }
                    where += 63ULL * 8ULL;
                }
            }
        }
        if (!sections.count(".rela.dyn")) return;
        auto sec = sections[".rela.dyn"];
        if (sec.size >= 4 && std::memcmp(&raw[sec.offset], "APS2", 4) == 0) {
            size_t pos = sec.offset + 4;
            int64_t num_relocs = readSLEB128(raw, pos);
            int64_t cur_offset = readSLEB128(raw, pos);
            int64_t cur_addend = 0;
            int64_t done = 0;
            while (done < num_relocs && pos < sec.offset + sec.size) {
                int64_t grp_size = readSLEB128(raw, pos);
                int64_t grp_flags = readSLEB128(raw, pos);
                int64_t grp_off_delta = (grp_flags & 2) ? readSLEB128(raw, pos) : 0;
                int64_t grp_info = (grp_flags & 1) ? readSLEB128(raw, pos) : 0;
                if ((grp_flags & 8) && (grp_flags & 4)) cur_addend += readSLEB128(raw, pos);
                else if (!(grp_flags & 8)) cur_addend = 0;
                for (int64_t g = 0; g < grp_size; ++g) {
                    cur_offset += (grp_flags & 2) ? grp_off_delta : readSLEB128(raw, pos);
                    uint64_t r_info = (grp_flags & 1) ? static_cast<uint64_t>(grp_info) : static_cast<uint64_t>(readSLEB128(raw, pos));
                    if ((grp_flags & 8) && !(grp_flags & 4)) cur_addend += readSLEB128(raw, pos);
                    uint32_t r_type = static_cast<uint32_t>(r_info & 0xFFFFFFFFULL);
                    uint32_t sym_idx = static_cast<uint32_t>(r_info >> 32);
                    if (static_cast<uint64_t>(cur_offset) + 8 <= elf.size()) {
                        if (r_type == 1027) {
                            writeU64(elf, cur_offset, static_cast<uint64_t>(cur_addend));
                            relocated_data_slots.insert(static_cast<uint64_t>(cur_offset));
                        } else if (sym_idx > 0 && sym_idx < dynsyms.size()) {
                            if (dynsyms[sym_idx].shndx != 0 && dynsyms[sym_idx].value != 0) {
                                writeU64(elf, cur_offset, dynsyms[sym_idx].value + cur_addend);
                                relocated_data_slots.insert(static_cast<uint64_t>(cur_offset));
                            } else if (dynsyms[sym_idx].shndx == 0 && (r_type == 1025 || r_type == 257)) {
                                glob_dat_relocs.push_back({static_cast<uint64_t>(cur_offset), r_type, dynsyms[sym_idx].name});
                            }
                        }
                    }
                    done++;
                }
            }
        } else {
            for (size_t pos = sec.offset; pos + 24 <= sec.offset + sec.size; pos += 24) {
                uint64_t r_offset = readU64(raw, pos);
                uint64_t r_info = readU64(raw, pos + 8);
                int64_t r_addend = static_cast<int64_t>(readU64(raw, pos + 16));
                uint32_t r_type = static_cast<uint32_t>(r_info & 0xFFFFFFFFULL);
                uint32_t sym_idx = static_cast<uint32_t>(r_info >> 32);
                if (r_offset + 8 <= elf.size()) {
                    if (r_type == 1027) {
                        writeU64(elf, r_offset, static_cast<uint64_t>(r_addend));
                        relocated_data_slots.insert(r_offset);
                    } else if (sym_idx > 0 && sym_idx < dynsyms.size()) {
                        if (dynsyms[sym_idx].shndx != 0 && dynsyms[sym_idx].value != 0) {
                            writeU64(elf, r_offset, dynsyms[sym_idx].value + r_addend);
                            relocated_data_slots.insert(r_offset);
                        } else if (dynsyms[sym_idx].shndx == 0 && (r_type == 1025 || r_type == 257)) {
                            glob_dat_relocs.push_back({r_offset, r_type, dynsyms[sym_idx].name});
                        }
                    }
                }
            }
        }
    }

    void parsePlt() {
        if (!sections.count(".rela.plt") || !sections.count(".plt")) return;
        auto rplt = sections[".rela.plt"];
        std::map<uint64_t, std::string> got_plt;
        for (size_t i = 0; i < rplt.size / 24; ++i) {
            uint64_t r_offset = readU64(raw, rplt.offset + i * 24);
            uint64_t r_info = readU64(raw, rplt.offset + i * 24 + 8);
            uint32_t sym_idx = static_cast<uint32_t>(r_info >> 32);
            if (sym_idx < dynsyms.size()) got_plt[r_offset] = dynsyms[sym_idx].name;
        }
        auto plt = sections[".plt"];
        for (uint64_t pc = plt.addr; pc + 16 <= plt.addr + plt.size; pc += 4) {
            uint32_t i0 = readU32(elf, pc);
            uint32_t i1 = readU32(elf, pc + 4);
            uint32_t i3 = readU32(elf, pc + 12);
            if ((i0 & 0x9F00001FU) == 0x90000010U && (i1 & 0xFFC003FFU) == 0xF9400211U && i3 == 0xD61F0220U) {
                int64_t imm = sext64(((i0 >> 5) & 0x7FFFFU) << 2 | ((i0 >> 29) & 3U), 21);
                uint64_t page = (pc & ~0xFFFULL) + static_cast<uint64_t>(imm << 12);
                uint64_t ldr_off = ((i1 >> 10) & 0xFFFULL) << 3;
                uint64_t g = page + ldr_off;
                if (got_plt.count(g)) plt_map[pc] = got_plt[g];
            }
        }
    }

public:
    uint64_t findFuncStart(uint64_t addr) const {
        if (func_by_addr.count(addr)) return addr;
        auto it = std::upper_bound(sorted_fstarts.begin(), sorted_fstarts.end(), addr);
        if (it != sorted_fstarts.begin()) {
            --it;
            uint64_t st = *it;
            uint64_t sz = func_by_addr.at(st).second;
            if (addr >= st && addr < st + sz) return st;
        }
        return 0;
    }

private:
    void discoverVulkanAndKbaseEntries() {
        // 1. Discover vk_hal_open from Android HAL Module Info (HMI->methods->open)
        if (hmi_rva != 0 && hmi_rva + 40 <= elf.size()) {
            uint64_t methods_ptr = readU64(elf, hmi_rva + 32) & 0xFFFFFFFFFFFFULL;
            if (methods_ptr != 0 && methods_ptr + 8 <= elf.size()) {
                uint64_t open_fn = readU64(elf, methods_ptr) & 0xFFFFFFFFFFFFULL;
                uint64_t fst = findFuncStart(open_fn);
                if (fst != 0) addr_by_name["vk_hal_open"] = fst;
            }
        }

        // 2. Discover full .data.rel.ro Vulkan ICD dispatch table & core Vulkan entry points
        if (sections.count(".rodata") && sections.count(".data.rel.ro")) {
            auto ro = sections[".rodata"];
            auto relro = sections[".data.rel.ro"];
            const char *anchor = "vkCreateInstance";
            size_t alen = std::strlen(anchor);
            uint64_t anchor_va = 0;
            for (uint64_t off = 0; off + alen + 1 <= ro.size; ++off) {
                if ((off == 0 || elf[ro.addr + off - 1] == 0) &&
                    std::memcmp(&elf[ro.addr + off], anchor, alen + 1) == 0) {
                    anchor_va = ro.addr + off;
                    break;
                }
            }
            if (anchor_va != 0) {
                for (uint64_t pos = relro.addr; pos + 64 <= relro.addr + relro.size; pos += 8) {
                    uint64_t val = readU64(elf, pos) & 0xFFFFFFFFFFFFULL;
                    if (val == anchor_va) {
                        for (uint64_t stride : {40ULL, 48ULL, 56ULL, 64ULL}) {
                            uint64_t nxt = readU64(elf, pos + stride) & 0xFFFFFFFFFFFFULL;
                            if (nxt >= ro.addr && nxt + 2 < ro.addr + ro.size &&
                                elf[nxt] == 'v' && elf[nxt + 1] == 'k') {
                                uint64_t start = pos;
                                while (start >= relro.addr + stride) {
                                    uint64_t p = readU64(elf, start - stride) & 0xFFFFFFFFFFFFULL;
                                    if (p >= ro.addr && p + 2 < ro.addr + ro.size &&
                                        elf[p] == 'v' && elf[p + 1] == 'k') {
                                        start -= stride;
                                    } else {
                                        break;
                                    }
                                }
                                uint64_t end = pos;
                                while (end + stride < relro.addr + relro.size) {
                                    uint64_t p = readU64(elf, end + stride) & 0xFFFFFFFFFFFFULL;
                                    if (p >= ro.addr && p + 2 < ro.addr + ro.size &&
                                        elf[p] == 'v' && elf[p + 1] == 'k') {
                                        end += stride;
                                    } else {
                                        break;
                                    }
                                }
                                vk_dispatch_table_rva = start;
                                vk_dispatch_table_stride = stride;
                                vk_dispatch_table_count = (end - start) / stride + 1;
                                for (uint64_t idx = 0; idx < vk_dispatch_table_count; ++idx) {
                                    uint64_t epos = vk_dispatch_table_rva + idx * vk_dispatch_table_stride;
                                    uint64_t s_va = readU64(elf, epos) & 0xFFFFFFFFFFFFULL;
                                    if (s_va >= ro.addr && s_va < ro.addr + ro.size) {
                                        std::string vname(reinterpret_cast<const char *>(&elf[s_va]));
                                        uint64_t c40 = (vk_dispatch_table_stride >= 48 && epos + 48 <= elf.size())
                                                           ? (readU64(elf, epos + 40) & 0xFFFFFFFFFFFFULL)
                                                           : 0ULL;
                                        uint64_t c32 = (epos + 40 <= elf.size())
                                                           ? (readU64(elf, epos + 32) & 0xFFFFFFFFFFFFULL)
                                                           : 0ULL;
                                        uint64_t fva = 0;
                                        if (c40 != 0 && findFuncStart(c40) != 0) fva = findFuncStart(c40);
                                        else if (c32 != 0 && findFuncStart(c32) != 0) fva = findFuncStart(c32);
                                        if (fva != 0 && !vname.empty()) {
                                            addr_by_name[vname] = fva;
                                            vk_dispatch_entries.push_back({vname, fva});
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            }

            const char *vk_targets[] = {
                "vkGetInstanceProcAddr",
                "vkGetDeviceProcAddr",
                "vkEnumerateInstanceVersion",
                "vkEnumerateInstanceLayerProperties",
                "vkEnumerateInstanceExtensionProperties",
                "vkEnumeratePhysicalDevices",
                "vkEnumeratePhysicalDeviceGroups",
                "vkGetPhysicalDeviceProperties",
                "vkGetPhysicalDeviceFeatures",
                "vkGetPhysicalDeviceFeatures2",
                "vkGetPhysicalDeviceMemoryProperties",
                "vkGetPhysicalDeviceMemoryProperties2",
                "vkGetPhysicalDeviceQueueFamilyProperties",
                "vkGetPhysicalDeviceQueueFamilyProperties2",
                "vkEnumerateDeviceExtensionProperties",
                "vkEnumerateDeviceLayerProperties",
                "vkGetDeviceQueue",
                "vkGetDeviceQueue2",
                "vkGetBufferMemoryRequirements",
                "vkGetBufferMemoryRequirements2",
                "vkGetBufferDeviceAddress",
                "vkGetImageMemoryRequirements",
                "vkGetImageMemoryRequirements2",
                "vkMapMemory",
                "vkUnmapMemory",
                "vkResetFences",
                "vkDestroyFence",
                "vkDestroyDescriptorSetLayout",
                "vkDestroyPipelineLayout",
                "vkCreateShaderModule",
                "vkDestroyShaderModule",
                "vkCmdSetBlendConstants",
                "vkCmdSetDepthBias",
                "vkCmdSetDepthBounds",
                "vkCmdSetLineWidth",
                "vkCmdSetStencilCompareMask",
                "vkCmdSetStencilReference",
                "vkCmdSetStencilWriteMask",
            };
            for (const char *sym : vk_targets) {
                size_t slen = std::strlen(sym);
                uint64_t str_va = 0;
                for (uint64_t off = 0; off + slen + 1 <= ro.size; ++off) {
                    if ((off == 0 || elf[ro.addr + off - 1] == 0) &&
                        std::memcmp(&elf[ro.addr + off], sym, slen + 1) == 0) {
                        str_va = ro.addr + off;
                        break;
                    }
                }
                if (str_va != 0) {
                    for (uint64_t pos = 0; pos + 48 <= relro.size; pos += 8) {
                        uint64_t val = readU64(elf, relro.addr + pos) & 0xFFFFFFFFFFFFULL;
                        if (val == str_va) {
                            uint64_t cand40 = readU64(elf, relro.addr + pos + 40) & 0xFFFFFFFFFFFFULL;
                            uint64_t cand32 = readU64(elf, relro.addr + pos + 32) & 0xFFFFFFFFFFFFULL;
                            uint64_t fva = 0;
                            if (findFuncStart(cand40) != 0) fva = findFuncStart(cand40);
                            else if (findFuncStart(cand32) != 0) fva = findFuncStart(cand32);
                            if (fva != 0) {
                                addr_by_name[sym] = fva;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // 3. Discover mali_gpu_props_get and /dev/mali0 kbase JM & CSF UAPI wrapper functions
        if (sections.count(".text")) {
            auto txt = sections[".text"];
            uint64_t prev_hit = 0;
            uint32_t w1_val = 0;
            uint64_t w1_pc = 0;
            const std::pair<uint32_t, const char*> kbase_ioctl_sigs[] = {
                {0xc0048034U, "kbase_version_check_csf"},
                {0xc0048036U, "kbase_version_check_reserved"},
                {0xc0018036U, "kbase_version_check_reserved"},
                {0x40048001U, "kbase_set_flags"},
                {0x40108002U, "kbase_job_submit"},
                {0x4010801cU, "kbase_soft_event_update"},
                {0x4018800eU, "kbase_mem_jit_init"},
                {0x40088026U, "kbase_mem_exec_init"},
                {0xc040803bU, "kbase_mem_alloc"},
                {0xc0208005U, "kbase_mem_alloc"},
                {0x40088007U, "kbase_mem_free"},
                {0xc0108006U, "kbase_mem_query"},
                {0x40108014U, "kbase_mem_commit"},
                {0xc0208015U, "kbase_mem_alias"},
                {0xc0188016U, "kbase_mem_import"},
                {0x40188017U, "kbase_mem_flags_change"},
                {0x4020800fU, "kbase_mem_sync"},
                {0xc0188033U, "kbase_cs_get_glb_iface"},
                {0x40108024U, "kbase_cs_queue_register"},
                {0x40288028U, "kbase_cs_queue_register_ex"},
                {0x40088029U, "kbase_cs_queue_terminate"},
                {0xc0108027U, "kbase_cs_queue_bind"},
                {0x40088025U, "kbase_cs_queue_kick"},
                {0xc028803aU, "kbase_cs_queue_group_create"},
                {0x4008802bU, "kbase_cs_queue_group_terminate"},
                {0xc0188030U, "kbase_cs_tiler_heap_init"},
                {0x40088031U, "kbase_cs_tiler_heap_term"},
                {0x8008802dU, "kbase_kcpu_queue_create"},
                {0x4008802eU, "kbase_kcpu_queue_delete"},
                {0x4010802fU, "kbase_kcpu_queue_enqueue"},
            };
            for (uint64_t pc = txt.addr; pc + 4 <= txt.addr + txt.size; pc += 4) {
                uint32_t w = readU32(elf, pc);
                if (w == 0x72A80201U) {
                    if (prev_hit != 0 && (pc - prev_hit) == 0x34 && !addr_by_name.count("mali_gpu_props_get")) {
                        uint64_t fst = findFuncStart(pc);
                        if (fst != 0) addr_by_name["mali_gpu_props_get"] = fst;
                    }
                    prev_hit = pc;
                }
                // Check for single-instruction MOVZ W1, #0x8004 (KBASE_IOCTL_POST_TERM = 0x00008004)
                if (w == 0x52900081U && !addr_by_name.count("kbase_post_term")) {
                    uint64_t fst = findFuncStart(pc);
                    if (fst != 0 && func_by_addr.count(fst) && func_by_addr[fst].second <= 0x60) {
                        addr_by_name["kbase_post_term"] = fst;
                    }
                }
                // Check for single-instruction MOVZ W1, #0x802c (KBASE_IOCTL_CS_EVENT_SIGNAL = 0x0000802c)
                if (w == 0x52900581U && !addr_by_name.count("kbase_cs_event_signal")) {
                    uint64_t fst = findFuncStart(pc);
                    if (fst != 0 && func_by_addr.count(fst) && func_by_addr[fst].second <= 0x80) {
                        addr_by_name["kbase_cs_event_signal"] = fst;
                    }
                }
                if ((w & 0x7F80001FU) == 0x52800001U) {
                    unsigned hw = (w >> 21) & 3U;
                    uint32_t imm16 = (w >> 5) & 0xFFFFU;
                    w1_val = imm16 << (hw * 16);
                    w1_pc = pc;
                } else if ((w & 0x7F80001FU) == 0x72800001U && pc - w1_pc <= 24) {
                    unsigned hw = (w >> 21) & 3U;
                    uint32_t imm16 = (w >> 5) & 0xFFFFU;
                    w1_val = (w1_val & ~(0xFFFFU << (hw * 16))) | (imm16 << (hw * 16));
                    for (const auto &sig : kbase_ioctl_sigs) {
                        if (w1_val == sig.first && !addr_by_name.count(sig.second)) {
                            uint64_t fst = findFuncStart(pc);
                            if (fst != 0) {
                                // For kbase_mem_free (0x40088007) and kbase_mem_query (0xc0188006),
                                // ensure we pick the standalone kbase wrapper (sz <= 0x100) rather than kbase_mem_alloc
                                if ((sig.first == 0x40088007U || sig.first == 0xc0108006U) &&
                                    func_by_addr.count(fst) && func_by_addr[fst].second > 0x100) {
                                    continue;
                                }
                                addr_by_name[sig.second] = fst;
                            }
                        }
                    }
                }
            }
            // In JM drivers (r26p0, r49p1_v4), kbase_version_check_jm (0xc0048000) immediately precedes kbase_set_flags
            if (addr_by_name.count("kbase_set_flags") && !addr_by_name.count("kbase_version_check_csf")) {
                uint64_t sf = addr_by_name["kbase_set_flags"];
                for (uint64_t back = 4; back <= 32 && sf >= back; back += 4) {
                    uint64_t prev_fn = findFuncStart(sf - back);
                    if (prev_fn != 0 && prev_fn < sf && (sf - prev_fn) <= 0x80) {
                        addr_by_name["kbase_version_check_jm"] = prev_fn;
                        break;
                    }
                }
            }
            // Auto-detect OSU_CONFIG_CPU_PAGE_SIZE_LOG2 global (.bss RVA) referenced inside kbase_cs_queue_bind or kbase_mem_alloc
            if (addr_by_name.count("kbase_cs_queue_bind")) {
                uint64_t bstart = addr_by_name["kbase_cs_queue_bind"];
                uint64_t bsz = func_by_addr.count(bstart) ? func_by_addr[bstart].second : 0x128;
                for (uint64_t pc = bstart; pc + 8 <= bstart + bsz; pc += 4) {
                    uint32_t i0 = readU32(elf, pc);
                    uint32_t i1 = readU32(elf, pc + 4);
                    if ((i0 & 0x9F000000U) == 0x90000000U && (i1 & 0xFFC00000U) == 0x91000000U) {
                        unsigned rd0 = i0 & 0x1FU;
                        unsigned rd1 = i1 & 0x1FU;
                        unsigned rn1 = (i1 >> 5) & 0x1FU;
                        if (rd0 == rd1 && rd0 == rn1) {
                            int64_t imm21 = sext64(((i0 >> 5) & 0x7FFFFU) << 2 | ((i0 >> 29) & 3U), 21);
                            uint64_t page = (pc & ~0xFFFULL) + static_cast<uint64_t>(imm21 << 12);
                            uint64_t off12 = (i1 >> 10) & 0xFFFULL;
                            kbase_page_shift_rva = page + off12;
                            break;
                        }
                    }
                }
            }
            if (kbase_page_shift_rva == 0 && addr_by_name.count("kbase_mem_alloc")) {
                uint64_t mstart = addr_by_name["kbase_mem_alloc"];
                uint64_t msz = func_by_addr.count(mstart) ? func_by_addr[mstart].second : 0x230;
                for (uint64_t pc = mstart; pc + 24 <= mstart + msz; pc += 4) {
                    uint32_t i0 = readU32(elf, pc);
                    if ((i0 & 0x9F000000U) == 0x90000000U) {
                        unsigned rd0 = i0 & 0x1FU;
                        int64_t imm21 = sext64(((i0 >> 5) & 0x7FFFFU) << 2 | ((i0 >> 29) & 3U), 21);
                        uint64_t page = (pc & ~0xFFFULL) + static_cast<uint64_t>(imm21 << 12);
                        for (uint64_t p2 = pc + 4; p2 <= pc + 24 && p2 + 8 <= mstart + msz; p2 += 4) {
                            uint32_t i1 = readU32(elf, p2);
                            uint32_t i2 = readU32(elf, p2 + 4);
                            uint32_t i3 = readU32(elf, p2 + 8);
                            if ((i1 & 0xFFC00000U) == 0xF9400000U && ((i1 >> 5) & 0x1FU) == rd0 && (i1 & 0x1FU) == rd0) {
                                uint64_t got_off = ((i1 >> 10) & 0xFFFULL) * 8ULL;
                                if (((i2 & 0xFFC003FFU) == (0xB9400000U | (rd0 << 5) | rd0)) ||
                                    ((i3 & 0xFFC003FFU) == (0xB9400000U | (rd0 << 5) | rd0))) {
                                    uint64_t got_addr = page + got_off;
                                    if (got_addr + 8 <= elf.size()) {
                                        kbase_page_shift_rva = readU64(elf, got_addr) & 0xFFFFFFFFFFFFULL;
                                        break;
                                    }
                                }
                            }
                        }
                        if (kbase_page_shift_rva != 0) break;
                    }
                }
            }
        }
    }

    void computeClosure() {
        std::deque<uint64_t> q;
        for (const char *sym : {
                 "cmpbe_v2_deserialize_MBS2_to_C",
                 "cmpbe_v2_init_options",
                 "cmpbe_v2_set_option_value",
                 "cmpbe_v2_free_compilation_result",
                 "cmpbe_v2_compile_multiple_shaders",
                 "cmpbe_v2_compile_single_shader",
                 "cmpbe_blend_shadergen_generate_shader",
                 "cmpbep_v2_get_variant_info",
                 "cmpbe_has_robustness",
                 "cmpbep_shutdown_llvm",
                 "vk_hal_open",
                 "vkGetInstanceProcAddr",
                 "vkGetDeviceProcAddr",
                 "vkEnumerateInstanceVersion",
                 "vkEnumerateInstanceLayerProperties",
                 "vkEnumerateInstanceExtensionProperties",
                 "vkEnumeratePhysicalDevices",
                 "vkEnumeratePhysicalDeviceGroups",
                 "vkGetPhysicalDeviceProperties",
                 "vkGetPhysicalDeviceFeatures",
                 "vkGetPhysicalDeviceFeatures2",
                 "vkGetPhysicalDeviceMemoryProperties",
                 "vkGetPhysicalDeviceMemoryProperties2",
                 "vkGetPhysicalDeviceQueueFamilyProperties",
                 "vkGetPhysicalDeviceQueueFamilyProperties2",
                 "vkEnumerateDeviceExtensionProperties",
                 "vkEnumerateDeviceLayerProperties",
                 "vkGetDeviceQueue",
                 "vkGetDeviceQueue2",
                 "vkGetBufferMemoryRequirements",
                 "vkGetBufferMemoryRequirements2",
                 "vkGetBufferDeviceAddress",
                 "vkGetImageMemoryRequirements",
                 "vkGetImageMemoryRequirements2",
                 "vkMapMemory",
                 "vkUnmapMemory",
                 "vkResetFences",
                 "vkDestroyFence",
                 "vkDestroyDescriptorSetLayout",
                 "vkDestroyPipelineLayout",
                 "vkCreateShaderModule",
                 "vkDestroyShaderModule",
                 "vkCmdSetBlendConstants",
                "vkCmdSetDepthBias",
                "vkCmdSetDepthBounds",
                "vkCmdSetLineWidth",
                "vkCmdSetStencilCompareMask",
                "vkCmdSetStencilReference",
                "vkCmdSetStencilWriteMask",
                 "mali_gpu_props_get",
                 "kbase_version_check_csf",
                 "kbase_version_check_jm",
                 "kbase_version_check_reserved",
                 "kbase_set_flags",
                 "kbase_job_submit",
                 "kbase_post_term",
                 "kbase_soft_event_update",
                 "kbase_mem_jit_init",
                 "kbase_mem_exec_init",
                 "kbase_mem_alloc",
                 "kbase_mem_free",
                 "kbase_mem_query",
                 "kbase_mem_commit",
                 "kbase_mem_alias",
                 "kbase_mem_import",
                 "kbase_mem_flags_change",
                 "kbase_mem_sync",
                 "kbase_cs_get_glb_iface",
                 "kbase_cs_event_signal",
                 "kbase_cs_queue_register",
                 "kbase_cs_queue_register_ex",
                 "kbase_cs_queue_terminate",
                 "kbase_cs_queue_bind",
                 "kbase_cs_queue_kick",
                 "kbase_cs_queue_group_create",
                 "kbase_cs_queue_group_terminate",
                 "kbase_cs_tiler_heap_init",
                 "kbase_cs_tiler_heap_term",
                 "kbase_kcpu_queue_create",
                 "kbase_kcpu_queue_delete",
                 "kbase_kcpu_queue_enqueue",
             }) {
            if (addr_by_name.count(sym)) {
                uint64_t st = findFuncStart(addr_by_name[sym]);
                q.push_back(st ? st : addr_by_name[sym]);
            }
        }
        if (seed_all_dynsyms) {
            for (const auto &kv : addr_by_name) {
                uint64_t st = findFuncStart(kv.second);
                uint64_t cand = st ? st : kv.second;
                if (isExecAddr(cand)) q.push_back(cand);
            }
        }
        // Seed all DT_INIT_ARRAY (.init_array) constructors (e.g. 399 entries in r54p3)
        if (sections.count(".init_array")) {
            auto ia_sec = sections[".init_array"];
            size_t count = static_cast<size_t>(ia_sec.size / 8);
            init_array_funcs.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                uint64_t va = ia_sec.addr + i * 8ULL;
                if (va + 8 > elf.size()) break;
                uint64_t raw_ptr = readU64(elf, va);
                uint64_t rva = (raw_ptr >= image_base) ? (raw_ptr - image_base) : (raw_ptr & 0xFFFFFFFFFFFFULL);
                if (!isExecAddr(rva)) continue;
                uint64_t st = findFuncStart(rva);
                uint64_t cand = st ? st : rva;
                init_array_funcs.push_back(cand);
                q.push_back(cand);
            }
        }
        for (const auto &vk_ent : vk_dispatch_entries) {
            uint64_t faddr = findFuncStart(vk_ent.second);
            if (faddr == 0) faddr = vk_ent.second;
            if (isExecAddr(faddr)) q.push_back(faddr);
        }
        for (uint64_t slot : relocated_data_slots) {
            if (slot + 8 <= elf.size()) {
                uint64_t ptr = readU64(elf, slot) & 0xFFFFFFFFFFFFULL;
                if (func_by_addr.count(ptr)) q.push_back(ptr);
            }
        }
        std::set<uint64_t> visited;
        std::set<uint64_t> scanned_vtables;
        uint64_t relro_lo = 0, relro_hi = 0;
        if (sections.count(".data.rel.ro")) {
            relro_lo = sections[".data.rel.ro"].offset;
            relro_hi = relro_lo + sections[".data.rel.ro"].size;
        }
        std::set<uint64_t> vtable_seeded_funcs;
        auto seedVtableSlots = [&](uint64_t base_addr) {
            if (base_addr == 0 || (base_addr & 7ULL) != 0 || isExecAddr(base_addr)) return;
            if (!scanned_vtables.insert(base_addr).second) return;
            if (relocated_data_slots.count(base_addr)) {
                uint64_t first_ptr = readU64(elf, base_addr) & 0xFFFFFFFFFFFFULL;
                if (func_by_addr.count(first_ptr)) {
                    for (uint64_t slot = base_addr; slot < base_addr + 0x400ULL && slot + 8 <= elf.size(); slot += 8) {
                        if (!relocated_data_slots.count(slot)) break;
                        uint64_t ptr = readU64(elf, slot) & 0xFFFFFFFFFFFFULL;
                        if (!func_by_addr.count(ptr)) break;
                        if (!visited.count(ptr) && vtable_seeded_funcs.insert(ptr).second) q.push_back(ptr);
                    }
                    return;
                }
            }
            if (base_addr >= relro_lo && base_addr < relro_hi) {
                // Also handle struct/opcode tables in .data.rel.ro (e.g. SPIR-V opcode table)
                for (uint64_t slot = base_addr; slot < std::min<uint64_t>(relro_hi, base_addr + 0x3000ULL) && slot + 8 <= elf.size(); slot += 8) {
                    if (!relocated_data_slots.count(slot)) continue;
                    uint64_t ptr = readU64(elf, slot) & 0xFFFFFFFFFFFFULL;
                    if (func_by_addr.count(ptr) && !visited.count(ptr) && vtable_seeded_funcs.insert(ptr).second) {
                        q.push_back(ptr);
                    }
                }
            }
        };
        while (!q.empty()) {
            uint64_t faddr = q.front();
            q.pop_front();
            if (visited.count(faddr)) continue;
            if (plt_map.count(faddr)) {
                closure_plts[faddr] = plt_map[faddr];
                continue;
            }
            uint64_t fstart = findFuncStart(faddr);
            if (fstart != 0 && fstart != faddr) {
                if (!visited.count(fstart)) q.push_back(fstart);
                continue;
            }
            visited.insert(faddr);
            uint64_t fsize = func_by_addr.count(faddr) ? func_by_addr[faddr].second : 4;
            if (fsize == 0) fsize = 4;
            bool follow_adrp_refs = (!addr_by_name.count("vk_hal_open") || faddr != addr_by_name["vk_hal_open"]);
            std::map<unsigned, uint64_t> regs_page;
            for (uint64_t off = 0; off < fsize; off += 4) {
                uint64_t pc = faddr + off;
                uint32_t insn = readU32(elf, pc);
                unsigned top6 = (insn >> 26) & 0x3F;
                if (top6 == 0x25 || top6 == 0x05) {
                    int64_t imm26 = sext64(insn & 0x3FFFFFFU, 26);
                    uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (imm26 << 2));
                    if (plt_map.count(target)) {
                        closure_plts[target] = plt_map[target];
                    } else if (top6 == 0x25 || !(target >= faddr && target < faddr + fsize)) {
                        uint64_t tst = findFuncStart(target);
                        uint64_t cand = tst ? tst : target;
                        if (!visited.count(cand)) q.push_back(cand);
                    }
                }
                if (!follow_adrp_refs) continue;
                if (((insn >> 24) & 0x1F) == 0x10) {
                    unsigned sf = (insn >> 31) & 1;
                    unsigned rd = insn & 0x1F;
                    int64_t imm = sext64(((insn >> 5) & 0x7FFFFU) << 2 | ((insn >> 29) & 3U), 21);
                    if (sf == 0) {
                        uint64_t tgt = static_cast<uint64_t>(static_cast<int64_t>(pc) + imm);
                        if (func_by_addr.count(tgt) && !visited.count(tgt)) q.push_back(tgt);
                    } else {
                        regs_page[rd] = (pc & ~0xFFFULL) + static_cast<uint64_t>(imm << 12);
                    }
                } else if (((insn >> 22) & 0x3FF) == 0x244) {
                    unsigned rn = (insn >> 5) & 0x1F;
                    uint64_t imm12 = (insn >> 10) & 0xFFF;
                    if (regs_page.count(rn)) {
                        uint64_t tgt = regs_page[rn] + imm12;
                        if (func_by_addr.count(tgt) && !visited.count(tgt)) {
                            if (vtable_seeded_funcs.count(faddr)) vtable_seeded_funcs.insert(tgt);
                            q.push_back(tgt);
                        } else if (!vtable_seeded_funcs.count(faddr)) {
                            seedVtableSlots(tgt);
                        }
                    }
                } else if ((insn & 0xFFC00000U) == 0xF9400000U) {
                    unsigned rn = (insn >> 5) & 0x1F;
                    uint64_t imm12 = ((insn >> 10) & 0xFFFULL) << 3;
                    if (regs_page.count(rn)) {
                        uint64_t got_addr = regs_page[rn] + imm12;
                        if (got_addr + 8 <= elf.size()) {
                            uint64_t ptr_val = readU64(elf, got_addr) & 0xFFFFFFFFFFFFULL;
                            if (func_by_addr.count(ptr_val) && !visited.count(ptr_val)) q.push_back(ptr_val);
                        }
                    }
                }
            }
        }
        // Also compute the full transitive BL/B closure of all discovered Vulkan 1.3 dispatch table entrypoints
        // (all 456 vk* functions in r54p3 / 362 in r44p1 / etc.) across the Vulkan ICD, CMPBE, Kbase, and OSU layers.
        uint64_t min_vk_rva = ~0ULL, max_vk_rva = 0ULL;
        std::deque<uint64_t> vk_q;
        for (const auto &vk_ent : vk_dispatch_entries) {
            uint64_t faddr = findFuncStart(vk_ent.second);
            if (faddr == 0) faddr = vk_ent.second;
            if (!isExecAddr(faddr)) continue;
            if (faddr < min_vk_rva) min_vk_rva = faddr;
            if (faddr > max_vk_rva) max_vk_rva = faddr;
            vk_q.push_back(faddr);
        }
        uint64_t vk_lo = (min_vk_rva > 0x20000ULL) ? (min_vk_rva - 0x20000ULL) : 0ULL;
        uint64_t vk_hi = max_vk_rva + 0x120000ULL;
        uint64_t kbase_anchor = addr_by_name.count("kbase_mem_alloc") ? addr_by_name["kbase_mem_alloc"]
                              : (addr_by_name.count("mali_gpu_props_get") ? addr_by_name["mali_gpu_props_get"] : 0ULL);
        uint64_t kb_lo = (kbase_anchor > 0x20000ULL) ? (kbase_anchor - 0x20000ULL) : 0ULL;
        uint64_t kb_hi = kbase_anchor ? (kbase_anchor + 0x30000ULL) : 0ULL;
        const char *env_full = std::getenv("MALI_AOT_FULL_CLOSURE");
        bool lift_full_closure = (!env_full || std::strcmp(env_full, "0") != 0);
        while (!vk_q.empty()) {
            uint64_t faddr = vk_q.front();
            vk_q.pop_front();
            if (visited.count(faddr)) continue;
            if (plt_map.count(faddr)) {
                closure_plts[faddr] = plt_map[faddr];
                continue;
            }
            uint64_t fstart = findFuncStart(faddr);
            if (fstart != 0 && fstart != faddr) {
                if (!visited.count(fstart)) vk_q.push_back(fstart);
                continue;
            }
            if (!isExecAddr(faddr)) continue;
            visited.insert(faddr);
            uint64_t fsize = func_by_addr.count(faddr) ? func_by_addr[faddr].second : 4;
            if (fsize == 0) fsize = 4;
            for (uint64_t off = 0; off < fsize; off += 4) {
                uint64_t pc = faddr + off;
                uint32_t insn = readU32(elf, pc);
                unsigned top6 = (insn >> 26) & 0x3F;
                if (top6 == 0x25 || top6 == 0x05) {
                    int64_t imm26 = sext64(insn & 0x3FFFFFFU, 26);
                    uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (imm26 << 2));
                    if (plt_map.count(target)) {
                        closure_plts[target] = plt_map[target];
                    } else if (top6 == 0x25 || !(target >= faddr && target < faddr + fsize)) {
                        uint64_t tst = findFuncStart(target);
                        uint64_t cand = tst ? tst : target;
                        bool in_vk_or_kb = (cand >= vk_lo && cand <= vk_hi) || (kb_hi > 0 && cand >= kb_lo && cand <= kb_hi) || (cand >= 0x01ad0000ULL && cand <= 0x02f21ab0ULL);
                        if (!visited.count(cand) && (lift_full_closure || in_vk_or_kb)) {
                            vk_q.push_back(cand);
                        }
                    }
                } else {
                    uint64_t cond_target = 0;
                    if ((insn & 0xFF000010U) == 0x54000000U || (insn & 0x7E000000U) == 0x34000000U) {
                        int64_t imm19 = sext64((insn >> 5) & 0x7FFFFU, 19);
                        cond_target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (imm19 << 2));
                    } else if ((insn & 0x7E000000U) == 0x36000000U) {
                        int64_t imm14 = sext64((insn >> 5) & 0x3FFFU, 14);
                        cond_target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (imm14 << 2));
                    }
                    if (cond_target != 0 && !(cond_target >= faddr && cond_target < faddr + fsize)) {
                        if (plt_map.count(cond_target)) {
                            closure_plts[cond_target] = plt_map[cond_target];
                        } else {
                            uint64_t tst = findFuncStart(cond_target);
                            uint64_t cand = tst ? tst : cond_target;
                            bool in_vk_or_kb = (cand >= vk_lo && cand <= vk_hi) || (kb_hi > 0 && cand >= kb_lo && cand <= kb_hi) || (cand >= 0x01ad0000ULL && cand <= 0x02f21ab0ULL);
                            if (!visited.count(cand) && (lift_full_closure || in_vk_or_kb)) {
                                vk_q.push_back(cand);
                            }
                        }
                    }
                }
            }
        }
        closure_funcs.assign(visited.begin(), visited.end());
    }

    void detectCtxMallocOffset() {
        if (!addr_by_name.count("cmpbe_v2_deserialize_MBS2_to_C")) return;
        uint64_t entry = addr_by_name["cmpbe_v2_deserialize_MBS2_to_C"];
        for (uint64_t off = 0; off < 64; off += 4) {
            uint32_t w = readU32(elf, entry + off);
            if ((w & 0xFFC003E0U) == 0xA9400000U) {
                ctx_malloc_offset = ((w >> 15) & 0x7FULL) << 3;
                break;
            }
        }
    }
};

// ============================================================================
// Pass 2 & Pass 3: CFGRecoveryAndPicJumpTablePass + AArch64ToLLVMIRLiftingPass
// ============================================================================
class AArch64ToLLVMIRLiftingPass {
public:
    using IRValue = ultrs::llvm_ir::Value;
    using IRType = ultrs::llvm_ir::TypeKind;
    using IROpTag = ultrs::llvm_ir::OpTag;
    using IRBuilder = ultrs::llvm_ir::IRBuilder;
    using IRModule = ultrs::llvm_ir::Module;
    using IRFunction = ultrs::llvm_ir::Function;
    using IRBasicBlock = ultrs::llvm_ir::BasicBlock;

    const ELF64LoaderAndCallGraphPass &loader;
    const LifterConfig &cfg;
    const std::map<std::string, size_t> &plt_index_map;
    std::set<uint64_t> func_set;
    std::set<uint64_t> called_guest_fns;
    uint64_t tmp_id = 0;
    std::unique_ptr<IRBuilder> B;
    IRValue reg_x_alloca[31];
    IRValue reg_sp_alloca;
    IRValue reg_v_alloca[32];
    IRValue flag_n_alloca;
    IRValue flag_z_alloca;
    IRValue flag_c_alloca;
    IRValue flag_v_alloca;

    struct LiftStats {
        int total_functions = 0;
        int total_instructions = 0;
        int jump_tables_lowered = 0;
        int simd_ops_lifted = 0;
        int crypto_sha1_lifted = 0;
        int atomic_ops_lifted = 0;
    } stats;

    bool instrument_calls = true;
    bool cur_uses_vec = false;

    AArch64ToLLVMIRLiftingPass(const ELF64LoaderAndCallGraphPass &ld,
                               const LifterConfig &config,
                               const std::map<std::string, size_t> &plt_idx,
                               bool inst_calls = true)
        : loader(ld), cfg(config), plt_index_map(plt_idx),
          func_set(ld.closure_funcs.begin(), ld.closure_funcs.end()), instrument_calls(inst_calls) {
        stats.total_functions = static_cast<int>(ld.closure_funcs.size());
    }

    static std::string escapeIrString(const std::string &str) {
        std::ostringstream oss;
        for (unsigned char c : str) {
            if (c >= 32 && c < 127 && c != '"' && c != '\\') oss << static_cast<char>(c);
            else {
                char b[8];
                std::snprintf(b, sizeof(b), "\\%02X", static_cast<unsigned>(c));
                oss << b;
            }
        }
        return oss.str();
    }

    static std::string hex8(uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08" PRIx64, v);
        return buf;
    }

    static IRType intType(bool is64) { return is64 ? IRType::Int64 : IRType::Int32; }
    static std::string intTyStr(bool is64) { return is64 ? "i64" : "i32"; }

    static IRType intTypeForBytes(unsigned bytes) {
        switch (bytes) {
            case 1: return IRType::Int8;
            case 2: return IRType::Int16;
            case 4: return IRType::Int32;
            case 8: return IRType::Int64;
            case 16: return IRType::Int128;
            default: return IRType::Int64;
        }
    }

    static IRType vecIntTypeFor(unsigned lanes, unsigned esz_bits) {
        if (esz_bits == 8) {
            if (lanes == 2) return IRType::Vec2xI8;
            if (lanes == 4) return IRType::Vec4xI8;
            return (lanes == 8) ? IRType::Vec8xI8 : IRType::Vec16xI8;
        }
        if (esz_bits == 16) {
            if (lanes == 2) return IRType::Vec2xI16;
            return (lanes == 4) ? IRType::Vec4xI16 : IRType::Vec8xI16;
        }
        if (esz_bits == 32) return (lanes == 2) ? IRType::Vec2xI32 : IRType::Vec4xI32;
        return IRType::Vec2xI64;
    }

    static IRType vecFloatTypeFor(unsigned lanes, bool fp64) {
        if (fp64) return IRType::Vec2xF64;
        return (lanes == 2) ? IRType::Vec2xF32 : IRType::Vec4xF32;
    }

    static IRType vecBoolTypeFor(unsigned lanes) {
        if (lanes == 16) return IRType::Vec16xI1;
        if (lanes == 8) return IRType::Vec8xI1;
        if (lanes == 4) return IRType::Vec4xI1;
        return IRType::Vec2xI1;
    }

    static IRValue splatIntVec(IRType vt, unsigned lanes, unsigned bits, int64_t val) {
        std::ostringstream ss;
        ss << "<";
        for (unsigned i = 0; i < lanes; ++i) {
            if (i > 0) ss << ", ";
            ss << "i" << bits << " " << val;
        }
        ss << ">";
        return IRValue{vt, ss.str(), true, 0, nullptr};
    }

    static IRValue splatUIntVec(IRType vt, unsigned lanes, unsigned bits, uint64_t val) {
        std::ostringstream ss;
        ss << "<";
        for (unsigned i = 0; i < lanes; ++i) {
            if (i > 0) ss << ", ";
            ss << "i" << bits << " " << val;
        }
        ss << ">";
        return IRValue{vt, ss.str(), true, 0, nullptr};
    }

    static bool isFullVecType(IRType t) {
        return t == IRType::Int128 || t == IRType::Vec16xI8 || t == IRType::Vec8xI16 ||
               t == IRType::Vec4xI32 || t == IRType::Vec2xI64 || t == IRType::Vec4xF32 ||
               t == IRType::Vec2xF64;
    }

    IRValue loadGpr(unsigned r, bool is64 = true) {
        if (r == 31) return IRValue::imm(intType(is64), 0);
        IRValue t = B->CreateLoad(IRType::Int64, reg_x_alloca[r], 8, IROpTag::CpuRegRead, "x", {}, 0, static_cast<int>(r));
        if (!is64) return B->cast("trunc", t, IRType::Int32, "w");
        return t;
    }

    IRValue loadGprOrSp(unsigned r, bool is64 = true) {
        const IRValue &ptr = (r == 31) ? reg_sp_alloca : reg_x_alloca[r];
        IRValue t = B->CreateLoad(IRType::Int64, ptr, 8, IROpTag::CpuRegRead, "xsp", {}, 0, static_cast<int>(r));
        if (!is64) return B->cast("trunc", t, IRType::Int32, "wsp");
        return t;
    }

    void storeGpr(unsigned r, const IRValue &val, bool is64 = true) {
        if (r == 31) return;
        IRValue v64 = is64 ? val : B->cast("zext", val, IRType::Int64, "zext");
        B->CreateStore(v64, reg_x_alloca[r], 8, IROpTag::CpuRegWrite, {}, 0, static_cast<int>(r));
    }

    void storeGprOrSp(unsigned r, const IRValue &val, bool is64 = true) {
        IRValue v64 = is64 ? val : B->cast("zext", val, IRType::Int64, "zext_sp");
        const IRValue &ptr = (r == 31) ? reg_sp_alloca : reg_x_alloca[r];
        B->CreateStore(v64, ptr, 8, IROpTag::CpuRegWrite, {}, 0, static_cast<int>(r));
    }

    IRValue loadVec(unsigned vr, IRType ty) {
        return B->CreateLoad(ty, reg_v_alloca[vr], 16, IROpTag::CpuRegRead, "vld", {}, 0, 64 + static_cast<int>(vr));
    }

    void storeVec(unsigned vr, const IRValue &val, bool zero_full = true) {
        if (zero_full && !isFullVecType(val.type)) {
            B->CreateStore(IRValue::imm(IRType::Int128, 0), reg_v_alloca[vr], 16, IROpTag::RawIR);
        }
        B->CreateStore(val, reg_v_alloca[vr], 16, IROpTag::CpuRegWrite, {}, 0, 64 + static_cast<int>(vr));
    }

    IRValue emitGuestLoad(IRType ty, const IRValue &addr_i64, uint64_t pc, unsigned align = 1) {
        IRValue ptr = B->CreateIntToPtr(addr_i64, "mptr");
        return B->CreateLoad(ty, ptr, align, IROpTag::GuestMemLoad, "gld", addr_i64, pc);
    }

    void emitGuestStore(const IRValue &val, const IRValue &addr_i64, uint64_t pc, unsigned align = 1) {
        IRValue ptr = B->CreateIntToPtr(addr_i64, "mptr");
        B->CreateStore(val, ptr, align, IROpTag::GuestMemStore, addr_i64, pc);
    }

    void emitFlagsLogic(const IRValue &res, bool is64) {
        IRType t = intType(is64);
        IRValue n_cmp = B->icmp("slt", t, res, IRValue::imm(t, 0), "fl_n");
        B->CreateStore(n_cmp, flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        IRValue z_cmp = B->icmp("eq", t, res, IRValue::imm(t, 0), "fl_z");
        B->CreateStore(z_cmp, flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
    }

    void emitFlagsAdd(const IRValue &a, const IRValue &b, const IRValue &cin, const IRValue &res, bool is64) {
        IRType ty = intType(is64);
        IRType ext_ty = is64 ? IRType::Int128 : IRType::Int64;
        unsigned shift = is64 ? 63 : 31;

        IRValue n_cmp = B->icmp("slt", ty, res, IRValue::imm(ty, 0), "fl_n");
        B->CreateStore(n_cmp, flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);

        IRValue z_cmp = B->icmp("eq", ty, res, IRValue::imm(ty, 0), "fl_z");
        B->CreateStore(z_cmp, flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);

        IRValue ea = B->cast("zext", a, ext_ty, "ea");
        IRValue eb = B->cast("zext", b, ext_ty, "eb");
        IRValue ec = B->cast("zext", cin, ext_ty, "ec");
        IRValue s1 = B->binop("add", ext_ty, ea, eb, "s1");
        IRValue s2 = B->binop("add", ext_ty, s1, ec, "s2");
        IRValue csh = B->binop("lshr", ext_ty, s2, IRValue::imm(ext_ty, shift + 1), "csh");
        IRValue c_cmp = B->icmp("ne", ext_ty, csh, IRValue::imm(ext_ty, 0), "fl_c");
        B->CreateStore(c_cmp, flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);

        IRValue all1 = IRValue::imm(ty, ~0ULL);
        IRValue axb = B->binop("xor", ty, a, b, "axb");
        IRValue naxb = B->binop("xor", ty, axb, all1, "naxb");
        IRValue axr = B->binop("xor", ty, a, res, "axr");
        IRValue vand = B->binop("and", ty, naxb, axr, "vand");
        IRValue v_cmp = B->icmp("slt", ty, vand, IRValue::imm(ty, 0), "fl_v");
        B->CreateStore(v_cmp, flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
    }

    IRValue evalCond(unsigned cond) {
        unsigned c_hi = cond >> 1;
        IRValue res;
        if (c_hi == 0) res = B->CreateLoad(IRType::Int1, flag_z_alloca, 1, IROpTag::CpuRegRead, "cond", {}, 0, 32);
        else if (c_hi == 1) res = B->CreateLoad(IRType::Int1, flag_c_alloca, 1, IROpTag::CpuRegRead, "cond", {}, 0, 32);
        else if (c_hi == 2) res = B->CreateLoad(IRType::Int1, flag_n_alloca, 1, IROpTag::CpuRegRead, "cond", {}, 0, 32);
        else if (c_hi == 3) res = B->CreateLoad(IRType::Int1, flag_v_alloca, 1, IROpTag::CpuRegRead, "cond", {}, 0, 32);
        else if (c_hi == 4) {
            IRValue fc = B->CreateLoad(IRType::Int1, flag_c_alloca, 1, IROpTag::CpuRegRead, "fc", {}, 0, 32);
            IRValue fz = B->CreateLoad(IRType::Int1, flag_z_alloca, 1, IROpTag::CpuRegRead, "fz", {}, 0, 32);
            IRValue nz = B->binop("xor", IRType::Int1, fz, IRValue::imm(IRType::Int1, 1), "nz");
            res = B->binop("and", IRType::Int1, fc, nz, "cond");
        } else if (c_hi == 5) {
            IRValue fn = B->CreateLoad(IRType::Int1, flag_n_alloca, 1, IROpTag::CpuRegRead, "fn", {}, 0, 32);
            IRValue fv = B->CreateLoad(IRType::Int1, flag_v_alloca, 1, IROpTag::CpuRegRead, "fv", {}, 0, 32);
            res = B->icmp("eq", IRType::Int1, fn, fv, "cond");
        } else if (c_hi == 6) {
            IRValue fn = B->CreateLoad(IRType::Int1, flag_n_alloca, 1, IROpTag::CpuRegRead, "fn", {}, 0, 32);
            IRValue fv = B->CreateLoad(IRType::Int1, flag_v_alloca, 1, IROpTag::CpuRegRead, "fv", {}, 0, 32);
            IRValue fz = B->CreateLoad(IRType::Int1, flag_z_alloca, 1, IROpTag::CpuRegRead, "fz", {}, 0, 32);
            IRValue nv = B->icmp("eq", IRType::Int1, fn, fv, "nv");
            IRValue nz = B->binop("xor", IRType::Int1, fz, IRValue::imm(IRType::Int1, 1), "nz");
            res = B->binop("and", IRType::Int1, nv, nz, "cond");
        } else {
            return IRValue::imm(IRType::Int1, 1);
        }
        if ((cond & 1) && cond != 0xF) {
            return B->binop("xor", IRType::Int1, res, IRValue::imm(IRType::Int1, 1), "cinv");
        }
        return res;
    }

    IRValue emitExtendReg(unsigned rm, unsigned option, unsigned shift) {
        IRValue raw = loadGpr(rm, true);
        unsigned opt = option & 7;
        IRValue ext = raw;
        if (opt != 3 && opt != 7) {
            unsigned bits = ((opt & 3) == 0) ? 8 : (((opt & 3) == 1) ? 16 : 32);
            IRType sub_t = intTypeForBytes(bits / 8);
            IRValue tr = B->cast("trunc", raw, sub_t, "tr");
            ext = B->cast((opt & 4) ? "sext" : "zext", tr, IRType::Int64, "ext");
        }
        if (shift > 0) {
            return B->binop("shl", IRType::Int64, ext, IRValue::imm(IRType::Int64, shift), "sh");
        }
        return ext;
    }

    IRValue emitShiftReg(const IRValue &val, unsigned shift_type, unsigned amount, bool is64) {
        if (amount == 0) return val;
        IRType ty = intType(is64);
        unsigned st = shift_type & 3;
        IRValue amt = IRValue::imm(ty, amount);
        if (st == 0) return B->binop("shl", ty, val, amt, "sh");
        if (st == 1) return B->binop("lshr", ty, val, amt, "sh");
        if (st == 2) return B->binop("ashr", ty, val, amt, "sh");
        return B->CreateCall(ty, "llvm.fshr." + intTyStr(is64), {val, val, amt}, "sh", IROpTag::RawIR);
    }

    void emitBionicPltCall(const std::string &sym) {
        IRValue cpu = IRValue::cpu_ptr();
        for (unsigned r = 0; r <= 8; ++r) {
            IRValue rv = loadGpr(r, true);
            IRValue rp = B->CreateGEP_i8(cpu, static_cast<int64_t>(r * 8), "plt_in");
            B->CreateStore(rv, rp, 8);
        }
        {
            IRValue lr_v = loadGpr(30, true);
            IRValue lr_p = B->CreateGEP_i8(cpu, 240, "plt_lr");
            B->CreateStore(lr_v, lr_p, 8);
            IRValue sp_v = B->CreateLoad(IRType::Int64, reg_sp_alloca, 8, IROpTag::CpuRegRead, "plt_spv", {}, 0, 31);
            IRValue sp_p = B->CreateGEP_i8(cpu, 248, "plt_spp");
            B->CreateStore(sp_v, sp_p, 8);
        }
        if (cur_uses_vec) {
            for (unsigned r = 0; r < 8; ++r) {
                IRValue vv = B->CreateLoad(IRType::Int128, reg_v_alloca[r], 16, IROpTag::CpuRegRead, "plt_vin", {}, 0, 64 + static_cast<int>(r));
                IRValue pv = B->CreateGEP_i8(cpu, static_cast<int64_t>(272 + r * 16), "plt_vptr");
                B->CreateStore(vv, pv, 16);
            }
        }
        auto it = plt_index_map.find(sym);
        if (!isHotInlinePltSym(sym) && it != plt_index_map.end()) {
            IRValue slot_p = B->CreateGEP_ptr(IRValue::global_sym(cfg.plt_table_name), IRValue::imm_u64(it->second), "plt_slot");
            IRValue fn_p = B->CreateLoad(IRType::Pointer, slot_p, 8, IROpTag::RawIR, "plt_fn");
            B->CreateVoidCallPtr(fn_p, {cpu}, IROpTag::IndirectCall);
        } else {
            B->CreateVoidCall("llvm.mali.bionic.plt64." + sym, {cpu}, IROpTag::IndirectCall);
        }
        if (cur_uses_vec) {
            IRValue pv0 = B->CreateGEP_i8(cpu, 272, "plt_vout");
            IRValue vv0 = B->CreateLoad(IRType::Int128, pv0, 16, IROpTag::RawIR, "plt_vres");
            B->CreateStore(vv0, reg_v_alloca[0], 16, IROpTag::CpuRegWrite, {}, 0, 64);
        }
        for (unsigned r = 0; r < 2; ++r) {
            IRValue ro_p = B->CreateGEP_i8(cpu, static_cast<int64_t>(r * 8), "plt_out");
            IRValue ro_v = B->CreateLoad(IRType::Int64, ro_p, 8, IROpTag::RawIR, "plt_res");
            storeGpr(r, ro_v, true);
        }
    }

    void emitCallToTarget(uint64_t target_rva, uint64_t pc = 0) {
        if (loader.closure_plts.count(target_rva)) {
            emitBionicPltCall(loader.closure_plts.at(target_rva));
        } else if (func_set.count(target_rva)) {
            called_guest_fns.insert(target_rva);
            B->CreateDirectCall(cfg.prefix + "_sub_" + hex8(target_rva), pc, target_rva);
        } else {
            uint64_t fst = loader.findFuncStart(target_rva);
            if (fst != 0 && fst == target_rva && func_set.count(fst)) {
                called_guest_fns.insert(fst);
                B->CreateDirectCall(cfg.prefix + "_sub_" + hex8(fst), pc, fst);
            } else {
                B->CreateVoidCall(cfg.prefix + "_indirect_dispatch",
                                  {IRValue::cpu_ptr(), IRValue::imm_u64(cfg.image_base + target_rva)},
                                  IROpTag::IndirectCall);
            }
        }
    }

    // Pass 2: CFGRecoveryAndPicJumpTablePass helper (supports both inline and loop-hoisted PIC jump tables)
    std::vector<uint64_t> detectPicJumpTable(uint64_t faddr, uint64_t end_addr, uint64_t pc) {
        std::vector<uint64_t> targets;
        if (pc < faddr + 20) return targets;

        unsigned br_reg = (readU32(loader.elf, pc) >> 5) & 0x1FU;
        uint32_t w_ldrb = 0, w_adr = 0;
        uint64_t ldrb_pc = 0, adr_pc = 0;
        uint32_t elem_sz = 0;
        for (uint64_t back = 4; back <= 128 && pc >= faddr + back; back += 4) {
            uint32_t w = readU32(loader.elf, pc - back);
            if (w_ldrb == 0 && (w & 0xFFE00C00U) == 0x38600800U) {
                w_ldrb = w;
                ldrb_pc = pc - back;
                elem_sz = 1;
            } else if (w_ldrb == 0 && (w & 0xFFE00C00U) == 0x78600800U) {
                w_ldrb = w;
                ldrb_pc = pc - back;
                elem_sz = 2;
            } else if (w_ldrb == 0 && ((w & 0xFFE00C00U) == 0xB8600800U || (w & 0xFFE00C00U) == 0xB8A00800U)) {
                w_ldrb = w;
                ldrb_pc = pc - back;
                elem_sz = 4;
            } else if (w_adr == 0 && (w & 0x9F00001FU) == (0x10000000U | br_reg)) {
                w_adr = w;
                adr_pc = pc - back;
            }
        }
        if (w_ldrb == 0 || w_adr == 0 || elem_sz == 0) return targets;

        unsigned tbl_reg = (w_ldrb >> 5) & 0x1FU;
        unsigned idx_reg = (w_ldrb >> 16) & 0x1FU;

        uint32_t w_add = 0, w_adrp = 0;
        uint64_t adrp_pc = 0;
        unsigned adrp_reg = tbl_reg;
        for (int64_t p = static_cast<int64_t>(ldrb_pc) - 4; p >= static_cast<int64_t>(faddr) && p >= static_cast<int64_t>(pc) - 8192; p -= 4) {
            uint32_t w = readU32(loader.elf, static_cast<uint64_t>(p));
            if (w_add == 0 && (w & 0xFF80001FU) == (0x91000000U | tbl_reg)) {
                w_add = w;
                adrp_reg = (w >> 5) & 0x1FU;
            } else if (w_add != 0 && (w & 0x9F00001FU) == (0x90000000U | adrp_reg)) {
                w_adrp = w;
                adrp_pc = static_cast<uint64_t>(p);
                break;
            }
        }
        if (w_add == 0 || w_adrp == 0) return targets;

        int max_idx = -1;
        for (int64_t p = static_cast<int64_t>(ldrb_pc) - 4; p >= static_cast<int64_t>(faddr) && p >= static_cast<int64_t>(ldrb_pc) - 128; p -= 4) {
            uint32_t w = readU32(loader.elf, static_cast<uint64_t>(p));
            if ((w & 0x7F80001FU) == 0x7100001FU) {
                max_idx = static_cast<int>((w >> 10) & 0xFFFU);
                break;
            }
            // Also match AND W_idx, W_src, #mask (32-bit logic immediate with immr == 0 and imms < 8)
            if ((w & 0xFFC0001FU) == (0x12000000U | idx_reg)) {
                unsigned immr = (w >> 16) & 0x3FU;
                unsigned imms = (w >> 10) & 0x3FU;
                if (immr == 0 && imms < 8) {
                    max_idx = (1 << (imms + 1)) - 1;
                    break;
                }
            }
        }
        if (max_idx < 0 || max_idx > 4096) return targets;

        int64_t imm21 = sext64(((w_adrp >> 5) & 0x7FFFFU) << 2 | ((w_adrp >> 29) & 3U), 21);
        uint64_t page = (adrp_pc & ~0xFFFULL) + static_cast<uint64_t>(imm21 << 12);
        uint64_t off12 = (w_add >> 10) & 0xFFFULL;
        uint64_t tbl_addr = page + off12;
        int64_t adr_imm = sext64(((w_adr >> 5) & 0x7FFFFU) << 2 | ((w_adr >> 29) & 3U), 21);
        uint64_t base_addr = static_cast<uint64_t>(static_cast<int64_t>(adr_pc) + adr_imm);
        for (int idx = 0; idx <= max_idx; ++idx) {
            uint64_t eoff = tbl_addr + static_cast<uint64_t>(idx) * elem_sz;
            if (eoff + elem_sz > loader.elf.size()) break;
            int64_t sval = 0;
            if (elem_sz == 1) {
                sval = static_cast<int64_t>(loader.elf[eoff]);
            } else if (elem_sz == 2) {
                uint16_t h = static_cast<uint16_t>(loader.elf[eoff]) | (static_cast<uint16_t>(loader.elf[eoff + 1]) << 8);
                sval = static_cast<int64_t>(h);
            } else {
                uint32_t v = readU32(loader.elf, eoff);
                sval = ((w_ldrb & 0xFFE00C00U) == 0xB8A00800U)
                     ? static_cast<int64_t>(static_cast<int32_t>(v))
                     : static_cast<int64_t>(v);
            }
            uint64_t t_rva = static_cast<uint64_t>(static_cast<int64_t>(base_addr) + (sval << 2));
            if (t_rva >= faddr && t_rva < end_addr) targets.push_back(t_rva);
        }
        return targets;
    }

    void liftFunction(uint64_t faddr, IRModule &M, IRFunction *fn) {
        auto fname_sz = loader.func_by_addr.count(faddr) ? loader.func_by_addr.at(faddr) : std::make_pair("sub_" + hex8(faddr), uint64_t(4));
        uint64_t fsize = fname_sz.second ? fname_sz.second : 4;
        uint64_t end_addr = faddr + fsize;

        // Pass 2: Discover Basic Blocks & PIC Jump Tables
        std::set<uint64_t> leaders = {faddr};
        std::map<uint64_t, std::vector<uint64_t>> jtables;
        for (uint64_t pc = faddr; pc < end_addr; pc += 4) {
            uint32_t insn = readU32(loader.elf, pc);
            unsigned top6 = (insn >> 26) & 0x3F;
            if (top6 == 0x05) {
                uint64_t tgt = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64(insn & 0x3FFFFFFU, 26) << 2));
                if (tgt >= faddr && tgt < end_addr) leaders.insert(tgt);
                if (pc + 4 < end_addr) leaders.insert(pc + 4);
            } else if (((insn >> 24) & 0xFF) == 0x54 || ((insn >> 25) & 0x3F) == 0x1A) {
                uint64_t tgt = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x7FFFFU, 19) << 2));
                if (tgt >= faddr && tgt < end_addr) leaders.insert(tgt);
                if (pc + 4 < end_addr) leaders.insert(pc + 4);
            } else if (((insn >> 25) & 0x3F) == 0x1B) {
                uint64_t tgt = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x3FFFU, 14) << 2));
                if (tgt >= faddr && tgt < end_addr) leaders.insert(tgt);
                if (pc + 4 < end_addr) leaders.insert(pc + 4);
            } else if ((insn & 0xFE000000U) == 0xD6000000U) {
                unsigned opc = (insn >> 21) & 0xF;
                if (opc == 0 || opc == 2 || opc == 4) {
                    if (opc == 0) {
                        auto jt = detectPicJumpTable(faddr, end_addr, pc);
                        if (!jt.empty()) {
                            jtables[pc] = jt;
                            for (uint64_t t : jt) leaders.insert(t);
                        }
                    }
                    if (pc + 4 < end_addr) leaders.insert(pc + 4);
                }
            }
        }

        bool uses_vec = false;
        uint32_t used_gpr_mask = (1U << 0) | (1U << 30);
        for (uint64_t pc = faddr; pc < end_addr; pc += 4) {
            uint32_t insn = readU32(loader.elf, pc);
            unsigned op0 = (insn >> 25) & 0xF;
            if ((op0 & 0x7) == 0x7 || (((op0 & 0x5) == 0x4) && (((insn >> 26) & 1) == 1) && (((insn >> 24) & 0x3FU) != 0x08U))) {
                uses_vec = true;
            }
            unsigned r0 = insn & 0x1F;
            unsigned r1 = (insn >> 5) & 0x1F;
            unsigned r2 = (insn >> 10) & 0x1F;
            unsigned r3 = (insn >> 16) & 0x1F;
            if (r0 < 31) used_gpr_mask |= (1U << r0);
            if (r1 < 31) used_gpr_mask |= (1U << r1);
            if (r2 < 31) used_gpr_mask |= (1U << r2);
            if (r3 < 31) used_gpr_mask |= (1U << r3);
            unsigned top6 = (insn >> 26) & 0x3F;
            if (top6 == 0x25 || top6 == 0x05) {
                int64_t imm26 = sext64(insn & 0x3FFFFFFU, 26);
                uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (imm26 << 2));
                if (loader.closure_plts.count(target)) used_gpr_mask |= 0x1FFU;
            }
        }
        std::vector<uint64_t> sorted_leaders(leaders.begin(), leaders.end());
        cur_uses_vec = uses_vec;
        IRBasicBlock *entry_bb = fn->create_block("entry", faddr);
        B = std::make_unique<IRBuilder>(fn, entry_bb);
        IRValue cpu = IRValue::cpu_ptr();
        for (unsigned r = 0; r < 31; ++r) {
            if ((used_gpr_mask >> r) & 1U) {
                reg_x_alloca[r] = B->CreateNamedAlloca(IRType::Int64, "reg_x" + std::to_string(r), 8);
            }
        }
        reg_sp_alloca = B->CreateNamedAlloca(IRType::Int64, "reg_sp", 8);
        if (uses_vec) {
            for (unsigned r = 0; r < 32; ++r) {
                reg_v_alloca[r] = B->CreateNamedAlloca(IRType::Int128, "reg_v" + std::to_string(r), 16);
            }
        }
        flag_n_alloca = B->CreateNamedAlloca(IRType::Int1, "flag_n", 1);
        flag_z_alloca = B->CreateNamedAlloca(IRType::Int1, "flag_z", 1);
        flag_c_alloca = B->CreateNamedAlloca(IRType::Int1, "flag_c", 1);
        flag_v_alloca = B->CreateNamedAlloca(IRType::Int1, "flag_v", 1);

        auto reloadState = [&](uint32_t gpr_mask, bool include_sp, unsigned vec_count) {
            for (unsigned r = 0; r < 31; ++r) {
                if (!((used_gpr_mask & gpr_mask) >> r & 1U)) continue;
                IRValue p = B->CreateGEP_i8(cpu, static_cast<int64_t>(r * 8), "rl_x");
                IRValue v = B->CreateLoad(IRType::Int64, p, 8, IROpTag::RawIR, "rv_x");
                B->CreateStore(v, reg_x_alloca[r], 8, IROpTag::CpuRegWrite, {}, 0, static_cast<int>(r));
            }
            if (include_sp) {
                IRValue psp = B->CreateGEP_i8(cpu, 248, "rl_sp");
                IRValue vsp = B->CreateLoad(IRType::Int64, psp, 8, IROpTag::RawIR, "rv_sp");
                B->CreateStore(vsp, reg_sp_alloca, 8, IROpTag::CpuRegWrite, {}, 0, 31);
            }
            if (uses_vec && vec_count > 0) {
                for (unsigned r = 0; r < vec_count; ++r) {
                    IRValue pv = B->CreateGEP_i8(cpu, static_cast<int64_t>(272 + r * 16), "rl_v");
                    IRValue vv = B->CreateLoad(IRType::Int128, pv, 16, IROpTag::RawIR, "rv_v");
                    B->CreateStore(vv, reg_v_alloca[r], 16, IROpTag::CpuRegWrite, {}, 0, 64 + static_cast<int>(r));
                }
            }
        };

        auto flushState = [&](uint32_t gpr_mask, bool include_sp, unsigned vec_count) {
            for (unsigned r = 0; r < 31; ++r) {
                if (!((used_gpr_mask & gpr_mask) >> r & 1U)) continue;
                IRValue v = B->CreateLoad(IRType::Int64, reg_x_alloca[r], 8, IROpTag::CpuRegRead, "fv_x", {}, 0, static_cast<int>(r));
                IRValue p = B->CreateGEP_i8(cpu, static_cast<int64_t>(r * 8), "fl_x");
                B->CreateStore(v, p, 8);
            }
            if (include_sp) {
                IRValue vsp = B->CreateLoad(IRType::Int64, reg_sp_alloca, 8, IROpTag::CpuRegRead, "fv_sp", {}, 0, 31);
                IRValue psp = B->CreateGEP_i8(cpu, 248, "fl_sp");
                B->CreateStore(vsp, psp, 8);
            }
            if (uses_vec && vec_count > 0) {
                for (unsigned r = 0; r < vec_count; ++r) {
                    IRValue vv = B->CreateLoad(IRType::Int128, reg_v_alloca[r], 16, IROpTag::CpuRegRead, "fv_v", {}, 0, 64 + static_cast<int>(r));
                    IRValue pv = B->CreateGEP_i8(cpu, static_cast<int64_t>(272 + r * 16), "fl_v");
                    B->CreateStore(vv, pv, 16);
                }
            }
        };

        reloadState(0x7FFF01FFU, true, 16);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        B->CreateStore(IRValue::imm(IRType::Int1, 0), flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
        if (instrument_calls) {
            B->CreateVoidCall("aot_so_trace_fn_enter",
                              {cpu, IRValue::imm_u64(cfg.image_base + faddr),
                               IRValue::global_sym("@." + cfg.prefix + "_fn_str_" + hex8(faddr))});
        }
        B->CreateBr("bb_" + hex8(faddr), faddr);

        for (size_t bi = 0; bi < sorted_leaders.size(); ++bi) {
            uint64_t b_start = sorted_leaders[bi];
            uint64_t b_stop = (bi + 1 < sorted_leaders.size()) ? sorted_leaders[bi + 1] : end_addr;
            IRBasicBlock *cur_bb = fn->create_block("bb_" + hex8(b_start), b_start);
            B->set_insert_point(cur_bb);
            bool terminated = false;

            for (uint64_t pc = b_start; pc < b_stop; pc += 4) {
                uint32_t insn = readU32(loader.elf, pc);
                stats.total_instructions++;
                if (insn == 0) {
                    B->CreateBr("exit_ret", end_addr);
                    terminated = true;
                    break;
                }
                unsigned op0 = (insn >> 25) & 0xF;
                unsigned rd = insn & 0x1F;
                unsigned rn = (insn >> 5) & 0x1F;
                unsigned rm = (insn >> 16) & 0x1F;
                unsigned ra = (insn >> 10) & 0x1F;
                unsigned sf = (insn >> 31) & 1;
                bool is64 = (sf == 1);
                IRType ty_t = intType(is64);
                std::string ty = is64 ? "i64" : "i32";

                // 1. Data Processing -- Immediate
                if ((op0 & 0xE) == 0x8) {
                    unsigned subop = (insn >> 23) & 0x7;
                    if (subop == 0 || subop == 1) {
                        int64_t imm = sext64(((insn >> 5) & 0x7FFFFU) << 2 | ((insn >> 29) & 3U), 21);
                        uint64_t val = sf ? (cfg.image_base + ((pc & ~0xFFFULL) + static_cast<uint64_t>(imm << 12)))
                                          : (cfg.image_base + static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
                        storeGpr(rd, IRValue::imm_u64(val), true);
                    } else if (subop == 2 || subop == 3) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned S = (insn >> 29) & 1;
                        unsigned sh = (insn >> 22) & 1;
                        uint64_t imm12 = static_cast<uint64_t>((insn >> 10) & 0xFFFU) << (sh ? 12 : 0);
                        IRValue a = loadGprOrSp(rn, is64);
                        IRValue res = B->binop(op ? "sub" : "add", ty_t, a, IRValue::imm(ty_t, imm12), "alu");
                        if (S) {
                            uint64_t rhs_v = op ? (is64 ? ~imm12 : static_cast<uint32_t>(~imm12)) : imm12;
                            emitFlagsAdd(a, IRValue::imm(ty_t, rhs_v), IRValue::imm(ty_t, op ? 1ULL : 0ULL), res, is64);
                            storeGpr(rd, res, is64);
                        } else {
                            storeGprOrSp(rd, res, is64);
                        }
                    } else if (subop == 4) {
                        unsigned opc = (insn >> 29) & 3;
                        auto masks = decodeBitMasks(sf, (insn >> 22) & 1, (insn >> 10) & 0x3F, (insn >> 16) & 0x3F);
                        uint64_t mval = is64 ? masks.first : static_cast<uint32_t>(masks.first);
                        IRValue a = loadGpr(rn, is64);
                        const char *op_str = (opc == 0 || opc == 3) ? "and" : (opc == 1 ? "or" : "xor");
                        IRValue res = B->binop(op_str, ty_t, a, IRValue::imm(ty_t, mval), "log");
                        if (opc == 3) {
                            emitFlagsLogic(res, is64);
                            storeGpr(rd, res, is64);
                        } else {
                            storeGprOrSp(rd, res, is64);
                        }
                    } else if (subop == 5) {
                        unsigned opc = (insn >> 29) & 3;
                        unsigned hw = ((insn >> 21) & 3) * 16;
                        uint64_t imm16 = (insn >> 5) & 0xFFFFULL;
                        if (opc == 0) {
                            uint64_t v = is64 ? ~(imm16 << hw) : static_cast<uint32_t>(~(imm16 << hw));
                            storeGpr(rd, IRValue::imm(ty_t, v), is64);
                        } else if (opc == 2) {
                            uint64_t v = is64 ? (imm16 << hw) : static_cast<uint32_t>(imm16 << hw);
                            storeGpr(rd, IRValue::imm(ty_t, v), is64);
                        } else if (opc == 3) {
                            IRValue cur = loadGpr(rd, is64);
                            uint64_t cm = is64 ? ~(0xFFFFULL << hw) : static_cast<uint32_t>(~(0xFFFFULL << hw));
                            uint64_t iv = is64 ? (imm16 << hw) : static_cast<uint32_t>(imm16 << hw);
                            IRValue c1 = B->binop("and", ty_t, cur, IRValue::imm(ty_t, cm), "movk1");
                            IRValue c2 = B->binop("or", ty_t, c1, IRValue::imm(ty_t, iv), "movk2");
                            storeGpr(rd, c2, is64);
                        }
                    } else if (subop == 6) {
                        unsigned opc = (insn >> 29) & 3;
                        unsigned immr = (insn >> 16) & 0x3F;
                        unsigned imms = (insn >> 10) & 0x3F;
                        auto masks = decodeBitMasks(sf, (insn >> 22) & 1, imms, immr);
                        uint64_t wmask = is64 ? masks.first : static_cast<uint32_t>(masks.first);
                        uint64_t tmask = is64 ? masks.second : static_cast<uint32_t>(masks.second);
                        IRValue src = loadGpr(rn, is64);
                        IRValue rot = emitShiftReg(src, 3, immr, is64);
                        IRValue bot = B->binop("and", ty_t, rot, IRValue::imm(ty_t, wmask), "bf_bot");
                        if (opc == 2) {
                            IRValue res = B->binop("and", ty_t, bot, IRValue::imm(ty_t, tmask), "ubfm");
                            storeGpr(rd, res, is64);
                        } else if (opc == 1) {
                            IRValue dst = loadGpr(rd, is64);
                            uint64_t bmask = wmask & tmask;
                            uint64_t nbmask = is64 ? ~bmask : static_cast<uint32_t>(~bmask);
                            IRValue d1 = B->binop("and", ty_t, dst, IRValue::imm(ty_t, nbmask), "bfm1");
                            IRValue b1 = B->binop("and", ty_t, bot, IRValue::imm(ty_t, bmask), "bfm2");
                            IRValue res = B->binop("or", ty_t, d1, b1, "bfm");
                            storeGpr(rd, res, is64);
                        } else {
                            uint64_t ntmask = is64 ? ~tmask : static_cast<uint32_t>(~tmask);
                            IRValue sh_s = B->binop("lshr", ty_t, src, IRValue::imm(ty_t, imms), "sbfm_sh");
                            IRValue bit_s = B->binop("and", ty_t, sh_s, IRValue::imm(ty_t, 1), "sbfm_b");
                            IRValue sext_s = B->binop("sub", ty_t, IRValue::imm(ty_t, 0), bit_s, "sbfm_sx");
                            IRValue top = B->binop("and", ty_t, sext_s, IRValue::imm(ty_t, ntmask), "sbfm_top");
                            IRValue b1 = B->binop("and", ty_t, bot, IRValue::imm(ty_t, tmask), "sbfm_bot");
                            IRValue res = B->binop("or", ty_t, top, b1, "sbfm");
                            storeGpr(rd, res, is64);
                        }
                    } else if (subop == 7) {
                        unsigned imms = (insn >> 10) & 0x3F;
                        IRValue n_val = loadGpr(rn, is64);
                        IRValue m_val = loadGpr(rm, is64);
                        if (imms == 0) {
                            storeGpr(rd, m_val, is64);
                        } else {
                            IRValue res = B->CreateCall(ty_t, "llvm.fshr." + ty, {n_val, m_val, IRValue::imm(ty_t, imms)}, "extr", IROpTag::RawIR);
                            storeGpr(rd, res, is64);
                        }
                    }
                    continue;
                }

                // 2. Branches, System Instructions, PAC/BTI, TPIDR_EL0
                if ((op0 & 0xE) == 0xA) {
                    unsigned top6 = (insn >> 26) & 0x3F;
                    if (top6 == 0x05 || top6 == 0x25) {
                        uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64(insn & 0x3FFFFFFU, 26) << 2));
                        if (top6 == 0x25) {
                            storeGpr(30, IRValue::imm_u64(cfg.image_base + pc + 4), true);
                            if (loader.closure_plts.count(target)) {
                                emitCallToTarget(target, pc);
                            } else {
                                flushState(0x600701FFU, true, 8);
                                emitCallToTarget(target, pc);
                                reloadState(0x000001FFU, false, 8);
                            }
                        } else {
                            if (target >= faddr && target < end_addr) {
                                B->CreateBr("bb_" + hex8(target), target);
                            } else {
                                if (loader.closure_plts.count(target)) {
                                    emitCallToTarget(target, pc);
                                } else {
                                    flushState(0x7FFF01FFU, true, 16);
                                    emitCallToTarget(target, pc);
                                    reloadState(0x000001FFU, false, 8);
                                }
                                B->CreateBr("exit_ret", end_addr);
                            }
                            terminated = true;
                            break;
                        }
                        continue;
                    }
                    if (((insn >> 24) & 0xFF) == 0x54 || ((insn >> 25) & 0x3F) == 0x1A || ((insn >> 25) & 0x3F) == 0x1B) {
                        uint64_t target = 0;
                        IRValue cval;
                        if (((insn >> 24) & 0xFF) == 0x54) {
                            target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x7FFFFU, 19) << 2));
                            cval = evalCond(insn & 0xF);
                        } else if (((insn >> 25) & 0x3F) == 0x1A) {
                            unsigned op = (insn >> 24) & 1;
                            target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x7FFFFU, 19) << 2));
                            IRValue v = loadGpr(rd, is64);
                            cval = B->icmp(op ? "ne" : "eq", ty_t, v, IRValue::imm(ty_t, 0), "cbz");
                        } else {
                            unsigned op = (insn >> 24) & 1;
                            unsigned bpos = ((insn >> 31) << 5) | ((insn >> 19) & 0x1F);
                            target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x3FFFU, 14) << 2));
                            IRValue v = loadGpr(rd, true);
                            IRValue masked = B->binop("and", IRType::Int64, v, IRValue::imm(IRType::Int64, 1ULL << bpos), "tbz_m");
                            cval = B->icmp(op ? "ne" : "eq", IRType::Int64, masked, IRValue::imm(IRType::Int64, 0), "tbz_c");
                        }
                        std::string fallthrough_lbl = (pc + 4 < end_addr) ? ("bb_" + hex8(pc + 4)) : "exit_ret";
                        if (target >= faddr && target < end_addr) {
                            B->CreateCondBr(cval, "bb_" + hex8(target), fallthrough_lbl, target, pc + 4);
                        } else {
                            std::string tc_lbl = "tailcall_" + hex8(pc) + "_" + std::to_string(++tmp_id);
                            B->CreateCondBr(cval, tc_lbl, fallthrough_lbl, target, pc + 4);
                            IRBasicBlock *tc_bb = fn->create_block(tc_lbl, pc);
                            B->set_insert_point(tc_bb);
                            if (loader.closure_plts.count(target)) {
                                emitCallToTarget(target, pc);
                            } else {
                                flushState(0x7FFF01FFU, true, 16);
                                emitCallToTarget(target, pc);
                                reloadState(0x000001FFU, false, 8);
                            }
                            B->CreateBr("exit_ret", end_addr);
                        }
                        terminated = true;
                        break;
                    }
                    if ((insn & 0xFE000000U) == 0xD6000000U) {
                        unsigned opc = (insn >> 21) & 0xF;
                        if (opc == 2 || opc == 4) {
                            if (insn == 0xD65F0BFFU || insn == 0xD65F0FFFU) {
                                B->CreateVoidCall("llvm.mali.arm64.retaa", {});
                            }
                            B->CreateBr("exit_ret", end_addr);
                            terminated = true;
                            break;
                        } else if (opc == 1) {
                            IRValue tgt = loadGpr(rn, true);
                            storeGpr(30, IRValue::imm_u64(cfg.image_base + pc + 4), true);
                            flushState(0x600701FFU, true, 8);
                            B->CreateVoidCall(cfg.prefix + "_indirect_dispatch", {cpu, tgt}, IROpTag::IndirectCall);
                            reloadState(0x000001FFU, false, 8);
                            continue;
                        } else if (opc == 0) {
                            if (jtables.count(pc)) {
                                stats.jump_tables_lowered++;
                                IRValue tgt = loadGpr(rn, true);
                                std::vector<std::pair<uint64_t, std::string>> cases;
                                std::set<uint64_t> seen;
                                for (uint64_t t_rva : jtables[pc]) {
                                    if (seen.insert(t_rva).second) {
                                        cases.emplace_back(cfg.image_base + t_rva, "bb_" + hex8(t_rva));
                                    }
                                }
                                B->CreateSwitch(tgt, "exit_ret", cases);
                                terminated = true;
                                break;
                            } else {
                                IRValue tgt = loadGpr(rn, true);
                                flushState(0x7FFF01FFU, true, 16);
                                B->CreateVoidCall(cfg.prefix + "_indirect_dispatch", {cpu, tgt}, IROpTag::IndirectCall);
                                reloadState(0x000001FFU, false, 8);
                                B->CreateBr("exit_ret", end_addr);
                                terminated = true;
                                break;
                            }
                        }
                    }
                    if ((insn & 0xFFF00000U) == 0xD5000000U || (insn & 0xFFF00000U) == 0xD5100000U ||
                        (insn & 0xFFF00000U) == 0xD5200000U || (insn & 0xFFF00000U) == 0xD5300000U) {
                        unsigned L = (insn >> 21) & 1;
                        unsigned sysreg = (insn >> 5) & 0xFFFF;
                        if (insn == 0xD503233FU || insn == 0xD50323BFU || insn == 0xD503245FU ||
                            insn == 0xD503249FU || insn == 0xD50324DFU) {
                            unsigned imm_hint = (insn >> 5) & 0x7F;
                            B->CreateVoidCall("llvm.mali.arm64.hint", {IRValue::imm_i32(static_cast<int64_t>(imm_hint))});
                            continue;
                        }
                        if ((insn & 0xFFE00000U) == 0xD5200000U && L == 1) {
                            IRValue tp = B->CreateCall(IRType::Int64, "llvm.mali.arm64.mrs",
                                                       {cpu, IRValue::imm_i32(static_cast<int64_t>(sysreg))},
                                                       "mrs_val", IROpTag::RawIR);
                            storeGpr(rd, tp, true);
                        }
                        continue;
                    }
                }

                // 3. Loads, Stores, and Load-Acquire / Store-Release Atomics
                if ((op0 & 0x5) == 0x4) {
                    unsigned size = (insn >> 30) & 3;
                    unsigned vr = (insn >> 26) & 1;
                    unsigned opc = (insn >> 22) & 3;

                    // Compare-and-Swap (CAS / CASA / CASL / CASAL) via shared ultrs::aarch64_isa::decode_cas
                    ultrs::aarch64_isa::CasInsn cas_info;
                    if (vr == 0 && ultrs::aarch64_isa::decode_cas(insn, cas_info)) {
                        stats.atomic_ops_lifted++;
                        unsigned cs_sz = cas_info.size;
                        unsigned rs = cas_info.rs;
                        IRValue addr_val = loadGprOrSp(cas_info.rn, true);
                        IRValue ptr_v = B->CreateIntToPtr(addr_val, "cas_ptr");
                        IRType mty_t = intTypeForBytes(1U << cs_sz);
                        IRValue cmp_v = loadGpr(rs, cs_sz == 3);
                        IRValue new_v = loadGpr(cas_info.rt, cs_sz == 3);
                        if (cs_sz < 2) {
                            cmp_v = B->cast("trunc", cmp_v, mty_t, "cas_ctr");
                            new_v = B->cast("trunc", new_v, mty_t, "cas_ntr");
                        }
                        IRValue old_v = B->CreateAtomicCmpXchgVal(
                            mty_t, ptr_v, cmp_v, new_v, 1U << cs_sz);
                        if (rs != 31U) {
                            if (cs_sz < 2) {
                                old_v = B->cast("zext", old_v, IRType::Int32, "cas_zx");
                            }
                            storeGpr(rs, old_v, cs_sz == 3);
                        }
                        continue;
                    }

                    // LSE Atomics (LDADD*, LDCLR*, LDEOR*, LDSET*, LDSMAX*, LDSMIN*, LDUMAX*, LDUMIN*, SWP*) via shared ultrs::aarch64_isa::decode_lse_atomic
                    ultrs::aarch64_isa::LseAtomicInsn lse_info;
                    if (vr == 0 && ultrs::aarch64_isa::decode_lse_atomic(insn, lse_info)) {
                        stats.atomic_ops_lifted++;
                        IRValue addr_val = loadGprOrSp(lse_info.rn, true);
                        IRValue ptr_v = B->CreateIntToPtr(addr_val, "lse_ptr");
                        IRType mty_t = intTypeForBytes(1U << lse_info.size);
                        IRValue val = loadGpr(lse_info.rs, lse_info.size == 3);
                        if (lse_info.size < 2) {
                            val = B->cast("trunc", val, mty_t, "lse_tr");
                        }
                        if (lse_info.invert_src) {
                            val = B->binop("xor", mty_t, val, IRValue::imm(mty_t, ~0ULL), "lse_inv");
                        }
                        IRValue old_v = B->CreateAtomicRMW(lse_info.llvm_rmw_op, mty_t, ptr_v, val, 1U << lse_info.size);
                        if (lse_info.size < 2) {
                            old_v = B->cast("zext", old_v, IRType::Int32, "lse_zx");
                        }
                        storeGpr(lse_info.rt, old_v, lse_info.size == 3);
                        continue;
                    }

                    // ARMv8.4-RCPC2 LDAPUR / STLUR via shared ultrs::aarch64_isa::decode_rcpc_unscaled
                    ultrs::aarch64_isa::RcpcUnscaledInsn rcpc_info;
                    if (vr == 0 && ultrs::aarch64_isa::decode_rcpc_unscaled(insn, rcpc_info)) {
                        stats.atomic_ops_lifted++;
                        IRValue base_val = loadGprOrSp(rcpc_info.rn, true);
                        IRValue addr_val = B->binop("add", IRType::Int64, base_val, IRValue::imm(IRType::Int64, static_cast<uint64_t>(rcpc_info.imm9)), "rcpc_a");
                        IRValue ptr_v = B->CreateIntToPtr(addr_val, "rcpc_p");
                        IRType mty_t = intTypeForBytes(1U << rcpc_info.size);
                        if (rcpc_info.opc == 0) {
                            IRValue val = loadGpr(rcpc_info.rt, rcpc_info.size == 3);
                            if (rcpc_info.size < 2) {
                                val = B->cast("trunc", val, mty_t, "stlur_tr");
                            }
                            B->CreateAtomicStore(mty_t, val, ptr_v, 1U << rcpc_info.size, llvm::AtomicOrdering::Release);
                        } else if (rcpc_info.opc == 1) {
                            IRValue lv = B->CreateAtomicLoad(mty_t, ptr_v, 1U << rcpc_info.size, llvm::AtomicOrdering::Acquire);
                            if (rcpc_info.size < 2) {
                                lv = B->cast("zext", lv, IRType::Int32, "ldapur_zx");
                            }
                            storeGpr(rcpc_info.rt, lv, rcpc_info.size == 3);
                        } else if (rcpc_info.opc == 2 && rcpc_info.size < 3) {
                            IRValue lv = B->CreateAtomicLoad(mty_t, ptr_v, 1U << rcpc_info.size, llvm::AtomicOrdering::Acquire);
                            IRValue sx = B->cast("sext", lv, IRType::Int64, "ldapurs_sx");
                            storeGpr(rcpc_info.rt, sx, true);
                        } else if (rcpc_info.opc == 3 && rcpc_info.size < 2) {
                            IRValue lv = B->CreateAtomicLoad(mty_t, ptr_v, 1U << rcpc_info.size, llvm::AtomicOrdering::Acquire);
                            IRValue sx = B->cast("sext", lv, IRType::Int32, "ldapursw_sx");
                            storeGpr(rcpc_info.rt, sx, false);
                        }
                        continue;
                    }

                    // Load-Acquire / Store-Release / Exclusive (LDAR, LDARB, LDARH, LDAXR, STLR, STLRB, STLRH, STLXR)
                    // and ARMv8.3-RCPC LDAPR / LDAPRB / LDAPRH ((insn & 0x3FFFFC00) == 0x38BFC000).
                    if ((((insn >> 24) & 0x3FU) == 0x08U && ((insn >> 21) & 1U) == 0U) || (insn & 0x3FFFFC00U) == 0x38BFC000U) {
                        stats.atomic_ops_lifted++;
                        bool is_ldapr = (insn & 0x3FFFFC00U) == 0x38BFC000U;
                        unsigned L = is_ldapr ? 1U : ((insn >> 22) & 1);
                        unsigned o2 = is_ldapr ? 1U : ((insn >> 23) & 1);
                        IRValue addr_val = loadGprOrSp(rn, true);
                        IRValue ptr_v = B->CreateIntToPtr(addr_val, "at_ptr");
                        IRType mty_t = intTypeForBytes(1U << size);
                        if (L == 1) {
                            IRValue lv = B->CreateAtomicLoad(mty_t, ptr_v, 1U << size, llvm::AtomicOrdering::Acquire);
                            if (size < 2) {
                                lv = B->cast("zext", lv, IRType::Int32, "ldar_zx");
                            }
                            storeGpr(rd, lv, size == 3);
                        } else {
                            IRValue val = loadGpr(rd, size == 3);
                            if (size < 2) {
                                val = B->cast("trunc", val, mty_t, "stlr_tr");
                            }
                            B->CreateAtomicStore(mty_t, val, ptr_v, 1U << size, llvm::AtomicOrdering::Release);
                            if (o2 == 0) {
                                storeGpr(rm, IRValue::imm(IRType::Int32, 0), false);
                            }
                        }
                        continue;
                    }

                    if (((insn >> 27) & 7) == 5) {
                        unsigned mode = (insn >> 23) & 3;
                        unsigned L = (insn >> 22) & 1;
                        unsigned rt2 = (insn >> 10) & 0x1F;
                        int64_t imm7 = sext64((insn >> 15) & 0x7FU, 7);
                        unsigned scale = vr ? (2 + size) : (2 + (size >> 1));
                        int64_t off = imm7 << scale;
                        IRValue base_val = loadGprOrSp(rn, true);
                        IRValue addr_val = base_val;
                        if (mode != 1 && off != 0) {
                            addr_val = B->binop("add", IRType::Int64, base_val, IRValue::imm(IRType::Int64, static_cast<uint64_t>(off)), "ldp_a");
                        }
                        unsigned esz = 1U << scale;
                        IRValue addr2_val = B->binop("add", IRType::Int64, addr_val, IRValue::imm_u64(esz), "ldp_a2");
                        if (vr == 0) {
                            IRType mty_t = (size == 0) ? IRType::Int32 : IRType::Int64;
                            if (L == 0) {
                                IRValue v1 = loadGpr(rd, size != 0), v2 = loadGpr(rt2, size != 0);
                                emitGuestStore(v1, addr_val, pc, 1);
                                emitGuestStore(v2, addr2_val, pc, 1);
                            } else {
                                if (size == 0) {
                                    IRValue v1 = emitGuestLoad(IRType::Int32, addr_val, pc, 1);
                                    IRValue v2 = emitGuestLoad(IRType::Int32, addr2_val, pc, 1);
                                    storeGpr(rd, v1, false);
                                    storeGpr(rt2, v2, false);
                                } else if (size == 1) {
                                    IRValue v1 = emitGuestLoad(IRType::Int32, addr_val, pc, 1);
                                    IRValue v2 = emitGuestLoad(IRType::Int32, addr2_val, pc, 1);
                                    IRValue sx1 = B->cast("sext", v1, IRType::Int64, "sx1");
                                    IRValue sx2 = B->cast("sext", v2, IRType::Int64, "sx2");
                                    storeGpr(rd, sx1, true);
                                    storeGpr(rt2, sx2, true);
                                } else {
                                    IRValue v1 = emitGuestLoad(mty_t, addr_val, pc, 1);
                                    IRValue v2 = emitGuestLoad(mty_t, addr2_val, pc, 1);
                                    storeGpr(rd, v1, true);
                                    storeGpr(rt2, v2, true);
                                }
                            }
                        } else {
                            IRType vty_t = intTypeForBytes(esz);
                            if (L == 0) {
                                IRValue v1 = loadVec(rd, vty_t), v2 = loadVec(rt2, vty_t);
                                emitGuestStore(v1, addr_val, pc, 1);
                                emitGuestStore(v2, addr2_val, pc, 1);
                            } else {
                                IRValue v1 = emitGuestLoad(vty_t, addr_val, pc, 1);
                                IRValue v2 = emitGuestLoad(vty_t, addr2_val, pc, 1);
                                storeVec(rd, v1);
                                storeVec(rt2, v2);
                            }
                        }
                        if (mode == 1 || mode == 3) {
                            IRValue wb = B->binop("add", IRType::Int64, base_val, IRValue::imm(IRType::Int64, static_cast<uint64_t>(off)), "wb");
                            storeGprOrSp(rn, wb, true);
                        }
                        continue;
                    }

                    if (((insn >> 27) & 7) == 7) {
                        unsigned p1_bit = (insn >> 24) & 1;
                        IRValue base_val = loadGprOrSp(rn, true);
                        unsigned scale = (vr == 1 && (opc & 2) != 0 && size == 0) ? 4 : size;
                        bool writeback = false;
                        IRValue wb_val = base_val, addr_val = base_val;
                        if (p1_bit == 1) {
                            uint64_t imm12 = static_cast<uint64_t>((insn >> 10) & 0xFFFU) << scale;
                            if (imm12 != 0) {
                                addr_val = B->binop("add", IRType::Int64, base_val, IRValue::imm_u64(imm12), "addr");
                            }
                        } else if (((insn >> 21) & 1) == 0) {
                            unsigned mode = (insn >> 10) & 3;
                            int64_t imm9 = sext64((insn >> 12) & 0x1FFU, 9);
                            IRValue off_val = B->binop("add", IRType::Int64, base_val, IRValue::imm(IRType::Int64, static_cast<uint64_t>(imm9)), "addr_imm9");
                            if (mode == 1) { addr_val = base_val; wb_val = off_val; writeback = true; }
                            else if (mode == 3) { addr_val = off_val; wb_val = off_val; writeback = true; }
                            else { addr_val = off_val; }
                        } else {
                            unsigned option = (insn >> 13) & 7;
                            unsigned S = (insn >> 12) & 1;
                            IRValue ext_rm = emitExtendReg(rm, option, S ? scale : 0);
                            addr_val = B->binop("add", IRType::Int64, base_val, ext_rm, "addr_reg");
                        }
                        IRType mty_t = intTypeForBytes(1U << scale);
                        if (vr == 0) {
                            if (opc == 0) {
                                IRValue val = loadGpr(rd, size == 3);
                                if (size < 2) {
                                    val = B->cast("trunc", val, mty_t, "str_tr");
                                }
                                emitGuestStore(val, addr_val, pc, 1);
                            } else if (opc == 1) {
                                IRValue lv = emitGuestLoad(mty_t, addr_val, pc, 1);
                                if (size < 2) {
                                    lv = B->cast("zext", lv, IRType::Int32, "ldr_zx");
                                }
                                storeGpr(rd, lv, size == 3);
                            } else if (opc == 2 && size < 3) {
                                IRValue lv = emitGuestLoad(mty_t, addr_val, pc, 1);
                                IRValue sx = B->cast("sext", lv, IRType::Int64, "ldr_sx64");
                                storeGpr(rd, sx, true);
                            } else if (opc == 3 && size < 2) {
                                IRValue lv = emitGuestLoad(mty_t, addr_val, pc, 1);
                                IRValue sx = B->cast("sext", lv, IRType::Int32, "ldr_sx32");
                                storeGpr(rd, sx, false);
                            }
                        } else {
                            if ((opc & 1) == 0) {
                                IRValue vv = loadVec(rd, mty_t);
                                emitGuestStore(vv, addr_val, pc, 1);
                            } else {
                                IRValue vv = emitGuestLoad(mty_t, addr_val, pc, 1);
                                storeVec(rd, vv);
                            }
                        }
                        if (writeback) storeGprOrSp(rn, wb_val, true);
                        continue;
                    }

                    if (((insn >> 24) & 0xBF) == 0x0D) {
                        stats.simd_ops_lifted++;
                        unsigned Q = (insn >> 30) & 1;
                        unsigned opcode = (insn >> 13) & 7;
                        unsigned S = (insn >> 12) & 1;
                        unsigned sz_bits = (insn >> 10) & 3;
                        unsigned esz = (opcode == 4 || opcode == 5) ? ((sz_bits & 1) ? 8 : 4) : (1U << (opcode >> 1));
                        unsigned lane = (esz == 8) ? Q : ((esz == 4) ? ((Q << 1) | S) : ((Q << 2) | (S << 1) | (sz_bits >> 1)));
                        IRType vty_t = vecIntTypeFor(16 / esz, esz * 8);
                        IRType ety_t = intTypeForBytes(esz);
                        IRValue addr_val = loadGprOrSp(rn, true);
                        IRValue elem = emitGuestLoad(ety_t, addr_val, pc, 1);
                        IRValue cur_v = loadVec(rd, vty_t);
                        IRValue new_v = B->CreateInsertElement(vty_t, cur_v, elem, lane, "ld1_v");
                        storeVec(rd, new_v, false);
                        continue;
                    }

                    if (((insn >> 23) & 0x1F) == 0x18 || ((insn >> 23) & 0x1F) == 0x19) {
                        stats.simd_ops_lifted++;
                        unsigned post_idx = (insn >> 23) & 1U;
                        unsigned L = (insn >> 22) & 1U;
                        unsigned Q = (insn >> 30) & 1U;
                        unsigned ld_op = (insn >> 12) & 0xFU;
                        unsigned esz_log = (insn >> 10) & 3U;
                        unsigned esz = 1U << esz_log;
                        unsigned reg_bytes = Q ? 16U : 8U;
                        unsigned nregs = 1U;
                        bool is_struct = true;
                        if (ld_op == 0x0U) { nregs = 4; is_struct = true; }
                        else if (ld_op == 0x2U) { nregs = 4; is_struct = false; }
                        else if (ld_op == 0x4U) { nregs = 3; is_struct = true; }
                        else if (ld_op == 0x6U) { nregs = 3; is_struct = false; }
                        else if (ld_op == 0x7U) { nregs = 1; is_struct = false; }
                        else if (ld_op == 0x8U) { nregs = 2; is_struct = true; }
                        else if (ld_op == 0xAU) { nregs = 2; is_struct = false; }
                        IRValue addr_val = loadGprOrSp(rn, true);
                        if (!is_struct) {
                            IRType rty_t = Q ? IRType::Int128 : IRType::Int64;
                            for (unsigned r_idx = 0; r_idx < nregs; ++r_idx) {
                                unsigned vreg = (rd + r_idx) & 31U;
                                IRValue ma = B->binop("add", IRType::Int64, addr_val, IRValue::imm_u64(r_idx * reg_bytes), "ld1_a");
                                if (L) {
                                    IRValue mv = emitGuestLoad(rty_t, ma, pc, 1);
                                    storeVec(vreg, mv, Q == 0);
                                } else {
                                    IRValue vv = loadVec(vreg, rty_t);
                                    emitGuestStore(vv, ma, pc, 1);
                                }
                            }
                        } else {
                            unsigned lanes = reg_bytes / esz;
                            IRType ety_t = intTypeForBytes(esz);
                            IRType vty_t = vecIntTypeFor(lanes, esz * 8U);
                            if (L) {
                                for (unsigned r_idx = 0; r_idx < nregs; ++r_idx) {
                                    B->CreateStore(IRValue::imm(IRType::Int128, 0), reg_v_alloca[(rd + r_idx) & 31U], 16);
                                }
                            }
                            for (unsigned lane = 0; lane < lanes; ++lane) {
                                for (unsigned r_idx = 0; r_idx < nregs; ++r_idx) {
                                    unsigned vreg = (rd + r_idx) & 31U;
                                    unsigned m_off = (lane * nregs + r_idx) * esz;
                                    IRValue ma = B->binop("add", IRType::Int64, addr_val, IRValue::imm_u64(m_off), "ldn_a");
                                    if (L) {
                                        IRValue mv = emitGuestLoad(ety_t, ma, pc, 1);
                                        IRValue cv = loadVec(vreg, vty_t);
                                        IRValue nv = B->CreateInsertElement(vty_t, cv, mv, lane, "ldn_nv");
                                        storeVec(vreg, nv, Q == 0);
                                    } else {
                                        IRValue cv = loadVec(vreg, vty_t);
                                        IRValue ev = B->CreateExtractElement(vty_t, ety_t, cv, lane, "stn_e");
                                        emitGuestStore(ev, ma, pc, 1);
                                    }
                                }
                            }
                        }
                        if (post_idx) {
                            IRValue wb;
                            if (rm == 31U) {
                                wb = B->binop("add", IRType::Int64, addr_val, IRValue::imm_u64(nregs * reg_bytes), "ldst_wb");
                            } else {
                                IRValue off_v = loadGpr(rm, true);
                                wb = B->binop("add", IRType::Int64, addr_val, off_v, "ldst_wb");
                            }
                            storeGprOrSp(rn, wb, true);
                        }
                        continue;
                    }
                }

                // 4. Data Processing -- Register
                if ((op0 & 0x7) == 0x5) {
                    unsigned op1 = (insn >> 28) & 1;
                    unsigned op2 = (insn >> 21) & 0xF;
                    if (op1 == 0 && (op2 & 8) == 0) {
                        unsigned opc = (insn >> 29) & 3;
                        unsigned N = (insn >> 21) & 1;
                        unsigned shift_type = (insn >> 22) & 3;
                        unsigned imm6 = (insn >> 10) & 0x3F;
                        IRValue a = loadGpr(rn, is64);
                        IRValue b = emitShiftReg(loadGpr(rm, is64), shift_type, imm6, is64);
                        if (N) {
                            b = B->binop("xor", ty_t, b, IRValue::imm(ty_t, ~0ULL), "orn");
                        }
                        const char *op_str = (opc == 0 || opc == 3) ? "and" : (opc == 1 ? "or" : "xor");
                        IRValue res = B->binop(op_str, ty_t, a, b, "logr");
                        if (opc == 3) {
                            emitFlagsLogic(res, is64);
                        }
                        storeGpr(rd, res, is64);
                    } else if (op1 == 0 && (op2 & 8) == 8) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned S = (insn >> 29) & 1;
                        unsigned ext = (insn >> 21) & 1;
                        IRValue a, b;
                        if (ext == 0) {
                            a = loadGpr(rn, is64);
                            b = emitShiftReg(loadGpr(rm, is64), (insn >> 22) & 3, (insn >> 10) & 0x3F, is64);
                        } else {
                            a = loadGprOrSp(rn, is64);
                            IRValue b64 = emitExtendReg(rm, (insn >> 13) & 7, (insn >> 10) & 7);
                            b = is64 ? b64 : B->cast("trunc", b64, IRType::Int32, "b32");
                        }
                        IRValue res = B->binop(op ? "sub" : "add", ty_t, a, b, "addsub");
                        if (S) {
                            if (op) {
                                IRValue nb = B->binop("xor", ty_t, b, IRValue::imm(ty_t, ~0ULL), "nb");
                                emitFlagsAdd(a, nb, IRValue::imm(ty_t, 1), res, is64);
                            } else {
                                emitFlagsAdd(a, b, IRValue::imm(ty_t, 0), res, is64);
                            }
                            storeGpr(rd, res, is64);
                        } else if (ext) {
                            storeGprOrSp(rd, res, is64);
                        } else {
                            storeGpr(rd, res, is64);
                        }
                    } else if (op1 == 1 && op2 == 2) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned cond = (insn >> 12) & 0xF;
                        unsigned is_imm = (insn >> 11) & 1;
                        unsigned nzcv = insn & 0xF;
                        IRValue c_ok = evalCond(cond);
                        IRValue a = loadGpr(rn, is64);
                        IRValue b = is_imm ? IRValue::imm(ty_t, rm) : loadGpr(rm, is64);
                        IRValue res = B->binop(op ? "sub" : "add", ty_t, a, b, "ccmp_r");
                        if (op) {
                            IRValue nb = B->binop("xor", ty_t, b, IRValue::imm(ty_t, ~0ULL), "ccmp_nb");
                            emitFlagsAdd(a, nb, IRValue::imm(ty_t, 1), res, is64);
                        } else {
                            emitFlagsAdd(a, b, IRValue::imm(ty_t, 0), res, is64);
                        }
                        IRValue fn_v = B->CreateLoad(IRType::Int1, flag_n_alloca, 1, IROpTag::CpuRegRead, "cc_n", {}, 0, 32);
                        IRValue fz_v = B->CreateLoad(IRType::Int1, flag_z_alloca, 1, IROpTag::CpuRegRead, "cc_z", {}, 0, 32);
                        IRValue fc_v = B->CreateLoad(IRType::Int1, flag_c_alloca, 1, IROpTag::CpuRegRead, "cc_c", {}, 0, 32);
                        IRValue fv_v = B->CreateLoad(IRType::Int1, flag_v_alloca, 1, IROpTag::CpuRegRead, "cc_v", {}, 0, 32);
                        IRValue sn = B->select(IRType::Int1, c_ok, fn_v, IRValue::imm(IRType::Int1, (nzcv >> 3) & 1), "s_n");
                        IRValue sz = B->select(IRType::Int1, c_ok, fz_v, IRValue::imm(IRType::Int1, (nzcv >> 2) & 1), "s_z");
                        IRValue sc = B->select(IRType::Int1, c_ok, fc_v, IRValue::imm(IRType::Int1, (nzcv >> 1) & 1), "s_c");
                        IRValue sv = B->select(IRType::Int1, c_ok, fv_v, IRValue::imm(IRType::Int1, nzcv & 1), "s_v");
                        B->CreateStore(sn, flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                        B->CreateStore(sz, flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                        B->CreateStore(sc, flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                        B->CreateStore(sv, flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                    } else if (op1 == 1 && op2 == 4) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned o2 = (insn >> 10) & 1;
                        unsigned cond = (insn >> 12) & 0xF;
                        IRValue c_ok = evalCond(cond);
                        IRValue t_val = loadGpr(rn, is64), f_val = loadGpr(rm, is64);
                        if (op) {
                            f_val = B->binop("xor", ty_t, f_val, IRValue::imm(ty_t, ~0ULL), "csinv");
                        }
                        if (o2) {
                            f_val = B->binop("add", ty_t, f_val, IRValue::imm(ty_t, 1), "csinc");
                        }
                        IRValue res = B->select(ty_t, c_ok, t_val, f_val, "csel");
                        storeGpr(rd, res, is64);
                    } else if (op1 == 1 && op2 == 6) {
                        unsigned is_1src = (insn >> 30) & 1;
                        unsigned opcode2 = (insn >> 10) & 0x3F;
                        if (is_1src == 0) {
                            IRValue n = loadGpr(rn, is64), m = loadGpr(rm, is64);
                            if (opcode2 == 0x02) {
                                IRValue iz = B->icmp("eq", ty_t, m, IRValue::imm(ty_t, 0), "udiv_z");
                                IRValue sm = B->select(ty_t, iz, IRValue::imm(ty_t, 1), m, "udiv_m");
                                IRValue q = B->CreateUDiv(ty_t, n, sm);
                                IRValue res = B->select(ty_t, iz, IRValue::imm(ty_t, 0), q, "udiv_r");
                                storeGpr(rd, res, is64);
                            } else if (opcode2 == 0x03) {
                                uint64_t min_u = is64 ? 0x8000000000000000ULL : 0xFFFFFFFF80000000ULL;
                                IRValue int_min = IRValue::imm(ty_t, min_u);
                                IRValue iz = B->icmp("eq", ty_t, m, IRValue::imm(ty_t, 0), "sdiv_z");
                                IRValue imin = B->icmp("eq", ty_t, n, int_min, "sdiv_min");
                                IRValue im1 = B->icmp("eq", ty_t, m, IRValue::imm(ty_t, ~0ULL), "sdiv_m1");
                                IRValue iovf = B->binop("and", IRType::Int1, imin, im1, "sdiv_ovf");
                                IRValue bad = B->binop("or", IRType::Int1, iz, iovf, "sdiv_bad");
                                IRValue sm = B->select(ty_t, bad, IRValue::imm(ty_t, 1), m, "sdiv_m");
                                IRValue q = B->CreateSDiv(ty_t, n, sm);
                                IRValue r1 = B->select(ty_t, iovf, int_min, q, "sdiv_r1");
                                IRValue res = B->select(ty_t, iz, IRValue::imm(ty_t, 0), r1, "sdiv_r");
                                storeGpr(rd, res, is64);
                            } else if (opcode2 >= 0x08 && opcode2 <= 0x0B) {
                                unsigned mask = is64 ? 63 : 31;
                                IRValue sh_amt = B->binop("and", ty_t, m, IRValue::imm(ty_t, mask), "sh_amt");
                                IRValue res;
                                if (opcode2 == 0x08) res = B->CreateShl(ty_t, n, sh_amt);
                                else if (opcode2 == 0x09) res = B->CreateLShr(ty_t, n, sh_amt);
                                else if (opcode2 == 0x0A) res = B->CreateAShr(ty_t, n, sh_amt);
                                else res = B->CreateCall(ty_t, "llvm.fshr." + ty, {n, n, sh_amt}, "shv", IROpTag::RawIR);
                                storeGpr(rd, res, is64);
                            }
                        } else {
                            IRValue n = loadGpr(rn, is64), res;
                            if (opcode2 == 0x00) res = B->CreateCall(ty_t, "llvm.bitreverse." + ty, {n}, "dp1", IROpTag::RawIR);
                            else if (opcode2 == 0x02 || opcode2 == 0x03) res = B->CreateCall(ty_t, "llvm.bswap." + ty, {n}, "dp1", IROpTag::RawIR);
                            else if (opcode2 == 0x04) res = B->CreateCall(ty_t, "llvm.ctlz." + ty, {n, IRValue::imm(IRType::Int1, 0)}, "dp1", IROpTag::RawIR);
                            else res = B->binop("add", ty_t, n, IRValue::imm(ty_t, 0), "dp1");
                            storeGpr(rd, res, is64);
                        }
                    } else if (op1 == 1 && (op2 & 8) == 8) {
                        unsigned op54 = (insn >> 29) & 3;
                        unsigned op31 = (insn >> 21) & 7;
                        unsigned o0 = (insn >> 15) & 1;
                        if (op54 == 0 && op31 == 0) {
                            IRValue a = loadGpr(ra, is64), n = loadGpr(rn, is64), m = loadGpr(rm, is64);
                            IRValue pr = B->binop("mul", ty_t, n, m, "mul");
                            IRValue res = B->binop(o0 ? "sub" : "add", ty_t, a, pr, "madd");
                            storeGpr(rd, res, is64);
                        } else if (op54 == 0 && (op31 == 1 || op31 == 5)) {
                            IRValue a = loadGpr(ra, true), n32 = loadGpr(rn, false), m32 = loadGpr(rm, false);
                            const char *ext_op = (op31 == 1) ? "sext" : "zext";
                            IRValue n64 = B->cast(ext_op, n32, IRType::Int64, "m_n");
                            IRValue m64 = B->cast(ext_op, m32, IRType::Int64, "m_m");
                            IRValue pr = B->binop("mul", IRType::Int64, n64, m64, "m_p");
                            IRValue res = B->binop(o0 ? "sub" : "add", IRType::Int64, a, pr, "maddl");
                            storeGpr(rd, res, true);
                        } else if (op54 == 0 && (op31 == 2 || op31 == 6)) {
                            IRValue n64 = loadGpr(rn, true), m64 = loadGpr(rm, true);
                            const char *ext_op = (op31 == 2) ? "sext" : "zext";
                            IRValue n128 = B->cast(ext_op, n64, IRType::Int128, "mh_n");
                            IRValue m128 = B->cast(ext_op, m64, IRType::Int128, "mh_m");
                            IRValue p128 = B->binop("mul", IRType::Int128, n128, m128, "mh_p");
                            IRValue sh128 = B->binop("lshr", IRType::Int128, p128, IRValue::imm(IRType::Int128, 64), "mh_s");
                            IRValue res = B->cast("trunc", sh128, IRType::Int64, "mulh");
                            storeGpr(rd, res, true);
                        }
                    }
                    continue;
                }

                // 5. Scalar FP, AdvSIMD Vector, and ARMv8 Crypto SHA-1 Operations
                if ((op0 & 0x7) == 0x7) {
                    stats.simd_ops_lifted++;
                    unsigned Q = (insn >> 30) & 1;
                    unsigned U = (insn >> 29) & 1;
                    unsigned sz = (insn >> 22) & 3;

                    // Scalar FP <-> Integer conversions & FMOV GP <-> FP (SCVTF, UCVTF, FCVTZS, FCVTZU, FMOV)
                    if (((insn >> 24) & 0x5FU) == 0x1EU && ((insn >> 21) & 1U) == 1U && ((insn >> 10) & 0x3FU) == 0U) {
                        unsigned rmode = (insn >> 19) & 3;
                        unsigned opcode = (insn >> 16) & 7;
                        bool fp64 = (sz & 1) != 0;
                        IRType f_int_t = fp64 ? IRType::Int64 : IRType::Int32;
                        IRType f_t = fp64 ? IRType::Double : IRType::Float;
                        if (rmode == 0 && opcode == 6) {
                            storeGpr(rd, loadVec(rn, intType(is64)), is64);
                        } else if (rmode == 0 && opcode == 7) {
                            storeVec(rd, loadGpr(rn, is64));
                        } else if (rmode == 0 && (opcode == 2 || opcode == 3)) {
                            IRValue iv = loadGpr(rn, is64);
                            IRValue fv = B->cast(opcode == 2 ? "sitofp" : "uitofp", iv, f_t, "cvtf");
                            IRValue bv = B->cast("bitcast", fv, f_int_t, "cvtf_b");
                            storeVec(rd, bv);
                        } else if (opcode == 0 || opcode == 1) {
                            IRValue bv = loadVec(rn, f_int_t);
                            IRValue fv = B->cast("bitcast", bv, f_t, "fcvt_f");
                            if (rmode != 3) {
                                const char* rfn = (rmode == 0) ? "roundeven" : ((rmode == 1) ? "ceil" : "floor");
                                fv = B->CreateCall(f_t, std::string("llvm.") + rfn + "." + (fp64 ? "f64" : "f32"), {fv}, "fcvt_r", IROpTag::RawIR);
                            }
                            IRValue iv = B->cast(opcode == 0 ? "fptosi" : "fptoui", fv, ty_t, "fcvt_i");
                            storeGpr(rd, iv, is64);
                        }
                        continue;
                    }

                    // Scalar Floating-Point Data Processing (1-src, FCMP, FMOV imm, FCSEL, 2-src)
                    if (((insn >> 24) & 0x5FU) == 0x1EU && ((insn >> 21) & 1U) == 1U) {
                        bool fp64 = (sz & 1) != 0;
                        IRType f_int_t = fp64 ? IRType::Int64 : IRType::Int32;
                        IRType f_t = fp64 ? IRType::Double : IRType::Float;
                        unsigned low6 = (insn >> 10) & 0x3FU;
                        if ((low6 & 0x1FU) == 0x10U) {
                            unsigned opc1 = (insn >> 15) & 0x3FU;
                            IRValue vn_i = loadVec(rn, f_int_t);
                            if (opc1 == 0) {
                                storeVec(rd, vn_i);
                            } else if (opc1 == 1) {
                                uint64_t msk = fp64 ? 9223372036854775807ULL : 2147483647ULL;
                                IRValue r_i = B->binop("and", f_int_t, vn_i, IRValue::imm(f_int_t, msk), "fabs_i");
                                storeVec(rd, r_i);
                            } else if (opc1 == 2) {
                                IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fneg_in");
                                IRValue r_f = B->CreateFNeg(f_t, vn_f, "fneg_r");
                                IRValue r_i = B->cast("bitcast", r_f, f_int_t, "fneg_i");
                                storeVec(rd, r_i);
                            } else if (opc1 == 3) {
                                IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fsqrt_in");
                                IRValue r_f = B->CreateCall(f_t, std::string("llvm.sqrt.") + (fp64 ? "f64" : "f32"), {vn_f}, "fsqrt_r", IROpTag::RawIR);
                                IRValue r_i = B->cast("bitcast", r_f, f_int_t, "fsqrt_i");
                                storeVec(rd, r_i);
                            } else if (opc1 == 4 || opc1 == 5) {
                                IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fcvt_in");
                                if (!fp64 && opc1 == 5) {
                                    IRValue r_f = B->cast("fpext", vn_f, IRType::Double, "fcvt_out");
                                    IRValue r_i = B->cast("bitcast", r_f, IRType::Int64, "fcvt_bi");
                                    storeVec(rd, r_i);
                                } else if (fp64 && opc1 == 4) {
                                    IRValue r_f = B->cast("fptrunc", vn_f, IRType::Float, "fcvt_out");
                                    IRValue r_i = B->cast("bitcast", r_f, IRType::Int32, "fcvt_bi");
                                    storeVec(rd, r_i);
                                } else {
                                    storeVec(rd, vn_i);
                                }
                            } else if (opc1 >= 8 && opc1 <= 15) {
                                const char* rfn = "nearbyint";
                                if (opc1 == 8) rfn = "roundeven";
                                else if (opc1 == 9) rfn = "ceil";
                                else if (opc1 == 10) rfn = "floor";
                                else if (opc1 == 11) rfn = "trunc";
                                else if (opc1 == 12) rfn = "round";
                                else if (opc1 == 14) rfn = "rint";
                                IRValue vn_f = B->cast("bitcast", vn_i, f_t, "frint_in");
                                IRValue r_f = B->CreateCall(f_t, std::string("llvm.") + rfn + "." + (fp64 ? "f64" : "f32"), {vn_f}, "frint_r", IROpTag::RawIR);
                                IRValue r_i = B->cast("bitcast", r_f, f_int_t, "frint_i");
                                storeVec(rd, r_i);
                            } else {
                                storeVec(rd, vn_i);
                            }
                            continue;
                        }
                        if (low6 == 0x08U) {
                            bool cmp_zero = ((insn >> 3) & 1U) != 0;
                            IRValue vn_i = loadVec(rn, f_int_t);
                            IRValue vm_i = cmp_zero ? IRValue::imm(f_int_t, 0) : loadVec(rm, f_int_t);
                            IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fcmp_a");
                            IRValue vm_f = B->cast("bitcast", vm_i, f_t, "fcmp_b");
                            IRValue flt = B->fcmp("olt", f_t, vn_f, vm_f, "fc_lt");
                            IRValue feq = B->fcmp("oeq", f_t, vn_f, vm_f, "fc_eq");
                            IRValue fgt = B->fcmp("ogt", f_t, vn_f, vm_f, "fc_gt");
                            IRValue funo = B->fcmp("uno", f_t, vn_f, vm_f, "fc_un");
                            IRValue c_tmp = B->binop("or", IRType::Int1, feq, fgt, "fc_ct");
                            IRValue fc_val = B->binop("or", IRType::Int1, c_tmp, funo, "fc_c");
                            B->CreateStore(flt, flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(feq, flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(fc_val, flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(funo, flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            continue;
                        }
                        if ((low6 & 0x3U) == 0x1U) {
                            unsigned cond = (insn >> 12) & 0xFU;
                            unsigned nzcv = insn & 0xFU;
                            IRValue c_ok = evalCond(cond);
                            IRValue vn_i = loadVec(rn, f_int_t), vm_i = loadVec(rm, f_int_t);
                            IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fcc_a");
                            IRValue vm_f = B->cast("bitcast", vm_i, f_t, "fcc_b");
                            IRValue flt = B->fcmp("olt", f_t, vn_f, vm_f, "fcc_lt");
                            IRValue feq = B->fcmp("oeq", f_t, vn_f, vm_f, "fcc_eq");
                            IRValue fgt = B->fcmp("ogt", f_t, vn_f, vm_f, "fcc_gt");
                            IRValue funo = B->fcmp("uno", f_t, vn_f, vm_f, "fcc_un");
                            IRValue fc_t = B->binop("or", IRType::Int1, feq, fgt, "fcc_ct");
                            IRValue fc_val = B->binop("or", IRType::Int1, fc_t, funo, "fcc_c");
                            IRValue sn = B->select(IRType::Int1, c_ok, flt, IRValue::imm(IRType::Int1, (nzcv >> 3) & 1), "fcc_sn");
                            IRValue sz_f = B->select(IRType::Int1, c_ok, feq, IRValue::imm(IRType::Int1, (nzcv >> 2) & 1), "fcc_sz");
                            IRValue sc = B->select(IRType::Int1, c_ok, fc_val, IRValue::imm(IRType::Int1, (nzcv >> 1) & 1), "fcc_sc");
                            IRValue sv = B->select(IRType::Int1, c_ok, funo, IRValue::imm(IRType::Int1, nzcv & 1), "fcc_sv");
                            B->CreateStore(sn, flag_n_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(sz_f, flag_z_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(sc, flag_c_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            B->CreateStore(sv, flag_v_alloca, 1, IROpTag::FlagWrite, {}, 0, 32);
                            continue;
                        }
                        if ((low6 & 0x7U) == 0x4U) {
                            unsigned imm8 = (insn >> 13) & 0xFFU;
                            if (fp64) {
                                uint64_t bits = expandVfpFloat64Imm(imm8);
                                storeVec(rd, IRValue::imm(IRType::Int64, bits));
                            } else {
                                uint32_t bits = expandVfpFloat32Imm(imm8);
                                storeVec(rd, IRValue::imm(IRType::Int32, bits));
                            }
                            continue;
                        }
                        if ((low6 & 0x3U) == 0x3U) {
                            unsigned cond = (insn >> 12) & 0xFU;
                            IRValue c_ok = evalCond(cond);
                            IRValue vn_i = loadVec(rn, f_int_t), vm_i = loadVec(rm, f_int_t);
                            IRValue res = B->select(f_int_t, c_ok, vn_i, vm_i, "fcsel");
                            storeVec(rd, res);
                            continue;
                        }
                        if ((low6 & 0x3U) == 0x2U) {
                            unsigned opc2 = (insn >> 12) & 0xFU;
                            IRValue vn_i = loadVec(rn, f_int_t), vm_i = loadVec(rm, f_int_t);
                            IRValue vn_f = B->cast("bitcast", vn_i, f_t, "fp2_a");
                            IRValue vm_f = B->cast("bitcast", vm_i, f_t, "fp2_b");
                            IRValue rf;
                            if (opc2 == 0) rf = B->binop("fmul", f_t, vn_f, vm_f, "fp2_r");
                            else if (opc2 == 1) rf = B->binop("fdiv", f_t, vn_f, vm_f, "fp2_r");
                            else if (opc2 == 2) rf = B->binop("fadd", f_t, vn_f, vm_f, "fp2_r");
                            else if (opc2 == 3) rf = B->binop("fsub", f_t, vn_f, vm_f, "fp2_r");
                            else if (opc2 == 4 || opc2 == 6) {
                                IRValue gt = B->fcmp("ogt", f_t, vn_f, vm_f, "fmax_c");
                                rf = B->select(f_t, gt, vn_f, vm_f, "fp2_r");
                            } else if (opc2 == 5 || opc2 == 7) {
                                IRValue lt = B->fcmp("olt", f_t, vn_f, vm_f, "fmin_c");
                                rf = B->select(f_t, lt, vn_f, vm_f, "fp2_r");
                            } else {
                                IRValue m_t = B->binop("fmul", f_t, vn_f, vm_f, "fnmul_t");
                                rf = B->CreateFNeg(f_t, m_t, "fp2_r");
                            }
                            IRValue ri = B->cast("bitcast", rf, f_int_t, "fp2_i");
                            storeVec(rd, ri);
                            continue;
                        }
                    }

                    // Scalar Floating-Point 3-Source (FMADD, FMSUB, FNMADD, FNMSUB)
                    if (((insn >> 24) & 0x5FU) == 0x1FU) {
                        bool fp64 = (sz & 1) != 0;
                        IRType f_int_t = fp64 ? IRType::Int64 : IRType::Int32;
                        IRType f_t = fp64 ? IRType::Double : IRType::Float;
                        unsigned o1 = (insn >> 21) & 1U;
                        unsigned o0 = (insn >> 15) & 1U;
                        IRValue vn_f = B->cast("bitcast", loadVec(rn, f_int_t), f_t, "fm3_n");
                        IRValue vm_f = B->cast("bitcast", loadVec(rm, f_int_t), f_t, "fm3_m");
                        IRValue va_f = B->cast("bitcast", loadVec(ra, f_int_t), f_t, "fm3_a");
                        IRValue p_f = B->binop("fmul", f_t, vn_f, vm_f, "fm3_p");
                        IRValue comb = B->binop(o0 ? "fsub" : "fadd", f_t, va_f, p_f, "fm3_c");
                        IRValue r_f = o1 ? B->CreateFNeg(f_t, comb, "fm3_r")
                                         : B->binop("fadd", f_t, comb, IRValue::imm_fp(f_t, "0.0"), "fm3_r");
                        IRValue r_i = B->cast("bitcast", r_f, f_int_t, "fm3_i");
                        storeVec(rd, r_i);
                        continue;
                    }

                    // AdvSIMD Scalar DUP sD/dD, vN.s/d[idx] (0x5E000400)
                    if ((insn & 0xFFE0FC00U) == 0x5E000400U) {
                        unsigned imm5 = (insn >> 16) & 0x1FU;
                        unsigned tz = imm5 ? __builtin_ctz(imm5) : 2;
                        unsigned esz = 1U << tz;
                        unsigned idx = imm5 >> (tz + 1);
                        IRType sty_t = intTypeForBytes(esz);
                        IRType full_vty_t = vecIntTypeFor(16 / esz, esz * 8);
                        IRValue vn = loadVec(rn, full_vty_t);
                        IRValue el = B->CreateExtractElement(full_vty_t, sty_t, vn, idx, "sdup_el");
                        storeVec(rd, el);
                        continue;
                    }

                    // ARMv8 Crypto SHA1H sD, sN (0x5E280800)
                    if ((insn & 0xFFFFFC00U) == 0x5E280800U) {
                        stats.crypto_sha1_lifted++;
                        IRValue sn = loadVec(rn, IRType::Int32);
                        IRValue res = B->CreateCall(IRType::Int32, "llvm.fshr.i32", {sn, sn, IRValue::imm_i32(2)}, "sha1h", IROpTag::RawIR);
                        storeVec(rd, res, true);
                        continue;
                    }

                    // ARMv8 Crypto SHA1SU1 vD.4s, vN.4s (0x5E281800)
                    if ((insn & 0xFFFFFC00U) == 0x5E281800U) {
                        stats.crypto_sha1_lifted++;
                        IRValue vd = loadVec(rd, IRType::Vec4xI32), vn = loadVec(rn, IRType::Vec4xI32);
                        IRValue n_shuf = B->CreateShuffleVector(IRType::Vec4xI32, vn, IRValue::zero_vec(IRType::Vec4xI32), {1, 2, 3, 4}, "su1_sh");
                        IRValue t_xor = B->binop("xor", IRType::Vec4xI32, vd, n_shuf, "su1_t");
                        IRValue c31 = IRValue{IRType::Vec4xI32, "<i32 31, i32 31, i32 31, i32 31>", true, 0, nullptr};
                        IRValue rol1 = B->CreateCall(IRType::Vec4xI32, "llvm.fshr.v4i32", {t_xor, t_xor, c31}, "su1_r1", IROpTag::RawIR);
                        IRValue r0 = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, rol1, 0, "su1_r0");
                        IRValue r0_rol1 = B->CreateCall(IRType::Int32, "llvm.fshr.i32", {r0, r0, IRValue::imm_i32(31)}, "su1_r0r", IROpTag::RawIR);
                        IRValue r3 = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, rol1, 3, "su1_r3");
                        IRValue r3_new = B->binop("xor", IRType::Int32, r3, r0_rol1, "su1_r3n");
                        IRValue res = B->CreateInsertElement(IRType::Vec4xI32, rol1, r3_new, 3, "su1_res");
                        storeVec(rd, res, false);
                        continue;
                    }

                    // ARMv8 Crypto SHA1C / SHA1P / SHA1M / SHA1SU0 (0x5E000000 .. 0x5E003000)
                    if ((insn & 0xFFE08C00U) == 0x5E000000U) {
                        stats.crypto_sha1_lifted++;
                        unsigned opc = (insn >> 12) & 7;
                        IRValue vd = loadVec(rd, IRType::Vec4xI32), vm = loadVec(rm, IRType::Vec4xI32);
                        if (opc == 3) {
                            IRValue vn = loadVec(rn, IRType::Vec4xI32);
                            IRValue sh = B->CreateShuffleVector(IRType::Vec4xI32, vd, vn, {2, 3, 4, 5}, "su0_sh");
                            IRValue x1 = B->binop("xor", IRType::Vec4xI32, sh, vd, "su0_x1");
                            IRValue res = B->binop("xor", IRType::Vec4xI32, x1, vm, "su0_r");
                            storeVec(rd, res, false);
                        } else {
                            IRValue a = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, vd, 0, "sha_a");
                            IRValue b = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, vd, 1, "sha_b");
                            IRValue c = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, vd, 2, "sha_c");
                            IRValue d = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, vd, 3, "sha_d");
                            IRValue e = loadVec(rn, IRType::Int32);
                            for (unsigned lane = 0; lane < 4; ++lane) {
                                IRValue w = B->CreateExtractElement(IRType::Vec4xI32, IRType::Int32, vm, lane, "sha_w");
                                IRValue t;
                                if (opc == 0) {
                                    IRValue cxd = B->binop("xor", IRType::Int32, c, d, "cxd");
                                    IRValue band = B->binop("and", IRType::Int32, b, cxd, "band");
                                    t = B->binop("xor", IRType::Int32, d, band, "sha_t");
                                } else if (opc == 1) {
                                    IRValue bxc = B->binop("xor", IRType::Int32, b, c, "bxc");
                                    t = B->binop("xor", IRType::Int32, bxc, d, "sha_t");
                                } else {
                                    IRValue bc = B->binop("and", IRType::Int32, b, c, "bc");
                                    IRValue borc = B->binop("or", IRType::Int32, b, c, "borc");
                                    IRValue bd = B->binop("and", IRType::Int32, borc, d, "bd");
                                    t = B->binop("or", IRType::Int32, bc, bd, "sha_t");
                                }
                                IRValue a_rol5 = B->CreateCall(IRType::Int32, "llvm.fshr.i32", {a, a, IRValue::imm_i32(27)}, "ar5", IROpTag::RawIR);
                                IRValue s1 = B->binop("add", IRType::Int32, e, a_rol5, "sh_s1");
                                IRValue s2 = B->binop("add", IRType::Int32, s1, t, "sh_s2");
                                IRValue e_new = B->binop("add", IRType::Int32, s2, w, "sh_en");
                                IRValue b_rol30 = B->CreateCall(IRType::Int32, "llvm.fshr.i32", {b, b, IRValue::imm_i32(2)}, "br30", IROpTag::RawIR);
                                e = d; d = c; c = b_rol30; b = a; a = e_new;
                            }
                            IRValue v0 = B->CreateInsertElement(IRType::Vec4xI32, IRValue::zero_vec(IRType::Vec4xI32), a, 0, "sh_v0");
                            IRValue v1 = B->CreateInsertElement(IRType::Vec4xI32, v0, b, 1, "sh_v1");
                            IRValue v2 = B->CreateInsertElement(IRType::Vec4xI32, v1, c, 2, "sh_v2");
                            IRValue v3 = B->CreateInsertElement(IRType::Vec4xI32, v2, d, 3, "sh_v3");
                            storeVec(rd, v3, false);
                        }
                        continue;
                    }

                    // AdvSIMD REV32 vD.16b, vN.16b (0x2E200800 / 0x6E200800)
                    if ((insn & 0xBF3FFC00U) == 0x2E200800U) {
                        IRType vty_t = Q ? IRType::Vec4xI32 : IRType::Vec2xI32;
                        const char *intr = Q ? "llvm.bswap.v4i32" : "llvm.bswap.v2i32";
                        IRValue vn = loadVec(rn, vty_t);
                        IRValue res = B->CreateCall(vty_t, intr, {vn}, "vrev32", IROpTag::RawIR);
                        storeVec(rd, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD EXT vD.16b, vN.16b, vM.16b, #imm4
                    if ((insn & 0xBF208400U) == 0x2E000000U) {
                        unsigned imm4 = (insn >> 11) & 0xF;
                        unsigned lanes = Q ? 16 : 8;
                        IRType vty_t = vecIntTypeFor(lanes, 8);
                        IRValue vn = loadVec(rn, vty_t), vm = loadVec(rm, vty_t);
                        std::vector<int> mask;
                        mask.reserve(lanes);
                        for (unsigned i = 0; i < lanes; ++i) mask.push_back(static_cast<int>(imm4 + i));
                        IRValue res = B->CreateShuffleVector(vty_t, vn, vm, mask, "vext");
                        storeVec(rd, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD modified immediate (MOVI / BIC imm)
                    if (((insn >> 19) & 0x1FF) == 0x1E0 && ((insn >> 10) & 3) == 1) {
                        unsigned cmode = (insn >> 12) & 0xF;
                        unsigned imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 0x1F);
                        uint64_t v64 = expandAdvSimdImm(U, cmode, imm8);
                        if (U == 1 && (cmode & 0x9) == 1) {
                            IRValue cur = loadVec(rd, IRType::Vec2xI64);
                            uint64_t mask = ~v64;
                            IRValue mvec{IRType::Vec2xI64, "<i64 " + std::to_string(mask) + ", i64 " + std::to_string(Q ? mask : 0ULL) + ">", true, 0, nullptr};
                            IRValue res = B->binop("and", IRType::Vec2xI64, cur, mvec, "vbic_i");
                            storeVec(rd, res, false);
                        } else {
                            IRValue cur_z = B->CreateInsertElement(IRType::Vec2xI64, IRValue::zero_vec(IRType::Vec2xI64), IRValue::imm_u64(v64), 0, "movi_v");
                            if (Q) {
                                cur_z = B->CreateInsertElement(IRType::Vec2xI64, cur_z, IRValue::imm_u64(v64), 1, "movi_v2");
                            }
                            storeVec(rd, cur_z, false);
                        }
                        continue;
                    }

                    // AdvSIMD Copy (UMOV, DUP vD, Rn, INS vD[idx], Rn, DUP vD, vN[idx])
                    if (((insn >> 21) & 0xFF) == 0x70 && ((insn >> 31) & 1) == 0 && ((insn >> 10) & 1) == 1) {
                        unsigned imm5 = (insn >> 16) & 0x1F;
                        unsigned imm4 = (insn >> 11) & 0xF;
                        unsigned tz = imm5 ? __builtin_ctz(imm5) : 4;
                        unsigned esz = 1U << tz;
                        unsigned idx = imm5 >> (tz + 1);
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        IRType sty_t = intTypeForBytes(esz);
                        IRType vty_t = vecIntTypeFor(lanes, esz * 8);
                        IRType full_vty_t = vecIntTypeFor(16 / esz, esz * 8);
                        if (U == 0 && imm4 == 7) {
                            IRValue v = loadVec(rn, full_vty_t);
                            IRValue el = B->CreateExtractElement(full_vty_t, sty_t, v, idx, "umov");
                            if (esz < 4) el = B->cast("zext", el, IRType::Int32, "umov_zx");
                            storeGpr(rd, el, Q == 1);
                        } else if (U == 0 && imm4 == 1) {
                            IRValue gval = loadGpr(rn, esz == 8);
                            if (esz < 4) gval = B->cast("trunc", gval, sty_t, "dup_tr");
                            IRValue cur = IRValue::zero_vec(vty_t);
                            for (unsigned i = 0; i < lanes; ++i) {
                                cur = B->CreateInsertElement(vty_t, cur, gval, i, "dup_v");
                            }
                            storeVec(rd, cur, Q == 0);
                        } else if (U == 0 && imm4 == 3) {
                            IRValue gval = loadGpr(rn, esz == 8);
                            if (esz < 4) gval = B->cast("trunc", gval, sty_t, "ins_tr");
                            IRValue cur = loadVec(rd, full_vty_t);
                            IRValue nxt = B->CreateInsertElement(full_vty_t, cur, gval, idx, "ins_v");
                            storeVec(rd, nxt, false);
                        } else if (U == 0 && imm4 == 0) {
                            IRValue vn = loadVec(rn, full_vty_t);
                            IRValue el = B->CreateExtractElement(full_vty_t, sty_t, vn, idx, "dupel");
                            IRValue cur = IRValue::zero_vec(vty_t);
                            for (unsigned i = 0; i < lanes; ++i) {
                                cur = B->CreateInsertElement(vty_t, cur, el, i, "dup_ve");
                            }
                            storeVec(rd, cur, Q == 0);
                        } else if (U == 1 && Q == 1) {
                            unsigned src_idx = imm4 >> tz;
                            IRValue vn = loadVec(rn, full_vty_t), vd = loadVec(rd, full_vty_t);
                            IRValue el = B->CreateExtractElement(full_vty_t, sty_t, vn, src_idx, "insel_v");
                            IRValue res = B->CreateInsertElement(full_vty_t, vd, el, idx, "insel_out");
                            storeVec(rd, res, false);
                        }
                        continue;
                    }

                    // AdvSIMD Shift-by-Immediate (SSHR / USHR / SHL)
                    if (((insn >> 23) & 0x3F) == 0x1E && ((insn >> 10) & 1) == 1 && ((insn >> 19) & 0xF) != 0) {
                        unsigned immh = (insn >> 19) & 0xF;
                        unsigned immb = (insn >> 16) & 7;
                        unsigned opcode = (insn >> 11) & 0x1F;
                        unsigned msb = 31U - static_cast<unsigned>(__builtin_clz(immh));
                        unsigned esz = 1U << msb;
                        unsigned bits = esz * 8U;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        IRType vty_t = vecIntTypeFor(lanes, bits);
                        unsigned imm7 = (immh << 3) | immb;
                        if (opcode == 0x0A && U == 0) {
                            unsigned sh = imm7 - bits;
                            IRValue a = loadVec(rn, vty_t);
                            IRValue res = B->binop("shl", vty_t, a, splatUIntVec(vty_t, lanes, bits, sh), "vshli");
                            storeVec(rd, res, Q == 0);
                            continue;
                        } else if (opcode == 0x00) {
                            unsigned sh = (2U * bits) - imm7;
                            IRValue a = loadVec(rn, vty_t);
                            IRValue res;
                            if (sh >= bits) {
                                if (U) res = IRValue::zero_vec(vty_t);
                                else res = B->binop("ashr", vty_t, a, splatUIntVec(vty_t, lanes, bits, bits - 1), "vshri");
                            } else {
                                res = B->binop(U ? "lshr" : "ashr", vty_t, a, splatUIntVec(vty_t, lanes, bits, sh), "vshri");
                            }
                            storeVec(rd, res, Q == 0);
                            continue;
                        }
                    }

                    // AdvSIMD vector logical (AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF)
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 21) & 1) == 1 && ((insn >> 10) & 0x3F) == 0x07) {
                        IRType lty_t = Q ? IRType::Int128 : IRType::Int64;
                        IRValue n = loadVec(rn, lty_t), m = loadVec(rm, lty_t), res;
                        if (U == 0 && sz == 0) {
                            res = B->binop("and", lty_t, n, m, "vlog");
                        } else if (U == 0 && sz == 1) {
                            IRValue nm = B->binop("xor", lty_t, m, IRValue::imm(lty_t, ~0ULL), "vnot");
                            res = B->binop("and", lty_t, n, nm, "vlog");
                        } else if (U == 0 && sz == 2) {
                            res = B->binop("or", lty_t, n, m, "vlog");
                        } else if (U == 0 && sz == 3) {
                            IRValue nm = B->binop("xor", lty_t, m, IRValue::imm(lty_t, ~0ULL), "vorn");
                            res = B->binop("or", lty_t, n, nm, "vlog");
                        } else if (U == 1 && sz == 0) {
                            res = B->binop("xor", lty_t, n, m, "vlog");
                        } else if (U == 1 && sz == 1) {
                            IRValue d = loadVec(rd, lty_t);
                            IRValue nd = B->binop("xor", lty_t, d, IRValue::imm(lty_t, ~0ULL), "bsl_nd");
                            IRValue t1 = B->binop("and", lty_t, n, d, "bsl_1");
                            IRValue t2 = B->binop("and", lty_t, m, nd, "bsl_2");
                            res = B->binop("or", lty_t, t1, t2, "vlog");
                        } else if (U == 1 && sz == 2) {
                            IRValue d = loadVec(rd, lty_t);
                            IRValue nm = B->binop("xor", lty_t, m, IRValue::imm(lty_t, ~0ULL), "bit_nm");
                            IRValue t1 = B->binop("and", lty_t, n, m, "bit_1");
                            IRValue t2 = B->binop("and", lty_t, d, nm, "bit_2");
                            res = B->binop("or", lty_t, t1, t2, "vlog");
                        } else {
                            IRValue d = loadVec(rd, lty_t);
                            IRValue nm = B->binop("xor", lty_t, m, IRValue::imm(lty_t, ~0ULL), "bif_nm");
                            IRValue t1 = B->binop("and", lty_t, n, nm, "bif_1");
                            IRValue t2 = B->binop("and", lty_t, d, m, "bif_2");
                            res = B->binop("or", lty_t, t1, t2, "vlog");
                        }
                        storeVec(rd, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD Three-Same (ADD / SUB / CMEQ / CMTST / CMGT / CMHI / CMGE / CMHS / SSHL / USHL / SMAX / UMAX / SMIN / UMIN / MUL)
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 21) & 1) == 1 && ((insn >> 10) & 1) == 1) {
                        unsigned opcode = (insn >> 11) & 0x1F;
                        unsigned bits = 8U << sz;
                        unsigned lanes = (Q ? 16U : 8U) >> sz;
                        IRType vty_t = vecIntTypeFor(lanes, bits);
                        IRValue a = loadVec(rn, vty_t), b = loadVec(rm, vty_t), res;
                        if (opcode == 0x10) {
                            res = B->binop(U ? "sub" : "add", vty_t, a, b, "v3same");
                        } else if (opcode == 0x11 && U == 1) {
                            IRValue cmp_v = B->icmp("eq", vty_t, a, b, "vcmeq");
                            res = B->cast("sext", cmp_v, vty_t, "v3same");
                        } else if (opcode == 0x11 && U == 0) {
                            IRValue and_v = B->binop("and", vty_t, a, b, "vcmtst_a");
                            IRValue cmp_v = B->icmp("ne", vty_t, and_v, IRValue::zero_vec(vty_t), "vcmtst");
                            res = B->cast("sext", cmp_v, vty_t, "v3same");
                        } else if (opcode == 0x06) {
                            IRValue cmp_v = B->icmp(U ? "ugt" : "sgt", vty_t, a, b, "vcmgt");
                            res = B->cast("sext", cmp_v, vty_t, "v3same");
                        } else if (opcode == 0x07) {
                            IRValue cmp_v = B->icmp(U ? "uge" : "sge", vty_t, a, b, "vcmge");
                            res = B->cast("sext", cmp_v, vty_t, "v3same");
                        } else if (opcode == 0x08) {
                            IRValue sh_s = b;
                            if (sz > 0) {
                                IRType i8v_t = vecIntTypeFor(lanes, 8);
                                IRValue sh_tr = B->cast("trunc", b, i8v_t, "vshl_tr");
                                sh_s = B->cast("sext", sh_tr, vty_t, "vshl_sx");
                            }
                            IRValue width_v = splatIntVec(vty_t, lanes, bits, static_cast<int64_t>(bits));
                            IRValue maxsh_v = splatIntVec(vty_t, lanes, bits, static_cast<int64_t>(bits - 1));
                            IRValue zvec = IRValue::zero_vec(vty_t);
                            IRValue is_neg = B->icmp("slt", vty_t, sh_s, zvec, "vshl_isneg");
                            IRValue pos_lt = B->icmp("slt", vty_t, sh_s, width_v, "vshl_plt");
                            IRValue pos_amt = B->select(vty_t, pos_lt, sh_s, zvec, "vshl_pamt");
                            IRValue shl_raw = B->binop("shl", vty_t, a, pos_amt, "vshl_l");
                            IRValue shl_res = B->select(vty_t, pos_lt, shl_raw, zvec, "vshl_lr");
                            IRValue neg_s = B->binop("sub", vty_t, zvec, sh_s, "vshl_ns");
                            IRValue neg_lt = B->icmp("ult", vty_t, neg_s, width_v, "vshl_nlt");
                            IRValue neg_amt = B->select(vty_t, neg_lt, neg_s, maxsh_v, "vshl_namt");
                            IRValue rsh_res;
                            if (U) {
                                IRValue rsh_raw = B->binop("lshr", vty_t, a, neg_amt, "vshl_r");
                                rsh_res = B->select(vty_t, neg_lt, rsh_raw, zvec, "vshl_rr");
                            } else {
                                rsh_res = B->binop("ashr", vty_t, a, neg_amt, "vshl_rr");
                            }
                            res = B->select(vty_t, is_neg, rsh_res, shl_res, "v3same");
                        } else if (opcode == 0x0c || opcode == 0x0d) {
                            const char *pred = (opcode == 0x0c) ? (U ? "ugt" : "sgt") : (U ? "ult" : "slt");
                            IRValue cmp_v = B->icmp(pred, vty_t, a, b, "vmx");
                            res = B->select(vty_t, cmp_v, a, b, "v3same");
                        } else if (opcode == 0x13 && U == 0) {
                            res = B->binop("mul", vty_t, a, b, "v3same");
                        } else {
                            res = B->binop("add", vty_t, a, b, "v3same");
                        }
                        storeVec(rd, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD SSHLL (U=0) / USHLL (U=1)
                    if (((insn >> 23) & 0x3F) == 0x1E && ((insn >> 10) & 0x3F) == 0x29 && ((insn >> 19) & 0xFU) != 0) {
                        unsigned immh = (insn >> 19) & 0xF;
                        unsigned immb = (insn >> 16) & 7;
                        unsigned l = 31 - __builtin_clz(immh);
                        unsigned esz = 1U << l;
                        unsigned sh = ((immh << 3) | immb) - (8U << l);
                        unsigned lanes = 8U / esz;
                        IRType src_t = vecIntTypeFor(lanes * 2, esz * 8);
                        IRType dst_t = vecIntTypeFor(lanes, esz * 16);
                        IRValue src_v = loadVec(rn, src_t);
                        std::vector<int> mask;
                        for (unsigned i = 0; i < lanes; ++i) mask.push_back(static_cast<int>(i + (Q ? lanes : 0)));
                        IRType half_t = vecIntTypeFor(lanes, esz * 8);
                        IRValue shuf = B->CreateShuffleVector(src_t, src_v, IRValue::poison(src_t), mask, "ushll_shuf", half_t);
                        IRValue zx = B->cast(U ? "zext" : "sext", shuf, dst_t, "ushll_zx");
                        if (sh > 0) {
                            zx = B->binop("shl", dst_t, zx, splatUIntVec(dst_t, lanes, esz * 16, sh), "ushll_sh");
                        }
                        storeVec(rd, zx, false);
                        continue;
                    }

                    // AdvSIMD Permute (UZP1, TRN1, ZIP1, UZP2, TRN2, ZIP2)
                    if (((insn >> 24) & 0x3F) == 0x0E && ((insn >> 21) & 1) == 0 && ((insn >> 10) & 3) == 2 && U == 0) {
                        unsigned perm_op = (insn >> 12) & 7;
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        IRType vty_t = vecIntTypeFor(lanes, esz * 8);
                        IRValue a = loadVec(rn, vty_t), b = loadVec(rm, vty_t);
                        std::vector<int> mask;
                        mask.reserve(lanes);
                        for (unsigned i = 0; i < lanes; ++i) {
                            unsigned src_idx = i;
                            if (perm_op == 1) src_idx = i * 2;
                            else if (perm_op == 5) src_idx = i * 2 + 1;
                            else if (perm_op == 2) src_idx = (i & 1) ? (lanes + (i & ~1U)) : i;
                            else if (perm_op == 6) src_idx = (i & 1) ? (lanes + i) : (i + 1);
                            else if (perm_op == 3) src_idx = (i & 1) ? (lanes + (i >> 1)) : (i >> 1);
                            else if (perm_op == 7) {
                                unsigned h = lanes / 2;
                                src_idx = (i & 1) ? (lanes + h + (i >> 1)) : (h + (i >> 1));
                            }
                            mask.push_back(static_cast<int>(src_idx));
                        }
                        IRValue res = B->CreateShuffleVector(vty_t, a, b, mask, "vperm");
                        storeVec(rd, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD Two-Register Misc: CNT, MVN, CM{EQ,GT,GE,LE,LT} #0, ABS, NEG
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 17) & 0x1F) == 0x10 && ((insn >> 10) & 3) == 2) {
                        unsigned opcode = (insn >> 12) & 0x1F;
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        IRType vty_t = vecIntTypeFor(lanes, esz * 8);
                        if (opcode == 0x05) {
                            if (U == 0) {
                                unsigned clanes = Q ? 16U : 8U;
                                IRType cty_t = vecIntTypeFor(clanes, 8);
                                IRValue vin = loadVec(rn, cty_t);
                                IRValue vout = B->CreateCall(cty_t, "llvm.ctpop.v" + std::to_string(clanes) + "i8", {vin}, "vcnt", IROpTag::RawIR);
                                storeVec(rd, vout, Q == 0);
                            } else {
                                IRValue vin = loadVec(rn, IRType::Int128);
                                IRValue vout = B->binop("xor", IRType::Int128, vin, IRValue::imm(IRType::Int128, ~0ULL), "vmvn");
                                storeVec(rd, vout, false);
                            }
                        } else if ((opcode == 0x08) || (opcode == 0x09) || (opcode == 0x0A && U == 0)) {
                            const char* pred = "eq";
                            if (opcode == 0x08 && U == 0) pred = "sgt";
                            else if (opcode == 0x08 && U == 1) pred = "sge";
                            else if (opcode == 0x09 && U == 0) pred = "eq";
                            else if (opcode == 0x09 && U == 1) pred = "sle";
                            else if (opcode == 0x0A && U == 0) pred = "slt";
                            IRValue vin = loadVec(rn, vty_t);
                            IRValue cmp_v = B->icmp(pred, vty_t, vin, IRValue::zero_vec(vty_t), "vcm0");
                            IRValue sext_v = B->cast("sext", cmp_v, vty_t, "vcm0_sx");
                            storeVec(rd, sext_v, Q == 0);
                        } else if (opcode == 0x0B) {
                            IRValue vin = loadVec(rn, vty_t);
                            IRValue neg_v = B->binop("sub", vty_t, IRValue::zero_vec(vty_t), vin, "vneg");
                            if (U == 0) {
                                IRValue is_neg = B->icmp("slt", vty_t, vin, IRValue::zero_vec(vty_t), "vabs_c");
                                IRValue abs_v = B->select(vty_t, is_neg, neg_v, vin, "vabs");
                                storeVec(rd, abs_v, Q == 0);
                            } else {
                                storeVec(rd, neg_v, Q == 0);
                            }
                        } else if (opcode == 0x00 && U == 0 && esz < 8) {
                            unsigned per64 = 8U / esz;
                            IRValue vin = loadVec(rn, vty_t);
                            std::vector<int> mask;
                            for (unsigned i = 0; i < lanes; ++i) {
                                unsigned base = (i / per64) * per64;
                                mask.push_back(static_cast<int>(base + (per64 - 1 - (i % per64))));
                            }
                            IRValue res = B->CreateShuffleVector(vty_t, vin, IRValue::poison(vty_t), mask, "vrev64");
                            storeVec(rd, res, Q == 0);
                        } else if (opcode == 0x01 && U == 0 && esz == 1) {
                            IRValue vin = loadVec(rn, vty_t);
                            std::vector<int> mask;
                            for (unsigned i = 0; i < lanes; ++i) mask.push_back(static_cast<int>(i ^ 1U));
                            IRValue res = B->CreateShuffleVector(vty_t, vin, IRValue::poison(vty_t), mask, "vrev16");
                            storeVec(rd, res, Q == 0);
                        } else if (opcode == 0x12 && U == 0 && sz < 3) {
                            unsigned nlanes = 8U / esz;
                            IRType src_t = vecIntTypeFor(nlanes, esz * 16U);
                            IRType half_t = vecIntTypeFor(nlanes, esz * 8U);
                            IRValue vin = loadVec(rn, src_t);
                            IRValue tr = B->cast("trunc", vin, half_t, "vxtn");
                            if (Q == 0) {
                                storeVec(rd, tr, true);
                            } else {
                                IRValue lo_v = loadVec(rd, half_t);
                                std::vector<int> mask;
                                for (unsigned i = 0; i < nlanes * 2; ++i) mask.push_back(static_cast<int>(i));
                                IRType full_t = vecIntTypeFor(nlanes * 2, esz * 8U);
                                IRValue res = B->CreateShuffleVector(half_t, lo_v, tr, mask, "vxtn2", full_t);
                                storeVec(rd, res, false);
                            }
                        } else if (opcode == 0x14 && sz < 3) {
                            unsigned nlanes = 8U / esz;
                            IRType src_t = vecIntTypeFor(nlanes, esz * 16U);
                            IRType half_t = vecIntTypeFor(nlanes, esz * 8U);
                            IRValue vin = loadVec(rn, src_t);
                            int64_t lo_b = U ? 0 : -(1LL << (esz * 8U - 1U));
                            int64_t hi_b = U ? ((1LL << (esz * 8U)) - 1LL) : ((1LL << (esz * 8U - 1U)) - 1LL);
                            IRValue lo_v = splatIntVec(src_t, nlanes, esz * 16U, lo_b);
                            IRValue hi_v = splatIntVec(src_t, nlanes, esz * 16U, hi_b);
                            IRValue c_hi = B->icmp(U ? "ugt" : "sgt", src_t, vin, hi_v, "qxt_chi");
                            IRValue s1 = B->select(src_t, c_hi, hi_v, vin, "qxt_s1");
                            IRValue tr;
                            if (U) {
                                tr = B->cast("trunc", s1, half_t, "qxt_tr");
                            } else {
                                IRValue c_lo = B->icmp("slt", src_t, s1, lo_v, "qxt_clo");
                                IRValue s2 = B->select(src_t, c_lo, lo_v, s1, "qxt_s2");
                                tr = B->cast("trunc", s2, half_t, "qxt_tr");
                            }
                            if (Q == 0) {
                                storeVec(rd, tr, true);
                            } else {
                                IRValue lo_h = loadVec(rd, half_t);
                                std::vector<int> mask;
                                for (unsigned i = 0; i < nlanes * 2; ++i) mask.push_back(static_cast<int>(i));
                                IRType full_t = vecIntTypeFor(nlanes * 2, esz * 8U);
                                IRValue res = B->CreateShuffleVector(half_t, lo_h, tr, mask, "qxt2", full_t);
                                storeVec(rd, res, false);
                            }
                        }
                        continue;
                    }

                    // AdvSIMD Across Lanes: ADDV / SMAXV / UMAXV / SMINV / UMINV
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 17) & 0x1F) == 0x18 && ((insn >> 10) & 3) == 2) {
                        unsigned opcode = (insn >> 12) & 0x1F;
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        IRType vty_t = vecIntTypeFor(lanes, esz * 8);
                        IRType sty_t = intTypeForBytes(esz);
                        IRValue vin = loadVec(rn, vty_t);
                        const char* intr = "add";
                        if (opcode == 0x1B) intr = "add";
                        else if (opcode == 0x0A) intr = U ? "umax" : "smax";
                        else if (opcode == 0x1A) intr = U ? "umin" : "smin";
                        std::string iname = std::string("llvm.vector.reduce.") + intr + ".v" + std::to_string(lanes) + "i" + std::to_string(esz * 8);
                        IRValue res = B->CreateCall(sty_t, iname, {vin}, "vred", IROpTag::RawIR);
                        storeVec(rd, res);
                        continue;
                    }

                    // AdvSIMD TBL / TBX
                    if (((insn >> 24) & 0xBF) == 0x0E && ((insn >> 21) & 1) == 0 && ((insn >> 15) & 1) == 0 && ((insn >> 10) & 3) == 0) {
                        unsigned len = ((insn >> 13) & 3U) + 1U;
                        unsigned is_tbx = (insn >> 12) & 1U;
                        unsigned nbytes = Q ? 16U : 8U;
                        IRType vty_t = vecIntTypeFor(nbytes, 8);
                        std::vector<IRValue> tbl_regs;
                        for (unsigned t = 0; t < len; ++t) {
                            tbl_regs.push_back(loadVec((rn + t) & 31U, IRType::Vec16xI8));
                        }
                        IRValue idx_v = loadVec(rm, vty_t);
                        IRValue base_out = is_tbx ? loadVec(rd, vty_t) : IRValue::zero_vec(vty_t);
                        IRValue cur_out = base_out;
                        for (unsigned i = 0; i < nbytes; ++i) {
                            IRValue ie = B->CreateExtractElement(vty_t, IRType::Int8, idx_v, i, "tbl_i");
                            IRValue acc = is_tbx ? B->CreateExtractElement(vty_t, IRType::Int8, base_out, i, "tbx_fb")
                                                 : IRValue::imm(IRType::Int8, 0);
                            for (unsigned t = 0; t < len; ++t) {
                                IRValue ic = B->icmp("ult", IRType::Int8, ie, IRValue::imm(IRType::Int8, (t + 1U) * 16U), "tbl_c");
                                IRValue in_rng = ic;
                                if (t > 0) {
                                    IRValue ge = B->icmp("uge", IRType::Int8, ie, IRValue::imm(IRType::Int8, t * 16U), "tbl_ge");
                                    in_rng = B->binop("and", IRType::Int1, ic, ge, "tbl_ir");
                                }
                                IRValue cl = B->binop("and", IRType::Int8, ie, IRValue::imm(IRType::Int8, 15), "tbl_cl");
                                IRValue tv = B->CreateExtractElementDyn(IRType::Vec16xI8, IRType::Int8, tbl_regs[t], cl, "tbl_v");
                                acc = B->select(IRType::Int8, in_rng, tv, acc, "tbl_s");
                            }
                            cur_out = B->CreateInsertElement(vty_t, cur_out, acc, i, "tbl_o");
                        }
                        storeVec(rd, cur_out, Q == 0);
                        continue;
                    }
                }
            }

            if (!terminated) {
                if (b_stop < end_addr) B->CreateBr("bb_" + hex8(b_stop), b_stop);
                else B->CreateBr("exit_ret", end_addr);
            }
        }
        IRBasicBlock *exit_bb = fn->create_block("exit_ret", end_addr);
        B->set_insert_point(exit_bb);
        flushState(0x000001FFU, true, 8);
        if (instrument_calls) {
            B->CreateVoidCall("aot_so_trace_fn_exit",
                              {cpu, IRValue::imm_u64(cfg.image_base + faddr),
                               IRValue::global_sym("@." + cfg.prefix + "_fn_str_" + hex8(faddr))});
        }
        B->CreateRetVoid(end_addr);
    }

    void populateModuleHeader(IRModule &M, const std::set<uint64_t> &defined_funcs, bool is_runtime_chunk) {
        M.emit_horizon_runtime_decls = false;
        M.needs_noalias_md = true;
        M.target_triple = "x86_64-pc-linux-gnu";
        M.data_layout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";

        static const char *const kFixedDecls[] = {
            "declare void @llvm.mali.arm64.hint(i32)",
            "declare void @llvm.mali.arm64.retaa()",
            "declare i64 @llvm.mali.arm64.mrs(ptr, i32)",
            "declare void @llvm.mali.arm64.dc_zva(i64)",
            "declare i64 @llvm.mali.bionic.tlsdesc(i64)",
            "declare ptr @calloc(i64, i64)",
            "declare ptr @realloc(ptr, i64)",
            "declare void @free(ptr)",
            "declare i32 @getpid()",
            "declare i32 @access(ptr, i32)",
            "declare i32 @stat(ptr, ptr)",
            "declare i32 @fstat(i32, ptr)",
            "declare i32 @dup(i32)",
            "declare i32 @dup2(i32, i32)",
            "declare i32 @fcntl(i32, i32, ...)",
            "declare i32 @eventfd(i32, i32)",
            "declare i32 @clock_gettime(i32, ptr)",
            "declare ptr @strncpy(ptr, ptr, i64)",
            "declare ptr @strndup(ptr, i64)",
            "declare i64 @strnlen(ptr, i64)",
            "declare i64 @strspn(ptr, ptr)",
            "declare ptr @strstr(ptr, ptr)",
            "declare i64 @strtol(ptr, ptr, i32)",
            "declare i64 @strtoul(ptr, ptr, i32)",
            "declare i64 @strtoull(ptr, ptr, i32)",
            "declare i64 @strlen(ptr)",
            "declare ptr @strchr(ptr, i32)",
            "declare i64 @strcspn(ptr, ptr)",
            "declare i32 @strncasecmp(ptr, ptr, i64)",
            "declare i32 @strcmp(ptr, ptr)",
            "declare i32 @strncmp(ptr, ptr, i64)",
            "declare ptr @strncat(ptr, ptr, i64)",
            "declare i32 @memcmp(ptr, ptr, i64)",
            "declare ptr @__errno_location()",
            "declare i64 @bionic_aapcs64_vsnprintf_bridge(i64, i64, i64, i64)",
            "declare i64 @bionic_mali_kbase_ioctl_stub(i64, i64, i64)",
            "declare i64 @bionic_mali_open_stub(i64, i64)",
            "declare i64 @bionic_mali_close_stub(i64)",
            "declare i64 @bionic_mali_mmap_stub(i64, i64, i64, i64, i64, i64)",
            "declare i64 @bionic_mali_munmap_stub(i64, i64)",
            "declare i64 @bionic_mali_read_stub(i64, i64, i64)",
            "declare i64 @bionic_mali_poll_stub(i64, i64, i64)",
            "declare i64 @bionic_mali_stat_stub(i64, i64)",
            "declare i64 @bionic_mali_fstat_stub(i64, i64)",
            "declare i64 @bionic_mali_access_stub(i64, i64)",
            "declare void @aot_unhandled_plt(ptr, i64)",
            "declare void @aot_so_trace_fn_enter(ptr, i64, ptr)",
            "declare void @aot_so_trace_fn_exit(ptr, i64, ptr)",
            "declare i32 @llvm.fshr.i32(i32, i32, i32)",
            "declare i64 @llvm.fshr.i64(i64, i64, i64)",
            "declare <4 x i32> @llvm.fshr.v4i32(<4 x i32>, <4 x i32>, <4 x i32>)",
            "declare i32 @llvm.bitreverse.i32(i32)",
            "declare i64 @llvm.bitreverse.i64(i64)",
            "declare i32 @llvm.bswap.i32(i32)",
            "declare i64 @llvm.bswap.i64(i64)",
            "declare <2 x i32> @llvm.bswap.v2i32(<2 x i32>)",
            "declare <4 x i32> @llvm.bswap.v4i32(<4 x i32>)",
            "declare i32 @llvm.ctlz.i32(i32, i1 immarg)",
            "declare i64 @llvm.ctlz.i64(i64, i1 immarg)",
            "declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg)",
            "declare void @llvm.memmove.p0.p0.i64(ptr nocapture writeonly, ptr nocapture readonly, i64, i1 immarg)",
            "declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)",
            "declare <8 x i8> @llvm.ctpop.v8i8(<8 x i8>)",
            "declare <16 x i8> @llvm.ctpop.v16i8(<16 x i8>)",
            "declare i8 @llvm.vector.reduce.add.v8i8(<8 x i8>)",
            "declare i8 @llvm.vector.reduce.add.v16i8(<16 x i8>)",
            "declare i16 @llvm.vector.reduce.add.v4i16(<4 x i16>)",
            "declare i16 @llvm.vector.reduce.add.v8i16(<8 x i16>)",
            "declare i32 @llvm.vector.reduce.add.v2i32(<2 x i32>)",
            "declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)",
            "declare i8 @llvm.vector.reduce.umax.v8i8(<8 x i8>)",
            "declare i8 @llvm.vector.reduce.umax.v16i8(<16 x i8>)",
            "declare i16 @llvm.vector.reduce.umax.v4i16(<4 x i16>)",
            "declare i16 @llvm.vector.reduce.umax.v8i16(<8 x i16>)",
            "declare i32 @llvm.vector.reduce.umax.v2i32(<2 x i32>)",
            "declare i32 @llvm.vector.reduce.umax.v4i32(<4 x i32>)",
            "declare i8 @llvm.vector.reduce.smax.v8i8(<8 x i8>)",
            "declare i8 @llvm.vector.reduce.smax.v16i8(<16 x i8>)",
            "declare i16 @llvm.vector.reduce.smax.v4i16(<4 x i16>)",
            "declare i16 @llvm.vector.reduce.smax.v8i16(<8 x i16>)",
            "declare i32 @llvm.vector.reduce.smax.v2i32(<2 x i32>)",
            "declare i32 @llvm.vector.reduce.smax.v4i32(<4 x i32>)",
            "declare i8 @llvm.vector.reduce.smin.v8i8(<8 x i8>)",
            "declare i8 @llvm.vector.reduce.smin.v16i8(<16 x i8>)",
            "declare i16 @llvm.vector.reduce.smin.v4i16(<4 x i16>)",
            "declare i16 @llvm.vector.reduce.smin.v8i16(<8 x i16>)",
            "declare i32 @llvm.vector.reduce.smin.v2i32(<2 x i32>)",
            "declare i32 @llvm.vector.reduce.smin.v4i32(<4 x i32>)",
            "declare i8 @llvm.vector.reduce.umin.v8i8(<8 x i8>)",
            "declare i8 @llvm.vector.reduce.umin.v16i8(<16 x i8>)",
            "declare i16 @llvm.vector.reduce.umin.v4i16(<4 x i16>)",
            "declare i16 @llvm.vector.reduce.umin.v8i16(<8 x i16>)",
            "declare i32 @llvm.vector.reduce.umin.v2i32(<2 x i32>)",
            "declare i32 @llvm.vector.reduce.umin.v4i32(<4 x i32>)",
            "declare float @llvm.sqrt.f32(float)",
            "declare double @llvm.sqrt.f64(double)",
            "declare float @llvm.roundeven.f32(float)",
            "declare double @llvm.roundeven.f64(double)",
            "declare float @llvm.ceil.f32(float)",
            "declare double @llvm.ceil.f64(double)",
            "declare float @llvm.floor.f32(float)",
            "declare double @llvm.floor.f64(double)",
            "declare float @llvm.trunc.f32(float)",
            "declare double @llvm.trunc.f64(double)",
            "declare float @llvm.round.f32(float)",
            "declare double @llvm.round.f64(double)",
            "declare float @llvm.rint.f32(float)",
            "declare double @llvm.rint.f64(double)",
            "declare float @llvm.nearbyint.f32(float)",
            "declare double @llvm.nearbyint.f64(double)",
        };
        for (const char *d : kFixedDecls) M.extra_decls.emplace_back(d);

        std::set<std::string> plt_syms = {"malloc", "free"};
        for (const auto &kv : loader.closure_plts) {
            if (isHotInlinePltSym(kv.second)) plt_syms.insert(kv.second);
        }
        for (const auto &sym : plt_syms) {
            M.extra_decls.push_back("declare void @llvm.mali.bionic.plt64." + sym + "(ptr)");
        }
        if (!plt_index_map.empty()) {
            M.global_defs.push_back("@" + cfg.plt_table_name + " = external global ptr, align 8");
        }
        if (!is_runtime_chunk) {
            M.extra_decls.push_back("declare void @" + cfg.prefix + "_indirect_dispatch(ptr, i64)");
        }
        const std::set<uint64_t> &needed_decls = is_runtime_chunk
            ? std::set<uint64_t>(loader.closure_funcs.begin(), loader.closure_funcs.end())
            : called_guest_fns;
        for (uint64_t faddr : needed_decls) {
            if (!defined_funcs.count(faddr)) {
                M.external_guest_fns.insert(cfg.prefix + "_sub_" + hex8(faddr));
            }
        }
        if (instrument_calls && !defined_funcs.empty()) {
            for (uint64_t faddr : defined_funcs) {
                auto fname_sz = loader.func_by_addr.count(faddr) ? loader.func_by_addr.at(faddr) : std::make_pair("sub_" + hex8(faddr), uint64_t(4));
                std::string fn_label = fname_sz.first.empty() ? ("sub_" + hex8(faddr)) : fname_sz.first;
                M.global_defs.push_back("@." + cfg.prefix + "_fn_str_" + hex8(faddr) + " = private unnamed_addr constant [" +
                                        std::to_string(fn_label.size() + 1) + " x i8] c\"" + escapeIrString(fn_label) + "\\00\", align 1");
            }
        }
    }

    void buildWrappersAndRuntime(IRModule &M) {
        // @<prefix>_get_init_array_count() -> i64
        M.extra_raw_functions.push_back(
            "define i64 @" + cfg.prefix + "_get_init_array_count() nounwind {\n"
            "  ret i64 " + std::to_string(loader.init_array_funcs.size()) + "\n"
            "}");

        // @<prefix>_get_init_array_entry(i64 %idx) -> i64
        {
            std::ostringstream fn_ss;
            fn_ss << "define i64 @" << cfg.prefix << "_get_init_array_entry(i64 %idx) nounwind {\nentry:\n"
                  << "  switch i64 %idx, label %def [ ";
            for (size_t i = 0; i < loader.init_array_funcs.size(); ++i) {
                fn_ss << "i64 " << i << ", label %e" << i << " ";
            }
            fn_ss << "]\ndef:\n  ret i64 0\n";
            for (size_t i = 0; i < loader.init_array_funcs.size(); ++i) {
                fn_ss << "e" << i << ":\n  ret i64 " << (cfg.image_base + loader.init_array_funcs[i]) << "\n";
            }
            fn_ss << "}";
            M.extra_raw_functions.push_back(fn_ss.str());
        }

        // @<prefix>_run_dt_init_array_entry(ptr %c, i64 %idx)
        {
            std::ostringstream fn_ss;
            fn_ss << "define void @" << cfg.prefix << "_run_dt_init_array_entry(ptr %c, i64 %idx) nounwind {\nentry:\n"
                  << "  switch i64 %idx, label %done [ ";
            for (size_t i = 0; i < loader.init_array_funcs.size(); ++i) {
                if (func_set.count(loader.init_array_funcs[i])) {
                    fn_ss << "i64 " << i << ", label %init_" << i << " ";
                }
            }
            fn_ss << "]\n";
            for (size_t i = 0; i < loader.init_array_funcs.size(); ++i) {
                uint64_t rva = loader.init_array_funcs[i];
                if (func_set.count(rva)) {
                    fn_ss << "init_" << i << ":\n  call void @" << cfg.prefix << "_sub_" << hex8(rva) << "(ptr %c)\n  ret void\n";
                }
            }
            fn_ss << "done:\n  ret void\n}";
            M.extra_raw_functions.push_back(fn_ss.str());
        }

        // @<prefix>_run_dt_init_array(ptr %c) via IRBuilder
        {
            IRFunction *fn = M.create_function(cfg.prefix + "_run_dt_init_array", 0, 0);
            IRBasicBlock *bb = fn->create_block("entry", 0);
            IRBuilder wb(fn, bb);
            for (uint64_t rva : loader.init_array_funcs) {
                if (func_set.count(rva)) {
                    wb.CreateDirectCall(cfg.prefix + "_sub_" + hex8(rva), 0, rva);
                }
            }
            wb.CreateRetVoid(0);
        }

        if (!cfg.is_libcpp) {
            std::set<std::string> emitted_aot_wrappers;
            auto emit_wrapper = [&](const std::string &wname, uint64_t rva) {
                IRFunction *fn = M.create_function("aot_" + wname, rva, rva + 4);
                IRBasicBlock *bb = fn->create_block("entry", rva);
                IRBuilder wb(fn, bb);
                wb.CreateDirectCall("aot_sub_" + hex8(rva), rva, rva);
                wb.CreateRetVoid(rva);
            };
            for (const auto &vk_ent : loader.vk_dispatch_entries) {
                uint64_t rva = loader.findFuncStart(vk_ent.second);
                if (rva == 0) rva = vk_ent.second;
                if (func_set.count(rva) && emitted_aot_wrappers.insert(vk_ent.first).second) {
                    emit_wrapper(vk_ent.first, rva);
                }
            }
            for (const char *sym : {
                     "cmpbe_v2_deserialize_MBS2_to_C", "cmpbe_v2_init_options", "cmpbe_v2_set_option_value",
                     "cmpbe_v2_free_compilation_result", "cmpbe_v2_compile_multiple_shaders",
                     "cmpbe_v2_compile_single_shader", "cmpbe_blend_shadergen_generate_shader",
                     "cmpbep_v2_get_variant_info", "cmpbe_has_robustness", "cmpbep_shutdown_llvm",
                     "vk_hal_open", "vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
                     "vkEnumerateInstanceVersion", "vkEnumerateInstanceLayerProperties",
                     "vkEnumerateInstanceExtensionProperties", "vkEnumeratePhysicalDevices",
                     "vkEnumeratePhysicalDeviceGroups", "vkGetPhysicalDeviceProperties",
                     "vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceFeatures2",
                     "vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceMemoryProperties2",
                     "vkGetPhysicalDeviceQueueFamilyProperties", "vkGetPhysicalDeviceQueueFamilyProperties2",
                     "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties",
                     "vkGetDeviceQueue", "vkGetDeviceQueue2", "vkGetBufferMemoryRequirements",
                     "vkGetBufferMemoryRequirements2", "vkGetBufferDeviceAddress",
                     "vkGetImageMemoryRequirements", "vkGetImageMemoryRequirements2",
                     "vkMapMemory", "vkUnmapMemory", "vkResetFences", "vkDestroyFence",
                     "vkDestroyDescriptorSetLayout", "vkDestroyPipelineLayout",
                     "vkCreateShaderModule", "vkDestroyShaderModule",
                     "vkCmdSetBlendConstants", "vkCmdSetDepthBias", "vkCmdSetDepthBounds",
                     "vkCmdSetLineWidth", "vkCmdSetStencilCompareMask", "vkCmdSetStencilReference",
                     "vkCmdSetStencilWriteMask", "mali_gpu_props_get",
                     "kbase_version_check_csf", "kbase_version_check_jm", "kbase_version_check_reserved",
                     "kbase_set_flags", "kbase_job_submit", "kbase_post_term", "kbase_soft_event_update",
                     "kbase_mem_jit_init", "kbase_mem_exec_init", "kbase_mem_alloc", "kbase_mem_free",
                     "kbase_mem_query", "kbase_mem_commit", "kbase_mem_alias", "kbase_mem_import",
                     "kbase_mem_flags_change", "kbase_mem_sync", "kbase_cs_get_glb_iface",
                     "kbase_cs_queue_register", "kbase_cs_queue_register_ex", "kbase_cs_queue_terminate",
                     "kbase_cs_queue_bind", "kbase_cs_queue_kick", "kbase_cs_queue_group_create",
                     "kbase_cs_queue_group_terminate", "kbase_cs_tiler_heap_init", "kbase_cs_tiler_heap_term",
                     "kbase_kcpu_queue_create", "kbase_kcpu_queue_delete", "kbase_kcpu_queue_enqueue",
                 }) {
                if (loader.addr_by_name.count(sym) && !emitted_aot_wrappers.count(sym)) {
                    uint64_t rva = loader.findFuncStart(loader.addr_by_name.at(sym));
                    if (func_set.count(rva) && emitted_aot_wrappers.insert(sym).second) {
                        emit_wrapper(sym, rva);
                    }
                }
            }
        }

        // @<prefix>_indirect_dispatch(ptr %c, i64 %target_va)
        {
            std::ostringstream fn_ss;
            fn_ss << "define void @" << cfg.prefix << "_indirect_dispatch(ptr %c, i64 %target_va) nounwind {\n"
                  << "entry:\n"
                  << "  %is_null = icmp eq i64 %target_va, 0\n"
                  << "  br i1 %is_null, label %handle_null, label %check_malloc\n"
                  << "handle_null:\n  ret void\n"
                  << "check_malloc:\n"
                  << "  %is_malloc = icmp eq i64 %target_va, " << HLE_MALLOC_MAGIC << "\n"
                  << "  br i1 %is_malloc, label %handle_malloc, label %check_free\n"
                  << "handle_malloc:\n  call void @llvm.mali.bionic.plt64.malloc(ptr %c)\n  ret void\n"
                  << "check_free:\n"
                  << "  %is_free = icmp eq i64 %target_va, " << HLE_FREE_MAGIC << "\n"
                  << "  br i1 %is_free, label %handle_free, label %check_vk_alloc\n"
                  << "handle_free:\n  call void @llvm.mali.bionic.plt64.free(ptr %c)\n  ret void\n"
                  << "check_vk_alloc:\n"
                  << "  %is_vk_alloc = icmp eq i64 %target_va, " << HLE_VK_ALLOC_MAGIC << "\n"
                  << "  br i1 %is_vk_alloc, label %handle_vk_alloc, label %check_vk_free\n"
                  << "handle_vk_alloc:\n"
                  << "  %vk_x1_ptr = getelementptr inbounds i8, ptr %c, i64 8\n"
                  << "  %vk_sz = load i64, ptr %vk_x1_ptr, align 8\n"
                  << "  %vk_mem = call ptr @calloc(i64 %vk_sz, i64 1)\n"
                  << "  %vk_mem_i = ptrtoint ptr %vk_mem to i64\n"
                  << "  %vk_x0_ptr = getelementptr inbounds i8, ptr %c, i64 0\n"
                  << "  store i64 %vk_mem_i, ptr %vk_x0_ptr, align 8\n  ret void\n"
                  << "check_vk_free:\n"
                  << "  %is_vk_free = icmp eq i64 %target_va, " << HLE_VK_FREE_MAGIC << "\n"
                  << "  br i1 %is_vk_free, label %handle_vk_free, label %check_tlsdesc\n"
                  << "handle_vk_free:\n"
                  << "  %vkf_x1_ptr = getelementptr inbounds i8, ptr %c, i64 8\n"
                  << "  %vkf_addr = load i64, ptr %vkf_x1_ptr, align 8\n"
                  << "  %vkf_ptr = inttoptr i64 %vkf_addr to ptr\n"
                  << "  call void @free(ptr %vkf_ptr)\n  ret void\n"
                  << "check_tlsdesc:\n"
                  << "  %is_tlsdesc = icmp eq i64 %target_va, " << HLE_TLSDESC_MAGIC << "\n"
                  << "  br i1 %is_tlsdesc, label %handle_tlsdesc, label %dispatch_switch\n"
                  << "handle_tlsdesc:\n"
                  << "  %x0_ptr = getelementptr inbounds i8, ptr %c, i64 0\n"
                  << "  %desc_addr = load i64, ptr %x0_ptr, align 8\n"
                  << "  %tls_off = call i64 @llvm.mali.bionic.tlsdesc(i64 %desc_addr)\n"
                  << "  store i64 %tls_off, ptr %x0_ptr, align 8\n  ret void\n"
                  << "dispatch_switch:\n  switch i64 %target_va, label %fallback [ ";
            for (uint64_t faddr : loader.closure_funcs) {
                fn_ss << "i64 " << (cfg.image_base + faddr) << ", label %call_" << hex8(faddr) << " ";
            }
            fn_ss << "]\n";
            for (uint64_t faddr : loader.closure_funcs) {
                fn_ss << "call_" << hex8(faddr) << ":\n  call void @" << cfg.prefix << "_sub_" << hex8(faddr) << "(ptr %c)\n  ret void\n";
            }
            fn_ss << "fallback:\n  call void @aot_unhandled_plt(ptr %c, i64 %target_va)\n  ret void\n}";
            M.extra_raw_functions.push_back(fn_ss.str());
        }

        if (!cfg.is_libcpp) {
            M.extra_raw_functions.push_back(
                "define i64 @__mali_cxa_guard_acquire(i64 %guard_addr) nounwind {\nentry:\n"
                "  %is_null = icmp eq i64 %guard_addr, 0\n"
                "  br i1 %is_null, label %ret_zero, label %check\ncheck:\n"
                "  %p = inttoptr i64 %guard_addr to ptr\n  %v = load i8, ptr %p, align 1\n"
                "  %uninit = icmp eq i8 %v, 0\n  %res = select i1 %uninit, i64 1, i64 0\n  ret i64 %res\n"
                "ret_zero:\n  ret i64 0\n}");
            M.extra_raw_functions.push_back(
                "define void @__mali_cxa_guard_release(i64 %guard_addr) nounwind {\nentry:\n"
                "  %is_null = icmp eq i64 %guard_addr, 0\n"
                "  br i1 %is_null, label %done, label %write\nwrite:\n"
                "  %p = inttoptr i64 %guard_addr to ptr\n  store i8 1, ptr %p, align 1\n  br label %done\n"
                "done:\n  ret void\n}");
            M.extra_raw_functions.push_back(
                "define i64 @__mali_next_prime(i64 %n) nounwind {\nentry:\n"
                "  %lt = icmp ult i64 %n, 17\n  %cand = select i1 %lt, i64 17, i64 %n\n"
                "  %or1 = or i64 %cand, 1\n  ret i64 %or1\n}");
            M.extra_raw_functions.push_back(
                "define i64 @__mali_sysconf_bionic(i64 %name) nounwind {\nentry:\n"
                "  %is_39 = icmp eq i64 %name, 39\n  %is_40 = icmp eq i64 %name, 40\n  %is_30 = icmp eq i64 %name, 30\n"
                "  %is_ps1 = or i1 %is_39, %is_40\n  %is_ps = or i1 %is_ps1, %is_30\n"
                "  %res = select i1 %is_ps, i64 4096, i64 8\n  ret i64 %res\n}");
            M.extra_raw_functions.push_back(
                "define i64 @__mali_ahb_stub(i64 %op, i64 %a0, i64 %a1) nounwind {\nentry:\n"
                "  %is_alloc = icmp eq i64 %op, 1\n  br i1 %is_alloc, label %do_alloc, label %chk_desc\ndo_alloc:\n"
                "  %mem = call ptr @calloc(i64 256, i64 1)\n  %mem_i = ptrtoint ptr %mem to i64\n"
                "  %has_d = icmp ne i64 %a0, 0\n  br i1 %has_d, label %copy_d, label %write_nh\ncopy_d:\n"
                "  %src_p = inttoptr i64 %a0 to ptr\n  call void @llvm.memcpy.p0.p0.i64(ptr %mem, ptr %src_p, i64 40, i1 false)\n  br label %write_nh\nwrite_nh:\n"
                "  %nh_p = getelementptr inbounds i8, ptr %mem, i64 64\n  store i32 12, ptr %nh_p, align 4\n"
                "  %nf_p = getelementptr inbounds i8, ptr %mem, i64 68\n  store i32 1, ptr %nf_p, align 4\n"
                "  %fd_p = getelementptr inbounds i8, ptr %mem, i64 76\n  store i32 100, ptr %fd_p, align 4\n"
                "  %has_out = icmp ne i64 %a1, 0\n  br i1 %has_out, label %store_out, label %ret_ok\nstore_out:\n"
                "  %out_p = inttoptr i64 %a1 to ptr\n  store i64 %mem_i, ptr %out_p, align 8\n  br label %ret_ok\nchk_desc:\n"
                "  %is_desc = icmp eq i64 %op, 2\n  br i1 %is_desc, label %do_desc, label %chk_nh\ndo_desc:\n"
                "  %ok_d1 = icmp ne i64 %a0, 0\n  %ok_d2 = icmp ne i64 %a1, 0\n  %ok_d = and i1 %ok_d1, %ok_d2\n"
                "  br i1 %ok_d, label %cp_desc, label %ret_ok\ncp_desc:\n"
                "  %dp0 = inttoptr i64 %a0 to ptr\n  %dp1 = inttoptr i64 %a1 to ptr\n"
                "  call void @llvm.memcpy.p0.p0.i64(ptr %dp1, ptr %dp0, i64 40, i1 false)\n  br label %ret_ok\nchk_nh:\n"
                "  %is_nh = icmp eq i64 %op, 3\n  br i1 %is_nh, label %do_nh, label %chk_id\ndo_nh:\n"
                "  %nh_va = add i64 %a0, 64\n  ret i64 %nh_va\nchk_id:\n"
                "  %is_id = icmp eq i64 %op, 4\n  %has_id_out = icmp ne i64 %a1, 0\n"
                "  %do_id_st = and i1 %is_id, %has_id_out\n  br i1 %do_id_st, label %st_id, label %ret_ok\nst_id:\n"
                "  %idp = inttoptr i64 %a1 to ptr\n  store i64 1095254529, ptr %idp, align 8\n  br label %ret_ok\n"
                "ret_ok:\n  ret i64 0\n}");
        }
    }

    static void runHorizonLoweringPasses(IRModule &M) {
        ultrs::passes::ModulePassManager mpm;
        mpm.addPass(std::make_unique<ultrs::passes::AArch64SysBarrierLoweringPass>(
            ultrs::passes::LoweringMode::UserspaceBionicSo));
        mpm.addPass(std::make_unique<ultrs::passes::GuestStackAndAlignmentPass>(true));
        mpm.addPass(std::make_unique<ultrs::passes::NZCVDeadFlagElimPass>(true));
        mpm.run(M);
    }

    template <typename ChunkCallback>
    void generateChunkedModules(size_t chunk_limit_bytes, ChunkCallback &&on_chunk) {
        size_t chunk_idx = 0;
        size_t funcs_done = 0;
        const size_t total_funcs = loader.closure_funcs.size();
        auto cur_mod = std::make_unique<IRModule>(cfg.prefix + "_chunk", "x86_64-pc-linux-gnu");
        std::set<uint64_t> chunk_funcs;
        size_t approx_ir_bytes = 0;

        auto flush_func_chunk = [&]() {
            if (chunk_funcs.empty()) return;
            populateModuleHeader(*cur_mod, chunk_funcs, false);
            runHorizonLoweringPasses(*cur_mod);
            std::string full_ir = cur_mod->serialize();
            char tag[32];
            std::snprintf(tag, sizeof(tag), "chunk_%02zu", chunk_idx++);
            on_chunk(tag, std::move(full_ir), funcs_done, total_funcs);
            chunk_funcs.clear();
            called_guest_fns.clear();
            approx_ir_bytes = 0;
            cur_mod = std::make_unique<IRModule>(cfg.prefix + "_chunk", "x86_64-pc-linux-gnu");
        };

        for (uint64_t faddr : loader.closure_funcs) {
            IRFunction *fn = cur_mod->create_function(cfg.prefix + "_sub_" + hex8(faddr), faddr, faddr + 4);
            liftFunction(faddr, *cur_mod, fn);
            size_t fn_est = 0;
            for (const auto &bb : fn->blocks) fn_est += bb->insts.size() * 64;
            approx_ir_bytes += fn_est;
            chunk_funcs.insert(faddr);
            funcs_done++;
            if (chunk_limit_bytes > 0 && approx_ir_bytes > chunk_limit_bytes) {
                flush_func_chunk();
            }
        }
        flush_func_chunk();

        // Emit runtime/wrapper chunk
        std::set<uint64_t> empty_defs;
        IRModule wrap_mod(cfg.prefix + "_wrappers", "x86_64-pc-linux-gnu");
        populateModuleHeader(wrap_mod, empty_defs, true);
        buildWrappersAndRuntime(wrap_mod);
        runHorizonLoweringPasses(wrap_mod);
        on_chunk("wrappers", wrap_mod.serialize(), total_funcs, total_funcs);
    }
};

// ============================================================================
// Pass 4 & Pass 5: Arm64BionicIdiosyncrasyLoweringPass & BionicToGlibcLibcLoweringPass
// ============================================================================
struct PassStats {
    int pac_bti_stripped = 0;
    int retaa_lowered = 0;
    int sysreg_lowered = 0;
    int dc_zva_lowered = 0;
    int tlsdesc_lowered = 0;
    int bionic_plt_lowered = 0;
    int external_stubs_lowered = 0;
    int kbase_uapi_lowered = 0;
};

static LLVMValueRef getOrCreateFunction(LLVMModuleRef M, const char *name, LLVMTypeRef retTy, std::vector<LLVMTypeRef> paramTys, bool isVarArg = false) {
    LLVMValueRef fn = LLVMGetNamedFunction(M, name);
    if (fn) return fn;
    LLVMTypeRef fnTy = LLVMFunctionType(retTy, paramTys.empty() ? nullptr : paramTys.data(), (unsigned)paramTys.size(), isVarArg ? 1 : 0);
    return LLVMAddFunction(M, name, fnTy);
}

void runArm64BionicIdiosyncrasyLoweringPass(LLVMModuleRef M, PassStats &stats) {
    LLVMContextRef C = LLVMGetModuleContext(M);
    LLVMTypeRef i1Ty  = LLVMInt1TypeInContext(C);
    LLVMTypeRef i8Ty  = LLVMInt8TypeInContext(C);
    LLVMTypeRef i64Ty = LLVMInt64TypeInContext(C);
    LLVMTypeRef ptrTy = LLVMPointerTypeInContext(C, 0);
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(C);

    LLVMBuilderRef B = LLVMCreateBuilderInContext(C);

    for (LLVMValueRef fn = LLVMGetFirstFunction(M); fn; fn = LLVMGetNextFunction(fn)) {
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
            for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; ) {
                LLVMValueRef nextInst = LLVMGetNextInstruction(inst);
                if (!LLVMIsACallInst(inst)) { inst = nextInst; continue; }
                LLVMValueRef callee = LLVMGetCalledValue(inst);
                if (!callee) { inst = nextInst; continue; }
                size_t nameLen = 0;
                const char *cname = LLVMGetValueName2(callee, &nameLen);
                if (!cname || nameLen == 0) { inst = nextInst; continue; }
                std::string name(cname, nameLen);

                if (name == "llvm.mali.arm64.hint") {
                    LLVMInstructionEraseFromParent(inst);
                    stats.pac_bti_stripped++;
                } else if (name == "llvm.mali.arm64.retaa") {
                    LLVMInstructionEraseFromParent(inst);
                    stats.retaa_lowered++;
                } else if (name == "llvm.mali.arm64.mrs") {
                    LLVMPositionBuilderBefore(B, inst);
                    LLVMValueRef cpuArg = LLVMGetArgOperand(inst, 0);
                    LLVMValueRef sysArg = LLVMGetArgOperand(inst, 1);
                    unsigned long long sysreg = LLVMConstIntGetZExtValue(sysArg);
                    LLVMValueRef repl = nullptr;
                    if (sysreg == 0xde82ULL || sysreg == 0xde83ULL) {
                        LLVMValueRef off = LLVMConstInt(i64Ty, 264, 0);
                        LLVMValueRef p = LLVMBuildGEP2(B, i8Ty, cpuArg, &off, 1, "tpidr_ptr");
                        repl = LLVMBuildLoad2(B, i64Ty, p, "tpidr_el0_val");
                    } else if (sysreg == 0xd807ULL) {
                        repl = LLVMConstInt(i64Ty, 4, 0);
                    } else {
                        repl = LLVMConstInt(i64Ty, 0, 0);
                    }
                    LLVMReplaceAllUsesWith(inst, repl);
                    LLVMInstructionEraseFromParent(inst);
                    stats.sysreg_lowered++;
                } else if (name == "llvm.mali.arm64.dc_zva") {
                    LLVMPositionBuilderBefore(B, inst);
                    LLVMValueRef addr = LLVMGetArgOperand(inst, 0);
                    LLVMValueRef mask = LLVMConstInt(i64Ty, ~63ULL, 0);
                    LLVMValueRef aligned = LLVMBuildAnd(B, addr, mask, "dczva_align");
                    LLVMValueRef ptr = LLVMBuildIntToPtr(B, aligned, ptrTy, "dczva_ptr");
                    LLVMValueRef memsetFn = getOrCreateFunction(M, "llvm.memset.p0.i64", voidTy, {ptrTy, i8Ty, i64Ty, i1Ty});
                    LLVMTypeRef msetParams[4] = {ptrTy, i8Ty, i64Ty, i1Ty};
                    LLVMTypeRef memsetTy = LLVMFunctionType(voidTy, msetParams, 4, 0);
                    LLVMValueRef args[4] = {ptr, LLVMConstInt(i8Ty, 0, 0), LLVMConstInt(i64Ty, 64, 0), LLVMConstInt(i1Ty, 0, 0)};
                    LLVMBuildCall2(B, memsetTy, memsetFn, args, 4, "");
                    LLVMInstructionEraseFromParent(inst);
                    stats.dc_zva_lowered++;
                } else if (name == "llvm.mali.bionic.tlsdesc") {
                    LLVMPositionBuilderBefore(B, inst);
                    LLVMValueRef gotSlot = LLVMGetArgOperand(inst, 0);
                    LLVMValueRef slotPlus8 = LLVMBuildAdd(B, gotSlot, LLVMConstInt(i64Ty, 8, 0), "tlsdesc_addend_addr");
                    LLVMValueRef addendPtr = LLVMBuildIntToPtr(B, slotPlus8, ptrTy, "tlsdesc_addend_ptr");
                    LLVMValueRef offsetVal = LLVMBuildLoad2(B, i64Ty, addendPtr, "tlsdesc_off");
                    LLVMReplaceAllUsesWith(inst, offsetVal);
                    LLVMInstructionEraseFromParent(inst);
                    stats.tlsdesc_lowered++;
                }
                inst = nextInst;
            }
        }
    }
    LLVMDisposeBuilder(B);
}

void runBionicToGlibcLibcLoweringPass(LLVMModuleRef M, PassStats &stats) {
    LLVMContextRef C = LLVMGetModuleContext(M);
    LLVMTypeRef i1Ty  = LLVMInt1TypeInContext(C);
    LLVMTypeRef i8Ty  = LLVMInt8TypeInContext(C);
    LLVMTypeRef i32Ty = LLVMInt32TypeInContext(C);
    (void)i32Ty;
    LLVMTypeRef i64Ty = LLVMInt64TypeInContext(C);
    LLVMTypeRef ptrTy = LLVMPointerTypeInContext(C, 0);
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(C);

    LLVMBuilderRef B = LLVMCreateBuilderInContext(C);

    auto loadReg64 = [&](LLVMValueRef cpu, unsigned idx) -> LLVMValueRef {
        LLVMValueRef off = LLVMConstInt(i64Ty, idx * 8ULL, 0);
        LLVMValueRef p = LLVMBuildGEP2(B, i8Ty, cpu, &off, 1, "");
        return LLVMBuildLoad2(B, i64Ty, p, "");
    };
    auto storeReg64 = [&](LLVMValueRef cpu, unsigned idx, LLVMValueRef val) {
        LLVMValueRef off = LLVMConstInt(i64Ty, idx * 8ULL, 0);
        LLVMValueRef p = LLVMBuildGEP2(B, i8Ty, cpu, &off, 1, "");
        LLVMBuildStore(B, val, p);
    };

    LLVMTypeRef memcpySig[4] = {ptrTy, ptrTy, i64Ty, i1Ty};
    LLVMTypeRef memcpyFnTy = LLVMFunctionType(voidTy, memcpySig, 4, 0);
    LLVMTypeRef memsetSig[4] = {ptrTy, i8Ty, i64Ty, i1Ty};
    LLVMTypeRef memsetFnTy = LLVMFunctionType(voidTy, memsetSig, 4, 0);

    for (LLVMValueRef fn = LLVMGetFirstFunction(M); fn; fn = LLVMGetNextFunction(fn)) {
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
            for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; ) {
                LLVMValueRef nextInst = LLVMGetNextInstruction(inst);
                if (!LLVMIsACallInst(inst)) { inst = nextInst; continue; }
                LLVMValueRef callee = LLVMGetCalledValue(inst);
                if (!callee) { inst = nextInst; continue; }
                size_t nameLen = 0;
                const char *cname = LLVMGetValueName2(callee, &nameLen);
                if (!cname || nameLen == 0) { inst = nextInst; continue; }
                std::string name(cname, nameLen);

                const std::string p64 = "llvm.mali.bionic.plt64.";
                if (name.rfind(p64, 0) == 0) {
                    std::string sym = name.substr(p64.size());
                    LLVMPositionBuilderBefore(B, inst);
                    LLVMValueRef cpu = LLVMGetArgOperand(inst, 0);

                    if (sym == "memcpy" || sym == "__memcpy_chk") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef dst = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef src = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "llvm.memcpy.p0.p0.i64", voidTy, {ptrTy, ptrTy, i64Ty, i1Ty});
                        LLVMValueRef args[4] = {dst, src, x2, LLVMConstInt(i1Ty, 0, 0)};
                        LLVMBuildCall2(B, memcpyFnTy, fnIntr, args, 4, "");
                        storeReg64(cpu, 0, x0);
                    } else if (sym == "memmove" || sym == "__memmove_chk") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef dst = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef src = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "llvm.memmove.p0.p0.i64", voidTy, {ptrTy, ptrTy, i64Ty, i1Ty});
                        LLVMValueRef args[4] = {dst, src, x2, LLVMConstInt(i1Ty, 0, 0)};
                        LLVMBuildCall2(B, memcpyFnTy, fnIntr, args, 4, "");
                        storeReg64(cpu, 0, x0);
                    } else if (sym == "memset" || sym == "__memset_chk") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef dst = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef val8 = LLVMBuildTrunc(B, x1, i8Ty, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "llvm.memset.p0.i64", voidTy, {ptrTy, i8Ty, i64Ty, i1Ty});
                        LLVMValueRef args[4] = {dst, val8, x2, LLVMConstInt(i1Ty, 0, 0)};
                        LLVMBuildCall2(B, memsetFnTy, fnIntr, args, 4, "");
                        storeReg64(cpu, 0, x0);
                    } else if (sym == "malloc" || sym == "_Znwm" || sym == "_ZnwmRKSt9nothrow_t" ||
                               sym == "_Znam" || sym == "_ZnamRKSt9nothrow_t" || sym == "_ZnwmSt11align_val_t") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef callocFn = getOrCreateFunction(M, "calloc", ptrTy, {i64Ty, i64Ty});
                        LLVMTypeRef calParams[2] = {i64Ty, i64Ty};
                        LLVMTypeRef callocTy = LLVMFunctionType(ptrTy, calParams, 2, 0);
                        LLVMValueRef args[2] = {x0, LLVMConstInt(i64Ty, 1, 0)};
                        LLVMValueRef resPtr = LLVMBuildCall2(B, callocTy, callocFn, args, 2, "");
                        LLVMValueRef resInt = LLVMBuildPtrToInt(B, resPtr, i64Ty, "");
                        storeReg64(cpu, 0, resInt);
                    } else if (sym == "calloc") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef callocFn = getOrCreateFunction(M, "calloc", ptrTy, {i64Ty, i64Ty});
                        LLVMTypeRef calParams[2] = {i64Ty, i64Ty};
                        LLVMTypeRef callocTy = LLVMFunctionType(ptrTy, calParams, 2, 0);
                        LLVMValueRef args[2] = {x0, x1};
                        LLVMValueRef resPtr = LLVMBuildCall2(B, callocTy, callocFn, args, 2, "");
                        storeReg64(cpu, 0, LLVMBuildPtrToInt(B, resPtr, i64Ty, ""));
                    } else if (sym == "realloc") {
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, loadReg64(cpu, 0), ptrTy, "");
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef reallocFn = getOrCreateFunction(M, "realloc", ptrTy, {ptrTy, i64Ty});
                        LLVMTypeRef rParams[2] = {ptrTy, i64Ty};
                        LLVMTypeRef reallocTy = LLVMFunctionType(ptrTy, rParams, 2, 0);
                        LLVMValueRef args[2] = {p0, x1};
                        LLVMValueRef resPtr = LLVMBuildCall2(B, reallocTy, reallocFn, args, 2, "");
                        storeReg64(cpu, 0, LLVMBuildPtrToInt(B, resPtr, i64Ty, ""));
                    } else if (sym == "free" || sym == "_ZdlPv" || sym == "_ZdlPvm" ||
                               sym == "_ZdaPv" || sym == "_ZdaPvm") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef ptr = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef freeFn = getOrCreateFunction(M, "free", voidTy, {ptrTy});
                        LLVMTypeRef freeTy = LLVMFunctionType(voidTy, &ptrTy, 1, 0);
                        LLVMBuildCall2(B, freeTy, freeFn, &ptr, 1, "");
                    } else if (sym == "strlen" || sym == "__strlen_chk") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef ptr = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef strlenFn = getOrCreateFunction(M, "strlen", i64Ty, {ptrTy});
                        LLVMTypeRef strlenTy = LLVMFunctionType(i64Ty, &ptrTy, 1, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, strlenTy, strlenFn, &ptr, 1, "");
                        storeReg64(cpu, 0, res);
                    } else if (sym == "__cxa_guard_acquire") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef fn = getOrCreateFunction(M, "__mali_cxa_guard_acquire", i64Ty, {i64Ty});
                        LLVMTypeRef fTy = LLVMFunctionType(i64Ty, &i64Ty, 1, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, fTy, fn, &x0, 1, "");
                        storeReg64(cpu, 0, res);
                    } else if (sym == "__cxa_guard_release") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef fn = getOrCreateFunction(M, "__mali_cxa_guard_release", voidTy, {i64Ty});
                        LLVMTypeRef fTy = LLVMFunctionType(voidTy, &i64Ty, 1, 0);
                        LLVMBuildCall2(B, fTy, fn, &x0, 1, "");
                        storeReg64(cpu, 0, LLVMConstInt(i64Ty, 0, 0));
                    } else if (sym == "__cxa_guard_abort") {
                        storeReg64(cpu, 0, LLVMConstInt(i64Ty, 0, 0));
                    } else {
                        // Unavailable Android / MediaTek vendor library hooks & single-threaded stubs:
                        // Use an opaque helper call rather than compile-time constant 0 so LLVM -O1
                        // cannot fold a subsequent pointer dereference into `unreachable`.
                        LLVMValueRef stubFn = getOrCreateFunction(M, "__mali_unhandled_plt_stub", i64Ty, {ptrTy});
                        LLVMTypeRef sTy = LLVMFunctionType(i64Ty, &ptrTy, 1, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, sTy, stubFn, &cpu, 1, "");
                        storeReg64(cpu, 0, res);
                        stats.external_stubs_lowered++;
                    }
                    LLVMInstructionEraseFromParent(inst);
                    stats.bionic_plt_lowered++;
                }
                inst = nextInst;
            }
        }
    }
    LLVMDisposeBuilder(B);
}

// ============================================================================
// Pass 6: LLVMO2OptimizationAndX86CodegenPass
// ============================================================================
static int runLoweringAndCodegenOnModule(LLVMModuleRef M, const char *outPath, bool emitLlvm, bool runO2, const char *optIrOut = nullptr) {
    char *errMsg = nullptr;
    PassStats stats{};
    runArm64BionicIdiosyncrasyLoweringPass(M, stats);
    runBionicToGlibcLibcLoweringPass(M, stats);

    printf("[LLVM Passes] Arm64BionicIdiosyncrasyLoweringPass:\n");
    printf("  PAC/BTI hints stripped : %d\n", stats.pac_bti_stripped);
    printf("  RETAA lowered          : %d\n", stats.retaa_lowered);
    printf("  MRS sysregs lowered    : %d\n", stats.sysreg_lowered);
    printf("  DC ZVA lowered         : %d\n", stats.dc_zva_lowered);
    printf("  TLSDESC lowered        : %d\n", stats.tlsdesc_lowered);
    printf("[LLVM Passes] BionicToGlibcLibcLoweringPass:\n");
    printf("  Bionic PLT64 calls     : %d (External/Vendor stubs: %d, /dev/mali0 kbase UAPI: %d)\n",
           stats.bionic_plt_lowered, stats.external_stubs_lowered, stats.kbase_uapi_lowered);

    if (LLVMVerifyModule(M, LLVMVerifierFailureActionReturnStatus, &errMsg) != 0) {
        fprintf(stderr, "Module Verification Failed after lowering passes:\n%s\n", errMsg ? errMsg : "");
        return 1;
    }
    if (errMsg) { LLVMDisposeMessage(errMsg); errMsg = nullptr; }

    const char *triple = "x86_64-pc-linux-gnu";
    LLVMTargetRef target = nullptr;
    if (LLVMGetTargetFromTriple(triple, &target, &errMsg) != 0) {
        fprintf(stderr, "Target error: %s\n", errMsg ? errMsg : "");
        return 1;
    }
    const char *env_o2 = std::getenv("MALI_AOT_FULL_O2");
    bool full_o2 = (env_o2 && std::strcmp(env_o2, "1") == 0);
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "x86-64-v3", "", full_o2 ? 3 : 1, 2, 0);

    if (runO2) {
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        const char *pass_pipeline = full_o2 ? "default<O2>" : "default<O1>";
        LLVMErrorRef err = LLVMRunPasses(M, pass_pipeline, tm, opts);
        if (err) {
            char *emsg = LLVMGetErrorMessage(err);
            fprintf(stderr, "LLVMRunPasses(%s) failed: %s\n", pass_pipeline, emsg);
            LLVMDisposeErrorMessage(emsg);
            return 1;
        }
        LLVMDisposePassBuilderOptions(opts);
        printf("[LLVM Passes] Offloaded SROA, mem2reg, InstCombine, GVN, SimplifyCFG, ADCE, DSE via LLVMRunPasses(%s)\n", pass_pipeline);
    }

    if (optIrOut) {
        if (LLVMPrintModuleToFile(M, optIrOut, &errMsg) != 0) {
            fprintf(stderr, "PrintModule error: %s\n", errMsg ? errMsg : "");
            return 1;
        }
        printf("[LLVM Passes] Emitted optimized LLVM IR to %s\n", optIrOut);
    }

    if (emitLlvm) {
        if (LLVMPrintModuleToFile(M, outPath, &errMsg) != 0) {
            fprintf(stderr, "PrintModule error: %s\n", errMsg ? errMsg : "");
            return 1;
        }
        printf("[LLVM Passes] Emitted lowered LLVM IR to %s\n", outPath);
    } else {
        if (LLVMTargetMachineEmitToFile(tm, M, const_cast<char *>(outPath), 1, &errMsg) != 0) {
            fprintf(stderr, "EmitToFile error: %s\n", errMsg ? errMsg : "");
            return 1;
        }
        printf("[LLVM Passes] Emitted native x86-64 object file to %s\n", outPath);
    }

    LLVMDisposeTargetMachine(tm);
    return 0;
}

int mali_llvm_passes_main(int argc, char **argv) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();

    // Mode 1b: Standalone Backend --compile-ll-dir <manifest.txt> <out.o> [-j N]
    if (argc >= 4 && std::strcmp(argv[1], "--compile-ll-dir") == 0) {
        std::string manifestPath = argv[2];
        std::string objPath = argv[3];
        size_t jobs = 1;
        if (const char *env_j = std::getenv("MALI_AOT_JOBS")) {
            size_t parsed = static_cast<size_t>(std::strtoull(env_j, nullptr, 10));
            if (parsed >= 1) jobs = parsed;
        }
        for (int i = 4; i < argc; ++i) {
            if ((std::strcmp(argv[i], "-j") == 0 || std::strcmp(argv[i], "--jobs") == 0) && i + 1 < argc) {
                size_t parsed = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
                if (parsed >= 1) jobs = parsed;
            } else if (std::strncmp(argv[i], "-j", 2) == 0 && argv[i][2] >= '0' && argv[i][2] <= '9') {
                size_t parsed = static_cast<size_t>(std::strtoull(argv[i] + 2, nullptr, 10));
                if (parsed >= 1) jobs = parsed;
            }
        }
        const char *keep_ll_env = std::getenv("MALI_AOT_KEEP_LL");
        bool keep_ll = (keep_ll_env && std::strcmp(keep_ll_env, "1") == 0);

        std::ifstream mf(manifestPath);
        if (!mf) {
            fprintf(stderr, "Failed to open LL manifest: %s\n", manifestPath.c_str());
            return 1;
        }
        std::vector<std::string> ll_files;
        std::string line;
        while (std::getline(mf, line)) {
            if (!line.empty()) ll_files.push_back(line);
        }
        printf("[LLVM Backend] Compiling %zu .ll chunks -> %s (jobs=%zu)...\n", ll_files.size(), objPath.c_str(), jobs);
        std::fflush(stdout);

        std::vector<std::string> chunk_objs;
        std::vector<pid_t> active_pids;
        int rc = 0;
        auto reap_one = [&]() {
            int status = 0;
            pid_t done = waitpid(-1, &status, 0);
            if (done > 0) {
                active_pids.erase(std::remove(active_pids.begin(), active_pids.end(), done), active_pids.end());
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) rc = 1;
            }
        };

        for (size_t idx = 0; idx < ll_files.size() && rc == 0; ++idx) {
            const std::string &llPath = ll_files[idx];
            std::string chunkObj = llPath + ".o";
            std::string chunkOpt = llPath + ".opt.ll";
            chunk_objs.push_back(chunkObj);
            while (active_pids.size() >= jobs && rc == 0) reap_one();
            if (rc != 0) break;

            printf("  [Backend %zu/%zu] %s -> %s\n", idx + 1, ll_files.size(), llPath.c_str(), chunkObj.c_str());
            std::fflush(stdout);
            std::fflush(stderr);
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); rc = 1; break; }
            if (pid == 0) {
                LLVMMemoryBufferRef memBuf = nullptr;
                char *errMsg = nullptr;
                if (LLVMCreateMemoryBufferWithContentsOfFile(llPath.c_str(), &memBuf, &errMsg) != 0) {
                    fprintf(stderr, "Error reading %s: %s\n", llPath.c_str(), errMsg ? errMsg : "unknown");
                    _exit(1);
                }
                LLVMContextRef C = LLVMContextCreate();
                LLVMModuleRef M = nullptr;
                if (LLVMParseIRInContext(C, memBuf, &M, &errMsg) != 0) {
                    fprintf(stderr, "LLVM IR Parse Error (%s): %s\n", llPath.c_str(), errMsg ? errMsg : "unknown");
                    LLVMContextDispose(C);
                    _exit(1);
                }
                int crc = runLoweringAndCodegenOnModule(M, chunkObj.c_str(), false, true, keep_ll ? chunkOpt.c_str() : nullptr);
                LLVMDisposeModule(M);
                LLVMContextDispose(C);
                std::fflush(stdout);
                std::fflush(stderr);
                _exit(crc);
            }
            active_pids.push_back(pid);
        }
        while (!active_pids.empty()) reap_one();
        if (rc != 0) return rc;

        std::string ld_cmd = "ld -r -o \"" + objPath + "\"";
        for (const auto &co : chunk_objs) ld_cmd += " \"" + co + "\"";
        printf("[Link] Merging %zu chunk objects into %s...\n", chunk_objs.size(), objPath.c_str());
        int ld_rc = std::system(ld_cmd.c_str());
        if (ld_rc != 0) {
            fprintf(stderr, "ld -r failed with exit code %d\n", ld_rc);
            return 1;
        }
        for (const auto &co : chunk_objs) std::remove(co.c_str());
        if (!keep_ll) {
            for (const auto &ll : ll_files) std::remove(ll.c_str());
            std::remove(manifestPath.c_str());
        }
        return 0;
    }

    // Mode 1: Full C++ 6-Pass Pipeline directly from 64-bit libGLES_mali.so or libc++_shared.so
    // Usage: mali_llvm_pass_driver --lift-so <libGLES_mali.so> <out_dir> [--emit-ll-only]
    //        mali_llvm_pass_driver --lift-libcpp <libc++_shared.so> <out_dir> [--emit-ll-only]
    if (argc >= 4 && (std::strcmp(argv[1], "--lift-so") == 0 || std::strcmp(argv[1], "--lift-libcpp") == 0)) {
        bool is_libcpp = (std::strcmp(argv[1], "--lift-libcpp") == 0);
        LifterConfig cfg;
        cfg.is_libcpp = is_libcpp;
        cfg.prefix = is_libcpp ? "libcpp" : "aot";
        cfg.plt_table_name = is_libcpp ? "g_libcpp_plt_table" : "g_aot_plt_table";
        cfg.image_base = is_libcpp ? 0x500000000ULL : 0x400000000ULL;
        cfg.seed_all_dynsyms = is_libcpp;

        std::string soPath = argv[2];
        std::string outDir = argv[3];

        printf("[LLVM Pass 1] ELF64LoaderAndCallGraphPass (%s): %s\n", cfg.prefix.c_str(), soPath.c_str());
        ELF64LoaderAndCallGraphPass loader;
        if (!loader.run(soPath, cfg.image_base, cfg.seed_all_dynsyms)) {
            fprintf(stderr, "Failed to load 64-bit ELF64 AArch64 binary: %s\n", soPath.c_str());
            return 1;
        }
        printf("  Driver version      : %s (build-id: %s)\n", loader.driver_version.c_str(), loader.build_id_short.c_str());
        printf("  Closure functions   : %zu internal functions, %zu PLT imports, %zu GLOB_DAT relocs\n",
               loader.closure_funcs.size(), loader.closure_plts.size(), loader.glob_dat_relocs.size());
        if (!is_libcpp) {
            printf("  Auto ctx malloc off : 0x%02" PRIx64 " | Vulkan HMI RVA: 0x%08" PRIx64 "\n", loader.ctx_malloc_offset, loader.hmi_rva);
        }

        // Build deterministic PLT dispatch table index map (0..N-1) for all non-hot PLT imports
        std::set<std::string> non_hot_syms;
        for (const auto &kv : loader.closure_plts) {
            if (!isHotInlinePltSym(kv.second)) non_hot_syms.insert(kv.second);
        }
        std::map<std::string, size_t> plt_index_map;
        std::vector<std::string> plt_index_list;
        plt_index_list.reserve(non_hot_syms.size());
        for (const auto &sym : non_hot_syms) {
            plt_index_map[sym] = plt_index_list.size();
            plt_index_list.push_back(sym);
        }

        // Emit PLT & GLOB_DAT manifest (.inc)
        {
            std::string incPath = outDir + "/" + (is_libcpp ? "r54p3_libcpp_plt_imports.inc" : "r54p3_plt_imports.inc");
            std::ofstream inc(incPath);
            std::string macroPrefix = is_libcpp ? "LIBCPP" : "MALI";
            inc << "// Auto-generated by mali_llvm_pass_driver from " << soPath << "\n";
            inc << "#ifndef " << macroPrefix << "_PLT_TABLE_COUNT\n";
            inc << "#define " << macroPrefix << "_PLT_TABLE_COUNT " << plt_index_list.size() << "\n";
            inc << "#endif\n";
            inc << "#ifdef " << macroPrefix << "_PLT_IMPORT\n";
            for (size_t i = 0; i < plt_index_list.size(); ++i) {
                inc << macroPrefix << "_PLT_IMPORT(" << i << ", \"" << plt_index_list[i] << "\")\n";
            }
            inc << "#endif\n";
            inc << "#ifdef " << macroPrefix << "_GLOB_DAT_IMPORT\n";
            for (const auto &gd : loader.glob_dat_relocs) {
                inc << macroPrefix << "_GLOB_DAT_IMPORT(0x" << std::hex << gd.r_offset << "ULL, "
                    << std::dec << gd.r_type << ", \"" << gd.sym_name << "\")\n";
            }
            inc << "#endif\n";
            printf("  Emitted manifest    : %s (%zu PLT slots, %zu GLOB_DAT)\n",
                   incPath.c_str(), plt_index_list.size(), loader.glob_dat_relocs.size());
        }

        if (is_libcpp) {
            std::string expPath = outDir + "/r54p3_libcpp_exports.inc";
            std::ofstream exp(expPath);
            std::set<uint64_t> closure_set(loader.closure_funcs.begin(), loader.closure_funcs.end());
            exp << "// Auto-generated by mali_llvm_pass_driver from " << soPath << "\n";
            exp << "#ifdef LIBCPP_FUNC_EXPORT\n";
            size_t fn_cnt = 0;
            for (const auto &kv : loader.exported_funcs) {
                if (closure_set.count(kv.second)) {
                    char hbuf[16];
                    std::snprintf(hbuf, sizeof(hbuf), "%08" PRIx64, kv.second);
                    exp << "LIBCPP_FUNC_EXPORT(\"" << kv.first << "\", 0x" << std::hex << kv.second << "ULL, libcpp_sub_" << hbuf << ")\n";
                    fn_cnt++;
                }
            }
            exp << "#endif\n";
            exp << "#ifdef LIBCPP_DATA_EXPORT\n";
            for (const auto &kv : loader.exported_data) {
                exp << "LIBCPP_DATA_EXPORT(\"" << kv.first << "\", 0x" << std::hex << kv.second << "ULL)\n";
            }
            exp << "#endif\n";
            printf("  Emitted exports     : %s (%zu func exports, %zu data exports)\n",
                   expPath.c_str(), fn_cnt, loader.exported_data.size());
        }

        bool instrumentCalls = true;
        bool emitLlOnly = false;
        if (const char *envInst = std::getenv("MALI_AOT_INSTRUMENT_CALLS")) {
            if (std::strcmp(envInst, "0") == 0 || std::strcmp(envInst, "false") == 0) instrumentCalls = false;
        }
        if (const char *envH2l = std::getenv("HORIZON2LLVM_INSTRUMENT_CALLS")) {
            if (std::strcmp(envH2l, "0") == 0 || std::strcmp(envH2l, "false") == 0) instrumentCalls = false;
        }
        size_t jobs = 1;
        if (const char *env_j = std::getenv("MALI_AOT_JOBS")) {
            size_t parsed = static_cast<size_t>(std::strtoull(env_j, nullptr, 10));
            if (parsed >= 1) jobs = parsed;
        }
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--instrument-calls") == 0 ||
                std::strcmp(argv[i], "--record-stackframes") == 0 ||
                std::strcmp(argv[i], "--profile-functions") == 0 ||
                std::strcmp(argv[i], "--debug-trace") == 0) {
                instrumentCalls = true;
            } else if (std::strcmp(argv[i], "--no-instrument-calls") == 0 ||
                       std::strcmp(argv[i], "--no-record-stackframes") == 0 ||
                       std::strcmp(argv[i], "--no-profile-functions") == 0) {
                instrumentCalls = false;
            } else if ((std::strcmp(argv[i], "-j") == 0 || std::strcmp(argv[i], "--jobs") == 0) && i + 1 < argc) {
                size_t parsed = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
                if (parsed >= 1) jobs = parsed;
            } else if (std::strncmp(argv[i], "-j", 2) == 0 && argv[i][2] >= '0' && argv[i][2] <= '9') {
                size_t parsed = static_cast<size_t>(std::strtoull(argv[i] + 2, nullptr, 10));
                if (parsed >= 1) jobs = parsed;
            } else if (std::strcmp(argv[i], "--emit-ll-only") == 0) {
                emitLlOnly = true;
            } else if (std::strncmp(argv[i], "--jobs=", 7) == 0) {
                size_t parsed = static_cast<size_t>(std::strtoull(argv[i] + 7, nullptr, 10));
                if (parsed >= 1) jobs = parsed;
            }
        }
        size_t chunk_mb = 40;
        if (const char *env_cmb = std::getenv("MALI_AOT_CHUNK_MB")) {
            chunk_mb = static_cast<size_t>(std::strtoull(env_cmb, nullptr, 10));
        }
        size_t chunk_limit_bytes = chunk_mb * 1024ULL * 1024ULL;
        const char *keep_ll_env = std::getenv("MALI_AOT_KEEP_LL");
        bool keep_ll = (keep_ll_env && std::strcmp(keep_ll_env, "1") == 0);
        std::string objPath = outDir + "/" + (is_libcpp ? "r54p3_libcpp_lifted.o" : "r54p3_lifted.o");

        // Remove stale monolithic/chunk artifacts before lifting
        std::remove((outDir + "/mali_mbs2_unopt.ll").c_str());
        std::remove((outDir + "/mali_mbs2_opt.ll").c_str());

        printf("[LLVM Pass 2 & 3] CFGRecoveryAndPicJumpTablePass + AArch64ToLLVMIRLiftingPass (instrument_calls=%d, chunk_mb=%zu, jobs=%zu)...\n",
               instrumentCalls ? 1 : 0, chunk_mb, jobs);

        std::vector<std::string> chunk_objs;
        std::vector<pid_t> active_pids;
        size_t total_ir_bytes = 0;
        int rc = 0;

        auto reap_one = [&]() {
            int status = 0;
            pid_t done = waitpid(-1, &status, 0);
            if (done > 0) {
                active_pids.erase(std::remove(active_pids.begin(), active_pids.end(), done), active_pids.end());
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    rc = 1;
                }
            }
        };

        std::vector<std::string> emitted_ll_files;
        {
            AArch64ToLLVMIRLiftingPass lifter(loader, cfg, plt_index_map, instrumentCalls);
            size_t chunk_num = 0;
            lifter.generateChunkedModules(chunk_limit_bytes, [&](const std::string &tag, std::string irText, size_t funcs_done, size_t total_funcs) {
                if (rc != 0) return;
                chunk_num++;
                total_ir_bytes += irText.size();
                std::string chunkUnopt = outDir + "/" + cfg.prefix + "_" + tag + "_unopt.ll";
                std::string chunkOpt   = outDir + "/" + cfg.prefix + "_" + tag + "_opt.ll";
                std::string chunkObj   = outDir + "/" + cfg.prefix + "_" + tag + ".o";
                double pct = total_funcs ? (100.0 * static_cast<double>(funcs_done) / static_cast<double>(total_funcs)) : 100.0;
                printf("  [Chunk %s (#%zu) | %zu/%zu funcs (%.1f%%)] %zu bytes IR -> %s\n",
                       tag.c_str(), chunk_num, funcs_done, total_funcs, pct, irText.size(),
                       emitLlOnly ? chunkUnopt.c_str() : chunkObj.c_str());
                std::fflush(stdout);

                if (emitLlOnly || keep_ll) {
                    std::ofstream out(chunkUnopt);
                    out.write(irText.data(), static_cast<std::streamsize>(irText.size()));
                    std::string().swap(irText);
                    if (emitLlOnly) {
                        emitted_ll_files.push_back(chunkUnopt);
                        return;
                    }
                }

                auto compile_chunk = [&]() -> int {
                    LLVMMemoryBufferRef memBuf = nullptr;
                    char *errMsg = nullptr;
                    if (keep_ll) {
                        if (LLVMCreateMemoryBufferWithContentsOfFile(chunkUnopt.c_str(), &memBuf, &errMsg) != 0) {
                            fprintf(stderr, "Error reading %s: %s\n", chunkUnopt.c_str(), errMsg ? errMsg : "unknown");
                            return 1;
                        }
                    } else {
                        memBuf = LLVMCreateMemoryBufferWithMemoryRangeCopy(irText.data(), irText.size(), chunkUnopt.c_str());
                        std::string().swap(irText);
                    }

                    LLVMContextRef C = LLVMContextCreate();
                    LLVMModuleRef M = nullptr;
                    if (LLVMParseIRInContext(C, memBuf, &M, &errMsg) != 0) {
                        fprintf(stderr, "LLVM IR Parse Error (%s): %s\n", tag.c_str(), errMsg ? errMsg : "unknown");
                        LLVMContextDispose(C);
                        return 1;
                    }
                    int crc = runLoweringAndCodegenOnModule(M, chunkObj.c_str(), false, true, keep_ll ? chunkOpt.c_str() : nullptr);
                    LLVMDisposeModule(M);
                    LLVMContextDispose(C);
                    return crc;
                };

                if (jobs <= 1) {
                    int crc = compile_chunk();
                    if (crc != 0) rc = crc;
                    else chunk_objs.push_back(chunkObj);
                } else {
                    while (active_pids.size() >= jobs && rc == 0) {
                        reap_one();
                    }
                    if (rc != 0) return;
                    std::fflush(stdout);
                    std::fflush(stderr);
                    pid_t pid = fork();
                    if (pid < 0) {
                        perror("fork");
                        rc = 1;
                        return;
                    }
                    if (pid == 0) {
                        int crc = compile_chunk();
                        std::fflush(stdout);
                        std::fflush(stderr);
                        _exit(crc);
                    }
                    std::string().swap(irText);
                    active_pids.push_back(pid);
                    chunk_objs.push_back(chunkObj);
                }
            });
            while (!active_pids.empty()) {
                reap_one();
            }
            printf("  Lifted instructions : %d (%d PIC jump tables, %d SIMD ops, %d Crypto SHA-1 ops, %d Atomics, total_ir=%zu bytes across %zu chunks)\n",
                   lifter.stats.total_instructions, lifter.stats.jump_tables_lowered, lifter.stats.simd_ops_lifted,
                   lifter.stats.crypto_sha1_lifted, lifter.stats.atomic_ops_lifted, total_ir_bytes, chunk_objs.size());
        }
        if (rc != 0) return rc;

        if (emitLlOnly) {
            std::string mfPath = outDir + "/" + cfg.prefix + "_ll_manifest.txt";
            std::ofstream mf(mfPath);
            for (const auto &lf : emitted_ll_files) mf << lf << "\n";
            printf("[Frontend+MiddleEnd] Wrote %zu lowered .ll chunks -> %s\n", emitted_ll_files.size(), mfPath.c_str());
            return 0;
        }

        std::string ld_cmd = "ld -r -o \"" + objPath + "\"";
        for (const auto &co : chunk_objs) {
            ld_cmd += " \"" + co + "\"";
        }
        printf("[Link] Merging %zu chunk objects into %s...\n", chunk_objs.size(), objPath.c_str());
        int ld_rc = std::system(ld_cmd.c_str());
        if (ld_rc != 0) {
            fprintf(stderr, "ld -r failed with exit code %d\n", ld_rc);
            return 1;
        }
        for (const auto &co : chunk_objs) {
            std::remove(co.c_str());
        }
        return 0;
    }

    // Mode 2: Run Lowering & Codegen Passes on an existing .ll file
    if (argc < 3) {
        fprintf(stderr, "Usage:\n  %s --lift-so <libGLES_mali.so> <out_dir>\n  %s <input.ll> <output.o|output.ll> [--emit-llvm] [--no-o2]\n", argv[0], argv[0]);
        return 1;
    }
    const char *inPath = argv[1];
    const char *outPath = argv[2];
    bool emitLlvm = false;
    bool runO2 = true;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--emit-llvm") == 0) emitLlvm = true;
        else if (std::strcmp(argv[i], "--no-o2") == 0) runO2 = false;
    }

    LLVMContextRef C = LLVMContextCreate();
    LLVMMemoryBufferRef memBuf = nullptr;
    char *errMsg = nullptr;
    if (LLVMCreateMemoryBufferWithContentsOfFile(inPath, &memBuf, &errMsg) != 0) {
        fprintf(stderr, "Error reading %s: %s\n", inPath, errMsg ? errMsg : "unknown");
        return 1;
    }

    LLVMModuleRef M = nullptr;
    if (LLVMParseIRInContext(C, memBuf, &M, &errMsg) != 0) {
        fprintf(stderr, "LLVM IR Parse Error: %s\n", errMsg ? errMsg : "unknown");
        return 1;
    }

    int rc = runLoweringAndCodegenOnModule(M, outPath, emitLlvm, runO2, nullptr);
    LLVMDisposeModule(M);
    LLVMContextDispose(C);
    return rc;
}

