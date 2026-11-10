// Unified Horizon2LLVM Kernel Module (.ko ET_REL) Lifter & Lowering Pipeline
// (src/aot/horizon2llvm/ko_lifter.cpp)
//
// Implements the KernelModuleKo mode of the unified C++ `horizon2llvm` AArch64-to-LLVM-IR
// lifting, lowering, and native recompilation framework (per Doc 48 comment
// doco.C5ZRkpgUyjGpcLnE-7ASWg / doco.TYY1k8IskfrqydKDlRIIvg and Doc 50):
//   1. ELF64 ET_REL section/symbol/relocation loader (.symtab, .strtab, .text, .rodata, .rela.text)
//   2. Dynamic libLLVM MC disassembler bridge (matching src/verifier/aarch64_mc_lifter.py)
//   3. Reuses the unified `ultrs::llvm_ir::Module` and `ultrs::passes::ModulePassManager`
//      lowering pipeline (`PassPipelineConfig::forKernelKo()`):
//        - `AArch64SysBarrierLoweringPass(LoweringMode::KernelModuleKo)`
//        - `GuestStackAndAlignmentPass`
//        - `AArch64IdiomDeconstructPass`
//        - `JumpTableLoweringPass`
//        - `NZCVPredicateFusionPass`
//        - `NZCVDeadFlagElimPass`
//        - `TailCallDevirtualizationPass`
//        - `AAPCS64ABIContractPass`
//        - `CpuStatePromotionPass`
//      while omitting Bionic-specific user-space TLS (`TPIDR_EL0` slot hooks) and PLT/GOT passes.
//   4. Lowers all 68 target kernel functions across `mali_kbase.ko` (MT6878 r44p1) and
//      `mali_kbase_husky.ko` (Pixel 8 Pro Tensor G3 r54p1/r54p3) directly into genuine SSA
//      LLVM IR (`src/emulator/lifted_kbase_ko.ll`), with zero lifted `.ko` functions in C,
//      and compiles `lifted_kbase_ko.ll` to native ELF64 PIC `.o` object files via `libLLVM`.

#include "ko_lifter.hpp"
#include "llvm_backend.hpp"
#include "llvm_ir.hpp"
#include "passes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace ultrs::ko_lifter {

namespace {

static uint16_t read_u16_le(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t read_u32_le(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static int32_t read_s32_le(const uint8_t* p) {
    int32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static int64_t read_s64_le(const uint8_t* p) {
    int64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

struct LLVMDisasmDynamic {
    void* handle = nullptr;
    using CreateFn = void* (*)(const char*, void*, int, void*, void*);
    using DisasmFn = size_t (*)(void*, uint8_t*, uint64_t, uint64_t, char*, size_t);
    using DisposeFn = void (*)(void*);
    using InitFn = void (*)();

    CreateFn create_disasm = nullptr;
    DisasmFn disasm_inst = nullptr;
    DisposeFn dispose_disasm = nullptr;
    void* ctx = nullptr;

    bool init(std::string& err) {
        const char* cands[] = {
            std::getenv("MALI_HOST_LIBLLVM"),
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
            handle = dlopen(c, RTLD_LAZY | RTLD_GLOBAL);
            if (handle) break;
        }
        if (!handle) {
            err = "Failed to dlopen libLLVM shared library for AArch64 MC disassembler";
            return false;
        }
        const char* init_syms[] = {
            "LLVMInitializeAArch64TargetInfo",
            "LLVMInitializeAArch64TargetMC",
            "LLVMInitializeAArch64Disassembler",
        };
        for (const char* s : init_syms) {
            auto fn = reinterpret_cast<InitFn>(dlsym(handle, s));
            if (fn) fn();
        }
        create_disasm = reinterpret_cast<CreateFn>(dlsym(handle, "LLVMCreateDisasm"));
        disasm_inst = reinterpret_cast<DisasmFn>(dlsym(handle, "LLVMDisasmInstruction"));
        dispose_disasm = reinterpret_cast<DisposeFn>(dlsym(handle, "LLVMDisasmDispose"));
        if (!create_disasm || !disasm_inst || !dispose_disasm) {
            err = "Missing LLVMCreateDisasm/LLVMDisasmInstruction in libLLVM";
            return false;
        }
        ctx = create_disasm("aarch64-linux-gnu", nullptr, 0, nullptr, nullptr);
        if (!ctx) {
            err = "LLVMCreateDisasm(\"aarch64-linux-gnu\") returned NULL";
            return false;
        }
        return true;
    }

    ~LLVMDisasmDynamic() {
        if (ctx && dispose_disasm) dispose_disasm(ctx);
    }

    std::vector<DisasmInsn> disasm_range(const std::vector<uint8_t>& text, uint64_t start, uint64_t end) {
        std::vector<DisasmInsn> out;
        char buf[256];
        for (uint64_t pc = start; pc + 4 <= end && pc + 4 <= text.size(); pc += 4) {
            uint8_t raw[4] = {text[pc], text[pc + 1], text[pc + 2], text[pc + 3]};
            uint32_t word = read_u32_le(raw);
            std::memset(buf, 0, sizeof(buf));
            size_t sz = disasm_inst(ctx, raw, 4, pc, buf, sizeof(buf) - 1);
            DisasmInsn d;
            d.addr = pc;
            d.word = word;
            if (sz == 0) {
                char hexbuf[32];
                std::snprintf(hexbuf, sizeof(hexbuf), "0x%08x", word);
                d.mnemonic = ".word";
                d.operands = hexbuf;
                d.raw_text = std::string(".word ") + hexbuf;
            } else {
                std::string s(buf);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                d.raw_text = s;
                size_t sp = 0;
                while (sp < s.size() && !std::isspace(static_cast<unsigned char>(s[sp]))) ++sp;
                d.mnemonic = s.substr(0, sp);
                std::string rest = (sp < s.size()) ? s.substr(sp) : "";
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
                d.operands = rest;
            }
            out.push_back(std::move(d));
        }
        return out;
    }
};

static const std::vector<std::string> kTargetFunctions = {
    "kbase_check_flags",
    "mali_kbase_supports_cap",
    "kbase_check_alloc_flags",
    "kbase_check_import_flags",
    "kbase_check_alloc_sizes",
    "kbase_gpuprops_parse_gpu_id",
    "kbase_gpuprops_get_curr_config_props",
    "kbase_gpuprops_populate_user_buffer",
    "kbase_file_create_kctx",
    "kbase_file_delete",
    "kbase_api_mem_alloc_ex",
    "kbase_api_mem_alloc",
    "kbase_api_mem_alias",
    "kbase_api_mem_import",
    "kbase_api_get_cpu_gpu_timeinfo",
    "kbase_ioctl_cs_get_glb_iface",
    "kbase_ioctl_read_user_page",
    "kbase_mem_flags_change",
    "kbase_csf_queue_register",
    "kbase_csf_queue_register_ex",
    "kbase_csf_alloc_command_stream_user_pages",
    "kbase_csf_queue_bind",
    "kbase_csf_queue_group_create",
    "kbasep_cs_queue_group_create",
    "kbasep_cs_queue_group_create_1_6",
    "kbasep_cs_queue_group_create_1_18",
    "kbasep_cs_queue_group_create_1_35",
    "kbasep_cs_tiler_heap_init",
    "kbasep_cs_tiler_heap_init_1_13",
    "kbase_open",
    "kbase_release",
    "kbase_mmap",
    "kbase_poll",
    "kbase_kfile_ioctl",
    "kbase_ioctl",
};

static const std::set<std::string> kDirectLiftedCallees = {
    "mali_kbase_supports_cap",
    "kbase_file_create_kctx",
    "kbase_file_delete",
    "kbase_api_mem_alloc",
    "kbase_api_mem_alloc_ex",
    "kbase_api_mem_alias",
    "kbase_api_mem_import",
    "kbase_api_get_cpu_gpu_timeinfo",
    "kbase_ioctl_cs_get_glb_iface",
    "kbase_ioctl_read_user_page",
    "kbasep_cs_queue_group_create",
    "kbasep_cs_queue_group_create_1_6",
    "kbasep_cs_queue_group_create_1_18",
    "kbasep_cs_queue_group_create_1_35",
    "kbase_csf_queue_group_create",
    "kbasep_cs_tiler_heap_init",
    "kbasep_cs_tiler_heap_init_1_13",
    "kbase_csf_queue_register",
    "kbase_csf_queue_register_ex",
    "kbase_kfile_ioctl",
};

struct RodataTableDesc {
    std::string fallback_symbol;
    uint32_t offset;
    uint32_t size;
};

static const std::map<std::pair<std::string, uint64_t>, RodataTableDesc> kRodataTableMap = {
    {{"r44p1", 67800}, {"g_lifted_r44p1_kbase_caps_table", 67800, 16}},
    {{"r44p1", 65408}, {"g_lifted_r44p1_gpu_property_mapping", 65408, 1992}},
    {{"r44p1", 61524}, {"g_lifted_r44p1_gpuprops_jtab", 61524, 32}},
    {{"r44p1", 62284}, {"g_lifted_r44p1_ioctl_jtab1", 62284, 168}},
    {{"r44p1", 62112}, {"g_lifted_r44p1_ioctl_jtab2", 62112, 176}},
    {{"r44p1", 61968}, {"g_lifted_r44p1_ioctl_jtab3", 61968, 148}},
    {{"husky_r54p1", 89160}, {"g_lifted_husky_r54p1_kbase_caps_table", 89160, 72}},
    {{"husky_r54p1", 78896}, {"g_lifted_husky_r54p1_gpu_property_mapping", 78896, 2040}},
    {{"husky_r54p1", 78776}, {"g_lifted_husky_r54p1_gpuprops_jtab", 78776, 32}},
    {{"husky_r54p1", 88900}, {"g_lifted_husky_r54p1_ioctl_jtab1", 88900, 200}},
    {{"husky_r54p1", 88584}, {"g_lifted_husky_r54p1_ioctl_jtab2", 88584, 148}},
    {{"husky_r54p1", 88728}, {"g_lifted_husky_r54p1_ioctl_jtab3", 88728, 176}},
    {{"husky_r54p1", 88456}, {"g_lifted_husky_r54p1_ioctl_jtab4", 88456, 132}},
};

static std::string trim_str(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static int64_t parse_imm(std::string s) {
    s = trim_str(s);
    if (!s.empty() && s[0] == '#') s = trim_str(s.substr(1));
    if (s.empty()) return 0;
    if (s[0] == '-') {
        uint64_t mag = std::stoull(s.substr(1), nullptr, 0);
        return -static_cast<int64_t>(mag);
    }
    uint64_t u = std::stoull(s, nullptr, 0);
    return static_cast<int64_t>(u);
}

static std::vector<std::string> split_operands(const std::string& ops_str) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char ch : ops_str) {
        if (ch == '[') {
            depth++;
            cur.push_back(ch);
        } else if (ch == ']') {
            depth--;
            cur.push_back(ch);
        } else if (ch == ',' && depth == 0) {
            out.push_back(trim_str(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!trim_str(cur).empty()) out.push_back(trim_str(cur));
    return out;
}

static std::string hex6(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%06llx", static_cast<unsigned long long>(v));
    return buf;
}

static std::string bb_label(uint64_t pc) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "bb_%06llx", static_cast<unsigned long long>(pc));
    return buf;
}

struct ModuleStringPool {
    std::map<std::string, std::string> pool;
    std::vector<std::pair<std::string, std::string>> ordered;

    std::string intern(const std::string& s) {
        auto it = pool.find(s);
        if (it != pool.end()) return it->second;
        std::string sym = "@.ko_str." + std::to_string(ordered.size());
        pool[s] = sym;
        ordered.emplace_back(sym, s);
        return sym;
    }

    void emit_globals(std::ostream& os) const {
        for (const auto& [sym, val] : ordered) {
            os << sym << " = private unnamed_addr constant [" << (val.size() + 1) << " x i8] c\"";
            for (unsigned char c : val) {
                if (c >= 32 && c < 127 && c != '"' && c != '\\') {
                    os << static_cast<char>(c);
                } else {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\%02X", static_cast<unsigned>(c));
                    os << b;
                }
            }
            os << "\\00\", align 1\n";
        }
    }
};

// Exact byte offsets in `struct LiftedKbaseCPU`:
//   x[0..30]:            0 .. 240 (idx * 8)
//   sp:                  248
//   x18:                 256
//   n, z, c, v:          264, 265, 266, 267 (uint8_t each)
//   sp_el0:              272
//   daif:                280
//   ttbr0_el1:           288
//   ttbr1_el1:           296
//   totalram_pages:      304
//   kmalloc_cache_dummy: 312
//   cpu_hwcaps:          320
//   dummy_global:        328
//   dev_list_head[2]:    336
//   insns_executed:      352
//   calls_executed:      360

enum LiftedKoProfileId : uint32_t {
    KO_PROFILE_R44P1       = 0,
    KO_PROFILE_HUSKY_R54P1 = 1,
};

enum LiftedKernelShimId : uint32_t {
    KO_SHIM_UNKNOWN                         = 0,
    KO_SHIM_INDIRECT_BLR                    = 1,
    KO_SHIM_KMALLOC                         = 2,
    KO_SHIM_KMALLOC_TRACE                   = 3,
    KO_SHIM_KFREE                           = 4,
    KO_SHIM_MEMSET                          = 5,
    KO_SHIM_MEMCPY                          = 6,
    KO_SHIM_COPY_FROM_USER                  = 7,
    KO_SHIM_COPY_TO_USER                    = 8,
    KO_SHIM_CHECK_OBJECT_SIZE               = 9,
    KO_SHIM_SW_HWEIGHT64                    = 10,
    KO_SHIM_KBASE_HAS_EXEC_VA_ZONE          = 11,
    KO_SHIM_SCNPRINTF                       = 12,
    KO_SHIM_KBASE_DEVICE_GET_LIST           = 13,
    KO_SHIM_KBASE_CREATE_CONTEXT            = 14,
    KO_SHIM_KBASE_DESTROY_CONTEXT           = 15,
    KO_SHIM_KBASE_MEM_POOL_ALLOC_PAGES      = 16,
    KO_SHIM_KERNEL_MAP_USER_IO_PAGES        = 17,
    KO_SHIM_KBASE_MEM_ALLOC                 = 18,
    KO_SHIM_KBASE_MEM_ALIAS                 = 19,
    KO_SHIM_KBASE_CSF_FIRMWARE_GET_GLB_IFACE = 20,
    KO_SHIM_CSF_QUEUE_REGISTER_INTERNAL     = 21,
    KO_SHIM_CREATE_QUEUE_GROUP              = 22,
    KO_SHIM_KBASE_CSF_TILER_HEAP_INIT       = 23,
    KO_SHIM_NOOP_EXTERNAL                   = 24,
};

static uint32_t resolve_profile_id(const std::string& mod_label) {
    return (mod_label == "husky_r54p1") ? KO_PROFILE_HUSKY_R54P1 : KO_PROFILE_R44P1;
}

static uint32_t resolve_shim_id(const std::string& callee) {
    if (callee == "__indirect_blr") return KO_SHIM_INDIRECT_BLR;
    if (callee == "__kmalloc" || callee == "vzalloc") return KO_SHIM_KMALLOC;
    if (callee == "kmalloc_trace") return KO_SHIM_KMALLOC_TRACE;
    if (callee == "kfree" || callee == "vfree") return KO_SHIM_KFREE;
    if (callee == "memset") return KO_SHIM_MEMSET;
    if (callee == "memcpy") return KO_SHIM_MEMCPY;
    if (callee == "__arch_copy_from_user" || callee.rfind("_copy_from_user", 0) == 0) return KO_SHIM_COPY_FROM_USER;
    if (callee == "__arch_copy_to_user" || callee.rfind("_copy_to_user", 0) == 0) return KO_SHIM_COPY_TO_USER;
    if (callee == "__check_object_size") return KO_SHIM_CHECK_OBJECT_SIZE;
    if (callee == "__sw_hweight64") return KO_SHIM_SW_HWEIGHT64;
    if (callee == "kbase_has_exec_va_zone") return KO_SHIM_KBASE_HAS_EXEC_VA_ZONE;
    if (callee == "scnprintf") return KO_SHIM_SCNPRINTF;
    if (callee == "kbase_device_get_list") return KO_SHIM_KBASE_DEVICE_GET_LIST;
    if (callee == "kbase_create_context") return KO_SHIM_KBASE_CREATE_CONTEXT;
    if (callee == "kbase_destroy_context") return KO_SHIM_KBASE_DESTROY_CONTEXT;
    if (callee == "kbase_mem_pool_alloc_pages") return KO_SHIM_KBASE_MEM_POOL_ALLOC_PAGES;
    if (callee == "kernel_map_user_io_pages" || callee == "kernel_alloc_user_io_pages") return KO_SHIM_KERNEL_MAP_USER_IO_PAGES;
    if (callee == "kbase_mem_alloc") return KO_SHIM_KBASE_MEM_ALLOC;
    if (callee == "kbase_mem_alias") return KO_SHIM_KBASE_MEM_ALIAS;
    if (callee == "kbase_csf_firmware_get_glb_iface") return KO_SHIM_KBASE_CSF_FIRMWARE_GET_GLB_IFACE;
    if (callee == "csf_queue_register_internal") return KO_SHIM_CSF_QUEUE_REGISTER_INTERNAL;
    if (callee == "create_queue_group" || callee == "kbase_csf_queue_group_create") return KO_SHIM_CREATE_QUEUE_GROUP;
    if (callee == "kbase_csf_tiler_heap_init") return KO_SHIM_KBASE_CSF_TILER_HEAP_INIT;
    return KO_SHIM_NOOP_EXTERNAL;
}

struct KoFunctionIRBuilder {
    const std::string& mod_label;
    const ParsedKoModule& mod;
    const std::set<uint64_t>& valid_pcs;
    const std::set<uint64_t>& block_leaders;
    ModuleStringPool& str_pool;
    ultrs::llvm_ir::Function* ir_fn = nullptr;
    ultrs::llvm_ir::BasicBlock* cur_bb = nullptr;
    unsigned ssa_id = 1;
    bool count_insns = true;
    int64_t bb_pending_insns = 0;

    KoFunctionIRBuilder(const std::string& lbl,
                        const ParsedKoModule& m,
                        const std::set<uint64_t>& pcs,
                        const std::set<uint64_t>& leaders,
                        ModuleStringPool& sp,
                        ultrs::llvm_ir::Function* f,
                        bool cnt_insns = true)
        : mod_label(lbl), mod(m), valid_pcs(pcs), block_leaders(leaders), str_pool(sp), ir_fn(f), count_insns(cnt_insns) {}

    void flush_bb_insn_count() {
        if (count_insns && bb_pending_insns > 0 && cur_bb != nullptr) {
            add_cpu_i64_at(352, bb_pending_insns);
            bb_pending_insns = 0;
        }
    }

    void emit_fallthrough(uint64_t next_pc, const std::string& next_lbl) {
        if (!valid_pcs.count(next_pc) || block_leaders.count(next_pc)) {
            flush_bb_insn_count();
            emit_line(std::string("br label %") + next_lbl, ultrs::llvm_ir::OpTag::UncondBranch);
        }
    }

    std::string next_reg(const char* pfx = "v") {
        return std::string("%") + pfx + std::to_string(ssa_id++);
    }

    void emit_line(const std::string& line, ultrs::llvm_ir::OpTag tag = ultrs::llvm_ir::OpTag::RawIR) {
        ultrs::llvm_ir::Instruction inst;
        inst.tag = tag;
        inst.text = line;
        cur_bb->insts.push_back(std::move(inst));
    }

    std::string target_bb_or_exit(uint64_t target_pc) const {
        if (valid_pcs.count(target_pc)) return bb_label(target_pc);
        return "exit_ret";
    }

    static bool is_64bit_reg(std::string r) {
        r = trim_str(r);
        return (!r.empty() && r[0] == 'x') || r == "sp";
    }

    static int reg_cpu_byte_offset(std::string r) {
        r = trim_str(r);
        if (r == "xzr" || r == "wzr") return -1;
        if (r == "sp" || r == "wsp") return 248;
        if (!r.empty() && (r[0] == 'x' || r[0] == 'w')) {
            int idx = std::stoi(r.substr(1));
            if (idx >= 0 && idx < 31) return idx * 8;
        }
        return -1;
    }

    // Returns an i64 SSA value (or "0")
    std::string load_reg64(std::string r) {
        r = trim_str(r);
        int off = reg_cpu_byte_offset(r);
        if (off < 0) return "0";
        std::string p = next_reg("rp");
        std::string v = next_reg("rv");
        emit_line(p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(off));
        emit_line(v + " = load i64, ptr " + p + ", align 8", ultrs::llvm_ir::OpTag::CpuRegRead);
        if (!r.empty() && r[0] == 'w') {
            std::string m = next_reg("rw");
            emit_line(m + " = and i64 " + v + ", 4294967295");
            return m;
        }
        return v;
    }

    // Returns an i32 SSA value (or "0")
    std::string load_reg32(std::string r) {
        r = trim_str(r);
        if (r == "xzr" || r == "wzr") return "0";
        std::string v64 = load_reg64(r);
        std::string v32 = next_reg("r32");
        emit_line(v32 + " = trunc i64 " + v64 + " to i32");
        return v32;
    }

    void store_reg64(std::string r, const std::string& val_i64) {
        r = trim_str(r);
        int off = reg_cpu_byte_offset(r);
        if (off < 0) return;
        std::string to_store = val_i64;
        if (!r.empty() && r[0] == 'w') {
            std::string m = next_reg("wm");
            emit_line(m + " = and i64 " + val_i64 + ", 4294967295");
            to_store = m;
        }
        std::string p = next_reg("wp");
        emit_line(p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(off));
        emit_line("store i64 " + to_store + ", ptr " + p + ", align 8", ultrs::llvm_ir::OpTag::CpuRegWrite);
    }

    void store_reg32(std::string r, const std::string& val_i32) {
        r = trim_str(r);
        int off = reg_cpu_byte_offset(r);
        if (off < 0) return;
        std::string z64 = next_reg("z64");
        emit_line(z64 + " = zext i32 " + val_i32 + " to i64");
        store_reg64(r, z64);
    }

    std::string load_cpu_i64_at(int byte_off) {
        std::string p = next_reg("cp");
        std::string v = next_reg("cv");
        emit_line(p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(byte_off));
        emit_line(v + " = load i64, ptr " + p + ", align 8");
        return v;
    }

    void add_cpu_i64_at(int byte_off, int64_t delta) {
        std::string p = next_reg("cp");
        std::string v = next_reg("cv");
        std::string n = next_reg("cn");
        emit_line(p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(byte_off));
        emit_line(v + " = load i64, ptr " + p + ", align 8");
        emit_line(n + " = add i64 " + v + ", " + std::to_string(delta));
        emit_line("store i64 " + n + ", ptr " + p + ", align 8");
    }

    // Evaluates shifted immediate or register operand as an i64 SSA string
    std::string eval_shifted_op64(const std::vector<std::string>& tokens, bool is_64) {
        std::string base_t = trim_str(tokens[0]);
        if (!base_t.empty() && base_t[0] == '#') {
            int64_t val = parse_imm(base_t);
            if (tokens.size() > 1 && tokens[1].rfind("lsl", 0) == 0) {
                std::istringstream iss(tokens[1]);
                std::string kw, sh_s;
                iss >> kw >> sh_s;
                int64_t sh = parse_imm(sh_s);
                val = static_cast<int64_t>(static_cast<uint64_t>(val) << sh);
            }
            if (is_64) {
                uint64_t uval = static_cast<uint64_t>(val);
                // Pass 2 (KernelSysRegAndTaskStructPass): Widen 39-bit Android kernel TASK_SIZE_64
                // (1ULL << 39 = 0x8000000000) access_ok() bounds to 48-bit host VA space.
                if (uval >= 0x7FFFFF0000ULL && uval <= 0x8000000000ULL) {
                    uval = (1ULL << 48) - (0x8000000000ULL - uval);
                }
                return std::to_string(static_cast<int64_t>(uval));
            }
            uint32_t u32 = static_cast<uint32_t>(val);
            return std::to_string(static_cast<uint64_t>(u32));
        }

        if (tokens.size() == 1) {
            return load_reg64(base_t);
        }

        std::istringstream iss(tokens[1]);
        std::string kind, sh_s;
        iss >> kind >> sh_s;
        int64_t sh = sh_s.empty() ? 0 : parse_imm(sh_s);

        if (kind == "lsl" || kind == "lsr" || kind == "asr") {
            const char* ir_op = (kind == "lsl") ? "shl" : ((kind == "lsr") ? "lshr" : "ashr");
            if (is_64) {
                std::string b64 = load_reg64(base_t);
                if (sh == 0) return b64;
                std::string r = next_reg("sh");
                emit_line(r + " = " + ir_op + " i64 " + b64 + ", " + std::to_string(sh));
                return r;
            } else {
                std::string b32 = load_reg32(base_t);
                std::string s32 = b32;
                if (sh != 0) {
                    s32 = next_reg("sh32");
                    emit_line(s32 + " = " + ir_op + " i32 " + b32 + ", " + std::to_string(sh));
                }
                std::string z64 = next_reg("shz");
                emit_line(z64 + " = zext i32 " + s32 + " to i64");
                return z64;
            }
        }
        if (kind == "uxtw") {
            std::string b32 = load_reg32(base_t);
            std::string z64 = next_reg("uxw");
            emit_line(z64 + " = zext i32 " + b32 + " to i64");
            if (sh == 0) return z64;
            std::string r = next_reg("uxsh");
            emit_line(r + " = shl i64 " + z64 + ", " + std::to_string(sh));
            return r;
        }
        if (kind == "sxtw") {
            std::string b32 = load_reg32(base_t);
            std::string s64 = next_reg("sxw");
            emit_line(s64 + " = sext i32 " + b32 + " to i64");
            if (sh == 0) return s64;
            std::string r = next_reg("sxsh");
            emit_line(r + " = shl i64 " + s64 + ", " + std::to_string(sh));
            return r;
        }
        return load_reg64(base_t);
    }

    std::string eval_shifted_op32(const std::vector<std::string>& tokens) {
        std::string v64 = eval_shifted_op64(tokens, false);
        std::string v32 = next_reg("op32");
        emit_line(v32 + " = trunc i64 " + v64 + " to i32");
        return v32;
    }

    std::string load_flag_i8(int flag_byte_off) {
        std::string p = next_reg("fp");
        std::string v = next_reg("fv");
        emit_line(p + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(flag_byte_off));
        emit_line(v + " = load i8, ptr " + p + ", align 1");
        return v;
    }

    std::string eval_cond_i1(std::string cc) {
        cc = trim_str(cc);
        for (char& ch : cc) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (cc == "eq" || cc == "ne") {
            std::string z = load_flag_i8(265);
            std::string r = next_reg("cc");
            emit_line(r + " = icmp " + (cc == "eq" ? "eq" : "ne") + " i8 " + z + ", 1");
            return r;
        }
        if (cc == "hs" || cc == "cs" || cc == "lo" || cc == "cc") {
            std::string c = load_flag_i8(266);
            std::string r = next_reg("cc");
            bool want_one = (cc == "hs" || cc == "cs");
            emit_line(r + " = icmp " + (want_one ? "eq" : "ne") + " i8 " + c + ", 1");
            return r;
        }
        if (cc == "mi" || cc == "pl") {
            std::string n = load_flag_i8(264);
            std::string r = next_reg("cc");
            emit_line(r + " = icmp " + (cc == "mi" ? "eq" : "ne") + " i8 " + n + ", 1");
            return r;
        }
        if (cc == "vs" || cc == "vc") {
            std::string v = load_flag_i8(267);
            std::string r = next_reg("cc");
            emit_line(r + " = icmp " + (cc == "vs" ? "eq" : "ne") + " i8 " + v + ", 1");
            return r;
        }
        if (cc == "hi" || cc == "ls") {
            std::string c = load_flag_i8(266);
            std::string z = load_flag_i8(265);
            std::string c1 = next_reg("c1");
            std::string z0 = next_reg("z0");
            std::string hi = next_reg("hi");
            emit_line(c1 + " = icmp eq i8 " + c + ", 1");
            emit_line(z0 + " = icmp eq i8 " + z + ", 0");
            emit_line(hi + " = and i1 " + c1 + ", " + z0);
            if (cc == "hi") return hi;
            std::string ls = next_reg("ls");
            emit_line(ls + " = xor i1 " + hi + ", true");
            return ls;
        }
        if (cc == "ge" || cc == "lt") {
            std::string n = load_flag_i8(264);
            std::string v = load_flag_i8(267);
            std::string r = next_reg("cc");
            emit_line(r + " = icmp " + (cc == "ge" ? "eq" : "ne") + " i8 " + n + ", " + v);
            return r;
        }
        if (cc == "gt" || cc == "le") {
            std::string z = load_flag_i8(265);
            std::string n = load_flag_i8(264);
            std::string v = load_flag_i8(267);
            std::string z0 = next_reg("z0");
            std::string nv = next_reg("nv");
            std::string gt = next_reg("gt");
            emit_line(z0 + " = icmp eq i8 " + z + ", 0");
            emit_line(nv + " = icmp eq i8 " + n + ", " + v);
            emit_line(gt + " = and i1 " + z0 + ", " + nv);
            if (cc == "gt") return gt;
            std::string le = next_reg("le");
            emit_line(le + " = xor i1 " + gt + ", true");
            return le;
        }
        return "false";
    }

    struct MemAddrIR {
        std::string addr_i64;
        std::string post_wb_reg;
        int64_t post_wb_off = 0;
        bool has_post_wb = false;
    };

    MemAddrIR emit_mem_addr(std::string mem_str) {
        mem_str = trim_str(mem_str);
        if (!mem_str.empty() && mem_str.back() == '!') {
            // Pre-indexed: [base, #off]!
            std::string inner = trim_str(mem_str.substr(1, mem_str.size() - 3));
            auto parts = split_operands(inner);
            std::string base_r = parts[0];
            int64_t off = (parts.size() > 1) ? parse_imm(parts[1]) : 0;
            std::string b64 = load_reg64(base_r);
            std::string nw = next_reg("pre");
            emit_line(nw + " = add i64 " + b64 + ", " + std::to_string(off));
            store_reg64(base_r, nw);
            return {nw, "", 0, false};
        }
        auto close_pos = mem_str.find("],");
        if (close_pos != std::string::npos) {
            // Post-indexed: [base], #off
            std::string base_r = trim_str(mem_str.substr(1, close_pos - 1));
            int64_t off = parse_imm(mem_str.substr(close_pos + 2));
            std::string b64 = load_reg64(base_r);
            return {b64, base_r, off, true};
        }
        std::string inner = trim_str(mem_str.substr(1, mem_str.size() - 2));
        auto parts = split_operands(inner);
        std::string base_r = parts[0];
        std::string b64 = load_reg64(base_r);
        if (parts.size() == 1) {
            return {b64, "", 0, false};
        }
        if (!parts[1].empty() && parts[1][0] == '#') {
            int64_t off = parse_imm(parts[1]);
            if (off == 0) return {b64, "", 0, false};
            std::string a = next_reg("ma");
            emit_line(a + " = add i64 " + b64 + ", " + std::to_string(off));
            return {a, "", 0, false};
        }
        std::vector<std::string> idx_tokens(parts.begin() + 1, parts.end());
        std::string idx64 = eval_shifted_op64(idx_tokens, true);
        std::string a = next_reg("ma");
        emit_line(a + " = add i64 " + b64 + ", " + idx64);
        return {a, "", 0, false};
    }

    void finish_post_wb(const MemAddrIR& m) {
        if (!m.has_post_wb) return;
        std::string b64 = load_reg64(m.post_wb_reg);
        std::string nw = next_reg("post");
        emit_line(nw + " = add i64 " + b64 + ", " + std::to_string(m.post_wb_off));
        store_reg64(m.post_wb_reg, nw);
    }

    void lift_insn_to_bb(const DisasmInsn& ins) {
        uint64_t pc = ins.addr;
        uint64_t next_pc = pc + 4;
        std::string next_lbl = target_bb_or_exit(next_pc);
        const std::string& mnem = ins.mnemonic;
        auto ops = ins.operands.empty() ? std::vector<std::string>{} : split_operands(ins.operands);
        auto rel_it = mod.relas.find(pc);
        const KoRelaEntry* rel = (rel_it != mod.relas.end()) ? &rel_it->second : nullptr;

        if (cur_bb == nullptr || block_leaders.count(pc)) {
            cur_bb = ir_fn->create_block(bb_label(pc), pc);
            bb_pending_insns = 0;
        }
        emit_line("; @pc " + hex6(pc) + " " + ins.raw_text, ultrs::llvm_ir::OpTag::Comment);
        bb_pending_insns++;

        if (mnem == "dmb" || mnem == "dsb" || mnem == "isb") {
            emit_line("fence seq_cst", ultrs::llvm_ir::OpTag::MemBarrier);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "hint" || mnem == "nop" || mnem == "prfm" || mnem == "msr") {
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "brk" || mnem == "ret") {
            flush_bb_insn_count();
            emit_line("br label %exit_ret", ultrs::llvm_ir::OpTag::Return);
            return;
        }
        if (mnem == ".word") {
            uint32_t w = ins.word;
            if (w == 0xB82A013FU) { // stadd w10, [x9]
                std::string a = load_reg64("x9");
                std::string v = load_reg32("w10");
                std::string p = next_reg("atp");
                std::string old = next_reg("ato");
                emit_line(p + " = inttoptr i64 " + a + " to ptr");
                emit_line(old + " = atomicrmw add ptr " + p + ", i32 " + v + " seq_cst, align 4");
            } else if (w == 0xF829011FU) { // stadd x9, [x8]
                std::string a = load_reg64("x8");
                std::string v = load_reg64("x9");
                std::string p = next_reg("atp");
                std::string old = next_reg("ato");
                emit_line(p + " = inttoptr i64 " + a + " to ptr");
                emit_line(old + " = atomicrmw add ptr " + p + ", i64 " + v + " seq_cst, align 8");
            } else if (w == 0xB829311FU) { // stset w9, [x8]
                std::string a = load_reg64("x8");
                std::string v = load_reg32("w9");
                std::string p = next_reg("atp");
                std::string old = next_reg("ato");
                emit_line(p + " = inttoptr i64 " + a + " to ptr");
                emit_line(old + " = atomicrmw or ptr " + p + ", i32 " + v + " seq_cst, align 4");
            } else if (w == 0x88AA7D02U || w == 0x88A97EA2U || w == 0x88E8FC02U || w == 0x88E9FC02U) {
                const char* addr_r = (w == 0x88AA7D02U) ? "x8" : (w == 0x88A97EA2U) ? "x21" : "x0";
                const char* exp_r  = (w == 0x88AA7D02U) ? "w10" : (w == 0x88E8FC02U) ? "w8" : "w9";
                std::string a = load_reg64(addr_r);
                std::string e = load_reg32(exp_r);
                std::string n = load_reg32("w2");
                std::string p = next_reg("cxp");
                std::string cx = next_reg("cxv");
                std::string old = next_reg("cxo");
                emit_line(p + " = inttoptr i64 " + a + " to ptr");
                emit_line(cx + " = cmpxchg ptr " + p + ", i32 " + e + ", i32 " + n + " seq_cst seq_cst, align 4");
                emit_line(old + " = extractvalue { i32, i1 } " + cx + ", 0");
                store_reg32(exp_r, old);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "mrs") {
            std::string dst = ops[0];
            std::string sysreg = ops[1];
            for (char& ch : sysreg) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (sysreg == "SP_EL0") store_reg64(dst, load_cpu_i64_at(272));
            else if (sysreg == "DAIF") store_reg64(dst, load_cpu_i64_at(280));
            else if (sysreg == "TTBR1_EL1") store_reg64(dst, load_cpu_i64_at(296));
            else store_reg64(dst, "0");
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "adrp") {
            std::string dst = ops[0];
            if (rel) {
                if (rel->sym_shndx == mod.ro_idx) {
                    uint64_t off_ro = static_cast<uint64_t>(static_cast<int64_t>(rel->sym_val) + rel->r_addend);
                    auto it = kRodataTableMap.find({mod_label, off_ro});
                    if (it != kRodataTableMap.end()) {
                        uint32_t prof_id = resolve_profile_id(mod_label);
                        std::string ro_p = next_reg("rop");
                        std::string ro_i = next_reg("roi");
                        emit_line(ro_p + " = call ptr @lifted_ko_rodata_ptr_id(ptr %c, i32 " + std::to_string(prof_id) +
                                  ", i32 " + std::to_string(off_ro) + ", ptr @" + it->second.fallback_symbol + ")");
                        emit_line(ro_i + " = ptrtoint ptr " + ro_p + " to i64");
                        store_reg64(dst, ro_i);
                        emit_fallthrough(next_pc, next_lbl);
                        return;
                    }
                }
                int field_off = 328; // dummy_global
                if (rel->sym_name == "_totalram_pages") field_off = 304;
                else if (rel->sym_name == "kmalloc_caches") field_off = 312;
                else if (rel->sym_name == "cpu_hwcaps") field_off = 320;
                else if (mod_label == "r44p1" && (static_cast<int64_t>(rel->sym_val) + rel->r_addend) == 0xBCA0) field_off = 336;
                std::string gp = next_reg("gp");
                std::string gi = next_reg("gi");
                emit_line(gp + " = getelementptr inbounds i8, ptr %c, i64 " + std::to_string(field_off));
                emit_line(gi + " = ptrtoint ptr " + gp + " to i64");
                store_reg64(dst, gi);
            } else {
                store_reg64(dst, "0");
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "adr") {
            std::string dst = ops[0];
            int64_t off = parse_imm(ops[1]);
            uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
            store_reg64(dst, std::to_string(target));
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "add" || mnem == "adds" || mnem == "sub" || mnem == "subs" ||
            mnem == "and" || mnem == "ands" || mnem == "orr" || mnem == "eor" ||
            mnem == "bic" || mnem == "bics") {
            if (mnem == "add" && rel && rel->r_type == 277) {
                store_reg64(ops[0], load_reg64(ops[1]));
                emit_fallthrough(next_pc, next_lbl);
                return;
            }
            std::string dst = ops[0], src1 = ops[1];
            bool is_64 = is_64bit_reg(dst);
            std::vector<std::string> rest(ops.begin() + 2, ops.end());
            if (is_64) {
                std::string a = load_reg64(src1);
                std::string b = eval_shifted_op64(rest, true);
                std::string r = next_reg("alu");
                if (mnem == "add") emit_line(r + " = add i64 " + a + ", " + b);
                else if (mnem == "sub") emit_line(r + " = sub i64 " + a + ", " + b);
                else if (mnem == "and") emit_line(r + " = and i64 " + a + ", " + b);
                else if (mnem == "orr") emit_line(r + " = or i64 " + a + ", " + b);
                else if (mnem == "eor") emit_line(r + " = xor i64 " + a + ", " + b);
                else if (mnem == "bic") {
                    std::string nb = next_reg("not");
                    emit_line(nb + " = xor i64 " + b + ", -1");
                    emit_line(r + " = and i64 " + a + ", " + nb);
                } else if (mnem == "adds") {
                    emit_line(r + " = call i64 @lifted_add64_flags(ptr %c, i64 " + a + ", i64 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "subs") {
                    emit_line(r + " = call i64 @lifted_sub64_flags(ptr %c, i64 " + a + ", i64 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "ands") {
                    emit_line(r + " = call i64 @lifted_and64_flags(ptr %c, i64 " + a + ", i64 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "bics") {
                    std::string nb = next_reg("not");
                    emit_line(nb + " = xor i64 " + b + ", -1");
                    emit_line(r + " = call i64 @lifted_and64_flags(ptr %c, i64 " + a + ", i64 " + nb + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                }
                store_reg64(dst, r);
            } else {
                std::string a = load_reg32(src1);
                std::string b = eval_shifted_op32(rest);
                std::string r = next_reg("alu32");
                if (mnem == "add") emit_line(r + " = add i32 " + a + ", " + b);
                else if (mnem == "sub") emit_line(r + " = sub i32 " + a + ", " + b);
                else if (mnem == "and") emit_line(r + " = and i32 " + a + ", " + b);
                else if (mnem == "orr") emit_line(r + " = or i32 " + a + ", " + b);
                else if (mnem == "eor") emit_line(r + " = xor i32 " + a + ", " + b);
                else if (mnem == "bic") {
                    std::string nb = next_reg("not32");
                    emit_line(nb + " = xor i32 " + b + ", -1");
                    emit_line(r + " = and i32 " + a + ", " + nb);
                } else if (mnem == "adds") {
                    emit_line(r + " = call i32 @lifted_add32_flags(ptr %c, i32 " + a + ", i32 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "subs") {
                    emit_line(r + " = call i32 @lifted_sub32_flags(ptr %c, i32 " + a + ", i32 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "ands") {
                    emit_line(r + " = call i32 @lifted_and32_flags(ptr %c, i32 " + a + ", i32 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                } else if (mnem == "bics") {
                    std::string nb = next_reg("not32");
                    emit_line(nb + " = xor i32 " + b + ", -1");
                    emit_line(r + " = call i32 @lifted_and32_flags(ptr %c, i32 " + a + ", i32 " + nb + ")", ultrs::llvm_ir::OpTag::FlagWrite);
                }
                store_reg32(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "cmp" || mnem == "cmn" || mnem == "tst") {
            std::string src1 = ops[0];
            bool is_64 = is_64bit_reg(src1);
            std::vector<std::string> rest(ops.begin() + 1, ops.end());
            std::string dummy = next_reg("cmp");
            if (is_64) {
                std::string a = load_reg64(src1);
                std::string b = eval_shifted_op64(rest, true);
                const char* fn = (mnem == "cmp") ? "@lifted_sub64_flags"
                               : (mnem == "cmn") ? "@lifted_add64_flags"
                                                 : "@lifted_and64_flags";
                emit_line(dummy + " = call i64 " + fn + "(ptr %c, i64 " + a + ", i64 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
            } else {
                std::string a = load_reg32(src1);
                std::string b = eval_shifted_op32(rest);
                const char* fn = (mnem == "cmp") ? "@lifted_sub32_flags"
                               : (mnem == "cmn") ? "@lifted_add32_flags"
                                                 : "@lifted_and32_flags";
                emit_line(dummy + " = call i32 " + fn + "(ptr %c, i32 " + a + ", i32 " + b + ")", ultrs::llvm_ir::OpTag::FlagWrite);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "ccmp") {
            std::string src1 = ops[0], op2_t = ops[1], nzcv_t = ops[2], cc = ops[3];
            bool is_64 = is_64bit_reg(src1);
            std::string cond_i1 = eval_cond_i1(cc);
            int64_t nzcv_imm = parse_imm(nzcv_t);
            if (is_64) {
                std::string a = load_reg64(src1);
                std::string b = eval_shifted_op64({op2_t}, true);
                emit_line("call void @lifted_ccmp64_flags(ptr %c, i1 " + cond_i1 + ", i64 " + a + ", i64 " + b +
                          ", i32 " + std::to_string(nzcv_imm) + ")", ultrs::llvm_ir::OpTag::FlagWrite);
            } else {
                std::string a = load_reg32(src1);
                std::string b = eval_shifted_op32({op2_t});
                emit_line("call void @lifted_ccmp32_flags(ptr %c, i1 " + cond_i1 + ", i32 " + a + ", i32 " + b +
                          ", i32 " + std::to_string(nzcv_imm) + ")", ultrs::llvm_ir::OpTag::FlagWrite);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "mov") {
            std::string dst = ops[0], src = ops[1];
            bool is_64 = is_64bit_reg(dst);
            store_reg64(dst, eval_shifted_op64({src}, is_64));
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "mvn") {
            std::string dst = ops[0];
            bool is_64 = is_64bit_reg(dst);
            std::vector<std::string> rest(ops.begin() + 1, ops.end());
            if (is_64) {
                std::string v = eval_shifted_op64(rest, true);
                std::string r = next_reg("mvn");
                emit_line(r + " = xor i64 " + v + ", -1");
                store_reg64(dst, r);
            } else {
                std::string v = eval_shifted_op32(rest);
                std::string r = next_reg("mvn32");
                emit_line(r + " = xor i32 " + v + ", -1");
                store_reg32(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "sxtw" || mnem == "sxth" || mnem == "sxtb") {
            std::string dst = ops[0], src = ops[1];
            std::string v64 = load_reg64(src);
            std::string tr = next_reg("sxtr");
            std::string sx = next_reg("sx");
            const char* small_ty = (mnem == "sxtw") ? "i32" : ((mnem == "sxth") ? "i16" : "i8");
            emit_line(tr + " = trunc i64 " + v64 + " to " + small_ty);
            emit_line(sx + " = sext " + small_ty + " " + tr + " to i64");
            store_reg64(dst, sx);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "movk") {
            std::string dst = ops[0];
            uint64_t imm = static_cast<uint64_t>(parse_imm(ops[1])) & 0xFFFFULL;
            int sh = 0;
            if (ops.size() > 2) {
                std::istringstream iss(ops[2]);
                std::string kw, sh_s;
                iss >> kw >> sh_s;
                sh = static_cast<int>(parse_imm(sh_s));
            }
            uint64_t mask = ~(0xFFFFULL << sh);
            uint64_t ins_val = imm << sh;
            std::string cur = load_reg64(dst);
            std::string m = next_reg("mkm");
            std::string r = next_reg("mkr");
            emit_line(m + " = and i64 " + cur + ", " + std::to_string(static_cast<int64_t>(mask)));
            emit_line(r + " = or i64 " + m + ", " + std::to_string(static_cast<int64_t>(ins_val)));
            store_reg64(dst, r);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "lsl" || mnem == "lsr" || mnem == "asr" || mnem == "mul" || mnem == "umulh" || mnem == "udiv") {
            std::string dst = ops[0], s1 = ops[1], s2 = ops[2];
            bool is_64 = is_64bit_reg(dst);
            if (mnem == "umulh") {
                std::string a = load_reg64(s1);
                std::string b = eval_shifted_op64({s2}, true);
                std::string a128 = next_reg("u128a");
                std::string b128 = next_reg("u128b");
                std::string p128 = next_reg("u128p");
                std::string sh128 = next_reg("u128s");
                std::string r64 = next_reg("umh");
                emit_line(a128 + " = zext i64 " + a + " to i128");
                emit_line(b128 + " = zext i64 " + b + " to i128");
                emit_line(p128 + " = mul i128 " + a128 + ", " + b128);
                emit_line(sh128 + " = lshr i128 " + p128 + ", 64");
                emit_line(r64 + " = trunc i128 " + sh128 + " to i64");
                store_reg64(dst, r64);
            } else if (is_64) {
                std::string a = load_reg64(s1);
                std::string b = eval_shifted_op64({s2}, true);
                std::string r = next_reg("dp2");
                if (mnem == "lsl" || mnem == "lsr" || mnem == "asr") {
                    std::string bm = next_reg("sh_m");
                    emit_line(bm + " = and i64 " + b + ", 63");
                    const char* op = (mnem == "lsl") ? "shl" : (mnem == "lsr") ? "lshr" : "ashr";
                    emit_line(r + " = " + op + " i64 " + a + ", " + bm);
                } else if (mnem == "mul") {
                    emit_line(r + " = mul i64 " + a + ", " + b);
                } else if (mnem == "udiv") {
                    std::string is_z = next_reg("dz");
                    std::string safe_b = next_reg("dsb");
                    std::string q = next_reg("dq");
                    emit_line(is_z + " = icmp eq i64 " + b + ", 0");
                    emit_line(safe_b + " = select i1 " + is_z + ", i64 1, i64 " + b);
                    emit_line(q + " = udiv i64 " + a + ", " + safe_b);
                    emit_line(r + " = select i1 " + is_z + ", i64 0, i64 " + q);
                }
                store_reg64(dst, r);
            } else {
                std::string a = load_reg32(s1);
                std::string b = eval_shifted_op32({s2});
                std::string r = next_reg("dp2_32");
                if (mnem == "lsl" || mnem == "lsr" || mnem == "asr") {
                    std::string bm = next_reg("sh_m32");
                    emit_line(bm + " = and i32 " + b + ", 31");
                    const char* op = (mnem == "lsl") ? "shl" : (mnem == "lsr") ? "lshr" : "ashr";
                    emit_line(r + " = " + op + " i32 " + a + ", " + bm);
                } else if (mnem == "mul") {
                    emit_line(r + " = mul i32 " + a + ", " + b);
                } else if (mnem == "udiv") {
                    std::string is_z = next_reg("dz32");
                    std::string safe_b = next_reg("dsb32");
                    std::string q = next_reg("dq32");
                    emit_line(is_z + " = icmp eq i32 " + b + ", 0");
                    emit_line(safe_b + " = select i1 " + is_z + ", i32 1, i32 " + b);
                    emit_line(q + " = udiv i32 " + a + ", " + safe_b);
                    emit_line(r + " = select i1 " + is_z + ", i32 0, i32 " + q);
                }
                store_reg32(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "msub" || mnem == "madd") {
            std::string dst = ops[0];
            bool is_64 = is_64bit_reg(dst);
            if (is_64) {
                std::string e1 = load_reg64(ops[1]);
                std::string e2 = load_reg64(ops[2]);
                std::string e3 = load_reg64(ops[3]);
                std::string p = next_reg("mp");
                std::string r = next_reg("mr");
                emit_line(p + " = mul i64 " + e1 + ", " + e2);
                emit_line(r + " = " + (mnem == "msub" ? "sub" : "add") + " i64 " + e3 + ", " + p);
                store_reg64(dst, r);
            } else {
                std::string e1 = load_reg32(ops[1]);
                std::string e2 = load_reg32(ops[2]);
                std::string e3 = load_reg32(ops[3]);
                std::string p = next_reg("mp32");
                std::string r = next_reg("mr32");
                emit_line(p + " = mul i32 " + e1 + ", " + e2);
                emit_line(r + " = " + (mnem == "msub" ? "sub" : "add") + " i32 " + e3 + ", " + p);
                store_reg32(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "ubfx" || mnem == "ubfiz" || mnem == "bfi" || mnem == "bfxil") {
            std::string dst = ops[0], src = ops[1];
            int lsb = static_cast<int>(parse_imm(ops[2]));
            int wid = static_cast<int>(parse_imm(ops[3]));
            uint64_t smask = (wid >= 64) ? ~0ULL : ((1ULL << wid) - 1ULL);
            std::string sv = load_reg64(src);
            if (mnem == "ubfx") {
                std::string sh = (lsb > 0) ? next_reg("bfsh") : sv;
                if (lsb > 0) emit_line(sh + " = lshr i64 " + sv + ", " + std::to_string(lsb));
                std::string r = next_reg("ubfx");
                emit_line(r + " = and i64 " + sh + ", " + std::to_string(static_cast<int64_t>(smask)));
                store_reg64(dst, r);
            } else if (mnem == "ubfiz") {
                std::string m = next_reg("ubm");
                emit_line(m + " = and i64 " + sv + ", " + std::to_string(static_cast<int64_t>(smask)));
                std::string r = (lsb > 0) ? next_reg("ubfiz") : m;
                if (lsb > 0) emit_line(r + " = shl i64 " + m + ", " + std::to_string(lsb));
                store_reg64(dst, r);
            } else if (mnem == "bfi") {
                uint64_t dmask = ~(smask << lsb);
                std::string dv = load_reg64(dst);
                std::string dm = next_reg("bfdm");
                std::string sm = next_reg("bfsm");
                std::string ssh = (lsb > 0) ? next_reg("bfssh") : sm;
                std::string r = next_reg("bfi");
                emit_line(dm + " = and i64 " + dv + ", " + std::to_string(static_cast<int64_t>(dmask)));
                emit_line(sm + " = and i64 " + sv + ", " + std::to_string(static_cast<int64_t>(smask)));
                if (lsb > 0) emit_line(ssh + " = shl i64 " + sm + ", " + std::to_string(lsb));
                emit_line(r + " = or i64 " + dm + ", " + ssh);
                store_reg64(dst, r);
            } else { // bfxil
                uint64_t dmask = ~smask;
                std::string dv = load_reg64(dst);
                std::string dm = next_reg("bfxd");
                std::string ssh = (lsb > 0) ? next_reg("bfxs") : sv;
                std::string sm = next_reg("bfxm");
                std::string r = next_reg("bfxil");
                emit_line(dm + " = and i64 " + dv + ", " + std::to_string(static_cast<int64_t>(dmask)));
                if (lsb > 0) emit_line(ssh + " = lshr i64 " + sv + ", " + std::to_string(lsb));
                emit_line(sm + " = and i64 " + ssh + ", " + std::to_string(static_cast<int64_t>(smask)));
                emit_line(r + " = or i64 " + dm + ", " + sm);
                store_reg64(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "csel" || mnem == "csinv" || mnem == "csinc" || mnem == "csneg") {
            std::string dst = ops[0], s1 = ops[1], s2 = ops[2], cc = ops[3];
            bool is_64 = is_64bit_reg(dst);
            std::string cond_i1 = eval_cond_i1(cc);
            if (is_64) {
                std::string e1 = load_reg64(s1);
                std::string e2 = load_reg64(s2);
                std::string alt = e2;
                if (mnem == "csinv") {
                    alt = next_reg("csinv");
                    emit_line(alt + " = xor i64 " + e2 + ", -1");
                } else if (mnem == "csinc") {
                    alt = next_reg("csinc");
                    emit_line(alt + " = add i64 " + e2 + ", 1");
                } else if (mnem == "csneg") {
                    alt = next_reg("csneg");
                    emit_line(alt + " = sub i64 0, " + e2);
                }
                std::string r = next_reg("csel");
                emit_line(r + " = select i1 " + cond_i1 + ", i64 " + e1 + ", i64 " + alt);
                store_reg64(dst, r);
            } else {
                std::string e1 = load_reg32(s1);
                std::string e2 = load_reg32(s2);
                std::string alt = e2;
                if (mnem == "csinv") {
                    alt = next_reg("csinv32");
                    emit_line(alt + " = xor i32 " + e2 + ", -1");
                } else if (mnem == "csinc") {
                    alt = next_reg("csinc32");
                    emit_line(alt + " = add i32 " + e2 + ", 1");
                } else if (mnem == "csneg") {
                    alt = next_reg("csneg32");
                    emit_line(alt + " = sub i32 0, " + e2);
                }
                std::string r = next_reg("csel32");
                emit_line(r + " = select i1 " + cond_i1 + ", i32 " + e1 + ", i32 " + alt);
                store_reg32(dst, r);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "cset") {
            std::string dst = ops[0], cc = ops[1];
            std::string cond_i1 = eval_cond_i1(cc);
            std::string r = next_reg("cset");
            emit_line(r + " = zext i1 " + cond_i1 + " to i64");
            store_reg64(dst, r);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "ldr" || mnem == "ldur" || mnem == "ldar" || mnem == "ldxr" ||
            mnem == "ldrb" || mnem == "ldrh" || mnem == "ldurh" || mnem == "ldrsb" || mnem == "ldrsw") {
            std::string dst = ops[0];
            std::string mem_arg = ops[1];
            for (size_t i = 2; i < ops.size(); ++i) mem_arg += ", " + ops[i];
            MemAddrIR m = emit_mem_addr(mem_arg);
            std::string ptr_r = next_reg("ldp");
            emit_line(ptr_r + " = inttoptr i64 " + m.addr_i64 + " to ptr");
            if (mnem == "ldr" || mnem == "ldur" || mnem == "ldar" || mnem == "ldxr") {
                if (is_64bit_reg(dst)) {
                    std::string v = next_reg("ld64");
                    emit_line(v + " = load i64, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                    store_reg64(dst, v);
                } else {
                    std::string v = next_reg("ld32");
                    emit_line(v + " = load i32, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                    store_reg32(dst, v);
                }
            } else if (mnem == "ldrb") {
                std::string v8 = next_reg("ldb");
                std::string z64 = next_reg("ldbz");
                emit_line(v8 + " = load i8, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                emit_line(z64 + " = zext i8 " + v8 + " to i64");
                store_reg64(dst, z64);
            } else if (mnem == "ldrh" || mnem == "ldurh") {
                std::string v16 = next_reg("ldh");
                std::string z64 = next_reg("ldhz");
                emit_line(v16 + " = load i16, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                emit_line(z64 + " = zext i16 " + v16 + " to i64");
                store_reg64(dst, z64);
            } else if (mnem == "ldrsb") {
                std::string v8 = next_reg("ldsb");
                emit_line(v8 + " = load i8, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                if (is_64bit_reg(dst)) {
                    std::string s64 = next_reg("ldsb64");
                    emit_line(s64 + " = sext i8 " + v8 + " to i64");
                    store_reg64(dst, s64);
                } else {
                    std::string s32 = next_reg("ldsb32");
                    emit_line(s32 + " = sext i8 " + v8 + " to i32");
                    store_reg32(dst, s32);
                }
            } else { // ldrsw
                std::string v32 = next_reg("ldsw");
                std::string s64 = next_reg("ldsw64");
                emit_line(v32 + " = load i32, ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                emit_line(s64 + " = sext i32 " + v32 + " to i64");
                store_reg64(dst, s64);
            }
            finish_post_wb(m);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "str" || mnem == "stur" || mnem == "strb" || mnem == "strh" || mnem == "sturh") {
            std::string src = ops[0];
            std::string mem_arg = ops[1];
            for (size_t i = 2; i < ops.size(); ++i) mem_arg += ", " + ops[i];
            MemAddrIR m = emit_mem_addr(mem_arg);
            std::string ptr_r = next_reg("stp");
            emit_line(ptr_r + " = inttoptr i64 " + m.addr_i64 + " to ptr");
            if (mnem == "str" || mnem == "stur") {
                if (is_64bit_reg(src) || trim_str(src) == "xzr") {
                    std::string v = load_reg64(src);
                    emit_line("store i64 " + v + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
                } else {
                    std::string v = load_reg32(src);
                    emit_line("store i32 " + v + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
                }
            } else if (mnem == "strb") {
                std::string v64 = load_reg64(src);
                std::string v8 = next_reg("stb");
                emit_line(v8 + " = trunc i64 " + v64 + " to i8");
                emit_line("store i8 " + v8 + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            } else {
                std::string v64 = load_reg64(src);
                std::string v16 = next_reg("sth");
                emit_line(v16 + " = trunc i64 " + v64 + " to i16");
                emit_line("store i16 " + v16 + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            }
            finish_post_wb(m);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "stxr" || mnem == "stlxr") {
            std::string status_r = ops[0], src_r = ops[1];
            std::string mem_arg = ops[2];
            for (size_t i = 3; i < ops.size(); ++i) mem_arg += ", " + ops[i];
            MemAddrIR m = emit_mem_addr(mem_arg);
            std::string ptr_r = next_reg("stxp");
            emit_line(ptr_r + " = inttoptr i64 " + m.addr_i64 + " to ptr");
            if (is_64bit_reg(src_r) || trim_str(src_r) == "xzr") {
                std::string v = load_reg64(src_r);
                emit_line("store i64 " + v + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            } else {
                std::string v = load_reg32(src_r);
                emit_line("store i32 " + v + ", ptr " + ptr_r + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            }
            store_reg64(status_r, "0");
            finish_post_wb(m);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "ldp") {
            std::string r1 = ops[0], r2 = ops[1];
            std::string mem_arg = ops[2];
            for (size_t i = 3; i < ops.size(); ++i) mem_arg += ", " + ops[i];
            MemAddrIR m = emit_mem_addr(mem_arg);
            bool is_64 = is_64bit_reg(r1);
            int step = is_64 ? 8 : 4;
            std::string a2 = next_reg("ldpa2");
            std::string p1 = next_reg("ldpp1");
            std::string p2 = next_reg("ldpp2");
            emit_line(a2 + " = add i64 " + m.addr_i64 + ", " + std::to_string(step));
            emit_line(p1 + " = inttoptr i64 " + m.addr_i64 + " to ptr");
            emit_line(p2 + " = inttoptr i64 " + a2 + " to ptr");
            if (is_64) {
                std::string v1 = next_reg("ldpv1");
                std::string v2 = next_reg("ldpv2");
                emit_line(v1 + " = load i64, ptr " + p1 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                emit_line(v2 + " = load i64, ptr " + p2 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                store_reg64(r1, v1);
                store_reg64(r2, v2);
            } else {
                std::string v1 = next_reg("ldpv1");
                std::string v2 = next_reg("ldpv2");
                emit_line(v1 + " = load i32, ptr " + p1 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                emit_line(v2 + " = load i32, ptr " + p2 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemLoad);
                store_reg32(r1, v1);
                store_reg32(r2, v2);
            }
            finish_post_wb(m);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "stp") {
            std::string r1 = ops[0], r2 = ops[1];
            std::string mem_arg = ops[2];
            for (size_t i = 3; i < ops.size(); ++i) mem_arg += ", " + ops[i];
            MemAddrIR m = emit_mem_addr(mem_arg);
            bool is_64 = is_64bit_reg(r1) || trim_str(r1) == "xzr";
            int step = is_64 ? 8 : 4;
            std::string a2 = next_reg("stpa2");
            std::string p1 = next_reg("stpp1");
            std::string p2 = next_reg("stpp2");
            emit_line(a2 + " = add i64 " + m.addr_i64 + ", " + std::to_string(step));
            emit_line(p1 + " = inttoptr i64 " + m.addr_i64 + " to ptr");
            emit_line(p2 + " = inttoptr i64 " + a2 + " to ptr");
            if (is_64) {
                std::string v1 = load_reg64(r1);
                std::string v2 = load_reg64(r2);
                emit_line("store i64 " + v1 + ", ptr " + p1 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
                emit_line("store i64 " + v2 + ", ptr " + p2 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            } else {
                std::string v1 = load_reg32(r1);
                std::string v2 = load_reg32(r2);
                emit_line("store i32 " + v1 + ", ptr " + p1 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
                emit_line("store i32 " + v2 + ", ptr " + p2 + ", align 1", ultrs::llvm_ir::OpTag::GuestMemStore);
            }
            finish_post_wb(m);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "b") {
            flush_bb_insn_count();
            int64_t off = parse_imm(ops[0]);
            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
            emit_line("br label %" + target_bb_or_exit(t_pc), ultrs::llvm_ir::OpTag::UncondBranch);
            return;
        }
        if (mnem.rfind("b.", 0) == 0) {
            std::string cc = mnem.substr(2);
            int64_t off = parse_imm(ops[0]);
            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
            std::string cond_i1 = eval_cond_i1(cc);
            flush_bb_insn_count();
            emit_line("br i1 " + cond_i1 + ", label %" + target_bb_or_exit(t_pc) + ", label %" + next_lbl,
                      ultrs::llvm_ir::OpTag::CondBranch);
            return;
        }
        if (mnem == "cbz" || mnem == "cbnz") {
            std::string r = ops[0];
            int64_t off = parse_imm(ops[1]);
            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
            std::string v = load_reg64(r);
            std::string cond_i1 = next_reg("cb");
            emit_line(cond_i1 + " = icmp " + (mnem == "cbz" ? "eq" : "ne") + " i64 " + v + ", 0");
            flush_bb_insn_count();
            emit_line("br i1 " + cond_i1 + ", label %" + target_bb_or_exit(t_pc) + ", label %" + next_lbl,
                      ultrs::llvm_ir::OpTag::CondBranch);
            return;
        }
        if (mnem == "tbz" || mnem == "tbnz") {
            std::string r = ops[0];
            int bit = static_cast<int>(parse_imm(ops[1]));
            int64_t off = parse_imm(ops[2]);
            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
            std::string v = load_reg64(r);
            std::string sh = (bit > 0) ? next_reg("tbsh") : v;
            if (bit > 0) emit_line(sh + " = lshr i64 " + v + ", " + std::to_string(bit));
            std::string b = next_reg("tbb");
            std::string cond_i1 = next_reg("tbc");
            emit_line(b + " = and i64 " + sh + ", 1");
            emit_line(cond_i1 + " = icmp " + (mnem == "tbz" ? "eq" : "ne") + " i64 " + b + ", 0");
            flush_bb_insn_count();
            emit_line("br i1 " + cond_i1 + ", label %" + target_bb_or_exit(t_pc) + ", label %" + next_lbl,
                      ultrs::llvm_ir::OpTag::CondBranch);
            return;
        }
        if (mnem == "br") {
            std::string t = load_reg64(ops[0]);
            std::string m = next_reg("brm");
            emit_line(m + " = and i64 " + t + ", 4294967295");
            flush_bb_insn_count();
            emit_line("store i64 " + m + ", ptr %ind_pc_slot, align 8");
            emit_line("br label %indirect_br_dispatch", ultrs::llvm_ir::OpTag::UncondBranch);
            return;
        }
        if (mnem == "blr") {
            flush_bb_insn_count();
            if (count_insns) add_cpu_i64_at(360, 1); // cpu->calls_executed++
            uint32_t prof_id = resolve_profile_id(mod_label);
            uint32_t shim_id = KO_SHIM_INDIRECT_BLR;
            emit_line("call void @lifted_kernel_shim_dispatch_id(ptr %c, i32 " + std::to_string(prof_id) +
                      ", i32 " + std::to_string(shim_id) + ")",
                      ultrs::llvm_ir::OpTag::IndirectCall);
            emit_fallthrough(next_pc, next_lbl);
            return;
        }
        if (mnem == "bl") {
            std::string target;
            if (rel) {
                if (!rel->sym_name.empty()) {
                    target = rel->sym_name;
                } else {
                    uint64_t taddr = static_cast<uint64_t>(static_cast<int64_t>(rel->sym_val) + rel->r_addend);
                    auto it = mod.sym_by_addr.find(taddr);
                    if (it != mod.sym_by_addr.end()) target = it->second;
                    else {
                        char b[32];
                        std::snprintf(b, sizeof(b), "sub_%llx", static_cast<unsigned long long>(taddr));
                        target = b;
                    }
                }
            } else {
                int64_t off = parse_imm(ops[0]);
                uint64_t taddr = static_cast<uint64_t>(static_cast<int64_t>(pc) + off);
                auto it = mod.sym_by_addr.find(taddr);
                if (it != mod.sym_by_addr.end()) target = it->second;
                else {
                    char b[32];
                    std::snprintf(b, sizeof(b), "sub_%llx", static_cast<unsigned long long>(taddr));
                    target = b;
                }
            }
            flush_bb_insn_count();
            if (count_insns) add_cpu_i64_at(360, 1); // cpu->calls_executed++
            if (kDirectLiftedCallees.count(target) && mod.sym_by_name.count(target)) {
                std::string cr = next_reg("call");
                emit_line(cr + " = call i64 @lifted_" + mod_label + "_" + target + "(ptr %c)",
                          ultrs::llvm_ir::OpTag::DirectCall);
            } else {
                uint32_t prof_id = resolve_profile_id(mod_label);
                uint32_t shim_id = resolve_shim_id(target);
                emit_line("call void @lifted_kernel_shim_dispatch_id(ptr %c, i32 " + std::to_string(prof_id) +
                          ", i32 " + std::to_string(shim_id) + ") ; " + target,
                          ultrs::llvm_ir::OpTag::DirectCall);
            }
            emit_fallthrough(next_pc, next_lbl);
            return;
        }

        emit_fallthrough(next_pc, next_lbl);
    }
};

static void emit_ir_flag_helpers(std::ostream& os) {
    os << R"LLVM(
; -----------------------------------------------------------------------------
; Inline AArch64 NZCV Flag Computation Helpers (Lowered by LLVM default<O2>)
; -----------------------------------------------------------------------------
define internal i32 @lifted_sub32_flags(ptr nocapture noundef nonnull %c, i32 %a, i32 %b) alwaysinline nounwind {
entry:
  %r = sub i32 %a, %b
  %n_b = icmp slt i32 %r, 0
  %z_b = icmp eq i32 %r, 0
  %c_b = icmp uge i32 %a, %b
  %x1 = xor i32 %a, %b
  %x2 = xor i32 %a, %r
  %va = and i32 %x1, %x2
  %v_b = icmp slt i32 %va, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %c_u = zext i1 %c_b to i8
  %v_u = zext i1 %v_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 %c_u, ptr %pc, align 1
  store i8 %v_u, ptr %pv, align 1
  ret i32 %r
}

define internal i64 @lifted_sub64_flags(ptr nocapture noundef nonnull %c, i64 %a, i64 %b) alwaysinline nounwind {
entry:
  %r = sub i64 %a, %b
  %n_b = icmp slt i64 %r, 0
  %z_b = icmp eq i64 %r, 0
  %c_b = icmp uge i64 %a, %b
  %x1 = xor i64 %a, %b
  %x2 = xor i64 %a, %r
  %va = and i64 %x1, %x2
  %v_b = icmp slt i64 %va, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %c_u = zext i1 %c_b to i8
  %v_u = zext i1 %v_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 %c_u, ptr %pc, align 1
  store i8 %v_u, ptr %pv, align 1
  ret i64 %r
}

define internal i32 @lifted_add32_flags(ptr nocapture noundef nonnull %c, i32 %a, i32 %b) alwaysinline nounwind {
entry:
  %r = add i32 %a, %b
  %n_b = icmp slt i32 %r, 0
  %z_b = icmp eq i32 %r, 0
  %c_b = icmp ult i32 %r, %a
  %x1 = xor i32 %a, %r
  %x2 = xor i32 %b, %r
  %va = and i32 %x1, %x2
  %v_b = icmp slt i32 %va, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %c_u = zext i1 %c_b to i8
  %v_u = zext i1 %v_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 %c_u, ptr %pc, align 1
  store i8 %v_u, ptr %pv, align 1
  ret i32 %r
}

define internal i64 @lifted_add64_flags(ptr nocapture noundef nonnull %c, i64 %a, i64 %b) alwaysinline nounwind {
entry:
  %r = add i64 %a, %b
  %n_b = icmp slt i64 %r, 0
  %z_b = icmp eq i64 %r, 0
  %c_b = icmp ult i64 %r, %a
  %x1 = xor i64 %a, %r
  %x2 = xor i64 %b, %r
  %va = and i64 %x1, %x2
  %v_b = icmp slt i64 %va, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %c_u = zext i1 %c_b to i8
  %v_u = zext i1 %v_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 %c_u, ptr %pc, align 1
  store i8 %v_u, ptr %pv, align 1
  ret i64 %r
}

define internal i32 @lifted_and32_flags(ptr nocapture noundef nonnull %c, i32 %a, i32 %b) alwaysinline nounwind {
entry:
  %r = and i32 %a, %b
  %n_b = icmp slt i32 %r, 0
  %z_b = icmp eq i32 %r, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 0, ptr %pc, align 1
  store i8 0, ptr %pv, align 1
  ret i32 %r
}

define internal i64 @lifted_and64_flags(ptr nocapture noundef nonnull %c, i64 %a, i64 %b) alwaysinline nounwind {
entry:
  %r = and i64 %a, %b
  %n_b = icmp slt i64 %r, 0
  %z_b = icmp eq i64 %r, 0
  %n_u = zext i1 %n_b to i8
  %z_u = zext i1 %z_b to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 0, ptr %pc, align 1
  store i8 0, ptr %pv, align 1
  ret i64 %r
}

define internal void @lifted_set_nzcv_imm(ptr nocapture noundef nonnull %c, i32 %imm4) alwaysinline nounwind {
entry:
  %n_sh = lshr i32 %imm4, 3
  %z_sh = lshr i32 %imm4, 2
  %c_sh = lshr i32 %imm4, 1
  %n_m = and i32 %n_sh, 1
  %z_m = and i32 %z_sh, 1
  %c_m = and i32 %c_sh, 1
  %v_m = and i32 %imm4, 1
  %n_u = trunc i32 %n_m to i8
  %z_u = trunc i32 %z_m to i8
  %c_u = trunc i32 %c_m to i8
  %v_u = trunc i32 %v_m to i8
  %pn = getelementptr inbounds i8, ptr %c, i64 264
  %pz = getelementptr inbounds i8, ptr %c, i64 265
  %pc = getelementptr inbounds i8, ptr %c, i64 266
  %pv = getelementptr inbounds i8, ptr %c, i64 267
  store i8 %n_u, ptr %pn, align 1
  store i8 %z_u, ptr %pz, align 1
  store i8 %c_u, ptr %pc, align 1
  store i8 %v_u, ptr %pv, align 1
  ret void
}

define internal void @lifted_ccmp64_flags(ptr nocapture noundef nonnull %c, i1 %cond, i64 %a, i64 %b, i32 %imm4) alwaysinline nounwind {
entry:
  br i1 %cond, label %do_cmp, label %do_imm
do_cmp:
  %dummy = call i64 @lifted_sub64_flags(ptr %c, i64 %a, i64 %b)
  ret void
do_imm:
  call void @lifted_set_nzcv_imm(ptr %c, i32 %imm4)
  ret void
}

define internal void @lifted_ccmp32_flags(ptr nocapture noundef nonnull %c, i1 %cond, i32 %a, i32 %b, i32 %imm4) alwaysinline nounwind {
entry:
  br i1 %cond, label %do_cmp, label %do_imm
do_cmp:
  %dummy = call i32 @lifted_sub32_flags(ptr %c, i32 %a, i32 %b)
  ret void
do_imm:
  call void @lifted_set_nzcv_imm(ptr %c, i32 %imm4)
  ret void
}
)LLVM";
}

} // namespace

bool parse_elf64_ko(const std::string& label, const std::string& path, ParsedKoModule& out_mod, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "Cannot open ELF64 .ko file: " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 64 || std::memcmp(data.data(), "\x7f""ELF", 4) != 0) {
        err = "Invalid ELF64 header in: " + path;
        return false;
    }
    out_mod.label = label;
    out_mod.path = path;

    uint64_t e_shoff = read_u64_le(&data[40]);
    uint16_t e_shentsize = read_u16_le(&data[58]);
    uint16_t e_shnum = read_u16_le(&data[60]);
    uint16_t e_shstrndx = read_u16_le(&data[62]);

    struct Shdr {
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_flags;
        uint64_t sh_addr;
        uint64_t sh_offset;
        uint64_t sh_size;
        uint32_t sh_link;
        uint32_t sh_info;
        uint64_t sh_addralign;
        uint64_t sh_entsize;
    };
    std::vector<Shdr> shdrs(e_shnum);
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const uint8_t* p = &data[e_shoff + static_cast<size_t>(i) * e_shentsize];
        shdrs[i].sh_name = read_u32_le(p + 0);
        shdrs[i].sh_type = read_u32_le(p + 4);
        shdrs[i].sh_flags = read_u64_le(p + 8);
        shdrs[i].sh_addr = read_u64_le(p + 16);
        shdrs[i].sh_offset = read_u64_le(p + 24);
        shdrs[i].sh_size = read_u64_le(p + 32);
        shdrs[i].sh_link = read_u32_le(p + 40);
        shdrs[i].sh_info = read_u32_le(p + 44);
        shdrs[i].sh_addralign = read_u64_le(p + 48);
        shdrs[i].sh_entsize = read_u64_le(p + 56);
    }

    const Shdr& shstr_sh = shdrs[e_shstrndx];
    const char* shstr = reinterpret_cast<const char*>(&data[shstr_sh.sh_offset]);

    int sym_idx = -1, text_idx = -1, ro_idx = -1;
    for (uint16_t i = 0; i < e_shnum; ++i) {
        std::string sname(shstr + shdrs[i].sh_name);
        if (sname == ".symtab") sym_idx = i;
        else if (sname == ".text") text_idx = i;
        else if (sname == ".rodata") ro_idx = i;
    }
    if (sym_idx < 0 || text_idx < 0 || ro_idx < 0) {
        err = "Missing .symtab, .text, or .rodata in " + path;
        return false;
    }
    out_mod.text_idx = static_cast<uint16_t>(text_idx);
    out_mod.ro_idx = static_cast<uint16_t>(ro_idx);

    const Shdr& text_sh = shdrs[text_idx];
    const Shdr& ro_sh = shdrs[ro_idx];
    out_mod.text.assign(data.begin() + text_sh.sh_offset, data.begin() + text_sh.sh_offset + text_sh.sh_size);
    out_mod.rodata.assign(data.begin() + ro_sh.sh_offset, data.begin() + ro_sh.sh_offset + ro_sh.sh_size);

    const Shdr& sym_sh = shdrs[sym_idx];
    const Shdr& str_sh = shdrs[sym_sh.sh_link];
    const char* strtab = reinterpret_cast<const char*>(&data[str_sh.sh_offset]);

    struct RawSym {
        std::string name;
        uint8_t info;
        uint16_t shndx;
        uint64_t val;
        uint64_t size;
    };
    std::vector<RawSym> syms;
    for (uint64_t off = sym_sh.sh_offset; off + sym_sh.sh_entsize <= sym_sh.sh_offset + sym_sh.sh_size; off += sym_sh.sh_entsize) {
        const uint8_t* p = &data[off];
        uint32_t st_name = read_u32_le(p + 0);
        uint8_t st_info = p[4];
        uint16_t st_shndx = read_u16_le(p + 6);
        uint64_t st_value = read_u64_le(p + 8);
        uint64_t st_size = read_u64_le(p + 16);
        std::string sname(strtab + st_name);
        syms.push_back({sname, st_info, st_shndx, st_value, st_size});
        if (!sname.empty()) {
            out_mod.sym_by_name[sname] = {st_info, st_shndx, st_value, st_size};
            if (st_shndx == static_cast<uint16_t>(text_idx) && sname[0] != '$' &&
                (sname.size() < 7 || sname.substr(sname.size() - 7) != ".cfi_jt")) {
                out_mod.sym_by_addr[st_value] = sname;
            }
        }
    }

    for (uint16_t i = 0; i < e_shnum; ++i) {
        if (shdrs[i].sh_type == 4 && shdrs[i].sh_info == static_cast<uint32_t>(text_idx)) {
            for (uint64_t off = shdrs[i].sh_offset; off + shdrs[i].sh_entsize <= shdrs[i].sh_offset + shdrs[i].sh_size; off += shdrs[i].sh_entsize) {
                const uint8_t* p = &data[off];
                uint64_t r_offset = read_u64_le(p + 0);
                uint64_t r_info = read_u64_le(p + 8);
                int64_t r_addend = read_s64_le(p + 16);
                uint32_t r_sym = static_cast<uint32_t>(r_info >> 32);
                uint32_t r_type = static_cast<uint32_t>(r_info & 0xFFFFFFFFULL);
                if (r_sym < syms.size()) {
                    out_mod.relas[r_offset] = {
                        r_type,
                        syms[r_sym].name,
                        syms[r_sym].shndx,
                        syms[r_sym].val,
                        r_addend
                    };
                }
            }
        }
    }
    return true;
}

bool lift_ko_modules_to_c_and_ir(
    const std::string& r44_ko_path,
    const std::string& husky_ko_path,
    const std::string& out_hdr_path,
    const std::string& out_c_path,
    const std::string& out_ll_path,
    std::string& err,
    const std::string& out_obj_x86_path,
    const std::string& out_obj_arm64_path,
    bool full_indirect_dispatch,
    bool count_insns) {
    if (const char* env_full = std::getenv("HORIZON2LLVM_FULL_INDIRECT_DISPATCH")) {
        if (std::string(env_full) == "1" || std::string(env_full) == "true") {
            full_indirect_dispatch = true;
        }
    }
    LLVMDisasmDynamic dis;
    if (!dis.init(err)) return false;

    ParsedKoModule r44_mod, husky_mod;
    if (!parse_elf64_ko("r44p1", r44_ko_path, r44_mod, err)) return false;
    if (!parse_elf64_ko("husky_r54p1", husky_ko_path, husky_mod, err)) return false;

    const ParsedKoModule* mods[2] = {&r44_mod, &husky_mod};

    // 1. Emit `lifted_kbase_ko.h`
    std::ofstream hf(out_hdr_path, std::ios::binary);
    if (!hf) {
        err = "Cannot open output header: " + out_hdr_path;
        return false;
    }

    hf << "/* Auto-generated by unified horizon2llvm (--lift-ko) — DO NOT EDIT MANUALLY */\n"
       << "#ifndef LIFTED_KBASE_KO_H\n"
       << "#define LIFTED_KBASE_KO_H\n\n"
       << "#include <stdbool.h>\n"
       << "#include <stdint.h>\n"
       << "#include <stddef.h>\n"
       << "#include \"aarch64_common_cpu_prefix.h\"\n\n"
       << "#ifdef __cplusplus\n"
       << "extern \"C\" {\n"
       << "#endif\n\n"
       << "typedef struct LiftedKbaseCPU LiftedKbaseCPU;\n"
       << "typedef void (*LiftedKbaseShimHook)(LiftedKbaseCPU *cpu, const char *ko_profile, const char *callee);\n\n"
       << "/* Target kernel module hardware/platform profile ID (per Doc 48 comment doco.Sqmp-w0Al4UvqeikyjWXcQ) */\n"
       << "enum LiftedKoProfileId {\n"
       << "    KO_PROFILE_R44P1       = 0, /* MediaTek MT6878 Mali-G615 r44p1/r49p1 (kbase v1.21) */\n"
       << "    KO_PROFILE_HUSKY_R54P1 = 1, /* Google Pixel 8 Pro Tensor G3 Husky Mali-G715 r54p1 (kbase v1.38) */\n"
       << "};\n\n"
       << "/* Compile-time integer enum ID for external kernel shim callouts (per Doc 48 comment doco.fy1VMDUccXqURl_ae6DDiw) */\n"
       << "enum LiftedKernelShimId {\n"
       << "    KO_SHIM_UNKNOWN                          = 0,\n"
       << "    KO_SHIM_INDIRECT_BLR                     = 1,\n"
       << "    KO_SHIM_KMALLOC                          = 2,\n"
       << "    KO_SHIM_KMALLOC_TRACE                    = 3,\n"
       << "    KO_SHIM_KFREE                            = 4,\n"
       << "    KO_SHIM_MEMSET                           = 5,\n"
       << "    KO_SHIM_MEMCPY                           = 6,\n"
       << "    KO_SHIM_COPY_FROM_USER                   = 7,\n"
       << "    KO_SHIM_COPY_TO_USER                     = 8,\n"
       << "    KO_SHIM_CHECK_OBJECT_SIZE                = 9,\n"
       << "    KO_SHIM_SW_HWEIGHT64                     = 10,\n"
       << "    KO_SHIM_KBASE_HAS_EXEC_VA_ZONE           = 11,\n"
       << "    KO_SHIM_SCNPRINTF                        = 12,\n"
       << "    KO_SHIM_KBASE_DEVICE_GET_LIST            = 13,\n"
       << "    KO_SHIM_KBASE_CREATE_CONTEXT             = 14,\n"
       << "    KO_SHIM_KBASE_DESTROY_CONTEXT            = 15,\n"
       << "    KO_SHIM_KBASE_MEM_POOL_ALLOC_PAGES       = 16,\n"
       << "    KO_SHIM_KERNEL_MAP_USER_IO_PAGES         = 17,\n"
       << "    KO_SHIM_KBASE_MEM_ALLOC                  = 18,\n"
       << "    KO_SHIM_KBASE_MEM_ALIAS                  = 19,\n"
       << "    KO_SHIM_KBASE_CSF_FIRMWARE_GET_GLB_IFACE = 20,\n"
       << "    KO_SHIM_CSF_QUEUE_REGISTER_INTERNAL      = 21,\n"
       << "    KO_SHIM_CREATE_QUEUE_GROUP               = 22,\n"
       << "    KO_SHIM_KBASE_CSF_TILER_HEAP_INIT        = 23,\n"
       << "    KO_SHIM_NOOP_EXTERNAL                    = 24,\n"
       << "};\n\n"
       << "/*\n"
       << " * Enclosing Kernel Heap Arena & Sanitizer State (`struct LiftedKoHeap`, per\n"
       << " * doco.fye2LblI44z3tNk1mh5Euw, doco.mbTC_NolEN_X7AF0b4BFRQ & doco.AmlJA4WGBr86E_Cg1P7hMw).\n"
       << " * Prepends a 16-byte `LiftedKoHeapBlockHdr` before every allocation, appends a\n"
       << " * 16-byte `0xFE` redzone canary after every payload, poisons freed blocks with\n"
       << " * `0x6B` (POISON_FREE), and prints a full guest + host stacktrace on any violation.\n"
       << " */\n"
       << "#define LIFTED_KO_HEAP_MAGIC        0x4B414C43U\n"
       << "#define LIFTED_KO_STATE_ALLOCATED   0xA110CA7EU\n"
       << "#define LIFTED_KO_STATE_FREED       0xF2EED00DU\n"
       << "#define LIFTED_KO_POISON_FREE_BYTE  0x6BU\n"
       << "#define LIFTED_KO_REDZONE_BYTE      0xFEU\n\n"
       << "struct LiftedKoHeapBlockHdr {\n"
       << "    uint32_t magic;\n"
       << "    uint32_t state;\n"
       << "    uint32_t payload_size;\n"
       << "    uint32_t alloc_seq;\n"
       << "    uint64_t alloc_guest_pc;\n"
       << "    uint64_t free_guest_pc;\n"
       << "    const char *alloc_fn;\n"
       << "    const char *free_fn;\n"
       << "    void *alloc_host_bt[4];\n"
       << "    void *free_host_bt[4];\n"
       << "} __attribute__((aligned(16)));\n\n"
       << "struct LiftedKoHeap {\n"
       << "    uint32_t used;\n"
       << "    uint32_t alloc_count;\n"
       << "    uint32_t free_count;\n"
       << "    uint32_t double_free_errors;\n"
       << "    uint32_t uaf_errors;\n"
       << "    uint8_t arena[65536] __attribute__((aligned(16)));\n"
       << "};\n\n"
       << "struct LiftedKbaseCPU {\n"
       << "    AARCH64_COMMON_CPU_PREFIX_FIELDS\n"
       << "    uint64_t x18;\n"
       << "    uint8_t n, z, c, v;\n"
       << "    uint64_t sp_el0;\n"
       << "    uint64_t daif;\n"
       << "    uint64_t ttbr0_el1;\n"
       << "    uint64_t ttbr1_el1;\n"
       << "    uint64_t totalram_pages;\n"
       << "    uint64_t kmalloc_cache_dummy;\n"
       << "    uint64_t cpu_hwcaps;\n"
       << "    uint64_t dummy_global;\n"
       << "    uint64_t dev_list_head[2];\n"
       << "    uint64_t insns_executed;\n"
       << "    uint64_t calls_executed;\n"
       << "    void *user_ctx;\n"
       << "    LiftedKbaseShimHook shim_hook;\n"
       << "    struct LiftedKoHeap heap;\n"
       << "    uint64_t scs_stack[128];\n"
       << "    uint8_t stack[65536] __attribute__((aligned(16)));\n"
       << "    uint8_t task_struct[4096] __attribute__((aligned(16)));\n"
       << "    uint64_t cur_pc;\n"
       << "    const char *cur_fn;\n"
       << "};\n\n"
       << "AARCH64_STATIC_ASSERT_CPU_PREFIX(struct LiftedKbaseCPU);\n\n"
       << "void lifted_kbase_cpu_init(LiftedKbaseCPU *cpu, void *user_ctx, LiftedKbaseShimHook shim_hook);\n"
       << "void *lifted_ko_heap_alloc(LiftedKbaseCPU *cpu, size_t sz);\n"
       << "int lifted_ko_heap_free(LiftedKbaseCPU *cpu, void *ptr);\n"
       << "bool lifted_ko_check_mem_access(LiftedKbaseCPU *cpu, const void *ptr, size_t sz, bool is_write);\n"
       << "void lifted_ko_print_stacktrace(LiftedKbaseCPU *cpu, const char *reason, const struct LiftedKoHeapBlockHdr *hdr);\n"
       << "const void *lifted_ko_rodata_ptr(LiftedKbaseCPU *cpu, const char *ko_profile, uint32_t rodata_offset, const void *fallback_slice);\n"
       << "const void *lifted_ko_rodata_ptr_id(LiftedKbaseCPU *cpu, uint32_t profile_id, uint32_t rodata_offset, const void *fallback_slice);\n"
       << "bool lifted_ko_is_image_mapped(const char *ko_profile, size_t *out_rodata_size);\n"
       << "const char *lifted_kernel_shim_id_name(uint32_t shim_id);\n"
       << "void lifted_kernel_shim_dispatch_id(LiftedKbaseCPU *cpu, uint32_t profile_id, uint32_t shim_id);\n"
       << "void lifted_kernel_shim_dispatch(LiftedKbaseCPU *cpu, const char *ko_profile, const char *callee);\n\n";

    for (const ParsedKoModule* m : mods) {
        for (const std::string& fn : kTargetFunctions) {
            if (m->sym_by_name.count(fn)) {
                hf << "int64_t lifted_" << m->label << "_" << fn << "(LiftedKbaseCPU *cpu);\n";
            }
        }
    }
    hf << "\n/* Single-.ko unified entrypoints (defaulting to active specialized .ko profile) */\n";
    for (const std::string& fn : kTargetFunctions) {
        if (r44_mod.sym_by_name.count(fn)) {
            hf << "int64_t lifted_kbase_" << fn << "(LiftedKbaseCPU *cpu);\n";
        }
    }

    hf << "\n#ifdef __cplusplus\n"
       << "}\n"
       << "#endif\n\n"
       << "#endif /* LIFTED_KBASE_KO_H */\n";
    hf.close();

    // 2. Emit thin runtime support file `lifted_kbase_ko.c` (ZERO lifted .ko functions in C;
    //    all 68 lifted .ko functions live in `lifted_kbase_ko.ll` and are compiled to `.o` via LLVM!)
    std::ofstream cf(out_c_path, std::ios::binary);
    if (!cf) {
        err = "Cannot open output C runtime support source: " + out_c_path;
        return false;
    }

    cf << "/* Auto-generated by unified horizon2llvm (--lift-ko) — Thin Host Runtime Bridge for lifted_kbase_ko.ll.\n"
       << " * All 68 lifted kernel functions (`lifted_r44p1_*` and `lifted_husky_r54p1_*`) are lowered directly\n"
       << " * to SSA LLVM IR in `src/emulator/lifted_kbase_ko.ll` and compiled to `lifted_kbase_ko_ll.o` via LLVM.\n"
       << " * This file contains ONLY the runtime support hooks (`lifted_kbase_cpu_init`, `lifted_ko_rodata_ptr`,\n"
       << " * `lifted_ko_heap_alloc/free/check`, and `lifted_kernel_shim_dispatch`).\n"
       << " */\n"
       << "#include \"lifted_kbase_ko.h\"\n"
       << "#include <fcntl.h>\n"
       << "#include <stdlib.h>\n"
       << "#include <string.h>\n"
       << "#include <stdio.h>\n"
       << "#include <sys/mman.h>\n"
       << "#include <sys/stat.h>\n"
       << "#include <unistd.h>\n"
       << "#if defined(__GLIBC__)\n"
       << "#include <execinfo.h>\n"
       << "#endif\n\n"
       << "struct MappedKoImage {\n"
       << "    bool attempted;\n"
       << "    bool valid;\n"
       << "    const uint8_t *map_base;\n"
       << "    size_t map_size;\n"
       << "    const uint8_t *rodata_base;\n"
       << "    size_t rodata_size;\n"
       << "};\n\n"
       << "static struct MappedKoImage g_mapped_r44p1;\n"
       << "static struct MappedKoImage g_mapped_husky_r54p1;\n\n"
       << "static void try_map_ko_image(struct MappedKoImage *img, const char *const *paths, size_t n_paths) {\n"
       << "    if (img->attempted) return;\n"
       << "    img->attempted = true;\n"
       << "    for (size_t p = 0; p < n_paths; ++p) {\n"
       << "        int fd = open(paths[p], O_RDONLY);\n"
       << "        if (fd < 0) continue;\n"
       << "        struct stat st;\n"
       << "        if (fstat(fd, &st) != 0 || st.st_size < 128) { close(fd); continue; }\n"
       << "        void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);\n"
       << "        close(fd);\n"
       << "        if (m == MAP_FAILED) continue;\n"
       << "        const uint8_t *b = (const uint8_t *)m;\n"
       << "        if (memcmp(b, \"\\x7f\" \"ELF\", 4) != 0) { munmap(m, (size_t)st.st_size); continue; }\n"
       << "        uint64_t e_shoff = 0; memcpy(&e_shoff, b + 40, 8);\n"
       << "        uint16_t e_shentsize = 0; memcpy(&e_shentsize, b + 58, 2);\n"
       << "        uint16_t e_shnum = 0; memcpy(&e_shnum, b + 60, 2);\n"
       << "        uint16_t e_shstrndx = 0; memcpy(&e_shstrndx, b + 62, 2);\n"
       << "        if (e_shoff + (uint64_t)e_shnum * e_shentsize > (uint64_t)st.st_size || e_shstrndx >= e_shnum) {\n"
       << "            munmap(m, (size_t)st.st_size);\n"
       << "            continue;\n"
       << "        }\n"
       << "        uint64_t shstr_off = 0;\n"
       << "        memcpy(&shstr_off, b + e_shoff + (size_t)e_shstrndx * e_shentsize + 24, 8);\n"
       << "        const char *shstr = (const char *)(b + shstr_off);\n"
       << "        for (uint16_t i = 0; i < e_shnum; ++i) {\n"
       << "            const uint8_t *sh = b + e_shoff + (size_t)i * e_shentsize;\n"
       << "            uint32_t sh_name = 0; memcpy(&sh_name, sh + 0, 4);\n"
       << "            if (strcmp(shstr + sh_name, \".rodata\") == 0) {\n"
       << "                uint64_t ro_off = 0, ro_sz = 0;\n"
       << "                memcpy(&ro_off, sh + 24, 8);\n"
       << "                memcpy(&ro_sz, sh + 32, 8);\n"
       << "                if (ro_off + ro_sz <= (uint64_t)st.st_size) {\n"
       << "                    img->map_base = b;\n"
       << "                    img->map_size = (size_t)st.st_size;\n"
       << "                    img->rodata_base = b + ro_off;\n"
       << "                    img->rodata_size = (size_t)ro_sz;\n"
       << "                    img->valid = true;\n"
       << "                }\n"
       << "                break;\n"
       << "            }\n"
       << "        }\n"
       << "        if (img->valid) return;\n"
       << "        munmap(m, (size_t)st.st_size);\n"
       << "    }\n"
       << "}\n\n"
       << "static void ensure_ko_images_mapped(void) {\n"
       << "    static const char *const r44_paths[] = {\n"
       << "        \"kernel/mali_kbase_mt6878_r44p1.ko\",\n"
       << "        \"kernel/mali_kbase_mtk.ko\",\n"
       << "        \"mali_kbase.ko\",\n"
       << "        \"../kernel/mali_kbase_mt6878_r44p1.ko\",\n"
       << "        \"../mali_kbase.ko\",\n"
       << "        \"../../mali_kbase.ko\",\n"
       << "    };\n"
       << "    static const char *const husky_paths[] = {\n"
       << "        \"kernel/mali_kbase_husky_r54p1.ko\",\n"
       << "        \"kernel/mali_kbase_husky.ko\",\n"
       << "        \"predownloaded/mali_kbase_husky.ko\",\n"
       << "        \"../kernel/mali_kbase_husky_r54p1.ko\",\n"
       << "        \"../predownloaded/mali_kbase_husky.ko\",\n"
       << "        \"../../predownloaded/mali_kbase_husky.ko\",\n"
       << "    };\n"
       << "    try_map_ko_image(&g_mapped_r44p1, r44_paths, sizeof(r44_paths) / sizeof(r44_paths[0]));\n"
       << "    try_map_ko_image(&g_mapped_husky_r54p1, husky_paths, sizeof(husky_paths) / sizeof(husky_paths[0]));\n"
       << "}\n\n"
       << "bool lifted_ko_is_image_mapped(const char *ko_profile, size_t *out_rodata_size) {\n"
       << "    ensure_ko_images_mapped();\n"
       << "    const struct MappedKoImage *img = (ko_profile && strcmp(ko_profile, \"husky_r54p1\") == 0)\n"
       << "        ? &g_mapped_husky_r54p1 : &g_mapped_r44p1;\n"
       << "    if (out_rodata_size) *out_rodata_size = img->valid ? img->rodata_size : 0;\n"
       << "    return img->valid;\n"
       << "}\n\n"
       << "const void *lifted_ko_rodata_ptr_id(LiftedKbaseCPU *cpu, uint32_t profile_id, uint32_t rodata_offset, const void *fallback_slice) {\n"
       << "    (void)cpu;\n"
       << "    ensure_ko_images_mapped();\n"
       << "    const struct MappedKoImage *img = (profile_id == KO_PROFILE_HUSKY_R54P1)\n"
       << "        ? &g_mapped_husky_r54p1 : &g_mapped_r44p1;\n"
       << "    if (img->valid && (size_t)rodata_offset < img->rodata_size) {\n"
       << "        return img->rodata_base + rodata_offset;\n"
       << "    }\n"
       << "    return fallback_slice;\n"
       << "}\n\n"
       << "const void *lifted_ko_rodata_ptr(LiftedKbaseCPU *cpu, const char *ko_profile, uint32_t rodata_offset, const void *fallback_slice) {\n"
       << "    uint32_t prof_id = (ko_profile && strcmp(ko_profile, \"husky_r54p1\") == 0) ? KO_PROFILE_HUSKY_R54P1 : KO_PROFILE_R44P1;\n"
       << "    return lifted_ko_rodata_ptr_id(cpu, prof_id, rodata_offset, fallback_slice);\n"
       << "}\n\n"
       << "void lifted_ko_print_stacktrace(LiftedKbaseCPU *cpu, const char *reason, const struct LiftedKoHeapBlockHdr *hdr) {\n"
       << "    fprintf(stderr, \"[lifted_ko_violation] %s\\n\", reason ? reason : \"runtime violation\");\n"
       << "    if (hdr) {\n"
       << "        fprintf(stderr, \"  block seq=%u size=%u state=0x%08x\\n\", hdr->alloc_seq, hdr->payload_size, hdr->state);\n"
       << "        fprintf(stderr, \"  allocated at guest fn=%s pc=0x%llx (host bt[0]=%p)\\n\",\n"
       << "                hdr->alloc_fn ? hdr->alloc_fn : \"<unknown>\", (unsigned long long)hdr->alloc_guest_pc, hdr->alloc_host_bt[0]);\n"
       << "        if (hdr->free_guest_pc || hdr->free_fn || hdr->free_host_bt[0]) {\n"
       << "            fprintf(stderr, \"  freed     at guest fn=%s pc=0x%llx (host bt[0]=%p)\\n\",\n"
       << "                    hdr->free_fn ? hdr->free_fn : \"<unknown>\", (unsigned long long)hdr->free_guest_pc, hdr->free_host_bt[0]);\n"
       << "        }\n"
       << "    }\n"
       << "    if (cpu) {\n"
       << "        fprintf(stderr, \"  guest trace: cur_fn=%s cur_pc=0x%llx lr(x30)=0x%llx\\n\",\n"
       << "                cpu->cur_fn ? cpu->cur_fn : \"<top>\", (unsigned long long)cpu->cur_pc, (unsigned long long)cpu->x[30]);\n"
       << "        uintptr_t scs_base = (uintptr_t)&cpu->scs_stack[0];\n"
       << "        if (cpu->x18 > scs_base && cpu->x18 <= scs_base + sizeof(cpu->scs_stack)) {\n"
       << "            size_t depth = (cpu->x18 - scs_base) / sizeof(uint64_t);\n"
       << "            for (size_t i = depth; i > 0; --i) {\n"
       << "                fprintf(stderr, \"    scs[%zu] = 0x%llx\\n\", i - 1, (unsigned long long)cpu->scs_stack[i - 1]);\n"
       << "            }\n"
       << "        }\n"
       << "    }\n"
       << "#if defined(__GLIBC__)\n"
       << "    void *bt[8];\n"
       << "    int n = backtrace(bt, 8);\n"
       << "    char **syms = backtrace_symbols(bt, n);\n"
       << "    if (syms) {\n"
       << "        for (int i = 0; i < n; ++i) fprintf(stderr, \"  host[%d] %s\\n\", i, syms[i]);\n"
       << "        free(syms);\n"
       << "    }\n"
       << "#endif\n"
       << "}\n\n"
       << "static bool check_redzone_intact(const struct LiftedKoHeapBlockHdr *hdr) {\n"
       << "    size_t aligned_sz = ((size_t)hdr->payload_size + 15U) & ~15ULL;\n"
       << "    const uint8_t *rz = (const uint8_t *)hdr + sizeof(struct LiftedKoHeapBlockHdr) + aligned_sz;\n"
       << "    for (size_t i = 0; i < 16U; ++i) {\n"
       << "        if (rz[i] != LIFTED_KO_REDZONE_BYTE) return false;\n"
       << "    }\n"
       << "    return true;\n"
       << "}\n\n"
       << "void *lifted_ko_heap_alloc(LiftedKbaseCPU *cpu, size_t sz) {\n"
       << "    size_t aligned_sz = (sz + 15U) & ~15ULL;\n"
       << "    size_t total = sizeof(struct LiftedKoHeapBlockHdr) + aligned_sz + 16U;\n"
       << "    if (!cpu || (size_t)cpu->heap.used + total > sizeof(cpu->heap.arena)) {\n"
       << "        if (cpu) lifted_ko_print_stacktrace(cpu, \"HEAP ARENA EXHAUSTION in lifted_ko_heap_alloc\", NULL);\n"
       << "        return NULL;\n"
       << "    }\n"
       << "    uint8_t *base = &cpu->heap.arena[cpu->heap.used];\n"
       << "    struct LiftedKoHeapBlockHdr *hdr = (struct LiftedKoHeapBlockHdr *)base;\n"
       << "    memset(hdr, 0, sizeof(*hdr));\n"
       << "    hdr->magic = LIFTED_KO_HEAP_MAGIC;\n"
       << "    hdr->state = LIFTED_KO_STATE_ALLOCATED;\n"
       << "    hdr->payload_size = (uint32_t)sz;\n"
       << "    hdr->alloc_seq = ++cpu->heap.alloc_count;\n"
       << "    hdr->alloc_guest_pc = cpu->cur_pc ? cpu->cur_pc : cpu->x[30];\n"
       << "    hdr->alloc_fn = cpu->cur_fn ? cpu->cur_fn : \"kmalloc\";\n"
       << "    hdr->alloc_host_bt[0] = __builtin_return_address(0);\n"
       << "#if defined(__GLIBC__)\n"
       << "    (void)backtrace(hdr->alloc_host_bt, 4);\n"
       << "#endif\n"
       << "    void *payload = base + sizeof(struct LiftedKoHeapBlockHdr);\n"
       << "    memset(payload, 0, aligned_sz);\n"
       << "    memset((uint8_t *)payload + aligned_sz, LIFTED_KO_REDZONE_BYTE, 16U);\n"
       << "    cpu->heap.used += (uint32_t)total;\n"
       << "    return payload;\n"
       << "}\n\n"
       << "int lifted_ko_heap_free(LiftedKbaseCPU *cpu, void *ptr) {\n"
       << "    if (!cpu || !ptr) return 0;\n"
       << "    uintptr_t p = (uintptr_t)ptr;\n"
       << "    uintptr_t arena_lo = (uintptr_t)&cpu->heap.arena[0];\n"
       << "    uintptr_t arena_hi = arena_lo + sizeof(cpu->heap.arena);\n"
       << "    if (p < arena_lo + sizeof(struct LiftedKoHeapBlockHdr) || p >= arena_hi) {\n"
       << "        lifted_ko_print_stacktrace(cpu, \"OUT-OF-ARENA pointer passed to lifted_ko_heap_free\", NULL);\n"
       << "        return -1;\n"
       << "    }\n"
       << "    struct LiftedKoHeapBlockHdr *hdr = (struct LiftedKoHeapBlockHdr *)(p - sizeof(struct LiftedKoHeapBlockHdr));\n"
       << "    if (hdr->magic != LIFTED_KO_HEAP_MAGIC) {\n"
       << "        lifted_ko_print_stacktrace(cpu, \"HEAP HEADER MAGIC CORRUPTION detected in lifted_ko_heap_free\", hdr);\n"
       << "        return -1;\n"
       << "    }\n"
       << "    if (hdr->state == LIFTED_KO_STATE_FREED) {\n"
       << "        cpu->heap.double_free_errors++;\n"
       << "        lifted_ko_print_stacktrace(cpu, \"DOUBLE-FREE detected in lifted_ko_heap_free\", hdr);\n"
       << "        return -1;\n"
       << "    }\n"
       << "    if (!check_redzone_intact(hdr)) {\n"
       << "        cpu->heap.uaf_errors++;\n"
       << "        lifted_ko_print_stacktrace(cpu, \"REDZONE CANARY CORRUPTION detected in lifted_ko_heap_free\", hdr);\n"
       << "        return -1;\n"
       << "    }\n"
       << "    hdr->state = LIFTED_KO_STATE_FREED;\n"
       << "    hdr->free_guest_pc = cpu->cur_pc ? cpu->cur_pc : cpu->x[30];\n"
       << "    hdr->free_fn = cpu->cur_fn ? cpu->cur_fn : \"kfree\";\n"
       << "    hdr->free_host_bt[0] = __builtin_return_address(0);\n"
       << "#if defined(__GLIBC__)\n"
       << "    (void)backtrace(hdr->free_host_bt, 4);\n"
       << "#endif\n"
       << "    size_t aligned_sz = ((size_t)hdr->payload_size + 15U) & ~15ULL;\n"
       << "    memset(ptr, LIFTED_KO_POISON_FREE_BYTE, aligned_sz);\n"
       << "    cpu->heap.free_count++;\n"
       << "    return 0;\n"
       << "}\n\n"
       << "bool lifted_ko_check_mem_access(LiftedKbaseCPU *cpu, const void *ptr, size_t sz, bool is_write) {\n"
       << "    (void)is_write;\n"
       << "    if (!cpu || !ptr || sz == 0) return false;\n"
       << "    uintptr_t p = (uintptr_t)ptr;\n"
       << "    uintptr_t arena_lo = (uintptr_t)&cpu->heap.arena[0];\n"
       << "    uintptr_t arena_hi = arena_lo + cpu->heap.used;\n"
       << "    if (p >= arena_lo && p < arena_hi) {\n"
       << "        size_t off = 0;\n"
       << "        while (off + sizeof(struct LiftedKoHeapBlockHdr) <= cpu->heap.used) {\n"
       << "            struct LiftedKoHeapBlockHdr *hdr = (struct LiftedKoHeapBlockHdr *)&cpu->heap.arena[off];\n"
       << "            if (hdr->magic != LIFTED_KO_HEAP_MAGIC) {\n"
       << "                lifted_ko_print_stacktrace(cpu, \"HEAP HEADER MAGIC CORRUPTION in lifted_ko_check_mem_access\", hdr);\n"
       << "                break;\n"
       << "            }\n"
       << "            size_t aligned_sz = ((size_t)hdr->payload_size + 15U) & ~15ULL;\n"
       << "            uintptr_t blk_lo = arena_lo + off + sizeof(struct LiftedKoHeapBlockHdr);\n"
       << "            uintptr_t blk_hi = blk_lo + aligned_sz;\n"
       << "            if (p >= blk_lo && p < blk_hi) {\n"
       << "                if (hdr->state == LIFTED_KO_STATE_FREED || p + sz > blk_lo + hdr->payload_size || !check_redzone_intact(hdr)) {\n"
       << "                    cpu->heap.uaf_errors++;\n"
       << "                    lifted_ko_print_stacktrace(cpu, \"USE-AFTER-FREE / OOB / REDZONE VIOLATION detected in lifted_ko_check_mem_access\", hdr);\n"
       << "                    return false;\n"
       << "                }\n"
       << "                return true;\n"
       << "            }\n"
       << "            off += sizeof(struct LiftedKoHeapBlockHdr) + aligned_sz + 16U;\n"
       << "        }\n"
       << "        cpu->heap.uaf_errors++;\n"
       << "        lifted_ko_print_stacktrace(cpu, \"UNALLOCATED / HEADER-ZONE ACCESS in lifted_ko_check_mem_access\", NULL);\n"
       << "        return false;\n"
       << "    }\n"
       << "    return true;\n"
       << "}\n\n"
       << "void lifted_kbase_cpu_init(LiftedKbaseCPU *cpu, void *user_ctx, LiftedKbaseShimHook shim_hook) {\n"
       << "    memset(cpu, 0, sizeof(*cpu));\n"
       << "    cpu->sp = (uintptr_t)&cpu->stack[sizeof(cpu->stack) - 256];\n"
       << "    cpu->x18 = (uintptr_t)&cpu->scs_stack[0];\n"
       << "    cpu->sp_el0 = (uintptr_t)&cpu->task_struct[0];\n"
       << "    cpu->totalram_pages = 2097152ULL; /* 8 GiB in 4K pages */\n"
       << "    cpu->user_ctx = user_ctx;\n"
       << "    cpu->shim_hook = shim_hook;\n"
       << "    /* Linux kernel addr_limit / thread_info.flags = 0 (no TIF_32BIT) */\n"
       << "    *(uint64_t *)(&cpu->task_struct[16]) = ~0ULL;\n"
       << "    /* Direct self-linked list_head for kbase_device_get_list */\n"
       << "    cpu->dev_list_head[0] = (uintptr_t)&cpu->dev_list_head[0];\n"
       << "    cpu->dev_list_head[1] = (uintptr_t)&cpu->dev_list_head[0];\n"
       << "    ensure_ko_images_mapped();\n"
       << "}\n\n"
       << "const char *lifted_kernel_shim_id_name(uint32_t shim_id) {\n"
       << "    switch ((enum LiftedKernelShimId)shim_id) {\n"
       << "    case KO_SHIM_INDIRECT_BLR:                    return \"__indirect_blr\";\n"
       << "    case KO_SHIM_KMALLOC:                         return \"__kmalloc\";\n"
       << "    case KO_SHIM_KMALLOC_TRACE:                   return \"kmalloc_trace\";\n"
       << "    case KO_SHIM_KFREE:                           return \"kfree\";\n"
       << "    case KO_SHIM_MEMSET:                          return \"memset\";\n"
       << "    case KO_SHIM_MEMCPY:                          return \"memcpy\";\n"
       << "    case KO_SHIM_COPY_FROM_USER:                  return \"_copy_from_user\";\n"
       << "    case KO_SHIM_COPY_TO_USER:                    return \"_copy_to_user\";\n"
       << "    case KO_SHIM_CHECK_OBJECT_SIZE:               return \"__check_object_size\";\n"
       << "    case KO_SHIM_SW_HWEIGHT64:                    return \"__sw_hweight64\";\n"
       << "    case KO_SHIM_KBASE_HAS_EXEC_VA_ZONE:          return \"kbase_has_exec_va_zone\";\n"
       << "    case KO_SHIM_SCNPRINTF:                       return \"scnprintf\";\n"
       << "    case KO_SHIM_KBASE_DEVICE_GET_LIST:           return \"kbase_device_get_list\";\n"
       << "    case KO_SHIM_KBASE_CREATE_CONTEXT:            return \"kbase_create_context\";\n"
       << "    case KO_SHIM_KBASE_DESTROY_CONTEXT:           return \"kbase_destroy_context\";\n"
       << "    case KO_SHIM_KBASE_MEM_POOL_ALLOC_PAGES:      return \"kbase_mem_pool_alloc_pages\";\n"
       << "    case KO_SHIM_KERNEL_MAP_USER_IO_PAGES:        return \"kernel_map_user_io_pages\";\n"
       << "    case KO_SHIM_KBASE_MEM_ALLOC:                 return \"kbase_mem_alloc\";\n"
       << "    case KO_SHIM_KBASE_MEM_ALIAS:                 return \"kbase_mem_alias\";\n"
       << "    case KO_SHIM_KBASE_CSF_FIRMWARE_GET_GLB_IFACE: return \"kbase_csf_firmware_get_glb_iface\";\n"
       << "    case KO_SHIM_CSF_QUEUE_REGISTER_INTERNAL:     return \"csf_queue_register_internal\";\n"
       << "    case KO_SHIM_CREATE_QUEUE_GROUP:              return \"create_queue_group\";\n"
       << "    case KO_SHIM_KBASE_CSF_TILER_HEAP_INIT:       return \"kbase_csf_tiler_heap_init\";\n"
       << "    case KO_SHIM_NOOP_EXTERNAL:                   return \"__noop_external\";\n"
       << "    default:                                      return \"unknown\";\n"
       << "    }\n"
       << "}\n\n"
       << "void lifted_kernel_shim_dispatch_id(LiftedKbaseCPU *cpu, uint32_t profile_id, uint32_t shim_id) {\n"
       << "    const char *ko_profile = (profile_id == KO_PROFILE_HUSKY_R54P1) ? \"husky_r54p1\" : \"r44p1\";\n"
       << "    switch ((enum LiftedKernelShimId)shim_id) {\n"
       << "    case KO_SHIM_KMALLOC:\n"
       << "    case KO_SHIM_KMALLOC_TRACE: {\n"
       << "        size_t sz = (shim_id == KO_SHIM_KMALLOC_TRACE) ? (size_t)cpu->x[2] : (size_t)cpu->x[0];\n"
       << "        if (sz == 0) sz = 64;\n"
       << "        void *ptr = lifted_ko_heap_alloc(cpu, sz);\n"
       << "        cpu->x[0] = (uintptr_t)ptr;\n"
       << "        return;\n"
       << "    }\n"
       << "    case KO_SHIM_KFREE:\n"
       << "        (void)lifted_ko_heap_free(cpu, (void *)(uintptr_t)cpu->x[0]);\n"
       << "        cpu->x[0] = 0;\n"
       << "        return;\n"
       << "    case KO_SHIM_MEMSET:\n"
       << "        if (cpu->x[0] && cpu->x[2]) memset((void *)(uintptr_t)cpu->x[0], (int)(cpu->x[1] & 0xFF), (size_t)cpu->x[2]);\n"
       << "        return;\n"
       << "    case KO_SHIM_MEMCPY:\n"
       << "        if (cpu->x[0] && cpu->x[1] && cpu->x[2]) memcpy((void *)(uintptr_t)cpu->x[0], (const void *)(uintptr_t)cpu->x[1], (size_t)cpu->x[2]);\n"
       << "        return;\n"
       << "    case KO_SHIM_COPY_FROM_USER:\n"
       << "    case KO_SHIM_COPY_TO_USER:\n"
       << "        if (cpu->x[0] && cpu->x[1] && cpu->x[2]) {\n"
       << "            memcpy((void *)(uintptr_t)cpu->x[0], (const void *)(uintptr_t)cpu->x[1], (size_t)cpu->x[2]);\n"
       << "            cpu->x[0] = 0;\n"
       << "        } else {\n"
       << "            cpu->x[0] = cpu->x[2];\n"
       << "        }\n"
       << "        return;\n"
       << "    case KO_SHIM_CHECK_OBJECT_SIZE:\n"
       << "        cpu->x[0] = 0;\n"
       << "        return;\n"
       << "    case KO_SHIM_SW_HWEIGHT64:\n"
       << "        cpu->x[0] = (uint64_t)__builtin_popcountll(cpu->x[0]);\n"
       << "        return;\n"
       << "    case KO_SHIM_KBASE_HAS_EXEC_VA_ZONE: {\n"
       << "        uint64_t v = *(uint64_t *)((uintptr_t)cpu->x[0] + 37072 + 192);\n"
       << "        cpu->x[0] = (v != ~0ULL && v != 0ULL) ? 1ULL : 0ULL;\n"
       << "        return;\n"
       << "    }\n"
       << "    case KO_SHIM_SCNPRINTF:\n"
       << "        cpu->x[0] = 16ULL;\n"
       << "        return;\n"
       << "    case KO_SHIM_NOOP_EXTERNAL:\n"
       << "        cpu->x[0] = 0;\n"
       << "        return;\n"
       << "    default:\n"
       << "        if (cpu->shim_hook) {\n"
       << "            cpu->shim_hook(cpu, ko_profile, lifted_kernel_shim_id_name(shim_id));\n"
       << "            return;\n"
       << "        }\n"
       << "        cpu->x[0] = 0;\n"
       << "        return;\n"
       << "    }\n"
       << "}\n\n"
       << "void lifted_kernel_shim_dispatch(LiftedKbaseCPU *cpu, const char *ko_profile, const char *callee) {\n"
       << "    uint32_t prof_id = (ko_profile && strcmp(ko_profile, \"husky_r54p1\") == 0) ? KO_PROFILE_HUSKY_R54P1 : KO_PROFILE_R44P1;\n"
       << "    if (!callee) { cpu->x[0] = 0; return; }\n"
       << "    if (strcmp(callee, \"__kmalloc\") == 0 || strcmp(callee, \"vzalloc\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_KMALLOC); return; }\n"
       << "    if (strcmp(callee, \"kmalloc_trace\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_KMALLOC_TRACE); return; }\n"
       << "    if (strcmp(callee, \"kfree\") == 0 || strcmp(callee, \"vfree\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_KFREE); return; }\n"
       << "    if (strcmp(callee, \"memset\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_MEMSET); return; }\n"
       << "    if (strcmp(callee, \"memcpy\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_MEMCPY); return; }\n"
       << "    if (strcmp(callee, \"__arch_copy_from_user\") == 0 || strncmp(callee, \"_copy_from_user\", 15) == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_COPY_FROM_USER); return; }\n"
       << "    if (strcmp(callee, \"__arch_copy_to_user\") == 0 || strncmp(callee, \"_copy_to_user\", 13) == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_COPY_TO_USER); return; }\n"
       << "    if (strcmp(callee, \"__check_object_size\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_CHECK_OBJECT_SIZE); return; }\n"
       << "    if (strcmp(callee, \"__sw_hweight64\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_SW_HWEIGHT64); return; }\n"
       << "    if (strcmp(callee, \"kbase_has_exec_va_zone\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_KBASE_HAS_EXEC_VA_ZONE); return; }\n"
       << "    if (strcmp(callee, \"scnprintf\") == 0) { lifted_kernel_shim_dispatch_id(cpu, prof_id, KO_SHIM_SCNPRINTF); return; }\n"
       << "    if (cpu->shim_hook) { cpu->shim_hook(cpu, ko_profile, callee); return; }\n"
       << "    cpu->x[0] = 0;\n"
       << "}\n";

    // Emit external fallback slices (used only if .ko files are not on disk at runtime)
    for (const auto& kv : kRodataTableMap) {
        const std::string& label = kv.first.first;
        const RodataTableDesc& desc = kv.second;
        const ParsedKoModule& m = (label == "r44p1") ? r44_mod : husky_mod;
        cf << "\n__attribute__((visibility(\"hidden\"), aligned(16))) const uint8_t " << desc.fallback_symbol << "[" << desc.size << "] = {\n";
        for (uint32_t i = 0; i < desc.size; i += 16) {
            cf << "    ";
            uint32_t lim = std::min(desc.size, i + 16U);
            for (uint32_t j = i; j < lim; ++j) {
                char b[16];
                std::snprintf(b, sizeof(b), "0x%02x", m.rodata[desc.offset + j]);
                cf << b << (j + 1 < lim ? ", " : ",");
            }
            cf << "\n";
        }
        cf << "};\n";
    }
    cf.close();

    // 3. Build `ultrs::llvm_ir::Module`, lift all 68 functions across `mali_kbase.ko` (r44p1)
    //    and `mali_kbase_husky.ko` (husky_r54p1), and run the unified `horizon2llvm` lowering
    //    pass pipeline (`PassPipelineConfig::forKernelKo()`).
    ultrs::llvm_ir::Module ir_mod("unified_horizon2llvm_kbase_ko", "x86_64-pc-linux-gnu");
    ModuleStringPool str_pool;

    for (const ParsedKoModule* m : mods) {
        for (const std::string& fn : kTargetFunctions) {
            auto it = m->sym_by_name.find(fn);
            if (it == m->sym_by_name.end()) continue;
            uint64_t val = it->second.st_value;
            uint64_t sz = it->second.st_size;
            auto insns = dis.disasm_range(m->text, val, val + sz);

            std::set<uint64_t> valid_pcs;
            for (const auto& ins : insns) valid_pcs.insert(ins.addr);

            // Pre-scan instructions to identify:
            //   (a) whether the function contains any `BR Xn` indirect branch (`has_indirect_br`),
            //   (b) statically recovered `.rodata` jump-table target PCs (`jump_table_targets`), and
            //   (c) basic-block leader PCs (`block_leaders`), eliminating full-function indirect_br_dispatch
            //       tables and coalescing straight-line instructions into single basic blocks unless
            //       `--full-indirect-dispatch` (`HORIZON2LLVM_FULL_INDIRECT_DISPATCH=1`) is requested.
            bool has_indirect_br = false;
            std::set<uint64_t> jump_table_targets;
            std::set<uint64_t> block_leaders;

            if (full_indirect_dispatch) {
                block_leaders = valid_pcs;
                jump_table_targets = valid_pcs;
            } else {
                if (!insns.empty()) block_leaders.insert(insns.front().addr);

                // Collect all .rodata jump tables referenced by `adrp` in this function
                std::vector<std::pair<uint64_t, const RodataTableDesc*>> fn_jtabs;
                for (const auto& ins : insns) {
                    if (ins.mnemonic == "adrp") {
                        auto r_it = m->relas.find(ins.addr);
                        if (r_it != m->relas.end() && r_it->second.sym_shndx == m->ro_idx) {
                            uint64_t off_ro = static_cast<uint64_t>(
                                static_cast<int64_t>(r_it->second.sym_val) + r_it->second.r_addend);
                            auto jt_it = kRodataTableMap.find({m->label, off_ro});
                            if (jt_it != kRodataTableMap.end() && jt_it->second.size <= 256U) {
                                fn_jtabs.emplace_back(ins.addr, &jt_it->second);
                            }
                        }
                    }
                }

                for (size_t idx = 0; idx < insns.size(); ++idx) {
                    const DisasmInsn& ins = insns[idx];
                    uint64_t pc = ins.addr;
                    const std::string& mnem = ins.mnemonic;
                    auto ops = ins.operands.empty() ? std::vector<std::string>{} : split_operands(ins.operands);

                    if (mnem == "b" || mnem.rfind("b.", 0) == 0) {
                        if (!ops.empty()) {
                            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + parse_imm(ops[0]));
                            if (valid_pcs.count(t_pc)) block_leaders.insert(t_pc);
                        }
                        if (valid_pcs.count(pc + 4)) block_leaders.insert(pc + 4);
                    } else if (mnem == "cbz" || mnem == "cbnz") {
                        if (ops.size() >= 2) {
                            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + parse_imm(ops[1]));
                            if (valid_pcs.count(t_pc)) block_leaders.insert(t_pc);
                        }
                        if (valid_pcs.count(pc + 4)) block_leaders.insert(pc + 4);
                    } else if (mnem == "tbz" || mnem == "tbnz") {
                        if (ops.size() >= 3) {
                            uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(pc) + parse_imm(ops[2]));
                            if (valid_pcs.count(t_pc)) block_leaders.insert(t_pc);
                        }
                        if (valid_pcs.count(pc + 4)) block_leaders.insert(pc + 4);
                    } else if (mnem == "ret" || mnem == "brk") {
                        if (valid_pcs.count(pc + 4)) block_leaders.insert(pc + 4);
                    } else if (mnem == "br") {
                        has_indirect_br = true;
                        if (valid_pcs.count(pc + 4)) block_leaders.insert(pc + 4);

                        // Recover `adr` base PC (typically `pc - 12`)
                        uint64_t adr_pc = pc - 12;
                        for (size_t back = 1; back <= 6 && back <= idx; ++back) {
                            if (insns[idx - back].mnemonic == "adr") {
                                auto a_ops = split_operands(insns[idx - back].operands);
                                int64_t a_off = (a_ops.size() >= 2) ? parse_imm(a_ops[1]) : 0;
                                adr_pc = static_cast<uint64_t>(static_cast<int64_t>(insns[idx - back].addr) + a_off);
                                break;
                            }
                        }

                        // Find nearest preceding `adrp` jump table (or hoisted function jump table)
                        const RodataTableDesc* matched_jt = nullptr;
                        for (auto it_jt = fn_jtabs.rbegin(); it_jt != fn_jtabs.rend(); ++it_jt) {
                            if (it_jt->first < pc) {
                                matched_jt = it_jt->second;
                                break;
                            }
                        }
                        if (!matched_jt && !fn_jtabs.empty()) {
                            matched_jt = fn_jtabs.front().second;
                        }
                        if (matched_jt && matched_jt->offset + matched_jt->size <= m->rodata.size()) {
                            uint32_t n_entries = matched_jt->size / 4U;
                            for (uint32_t e = 0; e < n_entries; ++e) {
                                int32_t rel32 = read_s32_le(&m->rodata[matched_jt->offset + e * 4U]);
                                uint64_t t_pc = static_cast<uint64_t>(static_cast<int64_t>(adr_pc) + rel32) & 0xFFFFFFFFULL;
                                if (valid_pcs.count(t_pc)) {
                                    jump_table_targets.insert(t_pc);
                                    block_leaders.insert(t_pc);
                                }
                            }
                        }
                    }
                }
                if (has_indirect_br && jump_table_targets.empty()) {
                    // Conservative fallback if a `br` has no recognized `.rodata` table
                    jump_table_targets = block_leaders;
                }
            }

            std::string full_fn_name = "lifted_" + m->label + "_" + fn;
            ultrs::llvm_ir::Function* ir_fn = ir_mod.create_function(full_fn_name, val, val + sz);

            // Entry block (only allocate `%ind_pc_slot` if the function uses `indirect_br_dispatch`)
            ultrs::llvm_ir::BasicBlock* entry_bb = ir_fn->create_block("entry", val);
            {
                ultrs::llvm_ir::Instruction i1;
                i1.tag = ultrs::llvm_ir::OpTag::RawIR;
                std::string entry_br = "br label %" + (insns.empty() ? std::string("exit_ret") : bb_label(insns.front().addr));
                if (has_indirect_br || full_indirect_dispatch) {
                    i1.text = "%ind_pc_slot = alloca i64, align 8\n  store i64 0, ptr %ind_pc_slot, align 8\n  " + entry_br;
                } else {
                    i1.text = entry_br;
                }
                entry_bb->insts.push_back(std::move(i1));
            }

            KoFunctionIRBuilder fb(m->label, *m, valid_pcs, block_leaders, str_pool, ir_fn, count_insns);
            for (const DisasmInsn& ins : insns) {
                fb.lift_insn_to_bb(ins);
            }
            fb.flush_bb_insn_count();

            // Emit `indirect_br_dispatch` ONLY when the function has an indirect branch (or `--full-indirect-dispatch` is set)
            if (has_indirect_br || full_indirect_dispatch) {
                ultrs::llvm_ir::BasicBlock* ind_bb = ir_fn->create_block("indirect_br_dispatch", val + sz);
                std::ostringstream sw;
                sw << "%ind_pc_val = load i64, ptr %ind_pc_slot, align 8\n"
                   << "  switch i64 %ind_pc_val, label %exit_ret [\n";
                const std::set<uint64_t>& sw_targets = full_indirect_dispatch ? valid_pcs : jump_table_targets;
                for (uint64_t pc_v : sw_targets) {
                    sw << "    i64 " << pc_v << ", label %" << bb_label(pc_v) << "\n";
                }
                sw << "  ]";
                ultrs::llvm_ir::Instruction sw_inst;
                sw_inst.tag = ultrs::llvm_ir::OpTag::RawIR;
                sw_inst.text = sw.str();
                ind_bb->insts.push_back(std::move(sw_inst));
            }

            // Unified exit block returning sign-extended w0 as i64
            ultrs::llvm_ir::BasicBlock* exit_bb = ir_fn->create_block("exit_ret", val + sz + 4);
            {
                ultrs::llvm_ir::Instruction ret_inst;
                ret_inst.tag = ultrs::llvm_ir::OpTag::Return;
                ret_inst.text = "%ret_x0_val = load i64, ptr %c, align 8\n"
                                "  %ret_w0_val = trunc i64 %ret_x0_val to i32\n"
                                "  %ret_sext = sext i32 %ret_w0_val to i64\n"
                                "  ret i64 %ret_sext";
                exit_bb->insts.push_back(std::move(ret_inst));
            }
        }
    }

    // Run the unified horizon2llvm KernelModuleKo lowering pipeline!
    {
        ultrs::passes::PassPipelineConfig ko_cfg = ultrs::passes::PassPipelineConfig::forKernelKo();
        // Keep exact LiftedKbaseCPU memory layout intact across kernel shim callouts
        ko_cfg.enable_cpu_state_promotion = false;
        ko_cfg.enable_aapcs64_abi_contract = false;
        ko_cfg.enable_tail_call_devirt = false;
        ko_cfg.enable_nzcv_dead_flag_elim = false;
        ko_cfg.enable_nzcv_fusion = false;
        ultrs::passes::ModulePassManager mpm;
        ultrs::passes::build_aarch64_lowering_pipeline(mpm, ko_cfg);
        mpm.run(ir_mod);
    }

    // 4. Serialize `lifted_kbase_ko.ll`
    std::ostringstream ll_ss;
    ll_ss << "; ModuleID = 'unified_horizon2llvm_kbase_ko'\n"
          << "source_filename = \"unified_horizon2llvm_kbase_ko.ll\"\n\n"
          << "declare dso_local ptr @lifted_ko_rodata_ptr_id(ptr, i32, i32, ptr) nounwind\n"
          << "declare dso_local void @lifted_kernel_shim_dispatch_id(ptr, i32, i32) nounwind\n\n";

    for (const auto& kv : kRodataTableMap) {
        ll_ss << "@" << kv.second.fallback_symbol << " = external hidden constant ["
              << kv.second.size << " x i8], align 16\n";
    }
    ll_ss << "\n";
    str_pool.emit_globals(ll_ss);
    emit_ir_flag_helpers(ll_ss);
    ll_ss << "\n";

    for (const auto& fn : ir_mod.functions) {
        ll_ss << "; Function Attrs: nounwind\n"
              << "define dso_local i64 @" << fn->name
              << "(ptr noundef nonnull align 16 %c) nounwind {\n";
        for (const auto& bb : fn->blocks) {
            ll_ss << bb->name << ":\n";
            for (const auto& inst : bb->insts) {
                if (inst.dead) continue;
                if (!inst.text.empty()) {
                    // Strip any !alias.scope metadata tags if metadata table is omitted
                    std::string line = inst.text;
                    auto md_pos = line.find(", !alias.scope");
                    if (md_pos != std::string::npos) {
                        line = line.substr(0, md_pos);
                    }
                    ll_ss << "  " << line << "\n";
                }
            }
        }
        ll_ss << "}\n\n";
    }

    for (const std::string& fn : kTargetFunctions) {
        if (r44_mod.sym_by_name.count(fn)) {
            ll_ss << "define dso_local i64 @lifted_kbase_" << fn << "(ptr noundef nonnull align 16 %c) nounwind {\n"
                  << "entry:\n"
                  << "  %r = call i64 @lifted_r44p1_" << fn << "(ptr %c)\n"
                  << "  ret i64 %r\n"
                  << "}\n\n";
        }
    }
    std::string final_ir = ll_ss.str();

    if (!out_ll_path.empty()) {
        std::ofstream lf(out_ll_path, std::ios::binary);
        if (!lf) {
            err = "Cannot open output LLVM IR file: " + out_ll_path;
            return false;
        }
        lf.write(final_ir.data(), static_cast<std::streamsize>(final_ir.size()));
    }

    // 5. Compile `lifted_kbase_ko.ll` directly to native ELF64 PIC object code (.o) via libLLVM
    if (!out_obj_x86_path.empty()) {
        auto res = ultrs::llvm_backend::process_module(
            final_ir, "x86_64-pc-linux-gnu", true, "default<O2>", "", "", out_obj_x86_path);
        if (!res.ok) {
            err = "x86_64 LLVM IR compilation failed: " + res.error_message;
            return false;
        }
    }
    if (!out_obj_arm64_path.empty()) {
        auto res = ultrs::llvm_backend::process_module(
            final_ir, "aarch64-unknown-linux-gnu", true, "default<O2>", "", "", out_obj_arm64_path);
        if (!res.ok) {
            err = "aarch64 LLVM IR compilation failed: " + res.error_message;
            return false;
        }
    }

    return true;
}

} // namespace ultrs::ko_lifter
