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
#include <glob.h>

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
        if (!dlsym(h, "LLVMContextCreate") || !dlsym(h, "LLVMRunPasses")) {
            dlclose(h);
            return false;
        }
        handle = h;
        loaded_path = path;
        p_LLVMPointerTypeInContext = reinterpret_cast<LLVMTypeRef (*)(LLVMContextRef, unsigned)>(
            dlsym(h, "LLVMPointerTypeInContext"));
        p_LLVMContextSetOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, LLVMBool)>(
            dlsym(h, "LLVMContextSetOpaquePointers"));
        p_cxx_setOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, bool)>(
            dlsym(h, "_ZNK4llvm11LLVMContext17setOpaquePointersEb"));
        if (!p_cxx_setOpaquePointers) {
            p_cxx_setOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef, bool)>(
                dlsym(h, "_ZN4llvm11LLVMContext17setOpaquePointersEb"));
        }
        p_cxx_enableOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef)>(
            dlsym(h, "_ZN4llvm11LLVMContext20enableOpaquePointersEv"));
        if (!p_cxx_enableOpaquePointers) {
            p_cxx_enableOpaquePointers = reinterpret_cast<void (*)(LLVMContextRef)>(
                dlsym(h, "_ZNK4llvm11LLVMContext20enableOpaquePointersEv"));
        }
        p_LLVMParseCommandLineOptions = reinterpret_cast<void (*)(int, const char *const *, const char *)>(
            dlsym(h, "LLVMParseCommandLineOptions"));
#define LOAD_SYM(ret, name, params, args) \
        p_##name = reinterpret_cast<ret (*) params>(dlsym(h, #name)); \
        if (!p_##name) { std::fprintf(stderr, "[LLVM_LOADER] Missing symbol %s in %s\n", #name, path); return false; }
        MALI_LLVM_API_LIST(LOAD_SYM)
#undef LOAD_SYM
        return true;
    }

    void ensureLoaded() {
        if (handle) return;
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
            const char *argv[] = {"mali_llvm_pass_driver", "-opaque-pointers"};
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

static constexpr uint64_t IMAGE_BASE = 0x400000000ULL;
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

static std::pair<uint64_t, uint64_t> decodeBitMasks(unsigned sf, unsigned immN, unsigned imms, unsigned immr) {
    unsigned combined = (immN << 6) | (~imms & 0x3F);
    if (combined == 0) return {0, 0};
    unsigned length = 31 - __builtin_clz(combined);
    unsigned levels = (1U << length) - 1U;
    unsigned s = imms & levels;
    unsigned r = immr & levels;
    unsigned esize = 1U << length;
    unsigned diff = (s - r) & (esize - 1U);
    uint64_t welem = (s + 1 == 64) ? ~0ULL : ((1ULL << (s + 1)) - 1ULL);
    uint64_t telem = (diff + 1 == 64) ? ~0ULL : ((1ULL << (diff + 1)) - 1ULL);
    uint64_t emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1ULL);
    welem &= emask;
    telem &= emask;
    if (r != 0) {
        welem = ((welem >> r) | (welem << (esize - r))) & emask;
    }
    uint64_t wmask = 0, tmask = 0;
    unsigned total = sf ? 64 : 32;
    for (unsigned i = 0; i < total; i += esize) {
        wmask |= (welem << i);
        tmask |= (telem << i);
    }
    return {wmask, tmask};
}

static uint64_t expandAdvSimdImm(unsigned op, unsigned cmode, unsigned imm8) {
    uint64_t u8 = imm8 & 0xFFULL;
    unsigned c_hi = cmode >> 1;
    if (c_hi == 0) return (u8 << 32) | u8;
    if (c_hi == 1) return ((u8 << 8) << 32) | (u8 << 8);
    if (c_hi == 2) return ((u8 << 16) << 32) | (u8 << 16);
    if (c_hi == 3) return ((u8 << 24) << 32) | (u8 << 24);
    if (c_hi == 4) return (u8 << 48) | (u8 << 32) | (u8 << 16) | u8;
    if (c_hi == 5) return (u8 << 56) | (u8 << 40) | (u8 << 24) | (u8 << 8);
    if (c_hi == 6) {
        uint64_t v = (cmode & 1) ? ((u8 << 16) | 0xFFFFULL) : ((u8 << 8) | 0xFFULL);
        return (v << 32) | v;
    }
    if (c_hi == 7) {
        if (op == 0 && (cmode & 1) == 0) {
            uint64_t r = 0;
            for (unsigned i = 0; i < 8; ++i) r |= (u8 << (i * 8));
            return r;
        }
        if (op == 1 && (cmode & 1) == 0) {
            uint64_t r = 0;
            for (unsigned i = 0; i < 8; ++i) {
                if ((u8 >> i) & 1ULL) r |= (0xFFULL << (i * 8));
            }
            return r;
        }
    }
    return 0;
}


static uint32_t expandVfpFloat32Imm(unsigned imm8) {
    uint32_t sign = (imm8 >> 7) & 1U;
    uint32_t exp_b6 = (imm8 >> 6) & 1U;
    uint32_t exp_low = (imm8 >> 4) & 3U;
    uint32_t frac = imm8 & 0xFU;
    uint32_t exp8 = ((exp_b6 ^ 1U) << 7) | (exp_b6 ? 0x7CU : 0U) | exp_low;
    return (sign << 31) | (exp8 << 23) | (frac << 19);
}

static uint64_t expandVfpFloat64Imm(unsigned imm8) {
    uint64_t sign = (imm8 >> 7) & 1ULL;
    uint64_t exp_b6 = (imm8 >> 6) & 1ULL;
    uint64_t exp_low = (imm8 >> 4) & 3ULL;
    uint64_t frac = imm8 & 0xFULL;
    uint64_t exp11 = ((exp_b6 ^ 1ULL) << 10) | (exp_b6 ? 0x3FCULL : 0ULL) | exp_low;
    return (sign << 63) | (exp11 << 52) | (frac << 48);
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
    std::vector<uint64_t> closure_funcs;
    std::map<uint64_t, std::string> closure_plts;
    uint64_t ctx_malloc_offset = 0x78;
    uint64_t hmi_rva = 0;
    uint64_t kbase_page_shift_rva = 0;
    uint64_t vk_dispatch_table_rva = 0;
    uint64_t vk_dispatch_table_count = 0;
    uint64_t vk_dispatch_table_stride = 0;
    std::vector<std::pair<std::string, uint64_t>> vk_dispatch_entries;

    bool run(const std::string &so_path) {
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
                        } else if (sym_idx > 0 && sym_idx < dynsyms.size()) {
                            if (dynsyms[sym_idx].shndx != 0 && dynsyms[sym_idx].value != 0) {
                                writeU64(elf, cur_offset, dynsyms[sym_idx].value + cur_addend);
                            }
                        }
                    }
                    done++;
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
        std::set<uint64_t> visited;
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
                        if (func_by_addr.count(tgt) && !visited.count(tgt)) q.push_back(tgt);
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
        // Also include all discovered Vulkan 1.3 dispatch table entrypoints (all 456 vk* functions in r54p3 / 362 in r44p1)
        for (const auto &vk_ent : vk_dispatch_entries) {
            uint64_t faddr = findFuncStart(vk_ent.second);
            if (faddr == 0) faddr = vk_ent.second;
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
    const ELF64LoaderAndCallGraphPass &loader;
    std::set<uint64_t> func_set;
    uint64_t tmp_id = 0;
    std::ostringstream out;

    struct LiftStats {
        int total_functions = 0;
        int total_instructions = 0;
        int jump_tables_lowered = 0;
        int simd_ops_lifted = 0;
        int crypto_sha1_lifted = 0;
        int atomic_ops_lifted = 0;
    } stats;

    explicit AArch64ToLLVMIRLiftingPass(const ELF64LoaderAndCallGraphPass &ld)
        : loader(ld), func_set(ld.closure_funcs.begin(), ld.closure_funcs.end()) {
        stats.total_functions = static_cast<int>(ld.closure_funcs.size());
    }

    std::string newTmp(const char *prefix = "t") {
        return std::string("%") + prefix + std::to_string(++tmp_id);
    }

    void emit(const std::string &s) { out << "  " << s << "\n"; }
    void emitRaw(const std::string &s) { out << s << "\n"; }

    static std::string hex8(uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08" PRIx64, v);
        return buf;
    }

    std::string loadGpr(unsigned r, bool is64 = true) {
        if (r == 31) return "0";
        std::string t = newTmp("x");
        emit(t + " = load i64, ptr %reg_x" + std::to_string(r) + ", align 8");
        if (!is64) {
            std::string t32 = newTmp("w");
            emit(t32 + " = trunc i64 " + t + " to i32");
            return t32;
        }
        return t;
    }

    std::string loadGprOrSp(unsigned r, bool is64 = true) {
        std::string t = newTmp("xsp");
        std::string reg = (r == 31) ? "%reg_sp" : ("%reg_x" + std::to_string(r));
        emit(t + " = load i64, ptr " + reg + ", align 8");
        if (!is64) {
            std::string t32 = newTmp("wsp");
            emit(t32 + " = trunc i64 " + t + " to i32");
            return t32;
        }
        return t;
    }

    void storeGpr(unsigned r, const std::string &val, bool is64 = true) {
        if (r == 31) return;
        std::string v64 = val;
        if (!is64) {
            v64 = newTmp("zext");
            emit(v64 + " = zext i32 " + val + " to i64");
        }
        emit("store i64 " + v64 + ", ptr %reg_x" + std::to_string(r) + ", align 8");
    }

    void storeGprOrSp(unsigned r, const std::string &val, bool is64 = true) {
        std::string v64 = val;
        if (!is64) {
            v64 = newTmp("zext_sp");
            emit(v64 + " = zext i32 " + val + " to i64");
        }
        std::string reg = (r == 31) ? "%reg_sp" : ("%reg_x" + std::to_string(r));
        emit("store i64 " + v64 + ", ptr " + reg + ", align 8");
    }

    std::string loadVec(unsigned vr, const std::string &ty) {
        std::string t = newTmp("vld");
        emit(t + " = load " + ty + ", ptr %reg_v" + std::to_string(vr) + ", align 16");
        return t;
    }

    void storeVec(unsigned vr, const std::string &ty, const std::string &val, bool zero_full = true) {
        if (zero_full && ty != "i128" && ty != "<16 x i8>" && ty != "<8 x i16>" && ty != "<4 x i32>" && ty != "<2 x i64>") {
            emit("store i128 0, ptr %reg_v" + std::to_string(vr) + ", align 16");
        }
        emit("store " + ty + " " + val + ", ptr %reg_v" + std::to_string(vr) + ", align 16");
    }

    void emitFlagsAdd(const std::string &a, const std::string &b, const std::string &cin, const std::string &res, bool is64) {
        std::string ty = is64 ? "i64" : "i32";
        std::string ext_ty = is64 ? "i128" : "i64";
        unsigned shift = is64 ? 63 : 31;

        std::string n_cmp = newTmp("fl_n");
        emit(n_cmp + " = icmp slt " + ty + " " + res + ", 0");
        emit("store i1 " + n_cmp + ", ptr %flag_n, align 1");

        std::string z_cmp = newTmp("fl_z");
        emit(z_cmp + " = icmp eq " + ty + " " + res + ", 0");
        emit("store i1 " + z_cmp + ", ptr %flag_z, align 1");

        std::string ea = newTmp("ea"), eb = newTmp("eb"), ec = newTmp("ec");
        std::string s1 = newTmp("s1"), s2 = newTmp("s2"), csh = newTmp("csh"), c_cmp = newTmp("fl_c");
        emit(ea + " = zext " + ty + " " + a + " to " + ext_ty);
        emit(eb + " = zext " + ty + " " + b + " to " + ext_ty);
        emit(ec + " = zext " + ty + " " + cin + " to " + ext_ty);
        emit(s1 + " = add " + ext_ty + " " + ea + ", " + eb);
        emit(s2 + " = add " + ext_ty + " " + s1 + ", " + ec);
        emit(csh + " = lshr " + ext_ty + " " + s2 + ", " + std::to_string(shift + 1));
        emit(c_cmp + " = icmp ne " + ext_ty + " " + csh + ", 0");
        emit("store i1 " + c_cmp + ", ptr %flag_c, align 1");

        std::string axb = newTmp("axb"), naxb = newTmp("naxb"), axr = newTmp("axr"), vand = newTmp("vand"), v_cmp = newTmp("fl_v");
        emit(axb + " = xor " + ty + " " + a + ", " + b);
        emit(naxb + " = xor " + ty + " " + axb + ", -1");
        emit(axr + " = xor " + ty + " " + a + ", " + res);
        emit(vand + " = and " + ty + " " + naxb + ", " + axr);
        emit(v_cmp + " = icmp slt " + ty + " " + vand + ", 0");
        emit("store i1 " + v_cmp + ", ptr %flag_v, align 1");
    }

    std::string evalCond(unsigned cond) {
        unsigned c_hi = cond >> 1;
        std::string res = newTmp("cond");
        if (c_hi == 0) emit(res + " = load i1, ptr %flag_z, align 1");
        else if (c_hi == 1) emit(res + " = load i1, ptr %flag_c, align 1");
        else if (c_hi == 2) emit(res + " = load i1, ptr %flag_n, align 1");
        else if (c_hi == 3) emit(res + " = load i1, ptr %flag_v, align 1");
        else if (c_hi == 4) {
            std::string fc = newTmp("fc"), fz = newTmp("fz"), nz = newTmp("nz");
            emit(fc + " = load i1, ptr %flag_c, align 1");
            emit(fz + " = load i1, ptr %flag_z, align 1");
            emit(nz + " = xor i1 " + fz + ", true");
            emit(res + " = and i1 " + fc + ", " + nz);
        } else if (c_hi == 5) {
            std::string fn = newTmp("fn"), fv = newTmp("fv");
            emit(fn + " = load i1, ptr %flag_n, align 1");
            emit(fv + " = load i1, ptr %flag_v, align 1");
            emit(res + " = icmp eq i1 " + fn + ", " + fv);
        } else if (c_hi == 6) {
            std::string fn = newTmp("fn"), fv = newTmp("fv"), fz = newTmp("fz"), nv = newTmp("nv"), nz = newTmp("nz");
            emit(fn + " = load i1, ptr %flag_n, align 1");
            emit(fv + " = load i1, ptr %flag_v, align 1");
            emit(fz + " = load i1, ptr %flag_z, align 1");
            emit(nv + " = icmp eq i1 " + fn + ", " + fv);
            emit(nz + " = xor i1 " + fz + ", true");
            emit(res + " = and i1 " + nv + ", " + nz);
        } else {
            return "true";
        }
        if ((cond & 1) && cond != 0xF) {
            std::string inv = newTmp("cinv");
            emit(inv + " = xor i1 " + res + ", true");
            return inv;
        }
        return res;
    }

    std::string emitExtendReg(unsigned rm, unsigned option, unsigned shift) {
        std::string raw = loadGpr(rm, true);
        unsigned opt = option & 7;
        std::string ext = raw;
        if (opt != 3 && opt != 7) {
            unsigned bits = ((opt & 3) == 0) ? 8 : (((opt & 3) == 1) ? 16 : 32);
            std::string tr = newTmp("tr");
            emit(tr + " = trunc i64 " + raw + " to i" + std::to_string(bits));
            ext = newTmp("ext");
            std::string op_str = (opt & 4) ? "sext" : "zext";
            emit(ext + " = " + op_str + " i" + std::to_string(bits) + " " + tr + " to i64");
        }
        if (shift > 0) {
            std::string sh = newTmp("sh");
            emit(sh + " = shl i64 " + ext + ", " + std::to_string(shift));
            return sh;
        }
        return ext;
    }

    std::string emitShiftReg(const std::string &val, unsigned shift_type, unsigned amount, bool is64) {
        if (amount == 0) return val;
        std::string ty = is64 ? "i64" : "i32";
        unsigned st = shift_type & 3;
        std::string res = newTmp("sh");
        if (st == 0) emit(res + " = shl " + ty + " " + val + ", " + std::to_string(amount));
        else if (st == 1) emit(res + " = lshr " + ty + " " + val + ", " + std::to_string(amount));
        else if (st == 2) emit(res + " = ashr " + ty + " " + val + ", " + std::to_string(amount));
        else emit(res + " = call " + ty + " @llvm.fshr." + ty + "(" + ty + " " + val + ", " + ty + " " + val + ", " + ty + " " + std::to_string(amount) + ")");
        return res;
    }

    void emitBionicPltCall(const std::string &sym) {
        for (unsigned r = 0; r < 6; ++r) {
            std::string rv = loadGpr(r, true);
            std::string rp = newTmp("plt_in");
            emit(rp + " = getelementptr inbounds i8, ptr %cpu, i64 " + std::to_string(r * 8));
            emit("store i64 " + rv + ", ptr " + rp + ", align 8");
        }
        emit("call void @llvm.mali.bionic.plt64." + sym + "(ptr %cpu)");
        std::string r0_p = newTmp("plt_out"), r0_v = newTmp("plt_res");
        emit(r0_p + " = getelementptr inbounds i8, ptr %cpu, i64 0");
        emit(r0_v + " = load i64, ptr " + r0_p + ", align 8");
        storeGpr(0, r0_v, true);
    }

    void emitCallToTarget(uint64_t target_rva) {
        if (loader.closure_plts.count(target_rva)) {
            emitBionicPltCall(loader.closure_plts.at(target_rva));
        } else if (func_set.count(target_rva)) {
            emit("call void @aot_sub_" + hex8(target_rva) + "(ptr %cpu)");
        } else {
            uint64_t fst = loader.findFuncStart(target_rva);
            if (fst != 0 && fst == target_rva && func_set.count(fst)) {
                emit("call void @aot_sub_" + hex8(fst) + "(ptr %cpu)");
            } else {
                emit("call void @aot_indirect_dispatch(ptr %cpu, i64 " + std::to_string(IMAGE_BASE + target_rva) + ")");
            }
        }
    }

    // Pass 2: CFGRecoveryAndPicJumpTablePass helper (supports both inline and loop-hoisted PIC jump tables)
    std::vector<uint64_t> detectPicJumpTable(uint64_t faddr, uint64_t end_addr, uint64_t pc) {
        std::vector<uint64_t> targets;
        if (pc < faddr + 20) return targets;

        uint32_t w_ldrb = 0, w_adr = 0;
        uint64_t ldrb_pc = 0, adr_pc = 0;
        for (uint64_t back = 4; back <= 20 && pc >= faddr + back; back += 4) {
            uint32_t w = readU32(loader.elf, pc - back);
            if (w_ldrb == 0 && (w & 0xFFE00C00U) == 0x38600800U) {
                w_ldrb = w;
                ldrb_pc = pc - back;
            } else if (w_adr == 0 && (w & 0x9F000000U) == 0x10000000U) {
                w_adr = w;
                adr_pc = pc - back;
            }
        }
        if (w_ldrb == 0 || w_adr == 0) return targets;

        unsigned tbl_reg = (w_ldrb >> 5) & 0x1FU;
        unsigned idx_reg = (w_ldrb >> 16) & 0x1FU;

        uint32_t w_add = 0, w_adrp = 0;
        uint64_t adrp_pc = 0;
        unsigned adrp_reg = tbl_reg;
        for (int64_t p = static_cast<int64_t>(ldrb_pc) - 4; p >= static_cast<int64_t>(faddr) && p >= static_cast<int64_t>(pc) - 256; p -= 4) {
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
        for (int64_t p = static_cast<int64_t>(ldrb_pc) - 4; p >= static_cast<int64_t>(faddr) && p >= static_cast<int64_t>(pc) - 64; p -= 4) {
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
        if (max_idx < 0 || max_idx > 256) return targets;

        int64_t imm21 = sext64(((w_adrp >> 5) & 0x7FFFFU) << 2 | ((w_adrp >> 29) & 3U), 21);
        uint64_t page = (adrp_pc & ~0xFFFULL) + static_cast<uint64_t>(imm21 << 12);
        uint64_t off12 = (w_add >> 10) & 0xFFFULL;
        uint64_t tbl_addr = page + off12;
        int64_t adr_imm = sext64(((w_adr >> 5) & 0x7FFFFU) << 2 | ((w_adr >> 29) & 3U), 21);
        uint64_t base_addr = static_cast<uint64_t>(static_cast<int64_t>(adr_pc) + adr_imm);
        for (int idx = 0; idx <= max_idx; ++idx) {
            uint8_t b = loader.elf[tbl_addr + idx];
            uint64_t t_rva = base_addr + (static_cast<uint64_t>(b) << 2);
            if (t_rva >= faddr && t_rva < end_addr) targets.push_back(t_rva);
        }
        return targets;
    }

    void liftFunction(uint64_t faddr) {
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

        std::vector<uint64_t> sorted_leaders(leaders.begin(), leaders.end());
        emitRaw("define void @aot_sub_" + hex8(faddr) + "(ptr %cpu) #0 {");
        emitRaw("entry:");
        for (unsigned r = 0; r < 31; ++r) emit("%reg_x" + std::to_string(r) + " = alloca i64, align 8");
        emit("%reg_sp = alloca i64, align 8");
        for (unsigned r = 0; r < 32; ++r) emit("%reg_v" + std::to_string(r) + " = alloca i128, align 16");
        emit("%flag_n = alloca i1, align 1");
        emit("%flag_z = alloca i1, align 1");
        emit("%flag_c = alloca i1, align 1");
        emit("%flag_v = alloca i1, align 1");

        auto reloadState = [&](bool include_vec = false) {
            for (unsigned r = 0; r < 31; ++r) {
                std::string p = newTmp("rl_x"), v = newTmp("rv_x");
                emit(p + " = getelementptr inbounds i8, ptr %cpu, i64 " + std::to_string(r * 8));
                emit(v + " = load i64, ptr " + p + ", align 8");
                emit("store i64 " + v + ", ptr %reg_x" + std::to_string(r) + ", align 8");
            }
            std::string psp = newTmp("rl_sp"), vsp = newTmp("rv_sp");
            emit(psp + " = getelementptr inbounds i8, ptr %cpu, i64 248");
            emit(vsp + " = load i64, ptr " + psp + ", align 8");
            emit("store i64 " + vsp + ", ptr %reg_sp, align 8");
            if (include_vec) {
                for (unsigned r = 0; r < 32; ++r) {
                    std::string pv = newTmp("rl_v"), vv = newTmp("rv_v");
                    emit(pv + " = getelementptr inbounds i8, ptr %cpu, i64 " + std::to_string(272 + r * 16));
                    emit(vv + " = load i128, ptr " + pv + ", align 16");
                    emit("store i128 " + vv + ", ptr %reg_v" + std::to_string(r) + ", align 16");
                }
            }
        };

        auto flushState = [&](bool include_vec = false) {
            for (unsigned r = 0; r < 31; ++r) {
                std::string p = newTmp("fl_x"), v = newTmp("fv_x");
                emit(v + " = load i64, ptr %reg_x" + std::to_string(r) + ", align 8");
                emit(p + " = getelementptr inbounds i8, ptr %cpu, i64 " + std::to_string(r * 8));
                emit("store i64 " + v + ", ptr " + p + ", align 8");
            }
            std::string psp = newTmp("fl_sp"), vsp = newTmp("fv_sp");
            emit(vsp + " = load i64, ptr %reg_sp, align 8");
            emit(psp + " = getelementptr inbounds i8, ptr %cpu, i64 248");
            emit("store i64 " + vsp + ", ptr " + psp + ", align 8");
            if (include_vec) {
                for (unsigned r = 0; r < 32; ++r) {
                    std::string pv = newTmp("fl_v"), vv = newTmp("fv_v");
                    emit(vv + " = load i128, ptr %reg_v" + std::to_string(r) + ", align 16");
                    emit(pv + " = getelementptr inbounds i8, ptr %cpu, i64 " + std::to_string(272 + r * 16));
                    emit("store i128 " + vv + ", ptr " + pv + ", align 16");
                }
            }
        };

        reloadState(true);
        emit("store i1 false, ptr %flag_n, align 1");
        emit("store i1 false, ptr %flag_z, align 1");
        emit("store i1 false, ptr %flag_c, align 1");
        emit("store i1 false, ptr %flag_v, align 1");
        emit("br label %bb_" + hex8(faddr));

        for (size_t bi = 0; bi < sorted_leaders.size(); ++bi) {
            uint64_t b_start = sorted_leaders[bi];
            uint64_t b_stop = (bi + 1 < sorted_leaders.size()) ? sorted_leaders[bi + 1] : end_addr;
            emitRaw("bb_" + hex8(b_start) + ":");
            bool terminated = false;

            for (uint64_t pc = b_start; pc < b_stop; pc += 4) {
                uint32_t insn = readU32(loader.elf, pc);
                stats.total_instructions++;
                if (insn == 0) {
                    emit("br label %exit_ret");
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
                std::string ty = is64 ? "i64" : "i32";

                // 1. Data Processing -- Immediate
                if ((op0 & 0xE) == 0x8) {
                    unsigned subop = (insn >> 23) & 0x7;
                    if (subop == 0 || subop == 1) {
                        int64_t imm = sext64(((insn >> 5) & 0x7FFFFU) << 2 | ((insn >> 29) & 3U), 21);
                        uint64_t val = sf ? (IMAGE_BASE + ((pc & ~0xFFFULL) + static_cast<uint64_t>(imm << 12)))
                                          : (IMAGE_BASE + static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
                        storeGpr(rd, std::to_string(val), true);
                    } else if (subop == 2 || subop == 3) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned S = (insn >> 29) & 1;
                        unsigned sh = (insn >> 22) & 1;
                        uint64_t imm12 = static_cast<uint64_t>((insn >> 10) & 0xFFFU) << (sh ? 12 : 0);
                        std::string a = loadGprOrSp(rn, is64);
                        std::string res = newTmp("alu");
                        emit(res + " = " + (op ? "sub " : "add ") + ty + " " + a + ", " + std::to_string(imm12));
                        if (S) {
                            uint64_t rhs_v = op ? (is64 ? ~imm12 : static_cast<uint32_t>(~imm12)) : imm12;
                            emitFlagsAdd(a, std::to_string(rhs_v), op ? "1" : "0", res, is64);
                            storeGpr(rd, res, is64);
                        } else {
                            storeGprOrSp(rd, res, is64);
                        }
                    } else if (subop == 4) {
                        unsigned opc = (insn >> 29) & 3;
                        auto masks = decodeBitMasks(sf, (insn >> 22) & 1, (insn >> 10) & 0x3F, (insn >> 16) & 0x3F);
                        uint64_t mval = is64 ? masks.first : static_cast<uint32_t>(masks.first);
                        std::string a = loadGpr(rn, is64);
                        std::string res = newTmp("log");
                        std::string op_str = (opc == 0 || opc == 3) ? "and" : (opc == 1 ? "or" : "xor");
                        emit(res + " = " + op_str + " " + ty + " " + a + ", " + std::to_string(mval));
                        if (opc == 3) {
                            std::string nc = newTmp("fl_n"), zc = newTmp("fl_z");
                            emit(nc + " = icmp slt " + ty + " " + res + ", 0");
                            emit(zc + " = icmp eq " + ty + " " + res + ", 0");
                            emit("store i1 " + nc + ", ptr %flag_n, align 1");
                            emit("store i1 " + zc + ", ptr %flag_z, align 1");
                            emit("store i1 false, ptr %flag_c, align 1");
                            emit("store i1 false, ptr %flag_v, align 1");
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
                            storeGpr(rd, std::to_string(v), is64);
                        } else if (opc == 2) {
                            uint64_t v = is64 ? (imm16 << hw) : static_cast<uint32_t>(imm16 << hw);
                            storeGpr(rd, std::to_string(v), is64);
                        } else if (opc == 3) {
                            std::string cur = loadGpr(rd, is64);
                            uint64_t cm = is64 ? ~(0xFFFFULL << hw) : static_cast<uint32_t>(~(0xFFFFULL << hw));
                            uint64_t iv = is64 ? (imm16 << hw) : static_cast<uint32_t>(imm16 << hw);
                            std::string c1 = newTmp("movk1"), c2 = newTmp("movk2");
                            emit(c1 + " = and " + ty + " " + cur + ", " + std::to_string(cm));
                            emit(c2 + " = or " + ty + " " + c1 + ", " + std::to_string(iv));
                            storeGpr(rd, c2, is64);
                        }
                    } else if (subop == 6) {
                        unsigned opc = (insn >> 29) & 3;
                        unsigned immr = (insn >> 16) & 0x3F;
                        unsigned imms = (insn >> 10) & 0x3F;
                        auto masks = decodeBitMasks(sf, (insn >> 22) & 1, imms, immr);
                        uint64_t wmask = is64 ? masks.first : static_cast<uint32_t>(masks.first);
                        uint64_t tmask = is64 ? masks.second : static_cast<uint32_t>(masks.second);
                        std::string src = loadGpr(rn, is64);
                        std::string rot = emitShiftReg(src, 3, immr, is64);
                        std::string bot = newTmp("bf_bot");
                        emit(bot + " = and " + ty + " " + rot + ", " + std::to_string(wmask));
                        if (opc == 2) {
                            std::string res = newTmp("ubfm");
                            emit(res + " = and " + ty + " " + bot + ", " + std::to_string(tmask));
                            storeGpr(rd, res, is64);
                        } else if (opc == 1) {
                            std::string dst = loadGpr(rd, is64);
                            uint64_t bmask = wmask & tmask;
                            uint64_t nbmask = is64 ? ~bmask : static_cast<uint32_t>(~bmask);
                            std::string d1 = newTmp("bfm1"), b1 = newTmp("bfm2"), res = newTmp("bfm");
                            emit(d1 + " = and " + ty + " " + dst + ", " + std::to_string(nbmask));
                            emit(b1 + " = and " + ty + " " + bot + ", " + std::to_string(bmask));
                            emit(res + " = or " + ty + " " + d1 + ", " + b1);
                            storeGpr(rd, res, is64);
                        } else {
                            uint64_t ntmask = is64 ? ~tmask : static_cast<uint32_t>(~tmask);
                            std::string sh_s = newTmp("sbfm_sh"), bit_s = newTmp("sbfm_b"), sext_s = newTmp("sbfm_sx");
                            std::string top = newTmp("sbfm_top"), b1 = newTmp("sbfm_bot"), res = newTmp("sbfm");
                            emit(sh_s + " = lshr " + ty + " " + src + ", " + std::to_string(imms));
                            emit(bit_s + " = and " + ty + " " + sh_s + ", 1");
                            emit(sext_s + " = sub " + ty + " 0, " + bit_s);
                            emit(top + " = and " + ty + " " + sext_s + ", " + std::to_string(ntmask));
                            emit(b1 + " = and " + ty + " " + bot + ", " + std::to_string(tmask));
                            emit(res + " = or " + ty + " " + top + ", " + b1);
                            storeGpr(rd, res, is64);
                        }
                    } else if (subop == 7) {
                        unsigned imms = (insn >> 10) & 0x3F;
                        std::string n_val = loadGpr(rn, is64);
                        std::string m_val = loadGpr(rm, is64);
                        if (imms == 0) {
                            storeGpr(rd, m_val, is64);
                        } else {
                            std::string res = newTmp("extr");
                            emit(res + " = call " + ty + " @llvm.fshr." + ty + "(" + ty + " " + n_val + ", " + ty + " " + m_val + ", " + ty + " " + std::to_string(imms) + ")");
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
                            storeGpr(30, std::to_string(IMAGE_BASE + pc + 4), true);
                            if (loader.closure_plts.count(target)) {
                                emitCallToTarget(target);
                            } else {
                                flushState();
                                emitCallToTarget(target);
                                reloadState();
                            }
                        } else {
                            if (target >= faddr && target < end_addr) {
                                emit("br label %bb_" + hex8(target));
                            } else {
                                if (loader.closure_plts.count(target)) {
                                    emitCallToTarget(target);
                                } else {
                                    flushState();
                                    emitCallToTarget(target);
                                    reloadState();
                                }
                                emit("br label %exit_ret");
                            }
                            terminated = true;
                            break;
                        }
                        continue;
                    }
                    if (((insn >> 24) & 0xFF) == 0x54) {
                        uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x7FFFFU, 19) << 2));
                        std::string cval = evalCond(insn & 0xF);
                        emit("br i1 " + cval + ", label %bb_" + hex8(target) + ", label %bb_" + hex8(pc + 4));
                        terminated = true;
                        break;
                    }
                    if (((insn >> 25) & 0x3F) == 0x1A) {
                        unsigned op = (insn >> 24) & 1;
                        uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x7FFFFU, 19) << 2));
                        std::string v = loadGpr(rd, is64);
                        std::string cmp_v = newTmp("cbz");
                        emit(cmp_v + " = icmp " + (op ? "ne " : "eq ") + ty + " " + v + ", 0");
                        emit("br i1 " + cmp_v + ", label %bb_" + hex8(target) + ", label %bb_" + hex8(pc + 4));
                        terminated = true;
                        break;
                    }
                    if (((insn >> 25) & 0x3F) == 0x1B) {
                        unsigned op = (insn >> 24) & 1;
                        unsigned bpos = ((insn >> 31) << 5) | ((insn >> 19) & 0x1F);
                        uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + (sext64((insn >> 5) & 0x3FFFU, 14) << 2));
                        std::string v = loadGpr(rd, true);
                        std::string masked = newTmp("tbz_m"), cmp_v = newTmp("tbz_c");
                        emit(masked + " = and i64 " + v + ", " + std::to_string(1ULL << bpos));
                        emit(cmp_v + " = icmp " + (op ? "ne " : "eq ") + "i64 " + masked + ", 0");
                        emit("br i1 " + cmp_v + ", label %bb_" + hex8(target) + ", label %bb_" + hex8(pc + 4));
                        terminated = true;
                        break;
                    }
                    if ((insn & 0xFE000000U) == 0xD6000000U) {
                        unsigned opc = (insn >> 21) & 0xF;
                        if (opc == 2 || opc == 4) {
                            if (insn == 0xD65F0BFFU || insn == 0xD65F0FFFU) {
                                emit("call void @llvm.mali.arm64.retaa()");
                            }
                            emit("br label %exit_ret");
                            terminated = true;
                            break;
                        } else if (opc == 1) {
                            std::string tgt = loadGpr(rn, true);
                            storeGpr(30, std::to_string(IMAGE_BASE + pc + 4), true);
                            flushState();
                            emit("call void @aot_indirect_dispatch(ptr %cpu, i64 " + tgt + ")");
                            reloadState();
                            continue;
                        } else if (opc == 0) {
                            if (jtables.count(pc)) {
                                stats.jump_tables_lowered++;
                                std::string tgt = loadGpr(rn, true);
                                std::ostringstream cases;
                                std::set<uint64_t> seen;
                                for (uint64_t t_rva : jtables[pc]) {
                                    if (seen.insert(t_rva).second) {
                                        cases << "i64 " << (IMAGE_BASE + t_rva) << ", label %bb_" << hex8(t_rva) << " ";
                                    }
                                }
                                emit("switch i64 " + tgt + ", label %exit_ret [ " + cases.str() + "]");
                                terminated = true;
                                break;
                            } else {
                                std::string tgt = loadGpr(rn, true);
                                flushState();
                                emit("call void @aot_indirect_dispatch(ptr %cpu, i64 " + tgt + ")");
                                reloadState();
                                emit("br label %exit_ret");
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
                            emit("call void @llvm.mali.arm64.hint(i32 " + std::to_string(imm_hint) + ")");
                            continue;
                        }
                        if ((insn & 0xFFE00000U) == 0xD5200000U && L == 1) {
                            std::string tp = newTmp("mrs_val");
                            emit(tp + " = call i64 @llvm.mali.arm64.mrs(ptr %cpu, i32 " + std::to_string(sysreg) + ")");
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

                    // Load-Acquire / Store-Release / Exclusive (LDAR, LDARB, LDARH, LDAXR, STLR, STLRB, STLRH, STLXR)
                    if (((insn >> 24) & 0x3FU) == 0x08U) {
                        stats.atomic_ops_lifted++;
                        unsigned L = (insn >> 22) & 1;
                        unsigned o2 = (insn >> 23) & 1;
                        std::string addr_val = loadGprOrSp(rn, true);
                        std::string ptr_v = newTmp("at_ptr");
                        emit(ptr_v + " = inttoptr i64 " + addr_val + " to ptr");
                        std::string mty = "i" + std::to_string(8U << size);
                        if (L == 1) {
                            std::string lv = newTmp("ldar");
                            emit(lv + " = load atomic " + mty + ", ptr " + ptr_v + " acquire, align " + std::to_string(1U << size));
                            if (size < 2) {
                                std::string zx = newTmp("ldar_zx");
                                emit(zx + " = zext " + mty + " " + lv + " to i32");
                                lv = zx;
                            }
                            storeGpr(rd, lv, size == 3);
                        } else {
                            std::string val = loadGpr(rd, size == 3);
                            if (size < 2) {
                                std::string tr = newTmp("stlr_tr");
                                emit(tr + " = trunc i32 " + val + " to " + mty);
                                val = tr;
                            }
                            emit("store atomic " + mty + " " + val + ", ptr " + ptr_v + " release, align " + std::to_string(1U << size));
                            if (o2 == 0) {
                                storeGpr(rm, "0", false);
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
                        std::string base_val = loadGprOrSp(rn, true);
                        std::string addr_val = base_val;
                        if (mode != 1 && off != 0) {
                            addr_val = newTmp("ldp_a");
                            emit(addr_val + " = add i64 " + base_val + ", " + std::to_string(off));
                        }
                        unsigned esz = 1U << scale;
                        std::string addr2_val = newTmp("ldp_a2");
                        emit(addr2_val + " = add i64 " + addr_val + ", " + std::to_string(esz));
                        std::string p1 = newTmp("p1"), p2 = newTmp("p2");
                        emit(p1 + " = inttoptr i64 " + addr_val + " to ptr");
                        emit(p2 + " = inttoptr i64 " + addr2_val + " to ptr");
                        if (vr == 0) {
                            std::string mty = (size == 0) ? "i32" : "i64";
                            if (L == 0) {
                                std::string v1 = loadGpr(rd, size != 0), v2 = loadGpr(rt2, size != 0);
                                emit("store " + mty + " " + v1 + ", ptr " + p1 + ", align 1");
                                emit("store " + mty + " " + v2 + ", ptr " + p2 + ", align 1");
                            } else {
                                std::string v1 = newTmp("ldp1"), v2 = newTmp("ldp2");
                                if (size == 0) {
                                    emit(v1 + " = load i32, ptr " + p1 + ", align 1");
                                    emit(v2 + " = load i32, ptr " + p2 + ", align 1");
                                    storeGpr(rd, v1, false);
                                    storeGpr(rt2, v2, false);
                                } else if (size == 1) {
                                    emit(v1 + " = load i32, ptr " + p1 + ", align 1");
                                    emit(v2 + " = load i32, ptr " + p2 + ", align 1");
                                    std::string sx1 = newTmp("sx1"), sx2 = newTmp("sx2");
                                    emit(sx1 + " = sext i32 " + v1 + " to i64");
                                    emit(sx2 + " = sext i32 " + v2 + " to i64");
                                    storeGpr(rd, sx1, true);
                                    storeGpr(rt2, sx2, true);
                                } else {
                                    emit(v1 + " = load i64, ptr " + p1 + ", align 1");
                                    emit(v2 + " = load i64, ptr " + p2 + ", align 1");
                                    storeGpr(rd, v1, true);
                                    storeGpr(rt2, v2, true);
                                }
                            }
                        } else {
                            std::string vty = "i" + std::to_string(esz * 8);
                            if (L == 0) {
                                std::string v1 = loadVec(rd, vty), v2 = loadVec(rt2, vty);
                                emit("store " + vty + " " + v1 + ", ptr " + p1 + ", align 1");
                                emit("store " + vty + " " + v2 + ", ptr " + p2 + ", align 1");
                            } else {
                                std::string v1 = newTmp("vldp1"), v2 = newTmp("vldp2");
                                emit(v1 + " = load " + vty + ", ptr " + p1 + ", align 1");
                                emit(v2 + " = load " + vty + ", ptr " + p2 + ", align 1");
                                storeVec(rd, vty, v1);
                                storeVec(rt2, vty, v2);
                            }
                        }
                        if (mode == 1 || mode == 3) {
                            std::string wb = newTmp("wb");
                            emit(wb + " = add i64 " + base_val + ", " + std::to_string(off));
                            storeGprOrSp(rn, wb, true);
                        }
                        continue;
                    }

                    if (((insn >> 27) & 7) == 7) {
                        unsigned p1_bit = (insn >> 24) & 1;
                        std::string base_val = loadGprOrSp(rn, true);
                        unsigned scale = (vr == 1 && (opc & 2) != 0 && size == 0) ? 4 : size;
                        bool writeback = false;
                        std::string wb_val = base_val, addr_val = base_val;
                        if (p1_bit == 1) {
                            uint64_t imm12 = static_cast<uint64_t>((insn >> 10) & 0xFFFU) << scale;
                            if (imm12 != 0) {
                                addr_val = newTmp("addr");
                                emit(addr_val + " = add i64 " + base_val + ", " + std::to_string(imm12));
                            }
                        } else if (((insn >> 21) & 1) == 0) {
                            unsigned mode = (insn >> 10) & 3;
                            int64_t imm9 = sext64((insn >> 12) & 0x1FFU, 9);
                            std::string off_val = newTmp("addr_imm9");
                            emit(off_val + " = add i64 " + base_val + ", " + std::to_string(imm9));
                            if (mode == 1) { addr_val = base_val; wb_val = off_val; writeback = true; }
                            else if (mode == 3) { addr_val = off_val; wb_val = off_val; writeback = true; }
                            else { addr_val = off_val; }
                        } else {
                            unsigned option = (insn >> 13) & 7;
                            unsigned S = (insn >> 12) & 1;
                            std::string ext_rm = emitExtendReg(rm, option, S ? scale : 0);
                            addr_val = newTmp("addr_reg");
                            emit(addr_val + " = add i64 " + base_val + ", " + ext_rm);
                        }
                        std::string ptr_v = newTmp("mptr");
                        emit(ptr_v + " = inttoptr i64 " + addr_val + " to ptr");
                        std::string mty = "i" + std::to_string(8U << scale);
                        if (vr == 0) {
                            if (opc == 0) {
                                std::string val = loadGpr(rd, size == 3);
                                if (size < 2) {
                                    std::string tr = newTmp("str_tr");
                                    emit(tr + " = trunc i32 " + val + " to " + mty);
                                    val = tr;
                                }
                                emit("store " + mty + " " + val + ", ptr " + ptr_v + ", align 1");
                            } else if (opc == 1) {
                                std::string lv = newTmp("ldr_u");
                                emit(lv + " = load " + mty + ", ptr " + ptr_v + ", align 1");
                                if (size < 2) {
                                    std::string zx = newTmp("ldr_zx");
                                    emit(zx + " = zext " + mty + " " + lv + " to i32");
                                    lv = zx;
                                }
                                storeGpr(rd, lv, size == 3);
                            } else if (opc == 2 && size < 3) {
                                std::string lv = newTmp("ldr_s"), sx = newTmp("ldr_sx64");
                                emit(lv + " = load " + mty + ", ptr " + ptr_v + ", align 1");
                                emit(sx + " = sext " + mty + " " + lv + " to i64");
                                storeGpr(rd, sx, true);
                            } else if (opc == 3 && size < 2) {
                                std::string lv = newTmp("ldr_s"), sx = newTmp("ldr_sx32");
                                emit(lv + " = load " + mty + ", ptr " + ptr_v + ", align 1");
                                emit(sx + " = sext " + mty + " " + lv + " to i32");
                                storeGpr(rd, sx, false);
                            }
                        } else {
                            if ((opc & 1) == 0) {
                                std::string vv = loadVec(rd, mty);
                                emit("store " + mty + " " + vv + ", ptr " + ptr_v + ", align 1");
                            } else {
                                std::string vv = newTmp("vldr");
                                emit(vv + " = load " + mty + ", ptr " + ptr_v + ", align 1");
                                storeVec(rd, mty, vv);
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
                        std::string vty = "<" + std::to_string(16 / esz) + " x i" + std::to_string(esz * 8) + ">";
                        std::string addr_val = loadGprOrSp(rn, true);
                        std::string ptr_v = newTmp("ld1_p"), elem = newTmp("ld1_e"), cur_v = loadVec(rd, vty), new_v = newTmp("ld1_v");
                        emit(ptr_v + " = inttoptr i64 " + addr_val + " to ptr");
                        emit(elem + " = load i" + std::to_string(esz * 8) + ", ptr " + ptr_v + ", align 1");
                        emit(new_v + " = insertelement " + vty + " " + cur_v + ", i" + std::to_string(esz * 8) + " " + elem + ", i32 " + std::to_string(lane));
                        storeVec(rd, vty, new_v, false);
                        continue;
                    }

                    if (((insn >> 23) & 0x1F) == 0x18 || ((insn >> 23) & 0x1F) == 0x19) {
                        stats.simd_ops_lifted++;
                        std::string addr_val = loadGprOrSp(rn, true);
                        for (unsigned r_idx = 0; r_idx < 4; ++r_idx) {
                            emit("store i128 0, ptr %reg_v" + std::to_string((rd + r_idx) & 31) + ", align 16");
                        }
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            for (unsigned r_idx = 0; r_idx < 4; ++r_idx) {
                                unsigned vreg = (rd + r_idx) & 31;
                                unsigned m_off = (lane * 4 + r_idx) * 4;
                                std::string ma = newTmp("ld4_a"), mp = newTmp("ld4_p"), mv = newTmp("ld4_v");
                                std::string cv = loadVec(vreg, "<4 x i32>"), nv = newTmp("ld4_nv");
                                emit(ma + " = add i64 " + addr_val + ", " + std::to_string(m_off));
                                emit(mp + " = inttoptr i64 " + ma + " to ptr");
                                emit(mv + " = load i32, ptr " + mp + ", align 1");
                                emit(nv + " = insertelement <4 x i32> " + cv + ", i32 " + mv + ", i32 " + std::to_string(lane));
                                storeVec(vreg, "<4 x i32>", nv, false);
                            }
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
                        std::string a = loadGpr(rn, is64);
                        std::string b = emitShiftReg(loadGpr(rm, is64), shift_type, imm6, is64);
                        if (N) {
                            std::string nb = newTmp("orn");
                            emit(nb + " = xor " + ty + " " + b + ", -1");
                            b = nb;
                        }
                        std::string res = newTmp("logr");
                        std::string op_str = (opc == 0 || opc == 3) ? "and" : (opc == 1 ? "or" : "xor");
                        emit(res + " = " + op_str + " " + ty + " " + a + ", " + b);
                        if (opc == 3) {
                            std::string nc = newTmp("fl_n"), zc = newTmp("fl_z");
                            emit(nc + " = icmp slt " + ty + " " + res + ", 0");
                            emit(zc + " = icmp eq " + ty + " " + res + ", 0");
                            emit("store i1 " + nc + ", ptr %flag_n, align 1");
                            emit("store i1 " + zc + ", ptr %flag_z, align 1");
                            emit("store i1 false, ptr %flag_c, align 1");
                            emit("store i1 false, ptr %flag_v, align 1");
                        }
                        storeGpr(rd, res, is64);
                    } else if (op1 == 0 && (op2 & 8) == 8) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned S = (insn >> 29) & 1;
                        unsigned ext = (insn >> 21) & 1;
                        std::string a, b;
                        if (ext == 0) {
                            a = loadGpr(rn, is64);
                            b = emitShiftReg(loadGpr(rm, is64), (insn >> 22) & 3, (insn >> 10) & 0x3F, is64);
                        } else {
                            a = loadGprOrSp(rn, is64);
                            std::string b64 = emitExtendReg(rm, (insn >> 13) & 7, (insn >> 10) & 7);
                            if (!is64) {
                                b = newTmp("b32");
                                emit(b + " = trunc i64 " + b64 + " to i32");
                            } else {
                                b = b64;
                            }
                        }
                        std::string res = newTmp("addsub");
                        emit(res + " = " + (op ? "sub " : "add ") + ty + " " + a + ", " + b);
                        if (S) {
                            if (op) {
                                std::string nb = newTmp("nb");
                                emit(nb + " = xor " + ty + " " + b + ", -1");
                                emitFlagsAdd(a, nb, "1", res, is64);
                            } else {
                                emitFlagsAdd(a, b, "0", res, is64);
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
                        std::string c_ok = evalCond(cond);
                        std::string a = loadGpr(rn, is64);
                        std::string b = is_imm ? std::to_string(rm) : loadGpr(rm, is64);
                        std::string res = newTmp("ccmp_r");
                        emit(res + " = " + (op ? "sub " : "add ") + ty + " " + a + ", " + b);
                        if (op) {
                            std::string nb = newTmp("ccmp_nb");
                            emit(nb + " = xor " + ty + " " + b + ", -1");
                            emitFlagsAdd(a, nb, "1", res, is64);
                        } else {
                            emitFlagsAdd(a, b, "0", res, is64);
                        }
                        std::string fn = newTmp("cc_n"), fz = newTmp("cc_z"), fc = newTmp("cc_c"), fv = newTmp("cc_v");
                        std::string sn = newTmp("s_n"), sz = newTmp("s_z"), sc = newTmp("s_c"), sv = newTmp("s_v");
                        emit(fn + " = load i1, ptr %flag_n, align 1");
                        emit(fz + " = load i1, ptr %flag_z, align 1");
                        emit(fc + " = load i1, ptr %flag_c, align 1");
                        emit(fv + " = load i1, ptr %flag_v, align 1");
                        emit(sn + " = select i1 " + c_ok + ", i1 " + fn + ", i1 " + (((nzcv >> 3) & 1) ? "true" : "false"));
                        emit(sz + " = select i1 " + c_ok + ", i1 " + fz + ", i1 " + (((nzcv >> 2) & 1) ? "true" : "false"));
                        emit(sc + " = select i1 " + c_ok + ", i1 " + fc + ", i1 " + (((nzcv >> 1) & 1) ? "true" : "false"));
                        emit(sv + " = select i1 " + c_ok + ", i1 " + fv + ", i1 " + ((nzcv & 1) ? "true" : "false"));
                        emit("store i1 " + sn + ", ptr %flag_n, align 1");
                        emit("store i1 " + sz + ", ptr %flag_z, align 1");
                        emit("store i1 " + sc + ", ptr %flag_c, align 1");
                        emit("store i1 " + sv + ", ptr %flag_v, align 1");
                    } else if (op1 == 1 && op2 == 4) {
                        unsigned op = (insn >> 30) & 1;
                        unsigned o2 = (insn >> 10) & 1;
                        unsigned cond = (insn >> 12) & 0xF;
                        std::string c_ok = evalCond(cond);
                        std::string t_val = loadGpr(rn, is64), f_val = loadGpr(rm, is64);
                        if (op) {
                            std::string nf = newTmp("csinv");
                            emit(nf + " = xor " + ty + " " + f_val + ", -1");
                            f_val = nf;
                        }
                        if (o2) {
                            std::string incf = newTmp("csinc");
                            emit(incf + " = add " + ty + " " + f_val + ", 1");
                            f_val = incf;
                        }
                        std::string res = newTmp("csel");
                        emit(res + " = select i1 " + c_ok + ", " + ty + " " + t_val + ", " + ty + " " + f_val);
                        storeGpr(rd, res, is64);
                    } else if (op1 == 1 && op2 == 6) {
                        unsigned S_bit = (insn >> 29) & 1;
                        unsigned opcode2 = (insn >> 10) & 0x3F;
                        if (S_bit == 0) {
                            std::string n = loadGpr(rn, is64), m = loadGpr(rm, is64);
                            if (opcode2 == 0x02) {
                                std::string iz = newTmp("udiv_z"), sm = newTmp("udiv_m"), q = newTmp("udiv_q"), res = newTmp("udiv_r");
                                emit(iz + " = icmp eq " + ty + " " + m + ", 0");
                                emit(sm + " = select i1 " + iz + ", " + ty + " 1, " + ty + " " + m);
                                emit(q + " = udiv " + ty + " " + n + ", " + sm);
                                emit(res + " = select i1 " + iz + ", " + ty + " 0, " + ty + " " + q);
                                storeGpr(rd, res, is64);
                            } else if (opcode2 == 0x03) {
                                std::string int_min = is64 ? "-9223372036854775808" : "-2147483648";
                                std::string iz = newTmp("sdiv_z"), imin = newTmp("sdiv_min"), im1 = newTmp("sdiv_m1");
                                std::string iovf = newTmp("sdiv_ovf"), bad = newTmp("sdiv_bad"), sm = newTmp("sdiv_m");
                                std::string q = newTmp("sdiv_q"), r1 = newTmp("sdiv_r1"), res = newTmp("sdiv_r");
                                emit(iz + " = icmp eq " + ty + " " + m + ", 0");
                                emit(imin + " = icmp eq " + ty + " " + n + ", " + int_min);
                                emit(im1 + " = icmp eq " + ty + " " + m + ", -1");
                                emit(iovf + " = and i1 " + imin + ", " + im1);
                                emit(bad + " = or i1 " + iz + ", " + iovf);
                                emit(sm + " = select i1 " + bad + ", " + ty + " 1, " + ty + " " + m);
                                emit(q + " = sdiv " + ty + " " + n + ", " + sm);
                                emit(r1 + " = select i1 " + iovf + ", " + ty + " " + int_min + ", " + ty + " " + q);
                                emit(res + " = select i1 " + iz + ", " + ty + " 0, " + ty + " " + r1);
                                storeGpr(rd, res, is64);
                            } else if (opcode2 >= 0x08 && opcode2 <= 0x0B) {
                                unsigned mask = is64 ? 63 : 31;
                                std::string sh_amt = newTmp("sh_amt"), res = newTmp("shv");
                                emit(sh_amt + " = and " + ty + " " + m + ", " + std::to_string(mask));
                                if (opcode2 == 0x08) emit(res + " = shl " + ty + " " + n + ", " + sh_amt);
                                else if (opcode2 == 0x09) emit(res + " = lshr " + ty + " " + n + ", " + sh_amt);
                                else if (opcode2 == 0x0A) emit(res + " = ashr " + ty + " " + n + ", " + sh_amt);
                                else emit(res + " = call " + ty + " @llvm.fshr." + ty + "(" + ty + " " + n + ", " + ty + " " + n + ", " + ty + " " + sh_amt + ")");
                                storeGpr(rd, res, is64);
                            }
                        } else {
                            std::string n = loadGpr(rn, is64), res = newTmp("dp1");
                            if (opcode2 == 0x00) emit(res + " = call " + ty + " @llvm.bitreverse." + ty + "(" + ty + " " + n + ")");
                            else if (opcode2 == 0x02 || opcode2 == 0x03) emit(res + " = call " + ty + " @llvm.bswap." + ty + "(" + ty + " " + n + ")");
                            else if (opcode2 == 0x04) emit(res + " = call " + ty + " @llvm.ctlz." + ty + "(" + ty + " " + n + ", i1 false)");
                            else emit(res + " = add " + ty + " " + n + ", 0");
                            storeGpr(rd, res, is64);
                        }
                    } else if (op1 == 1 && (op2 & 8) == 8) {
                        unsigned op54 = (insn >> 29) & 3;
                        unsigned op31 = (insn >> 21) & 7;
                        unsigned o0 = (insn >> 15) & 1;
                        if (op54 == 0 && op31 == 0) {
                            std::string a = loadGpr(ra, is64), n = loadGpr(rn, is64), m = loadGpr(rm, is64);
                            std::string p = newTmp("mul"), res = newTmp("madd");
                            emit(p + " = mul " + ty + " " + n + ", " + m);
                            emit(res + " = " + (o0 ? "sub " : "add ") + ty + " " + a + ", " + p);
                            storeGpr(rd, res, is64);
                        } else if (op54 == 0 && (op31 == 1 || op31 == 5)) {
                            std::string a = loadGpr(ra, true), n32 = loadGpr(rn, false), m32 = loadGpr(rm, false);
                            std::string n64 = newTmp("m_n"), m64 = newTmp("m_m"), p = newTmp("m_p"), res = newTmp("maddl");
                            std::string ext_op = (op31 == 1) ? "sext" : "zext";
                            emit(n64 + " = " + ext_op + " i32 " + n32 + " to i64");
                            emit(m64 + " = " + ext_op + " i32 " + m32 + " to i64");
                            emit(p + " = mul i64 " + n64 + ", " + m64);
                            emit(res + " = " + (o0 ? "sub " : "add ") + "i64 " + a + ", " + p);
                            storeGpr(rd, res, true);
                        } else if (op54 == 0 && (op31 == 2 || op31 == 6)) {
                            // SMULH (op31 == 2) / UMULH (op31 == 6)
                            std::string n64 = loadGpr(rn, true), m64 = loadGpr(rm, true);
                            std::string ext_op = (op31 == 2) ? "sext" : "zext";
                            std::string n128 = newTmp("mh_n"), m128 = newTmp("mh_m"), p128 = newTmp("mh_p"), sh128 = newTmp("mh_s"), res = newTmp("mulh");
                            emit(n128 + " = " + ext_op + " i64 " + n64 + " to i128");
                            emit(m128 + " = " + ext_op + " i64 " + m64 + " to i128");
                            emit(p128 + " = mul i128 " + n128 + ", " + m128);
                            emit(sh128 + " = lshr i128 " + p128 + ", 64");
                            emit(res + " = trunc i128 " + sh128 + " to i64");
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
                        std::string f_int_ty = fp64 ? "i64" : "i32";
                        std::string f_ty = fp64 ? "double" : "float";
                        if (rmode == 0 && opcode == 6) {
                            storeGpr(rd, loadVec(rn, is64 ? "i64" : "i32"), is64);
                        } else if (rmode == 0 && opcode == 7) {
                            storeVec(rd, is64 ? "i64" : "i32", loadGpr(rn, is64));
                        } else if (rmode == 0 && (opcode == 2 || opcode == 3)) {
                            // SCVTF (opcode==2) / UCVTF (opcode==3)
                            std::string iv = loadGpr(rn, is64), fv = newTmp("cvtf"), bv = newTmp("cvtf_b");
                            emit(fv + " = " + (opcode == 2 ? "sitofp " : "uitofp ") + ty + " " + iv + " to " + f_ty);
                            emit(bv + " = bitcast " + f_ty + " " + fv + " to " + f_int_ty);
                            storeVec(rd, f_int_ty, bv);
                        } else if (rmode == 3 && (opcode == 0 || opcode == 1)) {
                            // FCVTZS (opcode==0) / FCVTZU (opcode==1)
                            std::string bv = loadVec(rn, f_int_ty), fv = newTmp("fcvt_f"), iv = newTmp("fcvt_i");
                            emit(fv + " = bitcast " + f_int_ty + " " + bv + " to " + f_ty);
                            emit(iv + " = " + (opcode == 0 ? "fptosi " : "fptoui ") + f_ty + " " + fv + " to " + ty);
                            storeGpr(rd, iv, is64);
                        }
                        continue;
                    }

                    // Scalar Floating-Point Data Processing (1-src, FCMP, FMOV imm, FCSEL, 2-src)
                    if (((insn >> 24) & 0x5FU) == 0x1EU && ((insn >> 21) & 1U) == 1U) {
                        bool fp64 = (sz & 1) != 0;
                        std::string f_int_ty = fp64 ? "i64" : "i32";
                        std::string f_ty = fp64 ? "double" : "float";
                        unsigned low6 = (insn >> 10) & 0x3FU;
                        if ((low6 & 0x1FU) == 0x10U) {
                            // 1-source: FMOV, FABS, FNEG, FSQRT, FCVT
                            unsigned opc1 = (insn >> 15) & 0x3FU;
                            std::string vn_i = loadVec(rn, f_int_ty);
                            if (opc1 == 0) {
                                storeVec(rd, f_int_ty, vn_i);
                            } else if (opc1 == 1) {
                                // FABS
                                std::string msk = fp64 ? "9223372036854775807" : "2147483647";
                                std::string r_i = newTmp("fabs_i");
                                emit(r_i + " = and " + f_int_ty + " " + vn_i + ", " + msk);
                                storeVec(rd, f_int_ty, r_i);
                            } else if (opc1 == 2) {
                                // FNEG
                                std::string vn_f = newTmp("fneg_in"), r_f = newTmp("fneg_r"), r_i = newTmp("fneg_i");
                                emit(vn_f + " = bitcast " + f_int_ty + " " + vn_i + " to " + f_ty);
                                emit(r_f + " = fneg " + f_ty + " " + vn_f);
                                emit(r_i + " = bitcast " + f_ty + " " + r_f + " to " + f_int_ty);
                                storeVec(rd, f_int_ty, r_i);
                            } else if (opc1 == 4 || opc1 == 5) {
                                // FCVT single <-> double
                                std::string vn_f = newTmp("fcvt_in"), r_f = newTmp("fcvt_out"), r_i = newTmp("fcvt_bi");
                                emit(vn_f + " = bitcast " + f_int_ty + " " + vn_i + " to " + f_ty);
                                if (!fp64 && opc1 == 5) {
                                    emit(r_f + " = fpext float " + vn_f + " to double");
                                    emit(r_i + " = bitcast double " + r_f + " to i64");
                                    storeVec(rd, "i64", r_i);
                                } else if (fp64 && opc1 == 4) {
                                    emit(r_f + " = fptrunc double " + vn_f + " to float");
                                    emit(r_i + " = bitcast float " + r_f + " to i32");
                                    storeVec(rd, "i32", r_i);
                                } else {
                                    storeVec(rd, f_int_ty, vn_i);
                                }
                            } else {
                                storeVec(rd, f_int_ty, vn_i);
                            }
                            continue;
                        }
                        if (low6 == 0x08U) {
                            // FCMP / FCMPE
                            bool cmp_zero = ((insn >> 3) & 1U) != 0;
                            std::string vn_i = loadVec(rn, f_int_ty), vm_i = cmp_zero ? "0" : loadVec(rm, f_int_ty);
                            std::string vn_f = newTmp("fcmp_a"), vm_f = newTmp("fcmp_b");
                            emit(vn_f + " = bitcast " + f_int_ty + " " + vn_i + " to " + f_ty);
                            emit(vm_f + " = bitcast " + f_int_ty + " " + vm_i + " to " + f_ty);
                            std::string flt = newTmp("fc_lt"), feq = newTmp("fc_eq"), fgt = newTmp("fc_gt"), funo = newTmp("fc_un"), fc_val = newTmp("fc_c");
                            emit(flt + " = fcmp olt " + f_ty + " " + vn_f + ", " + vm_f);
                            emit(feq + " = fcmp oeq " + f_ty + " " + vn_f + ", " + vm_f);
                            emit(fgt + " = fcmp ogt " + f_ty + " " + vn_f + ", " + vm_f);
                            emit(funo + " = fcmp uno " + f_ty + " " + vn_f + ", " + vm_f);
                            std::string c_tmp = newTmp("fc_ct");
                            emit(c_tmp + " = or i1 " + feq + ", " + fgt);
                            emit(fc_val + " = or i1 " + c_tmp + ", " + funo);
                            emit("store i1 " + flt + ", ptr %flag_n, align 1");
                            emit("store i1 " + feq + ", ptr %flag_z, align 1");
                            emit("store i1 " + fc_val + ", ptr %flag_c, align 1");
                            emit("store i1 " + funo + ", ptr %flag_v, align 1");
                            continue;
                        }
                        if ((low6 & 0x7U) == 0x4U) {
                            // FMOV scalar immediate
                            unsigned imm8 = (insn >> 13) & 0xFFU;
                            if (fp64) {
                                uint64_t bits = expandVfpFloat64Imm(imm8);
                                storeVec(rd, "i64", std::to_string(bits));
                            } else {
                                uint32_t bits = expandVfpFloat32Imm(imm8);
                                storeVec(rd, "i32", std::to_string(bits));
                            }
                            continue;
                        }
                        if ((low6 & 0x3U) == 0x3U) {
                            // FCSEL
                            unsigned cond = (insn >> 12) & 0xFU;
                            std::string c_ok = evalCond(cond);
                            std::string vn_i = loadVec(rn, f_int_ty), vm_i = loadVec(rm, f_int_ty), res = newTmp("fcsel");
                            emit(res + " = select i1 " + c_ok + ", " + f_int_ty + " " + vn_i + ", " + f_int_ty + " " + vm_i);
                            storeVec(rd, f_int_ty, res);
                            continue;
                        }
                        if ((low6 & 0x3U) == 0x2U) {
                            // 2-source: FMUL (0), FDIV (1), FADD (2), FSUB (3), FMAX (4/6), FMIN (5/7), FNMUL (8)
                            unsigned opc2 = (insn >> 12) & 0xFU;
                            std::string vn_i = loadVec(rn, f_int_ty), vm_i = loadVec(rm, f_int_ty);
                            std::string vn_f = newTmp("fp2_a"), vm_f = newTmp("fp2_b"), rf = newTmp("fp2_r"), ri = newTmp("fp2_i");
                            emit(vn_f + " = bitcast " + f_int_ty + " " + vn_i + " to " + f_ty);
                            emit(vm_f + " = bitcast " + f_int_ty + " " + vm_i + " to " + f_ty);
                            if (opc2 == 0) emit(rf + " = fmul " + f_ty + " " + vn_f + ", " + vm_f);
                            else if (opc2 == 1) emit(rf + " = fdiv " + f_ty + " " + vn_f + ", " + vm_f);
                            else if (opc2 == 2) emit(rf + " = fadd " + f_ty + " " + vn_f + ", " + vm_f);
                            else if (opc2 == 3) emit(rf + " = fsub " + f_ty + " " + vn_f + ", " + vm_f);
                            else if (opc2 == 4 || opc2 == 6) {
                                std::string gt = newTmp("fmax_c");
                                emit(gt + " = fcmp ogt " + f_ty + " " + vn_f + ", " + vm_f);
                                emit(rf + " = select i1 " + gt + ", " + f_ty + " " + vn_f + ", " + f_ty + " " + vm_f);
                            } else if (opc2 == 5 || opc2 == 7) {
                                std::string lt = newTmp("fmin_c");
                                emit(lt + " = fcmp olt " + f_ty + " " + vn_f + ", " + vm_f);
                                emit(rf + " = select i1 " + lt + ", " + f_ty + " " + vn_f + ", " + f_ty + " " + vm_f);
                            } else {
                                std::string m_t = newTmp("fnmul_t");
                                emit(m_t + " = fmul " + f_ty + " " + vn_f + ", " + vm_f);
                                emit(rf + " = fneg " + f_ty + " " + m_t);
                            }
                            emit(ri + " = bitcast " + f_ty + " " + rf + " to " + f_int_ty);
                            storeVec(rd, f_int_ty, ri);
                            continue;
                        }
                    }

                    // Scalar Floating-Point 3-Source (FMADD, FMSUB, FNMADD, FNMSUB)
                    if (((insn >> 24) & 0x5FU) == 0x1FU) {
                        bool fp64 = (sz & 1) != 0;
                        std::string f_int_ty = fp64 ? "i64" : "i32";
                        std::string f_ty = fp64 ? "double" : "float";
                        unsigned o1 = (insn >> 21) & 1U;
                        unsigned o0 = (insn >> 15) & 1U;
                        std::string vn_f = newTmp("fm3_n"), vm_f = newTmp("fm3_m"), va_f = newTmp("fm3_a"), p_f = newTmp("fm3_p"), r_f = newTmp("fm3_r"), r_i = newTmp("fm3_i");
                        emit(vn_f + " = bitcast " + f_int_ty + " " + loadVec(rn, f_int_ty) + " to " + f_ty);
                        emit(vm_f + " = bitcast " + f_int_ty + " " + loadVec(rm, f_int_ty) + " to " + f_ty);
                        emit(va_f + " = bitcast " + f_int_ty + " " + loadVec(ra, f_int_ty) + " to " + f_ty);
                        emit(p_f + " = fmul " + f_ty + " " + vn_f + ", " + vm_f);
                        std::string comb = newTmp("fm3_c");
                        emit(comb + " = " + (o0 ? "fsub " : "fadd ") + f_ty + " " + va_f + ", " + p_f);
                        if (o1) emit(r_f + " = fneg " + f_ty + " " + comb);
                        else emit(r_f + " = fadd " + f_ty + " " + comb + ", 0.0");
                        emit(r_i + " = bitcast " + f_ty + " " + r_f + " to " + f_int_ty);
                        storeVec(rd, f_int_ty, r_i);
                        continue;
                    }

                    // AdvSIMD Scalar DUP sD/dD, vN.s/d[idx] (0x5E000400)
                    if ((insn & 0xFFE0FC00U) == 0x5E000400U) {
                        unsigned imm5 = (insn >> 16) & 0x1FU;
                        unsigned tz = imm5 ? __builtin_ctz(imm5) : 2;
                        unsigned esz = 1U << tz;
                        unsigned idx = imm5 >> (tz + 1);
                        std::string sty = "i" + std::to_string(esz * 8);
                        std::string full_vty = "<" + std::to_string(16 / esz) + " x " + sty + ">";
                        std::string vn = loadVec(rn, full_vty), el = newTmp("sdup_el");
                        emit(el + " = extractelement " + full_vty + " " + vn + ", i32 " + std::to_string(idx));
                        storeVec(rd, sty, el);
                        continue;
                    }

                    // ARMv8 Crypto SHA1H sD, sN (0x5E280800)
                    if ((insn & 0xFFFFFC00U) == 0x5E280800U) {
                        stats.crypto_sha1_lifted++;
                        std::string sn = loadVec(rn, "i32"), res = newTmp("sha1h");
                        emit(res + " = call i32 @llvm.fshr.i32(i32 " + sn + ", i32 " + sn + ", i32 2)");
                        storeVec(rd, "i32", res, true);
                        continue;
                    }

                    // ARMv8 Crypto SHA1SU1 vD.4s, vN.4s (0x5E281800)
                    if ((insn & 0xFFFFFC00U) == 0x5E281800U) {
                        stats.crypto_sha1_lifted++;
                        std::string vd = loadVec(rd, "<4 x i32>"), vn = loadVec(rn, "<4 x i32>");
                        std::string n_shuf = newTmp("su1_sh"), t_xor = newTmp("su1_t"), rol1 = newTmp("su1_r1");
                        std::string r0 = newTmp("su1_r0"), r0_rol1 = newTmp("su1_r0r"), r3 = newTmp("su1_r3");
                        std::string r3_new = newTmp("su1_r3n"), res = newTmp("su1_res");
                        emit(n_shuf + " = shufflevector <4 x i32> " + vn + ", <4 x i32> zeroinitializer, <4 x i32> <i32 1, i32 2, i32 3, i32 4>");
                        emit(t_xor + " = xor <4 x i32> " + vd + ", " + n_shuf);
                        emit(rol1 + " = call <4 x i32> @llvm.fshr.v4i32(<4 x i32> " + t_xor + ", <4 x i32> " + t_xor + ", <4 x i32> <i32 31, i32 31, i32 31, i32 31>)");
                        emit(r0 + " = extractelement <4 x i32> " + rol1 + ", i32 0");
                        emit(r0_rol1 + " = call i32 @llvm.fshr.i32(i32 " + r0 + ", i32 " + r0 + ", i32 31)");
                        emit(r3 + " = extractelement <4 x i32> " + rol1 + ", i32 3");
                        emit(r3_new + " = xor i32 " + r3 + ", " + r0_rol1);
                        emit(res + " = insertelement <4 x i32> " + rol1 + ", i32 " + r3_new + ", i32 3");
                        storeVec(rd, "<4 x i32>", res, false);
                        continue;
                    }

                    // ARMv8 Crypto SHA1C / SHA1P / SHA1M / SHA1SU0 (0x5E000000 .. 0x5E003000)
                    if ((insn & 0xFFE08C00U) == 0x5E000000U) {
                        stats.crypto_sha1_lifted++;
                        unsigned opc = (insn >> 12) & 7;
                        std::string vd = loadVec(rd, "<4 x i32>"), vm = loadVec(rm, "<4 x i32>");
                        if (opc == 3) {
                            // SHA1SU0 vD.4s, vN.4s, vM.4s
                            std::string vn = loadVec(rn, "<4 x i32>");
                            std::string sh = newTmp("su0_sh"), x1 = newTmp("su0_x1"), res = newTmp("su0_r");
                            emit(sh + " = shufflevector <4 x i32> " + vd + ", <4 x i32> " + vn + ", <4 x i32> <i32 2, i32 3, i32 4, i32 5>");
                            emit(x1 + " = xor <4 x i32> " + sh + ", " + vd);
                            emit(res + " = xor <4 x i32> " + x1 + ", " + vm);
                            storeVec(rd, "<4 x i32>", res, false);
                        } else {
                            // SHA1C (opc=0), SHA1P (opc=1), SHA1M (opc=2)
                            std::string a = newTmp("sha_a"), b = newTmp("sha_b"), c = newTmp("sha_c"), d = newTmp("sha_d");
                            emit(a + " = extractelement <4 x i32> " + vd + ", i32 0");
                            emit(b + " = extractelement <4 x i32> " + vd + ", i32 1");
                            emit(c + " = extractelement <4 x i32> " + vd + ", i32 2");
                            emit(d + " = extractelement <4 x i32> " + vd + ", i32 3");
                            std::string e = loadVec(rn, "i32");
                            for (unsigned lane = 0; lane < 4; ++lane) {
                                std::string w = newTmp("sha_w"), t = newTmp("sha_t");
                                emit(w + " = extractelement <4 x i32> " + vm + ", i32 " + std::to_string(lane));
                                if (opc == 0) {
                                    std::string cxd = newTmp("cxd"), band = newTmp("band");
                                    emit(cxd + " = xor i32 " + c + ", " + d);
                                    emit(band + " = and i32 " + b + ", " + cxd);
                                    emit(t + " = xor i32 " + d + ", " + band);
                                } else if (opc == 1) {
                                    std::string bxc = newTmp("bxc");
                                    emit(bxc + " = xor i32 " + b + ", " + c);
                                    emit(t + " = xor i32 " + bxc + ", " + d);
                                } else {
                                    std::string bc = newTmp("bc"), borc = newTmp("borc"), bd = newTmp("bd");
                                    emit(bc + " = and i32 " + b + ", " + c);
                                    emit(borc + " = or i32 " + b + ", " + c);
                                    emit(bd + " = and i32 " + borc + ", " + d);
                                    emit(t + " = or i32 " + bc + ", " + bd);
                                }
                                std::string a_rol5 = newTmp("ar5"), s1 = newTmp("sh_s1"), s2 = newTmp("sh_s2"), e_new = newTmp("sh_en"), b_rol30 = newTmp("br30");
                                emit(a_rol5 + " = call i32 @llvm.fshr.i32(i32 " + a + ", i32 " + a + ", i32 27)");
                                emit(s1 + " = add i32 " + e + ", " + a_rol5);
                                emit(s2 + " = add i32 " + s1 + ", " + t);
                                emit(e_new + " = add i32 " + s2 + ", " + w);
                                emit(b_rol30 + " = call i32 @llvm.fshr.i32(i32 " + b + ", i32 " + b + ", i32 2)");
                                e = d;
                                d = c;
                                c = b_rol30;
                                b = a;
                                a = e_new;
                            }
                            std::string v0 = newTmp("sh_v0"), v1 = newTmp("sh_v1"), v2 = newTmp("sh_v2"), v3 = newTmp("sh_v3");
                            emit(v0 + " = insertelement <4 x i32> zeroinitializer, i32 " + a + ", i32 0");
                            emit(v1 + " = insertelement <4 x i32> " + v0 + ", i32 " + b + ", i32 1");
                            emit(v2 + " = insertelement <4 x i32> " + v1 + ", i32 " + c + ", i32 2");
                            emit(v3 + " = insertelement <4 x i32> " + v2 + ", i32 " + d + ", i32 3");
                            storeVec(rd, "<4 x i32>", v3, false);
                        }
                        continue;
                    }

                    // AdvSIMD REV32 vD.16b, vN.16b (0x2E200800 / 0x6E200800)
                    if ((insn & 0xBF3FFC00U) == 0x2E200800U) {
                        std::string vty = Q ? "<4 x i32>" : "<2 x i32>";
                        std::string intr = Q ? "@llvm.bswap.v4i32" : "@llvm.bswap.v2i32";
                        std::string vn = loadVec(rn, vty), res = newTmp("vrev32");
                        emit(res + " = call " + vty + " " + intr + "(" + vty + " " + vn + ")");
                        storeVec(rd, vty, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD EXT vD.16b, vN.16b, vM.16b, #imm4
                    if ((insn & 0xBF208400U) == 0x2E000000U) {
                        unsigned imm4 = (insn >> 11) & 0xF;
                        unsigned lanes = Q ? 16 : 8;
                        std::string vty = "<" + std::to_string(lanes) + " x i8>";
                        std::string vn = loadVec(rn, vty), vm = loadVec(rm, vty), res = newTmp("vext");
                        std::ostringstream mask_ss;
                        for (unsigned i = 0; i < lanes; ++i) {
                            if (i > 0) mask_ss << ", ";
                            mask_ss << "i32 " << (imm4 + i);
                        }
                        emit(res + " = shufflevector " + vty + " " + vn + ", " + vty + " " + vm + ", <" + std::to_string(lanes) + " x i32> <" + mask_ss.str() + ">");
                        storeVec(rd, vty, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD modified immediate (MOVI / BIC imm)
                    if (((insn >> 19) & 0x1FF) == 0x1E0 && ((insn >> 10) & 3) == 1) {
                        unsigned cmode = (insn >> 12) & 0xF;
                        unsigned imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 0x1F);
                        uint64_t v64 = expandAdvSimdImm(U, cmode, imm8);
                        if (U == 1 && (cmode & 0x9) == 1) {
                            std::string cur = loadVec(rd, "<2 x i64>"), res = newTmp("vbic_i");
                            uint64_t mask = ~v64;
                            emit(res + " = and <2 x i64> " + cur + ", <i64 " + std::to_string(mask) + ", i64 " + std::to_string(Q ? mask : 0ULL) + ">");
                            storeVec(rd, "<2 x i64>", res, false);
                        } else {
                            std::string cur_z = newTmp("movi_v");
                            emit(cur_z + " = insertelement <2 x i64> zeroinitializer, i64 " + std::to_string(v64) + ", i32 0");
                            if (Q) {
                                std::string cur_z2 = newTmp("movi_v2");
                                emit(cur_z2 + " = insertelement <2 x i64> " + cur_z + ", i64 " + std::to_string(v64) + ", i32 1");
                                cur_z = cur_z2;
                            }
                            storeVec(rd, "<2 x i64>", cur_z, false);
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
                        std::string sty = "i" + std::to_string(esz * 8);
                        std::string vty = "<" + std::to_string(lanes) + " x " + sty + ">";
                        std::string full_vty = "<" + std::to_string(16 / esz) + " x " + sty + ">";
                        if (U == 0 && imm4 == 7) {
                            std::string v = loadVec(rn, full_vty), el = newTmp("umov");
                            emit(el + " = extractelement " + full_vty + " " + v + ", i32 " + std::to_string(idx));
                            if (esz < 4) {
                                std::string zx = newTmp("umov_zx");
                                emit(zx + " = zext " + sty + " " + el + " to i32");
                                el = zx;
                            }
                            storeGpr(rd, el, Q == 1);
                        } else if (U == 0 && imm4 == 1) {
                            // DUP vD.<T>, Rn
                            std::string gval = loadGpr(rn, esz == 8);
                            if (esz < 4) {
                                std::string tr = newTmp("dup_tr");
                                emit(tr + " = trunc i32 " + gval + " to " + sty);
                                gval = tr;
                            }
                            std::string cur = "zeroinitializer";
                            for (unsigned i = 0; i < lanes; ++i) {
                                std::string nxt = newTmp("dup_v");
                                emit(nxt + " = insertelement " + vty + " " + cur + ", " + sty + " " + gval + ", i32 " + std::to_string(i));
                                cur = nxt;
                            }
                            storeVec(rd, vty, cur, Q == 0);
                        } else if (U == 0 && imm4 == 3) {
                            // INS vD.<Ts>[idx], Rn
                            std::string gval = loadGpr(rn, esz == 8);
                            if (esz < 4) {
                                std::string tr = newTmp("ins_tr");
                                emit(tr + " = trunc i32 " + gval + " to " + sty);
                                gval = tr;
                            }
                            std::string cur = loadVec(rd, full_vty), nxt = newTmp("ins_v");
                            emit(nxt + " = insertelement " + full_vty + " " + cur + ", " + sty + " " + gval + ", i32 " + std::to_string(idx));
                            storeVec(rd, full_vty, nxt, false);
                        } else if (U == 0 && imm4 == 0) {
                            // DUP vD.<T>, vN.<Ts>[idx]
                            std::string vn = loadVec(rn, full_vty), el = newTmp("dupel");
                            emit(el + " = extractelement " + full_vty + " " + vn + ", i32 " + std::to_string(idx));
                            std::string cur = "zeroinitializer";
                            for (unsigned i = 0; i < lanes; ++i) {
                                std::string nxt = newTmp("dup_ve");
                                emit(nxt + " = insertelement " + vty + " " + cur + ", " + sty + " " + el + ", i32 " + std::to_string(i));
                                cur = nxt;
                            }
                            storeVec(rd, vty, cur, Q == 0);
                        }
                        continue;
                    }

                    // AdvSIMD vector logical (AND, BIC, ORR, EOR)
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 21) & 1) == 1 && ((insn >> 10) & 0x3F) == 0x07) {
                        std::string n = loadVec(rn, "i128"), m = loadVec(rm, "i128"), res = newTmp("vlog");
                        if (U == 0 && sz == 1) {
                            std::string nm = newTmp("vnot");
                            emit(nm + " = xor i128 " + m + ", -1");
                            emit(res + " = and i128 " + n + ", " + nm);
                        } else if (U == 0 && sz == 2) {
                            emit(res + " = or i128 " + n + ", " + m);
                        } else if (U == 0 && sz == 0) {
                            emit(res + " = and i128 " + n + ", " + m);
                        } else {
                            emit(res + " = xor i128 " + n + ", " + m);
                        }
                        storeVec(rd, "i128", res, false);
                        continue;
                    }

                    // AdvSIMD Three-Same (ADD / SUB / CMEQ / CMGT / CMHI)
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 21) & 1) == 1 && ((insn >> 10) & 1) == 1) {
                        unsigned opcode = (insn >> 11) & 0x1F;
                        unsigned lanes = (Q ? 16U : 8U) >> sz;
                        std::string vty = "<" + std::to_string(lanes) + " x i" + std::to_string(8U << sz) + ">";
                        std::string a = loadVec(rn, vty), b = loadVec(rm, vty), res = newTmp("vadd");
                        if (opcode == 0x10) {
                            emit(res + " = " + (U ? "sub " : "add ") + vty + " " + a + ", " + b);
                        } else if (opcode == 0x11 && U == 1) {
                            std::string cmp_v = newTmp("vcmeq");
                            emit(cmp_v + " = icmp eq " + vty + " " + a + ", " + b);
                            emit(res + " = sext <" + std::to_string(lanes) + " x i1> " + cmp_v + " to " + vty);
                        } else {
                            emit(res + " = add " + vty + " " + a + ", " + b);
                        }
                        storeVec(rd, vty, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD USHLL
                    if (((insn >> 23) & 0x3F) == 0x1E && ((insn >> 10) & 1) == 1) {
                        unsigned immh = (insn >> 19) & 0xF;
                        unsigned immb = (insn >> 16) & 7;
                        unsigned l = 31 - __builtin_clz(immh);
                        unsigned esz = 1U << l;
                        unsigned sh = ((immh << 3) | immb) - (8U << l);
                        unsigned lanes = 8U / esz;
                        std::string src_ty = "<" + std::to_string(lanes * 2) + " x i" + std::to_string(esz * 8) + ">";
                        std::string half_ty = "<" + std::to_string(lanes) + " x i" + std::to_string(esz * 8) + ">";
                        std::string dst_ty = "<" + std::to_string(lanes) + " x i" + std::to_string(esz * 16) + ">";
                        std::string src_v = loadVec(rn, src_ty), shuf = newTmp("ushll_shuf"), zx = newTmp("ushll_zx");
                        std::ostringstream m_ss;
                        for (unsigned i = 0; i < lanes; ++i) {
                            if (i > 0) m_ss << ", ";
                            m_ss << "i32 " << (i + (Q ? lanes : 0));
                        }
                        emit(shuf + " = shufflevector " + src_ty + " " + src_v + ", " + src_ty + " poison, <" + std::to_string(lanes) + " x i32> <" + m_ss.str() + ">");
                        emit(zx + " = zext " + half_ty + " " + shuf + " to " + dst_ty);
                        if (sh > 0) {
                            std::string sh_v = newTmp("ushll_sh");
                            std::ostringstream sp_ss;
                            for (unsigned i = 0; i < lanes; ++i) {
                                if (i > 0) sp_ss << ", ";
                                sp_ss << "i" << (esz * 16) << " " << sh;
                            }
                            emit(sh_v + " = shl " + dst_ty + " " + zx + ", <" + sp_ss.str() + ">");
                            zx = sh_v;
                        }
                        storeVec(rd, dst_ty, zx, false);
                        continue;
                    }

                    // AdvSIMD UZP1
                    if (((insn >> 24) & 0x3F) == 0x0E && ((insn >> 21) & 1) == 0 && ((insn >> 10) & 3) == 2) {
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        std::string vty = "<" + std::to_string(lanes) + " x i" + std::to_string(esz * 8) + ">";
                        std::string a = loadVec(rn, vty), b = loadVec(rm, vty), res = newTmp("uzp1");
                        std::ostringstream idx_ss;
                        for (unsigned i = 0; i < lanes; ++i) {
                            if (i > 0) idx_ss << ", ";
                            idx_ss << "i32 " << (i * 2);
                        }
                        emit(res + " = shufflevector " + vty + " " + a + ", " + vty + " " + b + ", <" + std::to_string(lanes) + " x i32> <" + idx_ss.str() + ">");
                        storeVec(rd, vty, res, Q == 0);
                        continue;
                    }

                    // AdvSIMD Two-Register Misc: CNT, MVN, CMEQ #0
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 17) & 0x1F) == 0x10 && ((insn >> 10) & 3) == 2) {
                        unsigned opcode = (insn >> 12) & 0x1F;
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        std::string vty = "<" + std::to_string(lanes) + " x i" + std::to_string(esz * 8) + ">";
                        if (opcode == 0x05) {
                            if (U == 0) {
                                std::string vin = loadVec(rn, "<8 x i8>"), vout = newTmp("vcnt");
                                emit(vout + " = call <8 x i8> @llvm.ctpop.v8i8(<8 x i8> " + vin + ")");
                                storeVec(rd, "<8 x i8>", vout);
                            } else {
                                std::string vin = loadVec(rn, "i128"), vout = newTmp("vmvn");
                                emit(vout + " = xor i128 " + vin + ", -1");
                                storeVec(rd, "i128", vout, false);
                            }
                        } else if (opcode == 0x09 && U == 0) {
                            std::string vin = loadVec(rn, vty), cmp_v = newTmp("cmeq0"), sext_v = newTmp("cmeq_sx");
                            emit(cmp_v + " = icmp eq " + vty + " " + vin + ", zeroinitializer");
                            emit(sext_v + " = sext <" + std::to_string(lanes) + " x i1> " + cmp_v + " to " + vty);
                            storeVec(rd, vty, sext_v, Q == 0);
                        }
                        continue;
                    }

                    // AdvSIMD Across Lanes: ADDV / UMAXV
                    if (((insn >> 24) & 0x1F) == 0x0E && ((insn >> 17) & 0x1F) == 0x18 && ((insn >> 10) & 3) == 2) {
                        unsigned opcode = (insn >> 12) & 0x1F;
                        unsigned esz = 1U << sz;
                        unsigned lanes = (Q ? 16U : 8U) / esz;
                        std::string vty = "<" + std::to_string(lanes) + " x i" + std::to_string(esz * 8) + ">";
                        std::string sty = "i" + std::to_string(esz * 8);
                        std::string vin = loadVec(rn, vty), res = newTmp("vred");
                        std::string intr = (opcode == 0x1B) ? "add" : "umax";
                        emit(res + " = call " + sty + " @llvm.vector.reduce." + intr + ".v" + std::to_string(lanes) + "i" + std::to_string(esz * 8) + "(" + vty + " " + vin + ")");
                        storeVec(rd, sty, res);
                        continue;
                    }

                    // AdvSIMD TBL
                    if (((insn >> 24) & 0xBF) == 0x0E && ((insn >> 21) & 1) == 0 && ((insn >> 15) & 1) == 0 && ((insn >> 10) & 3) == 0) {
                        std::string tbl_v = loadVec(rn, "<16 x i8>"), idx_v = loadVec(rm, "<16 x i8>");
                        std::string cur_out = "zeroinitializer";
                        for (unsigned i = 0; i < 16; ++i) {
                            std::string ie = newTmp("tbl_i"), ic = newTmp("tbl_c"), cl = newTmp("tbl_cl");
                            std::string tv = newTmp("tbl_v"), sv = newTmp("tbl_s"), nout = newTmp("tbl_o");
                            emit(ie + " = extractelement <16 x i8> " + idx_v + ", i32 " + std::to_string(i));
                            emit(ic + " = icmp ult i8 " + ie + ", 16");
                            emit(cl + " = and i8 " + ie + ", 15");
                            emit(tv + " = extractelement <16 x i8> " + tbl_v + ", i8 " + cl);
                            emit(sv + " = select i1 " + ic + ", i8 " + tv + ", i8 0");
                            emit(nout + " = insertelement <16 x i8> " + cur_out + ", i8 " + sv + ", i32 " + std::to_string(i));
                            cur_out = nout;
                        }
                        storeVec(rd, "<16 x i8>", cur_out, false);
                        continue;
                    }
                }
            }

            if (!terminated) {
                if (b_stop < end_addr) emit("br label %bb_" + hex8(b_stop));
                else emit("br label %exit_ret");
            }
        }

        emitRaw("exit_ret:");
        flushState(true);
        emit("ret void");
        emitRaw("}\n");
    }

    std::string generateModuleText() {
        out.str("");
        out.clear();
        emitRaw("; ============================================================================");
        emitRaw("; AOT Lifted LLVM-IR Module for 64-bit libGLES_mali.so (" + loader.driver_version + ")");
        emitRaw("; ============================================================================");
        emitRaw("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"");
        emitRaw("target triple = \"x86_64-pc-linux-gnu\"\n");

        emitRaw("declare void @llvm.mali.arm64.hint(i32)");
        emitRaw("declare void @llvm.mali.arm64.retaa()");
        emitRaw("declare i64 @llvm.mali.arm64.mrs(ptr, i32)");
        emitRaw("declare void @llvm.mali.arm64.dc_zva(i64)");
        emitRaw("declare i64 @llvm.mali.bionic.tlsdesc(i64)");
        std::set<std::string> plt_syms = {"malloc", "free"};
        for (const auto &kv : loader.closure_plts) plt_syms.insert(kv.second);
        for (const auto &sym : plt_syms) {
            emitRaw("declare void @llvm.mali.bionic.plt64." + sym + "(ptr)");
        }
        emitRaw("declare ptr @calloc(i64, i64)");
        emitRaw("declare void @free(ptr)");
        emitRaw("declare i64 @strlen(ptr)");
        emitRaw("declare ptr @strchr(ptr, i32)");
        emitRaw("declare i64 @strcspn(ptr, ptr)");
        emitRaw("declare i32 @strncasecmp(ptr, ptr, i64)");
        emitRaw("declare i32 @strcmp(ptr, ptr)");
        emitRaw("declare i32 @strncmp(ptr, ptr, i64)");
        emitRaw("declare ptr @strncat(ptr, ptr, i64)");
        emitRaw("declare i32 @memcmp(ptr, ptr, i64)");
        emitRaw("declare ptr @__errno_location()");
        emitRaw("declare i64 @bionic_aapcs64_vsnprintf_bridge(i64, i64, i64, i64)");
        emitRaw("declare i64 @bionic_mali_kbase_ioctl_stub(i64, i64, i64)");
        emitRaw("declare i64 @bionic_mali_open_stub(i64, i64)");
        emitRaw("declare i64 @bionic_mali_close_stub(i64)");
        emitRaw("declare i64 @bionic_mali_mmap_stub(i64, i64, i64, i64, i64, i64)");
        emitRaw("declare i64 @bionic_mali_munmap_stub(i64, i64)");
        emitRaw("declare i64 @bionic_mali_read_stub(i64, i64, i64)");
        emitRaw("declare i64 @bionic_mali_poll_stub(i64, i64, i64)");
        emitRaw("declare void @aot_unhandled_plt(ptr, i64)");
        emitRaw("declare i32 @llvm.fshr.i32(i32, i32, i32)");
        emitRaw("declare i64 @llvm.fshr.i64(i64, i64, i64)");
        emitRaw("declare <4 x i32> @llvm.fshr.v4i32(<4 x i32>, <4 x i32>, <4 x i32>)");
        emitRaw("declare i32 @llvm.bitreverse.i32(i32)");
        emitRaw("declare i64 @llvm.bitreverse.i64(i64)");
        emitRaw("declare i32 @llvm.bswap.i32(i32)");
        emitRaw("declare i64 @llvm.bswap.i64(i64)");
        emitRaw("declare <2 x i32> @llvm.bswap.v2i32(<2 x i32>)");
        emitRaw("declare <4 x i32> @llvm.bswap.v4i32(<4 x i32>)");
        emitRaw("declare i32 @llvm.ctlz.i32(i32, i1 immarg)");
        emitRaw("declare i64 @llvm.ctlz.i64(i64, i1 immarg)");
        emitRaw("declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg)");
        emitRaw("declare void @llvm.memmove.p0.p0.i64(ptr nocapture writeonly, ptr nocapture readonly, i64, i1 immarg)");
        emitRaw("declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)");
        emitRaw("declare <8 x i8> @llvm.ctpop.v8i8(<8 x i8>)");
        emitRaw("declare i8 @llvm.vector.reduce.add.v8i8(<8 x i8>)");
        emitRaw("declare i8 @llvm.vector.reduce.add.v16i8(<16 x i8>)");
        emitRaw("declare i16 @llvm.vector.reduce.add.v4i16(<4 x i16>)");
        emitRaw("declare i16 @llvm.vector.reduce.add.v8i16(<8 x i16>)");
        emitRaw("declare i32 @llvm.vector.reduce.add.v2i32(<2 x i32>)");
        emitRaw("declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)");
        emitRaw("declare i8 @llvm.vector.reduce.umax.v8i8(<8 x i8>)");
        emitRaw("declare i8 @llvm.vector.reduce.umax.v16i8(<16 x i8>)");
        emitRaw("declare i16 @llvm.vector.reduce.umax.v4i16(<4 x i16>)");
        emitRaw("declare i16 @llvm.vector.reduce.umax.v8i16(<8 x i16>)");
        emitRaw("declare i32 @llvm.vector.reduce.umax.v2i32(<2 x i32>)");
        emitRaw("declare i32 @llvm.vector.reduce.umax.v4i32(<4 x i32>)\n");

        for (uint64_t faddr : loader.closure_funcs) {
            liftFunction(faddr);
        }

        // Export version-independent entry wrappers & auto-detected offsets
        emitRaw("define i64 @aot_get_ctx_malloc_offset() #0 {");
        emit("ret i64 " + std::to_string(loader.ctx_malloc_offset));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_vk_hmi_addr() #0 {");
        emit("ret i64 " + std::to_string(loader.hmi_rva ? (IMAGE_BASE + loader.hmi_rva) : 0ULL));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_kbase_page_shift_addr() #0 {");
        emit("ret i64 " + std::to_string(loader.kbase_page_shift_rva ? (IMAGE_BASE + loader.kbase_page_shift_rva) : 0ULL));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_vk_dispatch_table_rva() #0 {");
        emit("ret i64 " + std::to_string(loader.vk_dispatch_table_rva));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_vk_dispatch_table_count() #0 {");
        emit("ret i64 " + std::to_string(loader.vk_dispatch_table_count));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_vk_dispatch_table_stride() #0 {");
        emit("ret i64 " + std::to_string(loader.vk_dispatch_table_stride));
        emitRaw("}\n");

        emitRaw("define i64 @aot_get_lifted_vk_entrypoint_count() #0 {");
        emit("ret i64 " + std::to_string(loader.vk_dispatch_entries.size()));
        emitRaw("}\n");

        std::set<std::string> emitted_aot_wrappers;
        for (const auto &vk_ent : loader.vk_dispatch_entries) {
            uint64_t rva = loader.findFuncStart(vk_ent.second);
            if (rva == 0) rva = vk_ent.second;
            if (func_set.count(rva) && emitted_aot_wrappers.insert(vk_ent.first).second) {
                emitRaw(std::string("define void @aot_") + vk_ent.first + "(ptr %cpu) #0 {");
                emit("call void @aot_sub_" + hex8(rva) + "(ptr %cpu)");
                emit("ret void");
                emitRaw("}\n");
            }
        }

        for (const char *sym : {
                 "cmpbe_v2_deserialize_MBS2_to_C",
                 "cmpbe_v2_init_options",
                 "cmpbe_v2_set_option_value",
                 "cmpbe_v2_free_compilation_result",
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
            if (loader.addr_by_name.count(sym) && !emitted_aot_wrappers.count(sym)) {
                uint64_t rva = loader.findFuncStart(loader.addr_by_name.at(sym));
                if (func_set.count(rva) && emitted_aot_wrappers.insert(sym).second) {
                    emitRaw(std::string("define void @aot_") + sym + "(ptr %cpu) #0 {");
                    emit("call void @aot_sub_" + hex8(rva) + "(ptr %cpu)");
                    emit("ret void");
                    emitRaw("}\n");
                }
            }
        }

        // Emit @aot_indirect_dispatch
        emitRaw("define void @aot_indirect_dispatch(ptr %cpu, i64 %target_va) #0 {");
        emitRaw("entry:");
        emit("  %is_null = icmp eq i64 %target_va, 0");
        emit("  br i1 %is_null, label %handle_null, label %check_malloc");
        emitRaw("handle_null:");
        emit("  ret void");
        emitRaw("check_malloc:");
        emit("  %is_malloc = icmp eq i64 %target_va, " + std::to_string(HLE_MALLOC_MAGIC));
        emit("  br i1 %is_malloc, label %handle_malloc, label %check_free");
        emitRaw("handle_malloc:");
        emit("  call void @llvm.mali.bionic.plt64.malloc(ptr %cpu)");
        emit("  ret void");
        emitRaw("check_free:");
        emit("  %is_free = icmp eq i64 %target_va, " + std::to_string(HLE_FREE_MAGIC));
        emit("  br i1 %is_free, label %handle_free, label %check_vk_alloc");
        emitRaw("handle_free:");
        emit("  call void @llvm.mali.bionic.plt64.free(ptr %cpu)");
        emit("  ret void");
        emitRaw("check_vk_alloc:");
        emit("  %is_vk_alloc = icmp eq i64 %target_va, " + std::to_string(HLE_VK_ALLOC_MAGIC));
        emit("  br i1 %is_vk_alloc, label %handle_vk_alloc, label %check_vk_free");
        emitRaw("handle_vk_alloc:");
        emit("  %vk_x1_ptr = getelementptr inbounds i8, ptr %cpu, i64 8");
        emit("  %vk_sz = load i64, ptr %vk_x1_ptr, align 8");
        emit("  %vk_mem = call ptr @calloc(i64 %vk_sz, i64 1)");
        emit("  %vk_mem_i = ptrtoint ptr %vk_mem to i64");
        emit("  %vk_x0_ptr = getelementptr inbounds i8, ptr %cpu, i64 0");
        emit("  store i64 %vk_mem_i, ptr %vk_x0_ptr, align 8");
        emit("  ret void");
        emitRaw("check_vk_free:");
        emit("  %is_vk_free = icmp eq i64 %target_va, " + std::to_string(HLE_VK_FREE_MAGIC));
        emit("  br i1 %is_vk_free, label %handle_vk_free, label %check_tlsdesc");
        emitRaw("handle_vk_free:");
        emit("  %vkf_x1_ptr = getelementptr inbounds i8, ptr %cpu, i64 8");
        emit("  %vkf_addr = load i64, ptr %vkf_x1_ptr, align 8");
        emit("  %vkf_ptr = inttoptr i64 %vkf_addr to ptr");
        emit("  call void @free(ptr %vkf_ptr)");
        emit("  ret void");
        emitRaw("check_tlsdesc:");
        emit("  %is_tlsdesc = icmp eq i64 %target_va, " + std::to_string(HLE_TLSDESC_MAGIC));
        emit("  br i1 %is_tlsdesc, label %handle_tlsdesc, label %dispatch_switch");
        emitRaw("handle_tlsdesc:");
        emit("  %x0_ptr = getelementptr inbounds i8, ptr %cpu, i64 0");
        emit("  %desc_addr = load i64, ptr %x0_ptr, align 8");
        emit("  %tls_off = call i64 @llvm.mali.bionic.tlsdesc(i64 %desc_addr)");
        emit("  store i64 %tls_off, ptr %x0_ptr, align 8");
        emit("  ret void");
        emitRaw("dispatch_switch:");
        std::ostringstream cases;
        for (uint64_t faddr : loader.closure_funcs) {
            cases << "i64 " << (IMAGE_BASE + faddr) << ", label %call_" << hex8(faddr) << " ";
        }
        emit("  switch i64 %target_va, label %fallback [ " + cases.str() + "]");
        for (uint64_t faddr : loader.closure_funcs) {
            emitRaw("call_" + hex8(faddr) + ":");
            emit("  call void @aot_sub_" + hex8(faddr) + "(ptr %cpu)");
            emit("  ret void");
        }
        emitRaw("fallback:");
        emit("  call void @aot_unhandled_plt(ptr %cpu, i64 %target_va)");
        emit("  ret void");
        emitRaw("}\n");

        emitRaw("attributes #0 = { nounwind \"frame-pointer\"=\"none\" }");
        return out.str();
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
                    } else if (sym == "malloc") {
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
                    } else if (sym == "free") {
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
                    } else if (sym == "strchr") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef ptr = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef ch = LLVMBuildTrunc(B, x1, i32Ty, "");
                        LLVMValueRef strchrFn = getOrCreateFunction(M, "strchr", ptrTy, {ptrTy, i32Ty});
                        LLVMTypeRef schParams[2] = {ptrTy, i32Ty};
                        LLVMTypeRef strchrTy = LLVMFunctionType(ptrTy, schParams, 2, 0);
                        LLVMValueRef args[2] = {ptr, ch};
                        LLVMValueRef resPtr = LLVMBuildCall2(B, strchrTy, strchrFn, args, 2, "");
                        storeReg64(cpu, 0, LLVMBuildPtrToInt(B, resPtr, i64Ty, ""));
                    } else if (sym == "strcspn") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "strcspn", i64Ty, {ptrTy, ptrTy});
                        LLVMTypeRef pTys[2] = {ptrTy, ptrTy};
                        LLVMTypeRef fnTy = LLVMFunctionType(i64Ty, pTys, 2, 0);
                        LLVMValueRef args[2] = {p0, p1};
                        LLVMValueRef res = LLVMBuildCall2(B, fnTy, fnIntr, args, 2, "");
                        storeReg64(cpu, 0, res);
                    } else if (sym == "strncasecmp" || sym == "memcmp" || sym == "strncmp") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, sym.c_str(), i32Ty, {ptrTy, ptrTy, i64Ty});
                        LLVMTypeRef pTys[3] = {ptrTy, ptrTy, i64Ty};
                        LLVMTypeRef fnTy = LLVMFunctionType(i32Ty, pTys, 3, 0);
                        LLVMValueRef args[3] = {p0, p1, x2};
                        LLVMValueRef res32 = LLVMBuildCall2(B, fnTy, fnIntr, args, 3, "");
                        storeReg64(cpu, 0, LLVMBuildZExt(B, res32, i64Ty, ""));
                    } else if (sym == "strncat") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "strncat", ptrTy, {ptrTy, ptrTy, i64Ty});
                        LLVMTypeRef pTys[3] = {ptrTy, ptrTy, i64Ty};
                        LLVMTypeRef fnTy = LLVMFunctionType(ptrTy, pTys, 3, 0);
                        LLVMValueRef args[3] = {p0, p1, x2};
                        LLVMValueRef resPtr = LLVMBuildCall2(B, fnTy, fnIntr, args, 3, "");
                        storeReg64(cpu, 0, LLVMBuildPtrToInt(B, resPtr, i64Ty, ""));
                    } else if (sym == "strcmp") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, x0, ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, x1, ptrTy, "");
                        LLVMValueRef fnIntr = getOrCreateFunction(M, "strcmp", i32Ty, {ptrTy, ptrTy});
                        LLVMTypeRef pTys[2] = {ptrTy, ptrTy};
                        LLVMTypeRef fnTy = LLVMFunctionType(i32Ty, pTys, 2, 0);
                        LLVMValueRef args[2] = {p0, p1};
                        LLVMValueRef res32 = LLVMBuildCall2(B, fnTy, fnIntr, args, 2, "");
                        storeReg64(cpu, 0, LLVMBuildZExt(B, res32, i64Ty, ""));
                    } else if (sym == "__errno") {
                        LLVMValueRef errnoFn = getOrCreateFunction(M, "__errno_location", ptrTy, {});
                        LLVMTypeRef errnoTy = LLVMFunctionType(ptrTy, nullptr, 0, 0);
                        LLVMValueRef resPtr = LLVMBuildCall2(B, errnoTy, errnoFn, nullptr, 0, "");
                        storeReg64(cpu, 0, LLVMBuildPtrToInt(B, resPtr, i64Ty, ""));
                    } else if (sym == "vsnprintf" || sym == "__vsnprintf_chk") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef x3 = loadReg64(cpu, 3);
                        LLVMValueRef bridgeFn = getOrCreateFunction(M, "bionic_aapcs64_vsnprintf_bridge", i64Ty, {i64Ty, i64Ty, i64Ty, i64Ty});
                        LLVMTypeRef vsnParams[4] = {i64Ty, i64Ty, i64Ty, i64Ty};
                        LLVMTypeRef bridgeTy = LLVMFunctionType(i64Ty, vsnParams, 4, 0);
                        LLVMValueRef args[4] = {x0, x1, x2, x3};
                        LLVMValueRef res = LLVMBuildCall2(B, bridgeTy, bridgeFn, args, 4, "");
                        storeReg64(cpu, 0, res);
                    } else if (sym == "ioctl") {
                        // Mali /dev/mali0 (kbase) Kernel UAPI ioctl shim
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef x2 = loadReg64(cpu, 2);
                        LLVMValueRef ioctlFn = getOrCreateFunction(M, "bionic_mali_kbase_ioctl_stub", i64Ty, {i64Ty, i64Ty, i64Ty});
                        LLVMTypeRef ioctlParams[3] = {i64Ty, i64Ty, i64Ty};
                        LLVMTypeRef ioctlTy = LLVMFunctionType(i64Ty, ioctlParams, 3, 0);
                        LLVMValueRef args[3] = {x0, x1, x2};
                        LLVMValueRef res = LLVMBuildCall2(B, ioctlTy, ioctlFn, args, 3, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "open" || sym == "__open_2" || sym == "open64") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef x1 = loadReg64(cpu, 1);
                        LLVMValueRef openFn = getOrCreateFunction(M, "bionic_mali_open_stub", i64Ty, {i64Ty, i64Ty});
                        LLVMTypeRef openParams[2] = {i64Ty, i64Ty};
                        LLVMTypeRef openTy = LLVMFunctionType(i64Ty, openParams, 2, 0);
                        LLVMValueRef args[2] = {x0, x1};
                        LLVMValueRef res = LLVMBuildCall2(B, openTy, openFn, args, 2, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "close") {
                        LLVMValueRef x0 = loadReg64(cpu, 0);
                        LLVMValueRef closeFn = getOrCreateFunction(M, "bionic_mali_close_stub", i64Ty, {i64Ty});
                        LLVMTypeRef closeTy = LLVMFunctionType(i64Ty, &i64Ty, 1, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, closeTy, closeFn, &x0, 1, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "mmap" || sym == "mmap64") {
                        LLVMValueRef args[6] = {
                            loadReg64(cpu, 0), loadReg64(cpu, 1), loadReg64(cpu, 2),
                            loadReg64(cpu, 3), loadReg64(cpu, 4), loadReg64(cpu, 5)
                        };
                        LLVMValueRef mmapFn = getOrCreateFunction(M, "bionic_mali_mmap_stub", i64Ty, {i64Ty, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty});
                        LLVMTypeRef mmapParams[6] = {i64Ty, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty};
                        LLVMTypeRef mmapTy = LLVMFunctionType(i64Ty, mmapParams, 6, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, mmapTy, mmapFn, args, 6, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "munmap") {
                        LLVMValueRef args[2] = {loadReg64(cpu, 0), loadReg64(cpu, 1)};
                        LLVMValueRef munmapFn = getOrCreateFunction(M, "bionic_mali_munmap_stub", i64Ty, {i64Ty, i64Ty});
                        LLVMTypeRef munmapParams[2] = {i64Ty, i64Ty};
                        LLVMTypeRef munmapTy = LLVMFunctionType(i64Ty, munmapParams, 2, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, munmapTy, munmapFn, args, 2, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "read" || sym == "__read_chk") {
                        LLVMValueRef args[3] = {loadReg64(cpu, 0), loadReg64(cpu, 1), loadReg64(cpu, 2)};
                        LLVMValueRef readFn = getOrCreateFunction(M, "bionic_mali_read_stub", i64Ty, {i64Ty, i64Ty, i64Ty});
                        LLVMTypeRef readParams[3] = {i64Ty, i64Ty, i64Ty};
                        LLVMTypeRef readTy = LLVMFunctionType(i64Ty, readParams, 3, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, readTy, readFn, args, 3, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "poll" || sym == "__poll_chk" || sym == "ppoll" || sym == "__ppoll_chk") {
                        LLVMValueRef args[3] = {loadReg64(cpu, 0), loadReg64(cpu, 1), loadReg64(cpu, 2)};
                        LLVMValueRef pollFn = getOrCreateFunction(M, "bionic_mali_poll_stub", i64Ty, {i64Ty, i64Ty, i64Ty});
                        LLVMTypeRef pollParams[3] = {i64Ty, i64Ty, i64Ty};
                        LLVMTypeRef pollTy = LLVMFunctionType(i64Ty, pollParams, 3, 0);
                        LLVMValueRef res = LLVMBuildCall2(B, pollTy, pollFn, args, 3, "");
                        storeReg64(cpu, 0, res);
                        stats.kbase_uapi_lowered++;
                    } else if (sym == "pthread_self") {
                        storeReg64(cpu, 0, LLVMConstInt(i64Ty, 1, 0));
                        stats.external_stubs_lowered++;
                    } else if (sym == "vkCreateXcbSurfaceKHR" || sym == "vkCreateXlibSurfaceKHR" ||
                               sym == "vkCreateWaylandSurfaceKHR") {
                        std::string hookName = "__mali_hook_" + sym;
                        LLVMValueRef hookFn = getOrCreateFunction(M, hookName.c_str(), i32Ty, {ptrTy, ptrTy, ptrTy, ptrTy});
                        LLVMTypeRef hSig[4] = {ptrTy, ptrTy, ptrTy, ptrTy};
                        LLVMTypeRef hTy = LLVMFunctionType(i32Ty, hSig, 4, 0);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, loadReg64(cpu, 0), ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, loadReg64(cpu, 1), ptrTy, "");
                        LLVMValueRef p2 = LLVMBuildIntToPtr(B, loadReg64(cpu, 2), ptrTy, "");
                        LLVMValueRef p3 = LLVMBuildIntToPtr(B, loadReg64(cpu, 3), ptrTy, "");
                        LLVMValueRef args[4] = {p0, p1, p2, p3};
                        LLVMValueRef res32 = LLVMBuildCall2(B, hTy, hookFn, args, 4, "");
                        storeReg64(cpu, 0, LLVMBuildZExt(B, res32, i64Ty, ""));
                    } else if (sym == "vkQueuePresentKHR") {
                        LLVMValueRef hookFn = getOrCreateFunction(M, "__mali_hook_vkQueuePresentKHR", i32Ty, {ptrTy, ptrTy});
                        LLVMTypeRef hSig[2] = {ptrTy, ptrTy};
                        LLVMTypeRef hTy = LLVMFunctionType(i32Ty, hSig, 2, 0);
                        LLVMValueRef p0 = LLVMBuildIntToPtr(B, loadReg64(cpu, 0), ptrTy, "");
                        LLVMValueRef p1 = LLVMBuildIntToPtr(B, loadReg64(cpu, 1), ptrTy, "");
                        LLVMValueRef args[2] = {p0, p1};
                        LLVMValueRef res32 = LLVMBuildCall2(B, hTy, hookFn, args, 2, "");
                        storeReg64(cpu, 0, LLVMBuildZExt(B, res32, i64Ty, ""));
                    } else if (sym == "__stack_chk_fail" || sym == "abort") {
                        LLVMValueRef abortFn = getOrCreateFunction(M, "__mali_stack_chk_fail_diag", voidTy, {ptrTy});
                        LLVMTypeRef dTy = LLVMFunctionType(voidTy, &ptrTy, 1, 0);
                        LLVMBuildCall2(B, dTy, abortFn, &cpu, 1, "");
                    } else {
                        // Unavailable Android / MediaTek vendor library hooks & single-threaded stubs:
                        // __android_log_print, __android_log_write, property_get*, gpud*,
                        // pthread_mutex_lock/unlock, getenv, prctl, clock_gettime, fprintf, fwrite, fflush, usleep
                        storeReg64(cpu, 0, LLVMConstInt(i64Ty, 0, 0));
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
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "x86-64-v3", "", 3, 2, 0);

    if (runO2) {
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMErrorRef err = LLVMRunPasses(M, "default<O1>", tm, opts);
        if (err) {
            char *emsg = LLVMGetErrorMessage(err);
            fprintf(stderr, "LLVMRunPasses(default<O2>) failed: %s\n", emsg);
            LLVMDisposeErrorMessage(emsg);
            return 1;
        }
        LLVMDisposePassBuilderOptions(opts);
        printf("[LLVM Passes] Offloaded SROA, mem2reg, InstCombine, GVN, SimplifyCFG, ADCE, DSE via LLVMRunPasses(default<O2>)\n");
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

int main(int argc, char **argv) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();

    // Mode 1: Full C++ 6-Pass Pipeline directly from 64-bit libGLES_mali.so
    // Usage: mali_llvm_pass_driver --lift-so <libGLES_mali.so> <out_dir>
    if (argc >= 4 && std::strcmp(argv[1], "--lift-so") == 0) {
        std::string soPath = argv[2];
        std::string outDir = argv[3];

        printf("[LLVM Pass 1] ELF64LoaderAndCallGraphPass: %s\n", soPath.c_str());
        ELF64LoaderAndCallGraphPass loader;
        if (!loader.run(soPath)) {
            fprintf(stderr, "Failed to load 64-bit ELF64 AArch64 binary: %s\n", soPath.c_str());
            return 1;
        }
        printf("  Driver version      : %s (build-id: %s)\n", loader.driver_version.c_str(), loader.build_id_short.c_str());
        printf("  Closure functions   : %zu internal functions, %zu PLT imports\n", loader.closure_funcs.size(), loader.closure_plts.size());
        printf("  Auto ctx malloc off : 0x%02" PRIx64 " | Vulkan HMI RVA: 0x%08" PRIx64 "\n", loader.ctx_malloc_offset, loader.hmi_rva);

        printf("[LLVM Pass 2 & 3] CFGRecoveryAndPicJumpTablePass + AArch64ToLLVMIRLiftingPass...\n");
        AArch64ToLLVMIRLiftingPass lifter(loader);
        std::string irText = lifter.generateModuleText();
        printf("  Lifted instructions : %d (%d PIC jump tables, %d SIMD ops, %d Crypto SHA-1 ops, %d Atomics)\n",
               lifter.stats.total_instructions, lifter.stats.jump_tables_lowered, lifter.stats.simd_ops_lifted,
               lifter.stats.crypto_sha1_lifted, lifter.stats.atomic_ops_lifted);

        std::string unoptPath = outDir + "/mali_mbs2_unopt.ll";
        std::string optPath   = outDir + "/mali_mbs2_opt.ll";
        std::string objPath   = outDir + "/mali_mbs2_aot.o";
        {
            std::ofstream out(unoptPath);
            out << irText;
        }
        printf("  Wrote unoptimized IR: %s (%zu bytes)\n", unoptPath.c_str(), irText.size());

        LLVMContextRef C = LLVMContextCreate();
        LLVMMemoryBufferRef memBuf = LLVMCreateMemoryBufferWithMemoryRangeCopy(irText.data(), irText.size(), "mali_mbs2_unopt.ll");
        LLVMModuleRef M = nullptr;
        char *errMsg = nullptr;
        if (LLVMParseIRInContext(C, memBuf, &M, &errMsg) != 0) {
            fprintf(stderr, "LLVM IR Parse Error: %s\n", errMsg ? errMsg : "unknown");
            return 1;
        }
        int rc = runLoweringAndCodegenOnModule(M, objPath.c_str(), false, true, optPath.c_str());
        LLVMDisposeModule(M);
        LLVMContextDispose(C);
        return rc;
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
