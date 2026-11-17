// Unified Horizon2LLVM Kernel Module (.ko ET_REL) Lifter & Lowering Pipeline Header
// (src/aot/horizon2llvm/ko_lifter.hpp)
//
// Unifies the lifting and lowering of AArch64 Linux kernel relocatable modules
// (`mali_kbase.ko` MT6878 r44p1 / Pixel 7 Tensor G2 r47p0 CSF 5.10.189,
//  `mali_kbase_husky.ko` Pixel 8 Pro Tensor G3 r54p1/r54p3 CSF,
//  `mali_kbase_gs201_r47p0_csf_5_10_198.ko` Pixel 7 Tensor G2 r47p0 CSF 5.10.198, and
//  `mali_kbase_gs101_r47p0_jm_5_10_198.ko` Pixel 6 Tensor G1 r47p0 JM 5.10.198)
// into the same C++ `horizon2llvm` LLVM-IR framework used for `libGLES_mali.so`.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ultrs::ko_lifter {

struct KoRelaEntry {
    uint32_t r_type = 0;
    std::string sym_name;
    uint16_t sym_shndx = 0;
    uint64_t sym_val = 0;
    int64_t r_addend = 0;
};

struct KoSymbolInfo {
    uint8_t st_info = 0;
    uint16_t st_shndx = 0;
    uint64_t st_value = 0;
    uint64_t st_size = 0;
};

struct RodataTableDesc {
    std::string fallback_symbol;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct ParsedKoModule {
    std::string label;
    std::string path;
    uint32_t profile_id = 0;
    std::vector<uint8_t> text;
    std::vector<uint8_t> rodata;
    uint16_t text_idx = 0;
    uint16_t ro_idx = 0;
    std::map<std::string, KoSymbolInfo> sym_by_name;
    std::map<uint64_t, std::string> sym_by_addr;
    std::map<uint64_t, std::string> ro_sym_by_addr;
    std::vector<uint64_t> ro_boundaries;
    std::map<uint64_t, RodataTableDesc> ro_tables;
    std::map<uint64_t, KoRelaEntry> relas;
};

struct DisasmInsn {
    uint64_t addr = 0;
    uint32_t word = 0;
    std::string mnemonic;
    std::string operands;
    std::string raw_text;
};

bool parse_elf64_ko(const std::string& label, const std::string& path, ParsedKoModule& out_mod, std::string& err);

uint32_t infer_rodata_slice_size(const ParsedKoModule& m, uint64_t off_ro);

bool lift_ko_modules_to_c_and_ir(
    const std::string& r44_ko_path,
    const std::string& husky_ko_path,
    const std::string& out_hdr_path,
    const std::string& out_c_path,
    const std::string& out_ll_path,
    std::string& err,
    const std::string& out_obj_x86_path = "",
    const std::string& out_obj_arm64_path = "",
    bool full_indirect_dispatch = false,
    bool count_insns = true,
    bool instrument_calls = true);

bool lift_single_ko_module_to_ir(
    const std::string& ko_path,
    const std::string& label,
    const std::string& out_ll_path,
    std::string& err,
    const std::string& out_obj_path = "",
    bool full_indirect_dispatch = false,
    bool count_insns = true,
    bool instrument_calls = true);

} // namespace ultrs::ko_lifter
