#!/usr/bin/env python3
"""
Single-Source-of-Truth Specification & C/C++ Header Generator for:
  1. Arm Mali Valhall / 5th-Gen CSF Bytecode (parsed from Mesa src/panfrost/genxml/v10.xml & v11.xml)
  2. Arm Mali Kbase UAPI & MMU/Region/Exception Invariants (parsed from MT6878 r44p1 kernel headers:
     mali_base_common_kernel.h, mali_base_kernel.h, mali_base_jm_kernel.h,
     mali_kbase_csf_registers.h, mali_kbase_gpu_regmap_csf.h, mali_kbase_mem.h,
     mali_kbase_mem.c, and mali_kbase_mem_linux.c).

Outputs:
  - specs/mali_csf_kbase_spec.json
  - include/mali_csf_kbase_spec_autogen.h
"""

import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

GENXML_V10_CANDIDATES = [
    "/working_dir/c_3106ac7496559798/work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/src/panfrost/genxml/v10.xml",
    os.path.join(ROOT_DIR, "../work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/src/panfrost/genxml/v10.xml"),
]
GENXML_V11_CANDIDATES = [
    "/working_dir/c_3106ac7496559798/work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/src/panfrost/genxml/v11.xml",
    os.path.join(ROOT_DIR, "../work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/src/panfrost/genxml/v11.xml"),
]
KBASE_R44_DIR_CANDIDATES = [
    "/working_dir/c_3106ac7496559798/mt6878_r44_src/android_kernel_modules_nothing_mt6878-mt6878-Tetris-u/gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard",
]


def first_existing(paths):
    for p in paths:
        if os.path.exists(p):
            return p
    return None


def c_ident(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", name.strip()).strip("_").upper()
    return s


def parse_genxml(xml_path: str):
    enums = {}
    instructions = {}
    if not xml_path or not os.path.exists(xml_path):
        return enums, instructions

    tree = ET.parse(xml_path)
    root = tree.getroot()
    for elem in root:
        if elem.tag == "enum":
            ename = elem.attrib.get("name", "")
            if ename.startswith("CS "):
                vals = {}
                for v in elem:
                    if v.tag == "value":
                        vals[v.attrib["name"]] = int(v.attrib["value"], 0)
                enums[ename] = vals
        elif elem.tag == "struct" and elem.attrib.get("name", "").startswith("CS "):
            raw_name = elem.attrib["name"]
            iname = raw_name[3:].strip()
            if iname == "Base":
                continue
            fields = []
            op_default = None
            for f in elem:
                if f.tag == "field":
                    if f.attrib.get("name") == "Opcode":
                        op_default = f.attrib.get("default")
                    fields.append({
                        "name": f.attrib["name"],
                        "start": int(f.attrib["start"], 0),
                        "size": int(f.attrib["size"], 0),
                        "type": f.attrib.get("type", "uint"),
                        "default": f.attrib.get("default"),
                    })
            op_map = enums.get("CS Opcode", {})
            opcode = op_map.get(op_default, op_map.get(iname, -1))
            instructions[iname] = {
                "opcode": opcode,
                "fields": fields,
                "source": f"{os.path.basename(xml_path)}:struct[{raw_name}]",
            }
    return enums, instructions


def build_spec():
    v11_path = first_existing(GENXML_V11_CANDIDATES)
    v10_path = first_existing(GENXML_V10_CANDIDATES)
    enums_v11, insns_v11 = parse_genxml(v11_path or v10_path)

    # Canonical fallback if run in CI container without full Mesa checkout mounted at /working_dir
    if not enums_v11:
        spec_json_path = os.path.join(ROOT_DIR, "specs", "mali_csf_kbase_spec.json")
        if os.path.exists(spec_json_path):
            with open(spec_json_path, "r", encoding="utf-8") as f:
                return json.load(f)

    spec = {
        "provenance": {
            "mesa_genxml_v10": "src/panfrost/genxml/v10.xml",
            "mesa_genxml_v11": "src/panfrost/genxml/v11.xml",
            "kbase_r44p1_mem_c": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/mali_kbase_mem.c",
            "kbase_r44p1_mem_linux_c": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/mali_kbase_mem_linux.c",
            "kbase_r44p1_mmu_aarch64_c": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/mmu/mali_kbase_mmu_mode_aarch64.c",
            "kbase_r44p1_csf_c": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/csf/mali_kbase_csf.c",
            "kbase_r44p1_jm_kernel_h": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/include/uapi/gpu/arm/midgard/jm/mali_base_jm_kernel.h",
            "kbase_r44p1_csf_registers_h": "gpu/gpu_mali/mali_avalon/mali-r44p1/drivers/gpu/arm/midgard/csf/mali_kbase_csf_registers.h",
        },
        "csf_enums": enums_v11,
        "csf_instructions": insns_v11,
        "csf_exception_types": {
            "NONE": {"code": 0x00, "source": "mali_base_jm_kernel.h:695"},
            "DONE": {"code": 0x01, "source": "mali_base_jm_kernel.h:696"},
            "TERMINATED": {"code": 0x04, "source": "mali_base_jm_kernel.h:698"},
            "KABOOM": {"code": 0x05, "source": "mali_base_jm_kernel.h:700"},
            "EUREKA": {"code": 0x06, "source": "mali_base_jm_kernel.h:701"},
            "ACTIVE": {"code": 0x08, "source": "mali_base_jm_kernel.h:702"},
            "CS_RESOURCE_TERMINATED": {"code": 0x0F, "source": "mali_base_jm_kernel.h:704"},
            "CS_CONFIG_FAULT": {"code": 0x40, "source": "mali_base_jm_kernel.h:707"},
            "CS_UNRECOVERABLE": {"code": 0x41, "source": "mali_base_jm_kernel.h:708"},
            "CS_ENDPOINT_FAULT": {"code": 0x44, "source": "mali_base_jm_kernel.h:709"},
            "CS_BUS_FAULT": {"code": 0x48, "source": "mali_base_jm_kernel.h:710"},
            "CS_INVALID_INSTRUCTION": {"code": 0x49, "source": "mali_base_jm_kernel.h:711"},
            "CS_CALL_STACK_OVERFLOW": {"code": 0x4A, "source": "mali_base_jm_kernel.h:712"},
            "CS_INHERIT_FAULT": {"code": 0x4B, "source": "mali_base_jm_kernel.h:713"},
            "INSTR_INVALID_PC": {"code": 0x50, "source": "mali_base_jm_kernel.h:716"},
            "INSTR_INVALID_ENC": {"code": 0x51, "source": "mali_base_jm_kernel.h:717"},
            "INSTR_BARRIER_FAULT": {"code": 0x55, "source": "mali_base_jm_kernel.h:719"},
            "DATA_INVALID_FAULT": {"code": 0x58, "source": "mali_base_jm_kernel.h:721"},
            "TILE_RANGE_FAULT": {"code": 0x59, "source": "mali_base_jm_kernel.h:722"},
            "ADDR_RANGE_FAULT": {"code": 0x5A, "source": "mali_base_jm_kernel.h:723"},
            "IMPRECISE_FAULT": {"code": 0x5B, "source": "mali_base_jm_kernel.h:724"},
            "OUT_OF_MEMORY": {"code": 0x60, "source": "mali_base_jm_kernel.h:726"},
            "GPU_BUS_FAULT": {"code": 0x80, "source": "mali_base_jm_kernel.h:794"},
            "GPU_SHAREABILITY_FAULT": {
                "code": 0x88,
                "source": "mali_base_jm_kernel.h:798",
                "description": "A cache has detected that the same line has been accessed as both shareable and non-shareable memory from inside the GPU.",
            },
            "SYSTEM_SHAREABILITY_FAULT": {
                "code": 0x89,
                "source": "mali_base_jm_kernel.h:802",
                "description": "A cache has detected that the same line has been accessed as both shareable and non-shareable memory from outside the GPU.",
            },
            "GPU_CACHE_WB_BUS_FAULT": {"code": 0x8A, "source": "mali_base_jm_kernel.h:806"},
            "TRANSLATION_FAULT_0": {"code": 0xC0, "source": "mali_base_jm_kernel.h:811"},
            "TRANSLATION_FAULT_1": {"code": 0xC1, "source": "mali_base_jm_kernel.h:812"},
            "TRANSLATION_FAULT_2": {"code": 0xC2, "source": "mali_base_jm_kernel.h:813"},
            "TRANSLATION_FAULT_3": {"code": 0xC3, "source": "mali_base_jm_kernel.h:814"},
            "TRANSLATION_FAULT_4": {"code": 0xC4, "source": "mali_base_jm_kernel.h:815"},
            "PERMISSION_FAULT_0": {"code": 0xC8, "source": "mali_base_jm_kernel.h:816"},
            "ACCESS_FLAG_0": {"code": 0xD8, "source": "mali_base_jm_kernel.h:822"},
            "ADDRESS_SIZE_FAULT_IN": {"code": 0xE0, "source": "mali_base_jm_kernel.h:828"},
            "MEMORY_ATTRIBUTE_FAULT_0": {"code": 0xE8, "source": "mali_base_jm_kernel.h:834"},
        },
        "csf_user_io_offsets": {
            "CS_INSERT_LO": {"page": 1, "offset": 0x0000, "source": "mali_kbase_csf_registers.h:112"},
            "CS_TILER_HEAP_START_LO": {"page": 1, "offset": 0x0040, "source": "mali_kbase_csf_registers.h:122"},
            "CS_TILER_HEAP_END_LO": {"page": 1, "offset": 0x0048, "source": "mali_kbase_csf_registers.h:126"},
            "CS_EXTRACT_LO": {"page": 2, "offset": 0x0000, "source": "mali_kbase_csf_registers.h:144"},
            "CS_ACTIVE": {"page": 2, "offset": 0x0004, "source": "mali_kbase_csf_registers.h:148"},
            "CS_FAULT": {"page": 2, "offset": 0x0008, "source": "mali_kbase_csf_registers.h:158"},
            "CS_FATAL": {"page": 2, "offset": 0x0010, "source": "mali_kbase_csf_registers.h:160"},
            "CS_FAULT_INFO_LO": {"page": 2, "offset": 0x0018, "source": "mali_kbase_csf_registers.h:162"},
            "CS_FATAL_INFO_LO": {"page": 2, "offset": 0x0020, "source": "mali_kbase_csf_registers.h:166"},
            "CS_HEAP_ADDRESS_LO": {"page": 2, "offset": 0x0040, "source": "mali_kbase_csf_registers.h:170"},
            "CS_HEAP_VT_START": {"page": 2, "offset": 0x0048, "source": "mali_kbase_csf_registers.h:174"},
            "CS_HEAP_VT_END": {"page": 2, "offset": 0x004C, "source": "mali_kbase_csf_registers.h:176"},
            "CS_HEAP_FRAG_END": {"page": 2, "offset": 0x0050, "source": "mali_kbase_csf_registers.h:178"},
        },
        "kbase_memory_rules": [
            {
                "id": "MEM_RULE_01_VALID_PROTECTION",
                "source": "mali_kbase_mem.c:2362 (kbase_check_alloc_flags)",
                "constraint": "(flags & (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_GPU_EX)) != 0",
                "description": "At least one CPU or GPU protection bit must be set.",
            },
            {
                "id": "MEM_RULE_02_WX_EXCLUSION",
                "source": "mali_kbase_mem.c:2366 (kbase_check_alloc_flags)",
                "constraint": "!((flags & BASE_MEM_PROT_GPU_EX) && (flags & BASE_MEM_PROT_GPU_WR))",
                "description": "GPU executable pages cannot be GPU writable (W^X).",
            },
            {
                "id": "MEM_RULE_03_SAME_VA_NO_GPU_EX",
                "source": "mali_kbase_reg_track.c:551",
                "constraint": "!((flags & BASE_MEM_SAME_VA) && (flags & BASE_MEM_PROT_GPU_EX))",
                "description": "Executable BOs live in EXEC_VA_ZONE (1ULL << 47) and cannot use BASE_MEM_SAME_VA.",
            },
            {
                "id": "MEM_RULE_04_COHERENT_SYSTEM_REQUIRES_SHARE_BOTH",
                "source": "mali_kbase_mem.c:2372 (kbase_check_alloc_flags)",
                "constraint": "!(flags & BASE_MEM_COHERENT_SYSTEM) || kbdev->system_coherency == COHERENCY_ACE",
                "description": "BASE_MEM_COHERENT_SYSTEM is rejected on non-ACE SoCs (such as MT6878 where system_coherency == COHERENCY_NONE == 31).",
            },
            {
                "id": "MEM_RULE_05_CSF_EVENT_NO_COHERENT_LOCAL",
                "source": "mali_kbase_mem_linux.c:544 (kbase_mem_alloc)",
                "constraint": "!((flags & BASE_MEM_CSF_EVENT) && (flags & (BASE_MEM_COHERENT_LOCAL | BASE_MEM_COHERENT_SYSTEM_REQUIRED)))",
                "description": "BASE_MEM_CSF_EVENT cannot be combined with BASE_MEM_COHERENT_LOCAL or BASE_MEM_COHERENT_SYSTEM_REQUIRED.",
            },
            {
                "id": "MEM_RULE_06_CSF_EVENT_REQUIRES_SAME_VA_CPU_RW",
                "source": "mali_kbase_mem_linux.c:538 (kbase_mem_alloc)",
                "constraint": "!(flags & BASE_MEM_CSF_EVENT) || ((flags & (BASE_MEM_SAME_VA | BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR)) == (BASE_MEM_SAME_VA | BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR))",
                "description": "BASE_MEM_CSF_EVENT requires BASE_MEM_SAME_VA | BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR and rejects GROW_ON_GPF, TILER_ALIGN_TOP, PROTECTED, andUNCACHED_GPU.",
            },
            {
                "id": "MEM_RULE_07_CSF_EVENT_STRIPS_GPU_CACHED_AND_SHARE_IN",
                "source": "mali_kbase_mem.c:2500-2516 (kbase_set_custom_flags)",
                "constraint": "On non-CPU-coherent SoCs (!kbase_device_is_cpu_coherent), BASE_MEM_CSF_EVENT clears KBASE_REG_GPU_CACHED and KBASE_REG_SHARE_IN | KBASE_REG_SHARE_BOTH, setting AS_MEMATTR_INDEX_NON_CACHEABLE.",
                "description": "Consequently, BASE_MEM_CSF_EVENT BOs cannot be aliased via KBASE_IOCTL_MEM_ALIAS and must never be accessed with mixed shareability (e.g. LOAD_MULTIPLE/STORE_MULTIPLE + SYNC_ADD.system on the same 64B cache line).",
            },
            {
                "id": "MEM_RULE_08_MEM_ALIAS_REQUIRES_GPU_CACHED",
                "source": "mali_kbase_mem_linux.c:2076-2085 (kbase_mem_alias)",
                "constraint": "(aliasing_reg->flags & KBASE_REG_GPU_CACHED) != 0 && !(aliasing_reg->flags & KBASE_REG_DONT_NEED) && !(aliasing_reg->flags & KBASE_REG_GROWABLE)",
                "description": "KBASE_IOCTL_MEM_ALIAS rejects any target BO that lacks KBASE_REG_GPU_CACHED (including any BASE_MEM_CSF_EVENT or BASE_MEM_UNCACHED_GPU BO) with -ENOMEM (errno=12).",
            },
        ],
    }
    return spec


def generate_header(spec):
    lines = [
        "/* ==========================================================================",
        " * AUTO-GENERATED FILE — DO NOT EDIT MANUALLY",
        " * Generated by: scripts/gen_mali_hw_csf_kbase_spec.py",
        " * Sources of truth:",
        f" *   - Mesa genxml: {spec['provenance']['mesa_genxml_v10']}, {spec['provenance']['mesa_genxml_v11']}",
        f" *   - Kbase r44p1: {spec['provenance']['kbase_r44p1_mem_c']}, {spec['provenance']['kbase_r44p1_mem_linux_c']}",
        f" *   - Kbase CSF:   {spec['provenance']['kbase_r44p1_csf_registers_h']}, {spec['provenance']['kbase_r44p1_jm_kernel_h']}",
        " * ========================================================================== */",
        "#ifndef MALI_CSF_KBASE_SPEC_AUTOGEN_H",
        "#define MALI_CSF_KBASE_SPEC_AUTOGEN_H",
        "",
        "#include <stdint.h>",
        "#include <stdbool.h>",
        "",
        "/* --------------------------------------------------------------------------",
        " * 1. Valhall / 5th-Gen CSF Opcodes (bits [63:56], from v10.xml / v11.xml)",
        " * -------------------------------------------------------------------------- */",
    ]

    opcodes = spec["csf_enums"].get("CS Opcode", {})
    for name, val in sorted(opcodes.items(), key=lambda kv: kv[1]):
        macro = f"MALI_CSF_OP_{c_ident(name)}"
        lines.append(f"#define {macro:<42} UINT64_C(0x{val:02x}) /* {val} */")

    lines += [
        "",
        "/* --------------------------------------------------------------------------",
        " * 2. CSF Enums (CS Sync Scope, CS Heap Operation, CS Condition, CS Flush Mode)",
        " * -------------------------------------------------------------------------- */",
    ]
    for ename in [
        "CS Sync Scope",
        "CS Heap Operation",
        "CS Condition",
        "CS Flush Mode",
        "CS Other Flush Mode",
        "CS Defer Mode",
        "CS Sync Error Propagate",
    ]:
        ev = spec["csf_enums"].get(ename, {})
        prefix = f"MALI_{c_ident(ename)}"
        for k, v in sorted(ev.items(), key=lambda kv: kv[1]):
            macro = f"{prefix}_{c_ident(k)}"
            lines.append(f"#define {macro:<52} UINT64_C({v})")

    lines += [
        "",
        "/* --------------------------------------------------------------------------",
        " * 3. CSF Instruction Field Bit Shifts & Masks (from v10.xml / v11.xml <ins>)",
        " * -------------------------------------------------------------------------- */",
    ]
    key_insns = [
        "MOVE48",
        "MOVE32",
        "WAIT",
        "CALL",
        "LOAD_MULTIPLE",
        "STORE_MULTIPLE",
        "FINISH_FRAGMENT",
        "FLUSH_CACHE2",
        "SYNC_ADD32",
        "SYNC_SET32",
        "SYNC_WAIT32",
        "SYNC_ADD64",
        "SYNC_SET64",
        "SYNC_WAIT64",
        "HEAP_SET",
        "HEAP_OPERATION",
        "SET_STATE",
        "SET_STATE_IMM32",
        "ERROR_BARRIER",
    ]
    for iname in key_insns:
        info = spec["csf_instructions"].get(iname)
        if not info:
            continue
        iprefix = f"MALI_CSF_INS_{c_ident(iname)}"
        lines.append(f"/* {iname} (opcode = 0x{info['opcode']:02x} = {info['opcode']}, {info['source']}) */")
        for f in info["fields"]:
            if f["name"] == "Opcode":
                continue
            fname = c_ident(f["name"])
            start = f["start"]
            size = f["size"]
            mask = ((1 << size) - 1) << start
            lines.append(f"#define {iprefix}_{fname}_SHIFT {start}U")
            lines.append(f"#define {iprefix}_{fname}_MASK  UINT64_C(0x{mask:016x})")

    lines += [
        "",
        "/* --------------------------------------------------------------------------",
        " * 4. CSF Scoreboard Slot Conventions in PanVK (src/panfrost/vulkan/csf/)",
        " * -------------------------------------------------------------------------- */",
        "#define MALI_PANVK_SB_ID_LS                        0U",
        "#define MALI_PANVK_SB_ID_DEFERRED_SYNC             1U",
        "#define MALI_PANVK_SB_ID_DEFERRED_FLUSH            2U",
        "#define MALI_PANVK_SB_ID_IMM_FLUSH                 0U",
        "#define MALI_PANVK_SB_MASK_LS                      UINT16_C(1U << MALI_PANVK_SB_ID_LS)",
        "#define MALI_PANVK_SB_MASK_DEFERRED_SYNC           UINT16_C(1U << MALI_PANVK_SB_ID_DEFERRED_SYNC)",
        "",
        "/* --------------------------------------------------------------------------",
        " * 5. PanVK struct panvk_cs_subqueue_context Offsets (panvk_cmd_buffer.h:149)",
        " * -------------------------------------------------------------------------- */",
        "#define MALI_PANVK_SUBQ_CTX_SYNCOBJS_OFFSET        0U   /* uint64_t syncobjs */",
        "#define MALI_PANVK_SUBQ_CTX_ITER_SB_OFFSET         8U   /* uint32_t iter_sb / pad */",
        "#define MALI_PANVK_SUBQ_CTX_LAST_ERROR_OFFSET      12U  /* uint32_t last_error */",
        "#define MALI_PANVK_SUBQ_CTX_REG_DUMP_ADDR_OFFSET   16U  /* uint64_t reg_dump_addr */",
        "#define MALI_PANVK_SUBQ_CTX_RINGBUF_SYNCOBJ_OFFSET 24U  /* uint64_t render.desc_ringbuf.syncobj */",
        "#define MALI_PANVK_SUBQ_CTX_RINGBUF_PTR_OFFSET     32U  /* uint64_t render.desc_ringbuf.ptr */",
        "#define MALI_PANVK_SUBQ_CTX_RINGBUF_POS_OFFSET     40U  /* uint32_t render.desc_ringbuf.pos */",
        "#define MALI_PANVK_SUBQ_CTX_TILER_HEAP_OFFSET      48U  /* uint64_t render.tiler_heap */",
        "#define MALI_PANVK_SUBQ_CTX_GEOM_BUF_OFFSET        56U  /* uint64_t render.geom_buf */",
        "#define MALI_PANVK_RENDER_DESC_RINGBUF_SIZE        (512U * 1024U)",
        "",
        "/* --------------------------------------------------------------------------",
        " * 6. Kbase CSF USER_IO Page Offsets (mali_kbase_csf_registers.h)",
        " * -------------------------------------------------------------------------- */",
    ]
    for rname, rinfo in spec["csf_user_io_offsets"].items():
        lines.append(
            f"#define MALI_KBASE_USER_IO_{rname:<24} 0x{rinfo['offset']:04x}U /* page {rinfo['page']}, {rinfo['source']} */"
        )

    lines += [
        "",
        "/* --------------------------------------------------------------------------",
        " * 7. Kbase CSF / GPU Fault & Fatal Exception Codes (mali_base_jm_kernel.h)",
        " * -------------------------------------------------------------------------- */",
    ]
    for ename, einfo in spec["csf_exception_types"].items():
        lines.append(
            f"#define MALI_KBASE_EXC_{ename:<28} 0x{einfo['code']:02x}U /* {einfo['source']} */"
        )

    lines += [
        "",
        "#endif /* MALI_CSF_KBASE_SPEC_AUTOGEN_H */",
        "",
    ]
    return "\n".join(lines)


def main():
    os.makedirs(os.path.join(ROOT_DIR, "specs"), exist_ok=True)
    os.makedirs(os.path.join(ROOT_DIR, "include"), exist_ok=True)
    spec = build_spec()
    json_path = os.path.join(ROOT_DIR, "specs", "mali_csf_kbase_spec.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(spec, f, indent=2)
    hdr_path = os.path.join(ROOT_DIR, "include", "mali_csf_kbase_spec_autogen.h")
    with open(hdr_path, "w", encoding="utf-8") as f:
        f.write(generate_header(spec))
    print(f"Generated {json_path} ({len(spec['csf_instructions'])} CSF instructions) and {hdr_path}")


if __name__ == "__main__":
    main()
