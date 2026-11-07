#pragma once
// ============================================================================
// mali0_emu_uapi.h
// Standalone /dev/mali0 (mali_kbase CSF & Job Manager) + /dev/dma_heap UAPI
// Definitions, Framebuffer (MFBD) & IDVS Draw Descriptors, and C Hook API
// ============================================================================
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Section 1: Mali /dev/mali0 (kbase CSF) UAPI Constants & Structures
// ============================================================================
static constexpr uint64_t KBASE_PAGE_SHIFT = 12ULL;
static constexpr uint64_t KBASE_PAGE_SIZE  = 4096ULL;

// Special mmap handles from mali_base_common_kernel.h & mali_base_csf_kernel.h
static constexpr uint64_t BASE_MEM_MAP_TRACKING_HANDLE       = (3ULL  << KBASE_PAGE_SHIFT); // 0x3000
static constexpr uint64_t BASEP_MEM_WRITE_ALLOC_PAGES_HANDLE = (4ULL  << KBASE_PAGE_SHIFT); // 0x4000
static constexpr uint64_t BASEP_MEM_CSF_USER_REG_PAGE_HANDLE = (47ULL << KBASE_PAGE_SHIFT); // 0x2f000
static constexpr uint64_t BASEP_MEM_CSF_USER_IO_PAGES_HANDLE = (48ULL << KBASE_PAGE_SHIFT); // 0x30000
static constexpr uint64_t BASE_MEM_COOKIE_BASE               = (1024ULL << KBASE_PAGE_SHIFT); // 0x400000

// Memory allocation flags from mali_base_common_kernel.h
static constexpr uint64_t BASE_MEM_PROT_CPU_RD = (1ULL << 0);
static constexpr uint64_t BASE_MEM_PROT_CPU_WR = (1ULL << 1);
static constexpr uint64_t BASE_MEM_PROT_GPU_RD = (1ULL << 2);
static constexpr uint64_t BASE_MEM_PROT_GPU_WR = (1ULL << 3);
static constexpr uint64_t BASE_MEM_PROT_GPU_EX = (1ULL << 4);
static constexpr uint64_t BASE_MEM_GROW_ON_GPF = (1ULL << 9);
static constexpr uint64_t BASE_MEM_COHERENT_SYSTEM = (1ULL << 10);
static constexpr uint64_t BASE_MEM_COHERENT_LOCAL  = (1ULL << 11);
static constexpr uint64_t BASE_MEM_CACHED_CPU  = (1ULL << 12);
static constexpr uint64_t BASE_MEM_SAME_VA     = (1ULL << 13);
static constexpr uint64_t BASE_MEM_NEED_MMAP   = (1ULL << 14);
static constexpr uint64_t BASE_MEM_CSF_EVENT   = (1ULL << 19);

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

// UAPI ioctl structs matching kbase_uapi.h & kbase_csf_uapi.h
struct KbaseIoctlVersionCheck {
    uint16_t major;
    uint16_t minor;
};

struct KbaseIoctlSetFlags {
    uint32_t create_flags;
};

struct KbaseIoctlGetGpuprops {
    uint64_t buffer;
    uint32_t size;
    uint32_t flags;
};

union KbaseIoctlMemAlloc {
    struct {
        uint64_t va_pages;
        uint64_t commit_pages;
        uint64_t extension;
        uint64_t flags;
    } in;
    struct {
        uint64_t flags;
        uint64_t gpu_va;
    } out;
};

union KbaseIoctlMemAllocEx {
    struct {
        uint64_t va_pages;
        uint64_t commit_pages;
        uint64_t extension;
        uint64_t flags;
        uint64_t fixed_address;
        uint64_t extra[3];
    } in;
    struct {
        uint64_t flags;
        uint64_t gpu_va;
    } out;
};

union KbaseIoctlMemQuery {
    struct {
        uint64_t gpu_addr;
        uint64_t query;
    } in;
    struct {
        uint64_t value;
    } out;
};

struct KbaseIoctlMemFree {
    uint64_t gpu_addr;
};

struct KbaseIoctlMemCommit {
    uint64_t gpu_addr;
    uint64_t pages;
};

union KbaseIoctlMemAlias {
    struct {
        uint64_t flags;
        uint64_t stride;
        uint64_t nents;
        uint64_t aliasing_info;
    } in;
    struct {
        uint64_t flags;
        uint64_t gpu_va;
        uint64_t va_pages;
    } out;
};

struct KbaseIoctlMemSync {
    uint64_t handle;
    uint64_t user_addr;
    uint64_t size;
    uint8_t  type;
    uint8_t  padding[7];
};

struct BasepCsStreamControl {
    uint32_t features;
    uint32_t padding;
};

struct BasepCsGroupControl {
    uint32_t features;
    uint32_t stream_num;
    uint32_t suspend_size;
    uint32_t padding;
};

union KbaseIoctlCsGetGlbIface {
    struct {
        uint32_t max_group_num;
        uint32_t max_total_stream_num;
        uint64_t groups_ptr;
        uint64_t streams_ptr;
    } in;
    struct {
        uint32_t glb_version;
        uint32_t features;
        uint32_t group_num;
        uint32_t max_total_stream_num;
        uint32_t total_stream_num;
        uint32_t instr_features;
    } out;
};

struct KbaseIoctlCsQueueRegister {
    uint64_t buffer_gpu_addr;
    uint32_t buffer_size;
    uint8_t  priority;
    uint8_t  padding[3];
};

struct KbaseIoctlCsQueueKick {
    uint64_t buffer_gpu_addr;
};

union KbaseIoctlCsQueueBind {
    struct {
        uint64_t buffer_gpu_addr;
        uint8_t  group_handle;
        uint8_t  csi_index;
        uint8_t  padding[6];
    } in;
    struct {
        uint64_t mmap_handle;
    } out;
};

union KbaseIoctlCsQueueGroupCreate {
    struct {
        uint64_t tiler_mask;
        uint64_t fragment_mask;
        uint64_t compute_mask;
        uint8_t  cs_min;
        uint8_t  priority;
        uint8_t  tiler_max;
        uint8_t  fragment_max;
        uint8_t  compute_max;
        uint8_t  csi_handlers;
        uint8_t  neural_max;
        uint8_t  padding;
        uint64_t d_vs_buf;
        uint64_t neural_mask;
        uint64_t reserved[8];
    } in;
    struct {
        uint8_t  group_handle;
        uint8_t  padding[3];
        uint32_t group_uid;
        uint64_t user_io_gpu_va;
    } out;
    uint8_t raw_storage[128];
};

union KbaseIoctlCsTilerHeapInit {
    struct {
        uint32_t chunk_size;
        uint32_t initial_chunks;
        uint32_t max_chunks;
        uint16_t target_in_flight;
        uint8_t  group_id;
        uint8_t  padding;
        uint64_t buf_desc_va;
    } in;
    struct {
        uint64_t gpu_heap_va;
        uint64_t first_chunk_va;
    } out;
};

struct KbaseIoctlCsTilerHeapTerm {
    uint64_t gpu_heap_va;
};

struct KbaseIoctlKcpuQueueNew {
    uint8_t id;
    uint8_t padding[7];
};

struct KbaseIoctlKcpuQueueDelete {
    uint8_t id;
    uint8_t padding[7];
};

struct KbaseIoctlKcpuQueueEnqueue {
    uint64_t addr;
    uint32_t nr_commands;
    uint8_t  id;
    uint8_t  padding[3];
};


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
static constexpr uint32_t BASE_MEM_IMPORT_TYPE_UMM          = 2U; // DMA-BUF fd
static constexpr uint32_t BASE_MEM_IMPORT_TYPE_USER_BUFFER  = 3U;

struct DmaHeapAllocationData {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};

struct KbaseIoctlMemImport {
    union {
        struct {
            uint64_t flags;
            uint64_t phandle;
            uint32_t type;
            uint32_t padding;
        } in;
        struct {
            uint64_t flags;
            uint64_t gpu_va;
            uint64_t va_pages;
        } out;
    };
};

// ============================================================================
// Section 1D: Multi-Target Framebuffer Descriptor (MFBD) & IDVS 3D Draw Packet
// Matches panfrost/genxml/v9.xml (Valhall) & v7.xml (Bifrost) MFBD + IDVS layout
// ============================================================================
static constexpr uint32_t MALI_MFBD_MAGIC = 0x4D464244U; // 'MFBD'
static constexpr uint32_t MALI_IDVS_MAGIC = 0x49445653U; // 'IDVS'

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

struct MaliCubeVertex {
    float px, py, pz, pw;        // 3D object-space position (x, y, z, 1.0)
    float cr, cg, cb, ca;        // Vertex RGBA color
    float nx, ny, nz, nw;        // Vertex / face normal
};

struct MaliIdvsDrawPacket {
    uint32_t magic;              // MALI_IDVS_MAGIC ('IDVS')
    uint32_t vertex_count;       // Number of vertices (e.g., 8 or 24 for cube)
    uint32_t index_count;        // Number of indices (36 for 12 triangles)
    uint32_t instance_count;     // Number of instances (1)
    uint32_t frame_index;        // Frame number (0, 1, ...)
    float    rotation_deg;       // Cube rotation angle in degrees
    uint64_t vertex_bo_gpu_va;   // MaliCubeVertex[] BO GPU VA
    uint64_t index_bo_gpu_va;    // uint32_t indices[36] BO GPU VA
    uint64_t mvp_ubo_gpu_va;     // float mvp[16] + light_dir[4] UBO/FAU GPU VA
    uint64_t mfbd_gpu_va;        // MaliMfbdDescriptor GPU VA
    uint64_t vs_spd_gpu_va;      // Vertex Shader SPD/RSD GPU VA
    uint64_t fs_spd_gpu_va;      // Fragment Shader SPD/RSD GPU VA
};

// Configurable GPU Hardware Profiles for /dev/mali0 Emulation
enum MaliGpuProfileId : uint32_t {
    MALI_PROFILE_G76_JM   = 0x7212U, // Bifrost Arch 7.2.1 (JM, r26p0)
    MALI_PROFILE_G78_JM   = 0x9091U, // Valhall Arch 9.0.9 (JM, r49p1_v4)
    MALI_PROFILE_G615_CSF = 0xB883U, // Valhall Arch 11.8.3 (CSF, r54p3)
    MALI_PROFILE_G720_CSF = 0xC880U, // 5th-Gen Arch 12.8.0 (CSF, r54p3)
    MALI_PROFILE_G925_CSF = 0xD880U  // 5th-Gen Arch 13.8.0 (CSF, r49p1_v5)
};

// C ABI Hooks for standalone / LD_PRELOAD usage
uint64_t mali0_emu_open(const char* path, uint64_t flags);
uint64_t mali0_emu_close(uint64_t fd);
uint64_t mali0_emu_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg);
uint64_t mali0_emu_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset);
uint64_t mali0_emu_munmap(uint64_t addr, uint64_t length);
uint64_t mali0_emu_read(uint64_t fd, uint64_t buf_addr, uint64_t count);
uint64_t mali0_emu_poll(uint64_t fds_addr, uint64_t nfds, uint64_t timeout);

#ifdef __cplusplus
}
#endif
