#pragma once
// ============================================================================
// mali_csf_uapi_types.h
// Mali /dev/mali0 (kbase CSF & Job Manager) + /dev/dma_heap UAPI extensions
// and Framebuffer (MFBD) / GPU Profile types for r54p3_vulkan_icd.
// Authoritative kbase UAPI constants & ioctl structs come from mali_kbase_uapi.h.
// ============================================================================
#include <cstdint>
#include <cstddef>
#include "mali_kbase_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Section 1: Mali /dev/mali0 (kbase CSF) ICD Convenience Aliases & Ring Offsets
// ============================================================================
static constexpr uint64_t KBASE_PAGE_SHIFT = 12ULL;
static constexpr uint64_t KBASE_PAGE_SIZE  = 4096ULL;

// CSF USER_IO register offsets from mali_kbase_csf_registers.h
static constexpr uint32_t CS_INSERT_LO       = 0x0000;
static constexpr uint32_t CS_INSERT_HI       = 0x0004;
static constexpr uint32_t CS_EXTRACT_INIT_LO = 0x0008;
static constexpr uint32_t CS_EXTRACT_INIT_HI = 0x000C;
static constexpr uint32_t CS_EXTRACT_LO      = 0x0000;
static constexpr uint32_t CS_EXTRACT_HI      = 0x0004;
static constexpr uint32_t CS_ACTIVE          = 0x0008;

// CSF Command Stream 64-bit Instruction Opcodes from panfrost/genxml/v10.xml
enum MaliCsOpcode : uint8_t {
    MALI_CS_OPCODE_NOP                  = 0,
    MALI_CS_OPCODE_MOVE48               = 1,
    MALI_CS_OPCODE_MOVE32               = 2,
    MALI_CS_OPCODE_WAIT                 = 3,
    MALI_CS_OPCODE_RUN_COMPUTE          = 4,
    MALI_CS_OPCODE_RUN_TILING           = 5,
    MALI_CS_OPCODE_RUN_IDVS             = 6,
    MALI_CS_OPCODE_RUN_FRAGMENT         = 7,
    MALI_CS_OPCODE_RUN_FULLSCREEN       = 8,
    MALI_CS_OPCODE_FINISH_TILING        = 9,
    MALI_CS_OPCODE_FINISH_FRAGMENT      = 11,
    MALI_CS_OPCODE_ADD_IMMEDIATE32      = 16,
    MALI_CS_OPCODE_ADD_IMMEDIATE64      = 17,
    MALI_CS_OPCODE_COMPARE_SELECT32     = 18,
    MALI_CS_OPCODE_LOAD_MULTIPLE        = 20,
    MALI_CS_OPCODE_STORE_MULTIPLE       = 21,
    MALI_CS_OPCODE_BRANCH               = 22,
    MALI_CS_OPCODE_SET_SB_ENTRY         = 23,
    MALI_CS_OPCODE_PROGRESS_WAIT        = 24,
    MALI_CS_OPCODE_SET_EXCEPTION_HANDLER = 25,
    MALI_CS_OPCODE_CALL                 = 32,
    MALI_CS_OPCODE_JUMP                 = 33,
    MALI_CS_OPCODE_REQ_RESOURCE         = 34,
    MALI_CS_OPCODE_FLUSH_CACHE2         = 36,
    MALI_CS_OPCODE_SYNC_ADD32           = 37,
    MALI_CS_OPCODE_SYNC_SET32           = 38,
    MALI_CS_OPCODE_SYNC_WAIT32          = 39,
    MALI_CS_OPCODE_STORE_STATE          = 40,
    MALI_CS_OPCODE_RUN_COMPUTE_INDIRECT = 44,
    MALI_CS_OPCODE_ERROR_BARRIER        = 47,
    MALI_CS_OPCODE_HEAP_SET             = 48,
    MALI_CS_OPCODE_HEAP_OPERATION       = 49,
    MALI_CS_OPCODE_SYNC_ADD64           = 51,
    MALI_CS_OPCODE_SYNC_SET64           = 52,
    MALI_CS_OPCODE_SYNC_WAIT64          = 53,
};

// CamelCase aliases onto authoritative include/mali_kbase_uapi.h structs
typedef struct kbase_ioctl_version_check       KbaseIoctlVersionCheck;
typedef struct kbase_ioctl_set_flags           KbaseIoctlSetFlags;
typedef struct kbase_ioctl_get_gpuprops        KbaseIoctlGetGpuprops;
typedef union  kbase_ioctl_mem_alloc           KbaseIoctlMemAlloc;
typedef union  kbase_ioctl_mem_alloc_ex        KbaseIoctlMemAllocEx;
typedef union  kbase_ioctl_mem_query           KbaseIoctlMemQuery;
typedef struct kbase_ioctl_mem_free            KbaseIoctlMemFree;
typedef struct kbase_ioctl_mem_commit          KbaseIoctlMemCommit;
typedef union  kbase_ioctl_mem_alias           KbaseIoctlMemAlias;
typedef struct kbase_ioctl_mem_sync            KbaseIoctlMemSync;
typedef union  kbase_ioctl_mem_import          KbaseIoctlMemImport;
typedef union  kbase_ioctl_cs_get_glb_iface    KbaseIoctlCsGetGlbIface;
typedef struct kbase_ioctl_cs_queue_register   KbaseIoctlCsQueueRegister;
typedef struct kbase_ioctl_cs_queue_kick       KbaseIoctlCsQueueKick;
typedef union  kbase_ioctl_cs_queue_bind       KbaseIoctlCsQueueBind;
typedef union  kbase_ioctl_cs_tiler_heap_init  KbaseIoctlCsTilerHeapInit;
typedef struct kbase_ioctl_cs_tiler_heap_term  KbaseIoctlCsTilerHeapTerm;
typedef struct kbase_ioctl_kcpu_queue_new      KbaseIoctlKcpuQueueNew;
typedef struct kbase_ioctl_kcpu_queue_delete   KbaseIoctlKcpuQueueDelete;
typedef struct kbase_ioctl_kcpu_queue_enqueue  KbaseIoctlKcpuQueueEnqueue;


// ============================================================================
// Section 1B: Mali /dev/mali0 Job Manager (JM) UAPI & Job Descriptor Structures
//             (from gpu.zip: mali_kbase_jm_ioctl.h, mali_base_jm_kernel.h &
//              Archive.zip: kbase_kmod.c, decode_jm.c, v7.xml, v9.xml)
// ============================================================================
static constexpr uint32_t BASE_JD_EVENT_DONE     = 0x01U;
static constexpr uint32_t BASE_JD_EVENT_ERR_MASK = 0x40U;

static constexpr uint32_t BASE_JD_REQ_FS           = (1U << 0);
static constexpr uint32_t BASE_JD_REQ_CS           = (1U << 1);
static constexpr uint32_t BASE_JD_REQ_T            = (1U << 2);
static constexpr uint32_t BASE_JD_REQ_CF           = (1U << 3);
static constexpr uint32_t BASE_JD_REQ_V            = (1U << 4);
static constexpr uint32_t BASE_JD_REQ_ONLY_COMPUTE = (1U << 5);

enum MaliJmJobType : uint8_t {
    MALI_JOB_TYPE_NOT_STARTED    = 0,
    MALI_JOB_TYPE_WRITE_VALUE    = 1,
    MALI_JOB_TYPE_CACHE_FLUSH    = 2,
    MALI_JOB_TYPE_COMPUTE        = 4,
    MALI_JOB_TYPE_VERTEX         = 5,
    MALI_JOB_TYPE_GEOMETRY       = 6,
    MALI_JOB_TYPE_TILER          = 7,
    MALI_JOB_TYPE_FUSED          = 8,
    MALI_JOB_TYPE_FRAGMENT       = 9,
    MALI_JOB_TYPE_INDEXED_VERTEX = 10,
    MALI_JOB_TYPE_MALLOC_VERTEX  = 11,
};

struct BaseJdUdata {
    uint64_t blob[2];
};

struct BaseJdAtomV2 {
    uint64_t    jc;
    BaseJdUdata udata;
    uint64_t    extres_list;
    uint16_t    nr_extres;
    uint8_t     jit_id[2];
    struct {
        uint8_t atom_id;
        uint8_t dependency_type;
    } pre_dep[2];
    uint8_t     atom_number;
    uint8_t     prio;
    uint8_t     device_nr;
    uint8_t     jobslot;
    uint32_t    core_req;
    uint8_t     renderpass_id;
    uint8_t     padding[15];
};

struct BaseJdEventV2 {
    uint32_t    event_code;
    uint8_t     atom_number;
    uint8_t     padding[3];
    BaseJdUdata udata;
};

struct KbaseIoctlJobSubmit {
    uint64_t addr;
    uint32_t nr_atoms;
    uint32_t stride;
};

struct KbaseIoctlSoftEventUpdate {
    uint64_t event;
    uint32_t new_status;
    uint32_t flags;
};

// 32-byte Mali Job Manager Job Header (panfrost/genxml/v7.xml & v9.xml)
struct MaliJmJobHeader {
    uint32_t exception_status;       // +0x00: set to 0x01 (COMPLETE) by GPU on completion
    uint32_t first_incomplete_task;  // +0x04
    uint64_t fault_pointer;          // +0x08
    uint32_t flags_and_index;        // +0x10: bit[0]=is_64b, bits[7:1]=type, bit[8]=barrier, bits[31:16]=index
    uint32_t dependencies;           // +0x14: bits[15:0]=dep1, bits[31:16]=dep2
    uint64_t next;                   // +0x18: GPU VA of next Job Descriptor in chain (or 0)
};

// ============================================================================
// Section 2: Valhall (Mali-G615) 64-Bit ISA OBJC -> SPIR-V 1.3 Translator
// ============================================================================
// Matches panfrost/compiler/bifrost/valhall/ISA.xml & kraid/isa-v9-v14.xml:
//   - Every Valhall instruction is a 64-bit word:
//     * bits[7:0]   = src0 (0x00..0x3F = r0..r63, 0x80..0xBF = u0..u63, 0xC0..0xDF = va_immediates[0..31])
//     * bits[15:8]  = src1 / offset_lo / imm
//     * bits[23:16] = src2 / opcode2
//     * bits[45:40] = dest_register_index / staging_register_index (r0..r63)
//     * bits[47:46] = dest_width / staging_register_control_32
//     * bits[56:48] = 9-bit primary opcode (instruction_class[56:52] | opcode1[51:48])
//     * bits[58:57] = fau_page_index
//     * bits[62:59] = flow control (0=none, 1=wait0, 2=wait1, 7=wait012, 10=reconverge, 15=end)


// ============================================================================
// Section 1C: /dev/dma_heap/system WSI Allocation & KBASE_IOCTL_MEM_IMPORT
// ============================================================================
static constexpr uint32_t DMA_HEAP_IOCTL_ALLOC              = 0xc0184800U;

struct DmaHeapAllocationData {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};

// ============================================================================
// Section 1D: Multi-Target Framebuffer Descriptor (MFBD)
// Matches panfrost/genxml/v9.xml (Valhall) & v7.xml (Bifrost) MFBD layout
// ============================================================================
static constexpr uint32_t MALI_MFBD_MAGIC = 0x4D464244U; // 'MFBD'

struct MaliMfbdDescriptor {
    uint32_t magic;              // MALI_MFBD_MAGIC ('MFBD')
    uint32_t width;              // Framebuffer width in pixels (e.g., 64)
    uint32_t height;             // Framebuffer height in pixels (e.g., 64)
    uint32_t sample_count;       // MSAA sample count (1)
    uint32_t clear_color_rgba8;  // Clear color (0xAABBGGRR)
    float    clear_depth;        // Clear depth (1.0f)
    uint64_t color_rt0_gpu_va;   // Render Target 0 RGBA8 BO GPU VA (can be imported dma_buf WSI BO)
    uint64_t depth_bo_gpu_va;    // Depth Z32_FLOAT BO GPU VA
    uint64_t tiler_heap_gpu_va;  // CSF/JM Tiler heap chunk GPU VA
    uint64_t stats_bo_gpu_va;    // Optional output stats BO GPU VA
};

// Configurable GPU Hardware Profiles for /dev/mali0 Emulation
enum MaliGpuProfileId : uint32_t {
    MALI_PROFILE_G76_JM   = 0x7212U, // Bifrost Arch 7.2.1 (JM, r26p0)
    MALI_PROFILE_G78_JM   = 0x9091U, // Valhall Arch 9.0.9 (JM, r49p1_v4)
    MALI_PROFILE_G615_CSF = 0xB883U, // Valhall Arch 11.8.3 (CSF, r54p3)
    MALI_PROFILE_G720_CSF = 0xC880U, // 5th-Gen Arch 12.8.0 (CSF, r54p3)
    MALI_PROFILE_G925_CSF = 0xD880U  // 5th-Gen Arch 13.8.0 (CSF, r49p1_v5)
};

#ifdef __cplusplus
}
#endif
