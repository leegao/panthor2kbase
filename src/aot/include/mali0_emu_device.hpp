#pragma once
// ============================================================================
// mali0_emu_device.hpp
// Standalone /dev/mali0 (mali_kbase CSF & Job Manager) + /dev/dma_heap/system
// Kernel Driver Emulator with Compute & 3D Graphics (vkcube) Execution on
// Host Vulkan (Mesa Lavapipe / llvmpipe)
// ============================================================================
#include "mali0_emu_uapi.h"
#include "mali0_emu_isa.hpp"
#include "mali0_emu_lvp.hpp"
#include <sys/mman.h>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <algorithm>

#include <unistd.h>
extern "C" {
    typedef void (*mali_kbase_emu_csf_job_hook_fn)(uint8_t cs_opcode, uint64_t insn, const uint64_t *cs_regs, void *user_data);
    __attribute__((weak)) void mali_kbase_emu_register_csf_job_hook(mali_kbase_emu_csf_job_hook_fn fn, void *user_data);
    struct kbase_emu_stats {
        uint32_t ioctl_count;
        uint32_t mem_alloc_count;
        uint32_t queue_kick_count;
        uint32_t csf_instructions_executed;
        uint32_t csf_calls_followed;
        uint32_t valhall_shaders_executed;
        uint32_t valhall_instructions_executed;
        uint32_t compute_jobs_run;
        uint32_t idvs_jobs_run;
        uint32_t fragment_jobs_run;
        uint32_t lvp_bridges_run;
        uint32_t csg_scope_warnings;
        uint32_t oom_heap_stat_errors;
        uint32_t same_va_munmap_destroys;
        uint32_t unwaited_deferred_sync_errors;
        uint32_t csf_jumps_followed;
        uint32_t lifted_ko_calls_executed;
        uint64_t lifted_ko_insns_executed;
        uint32_t active_profile;
        uint16_t kbase_ver_major;
        uint16_t kbase_ver_minor;
    };
    __attribute__((weak)) void mali_kbase_emu_get_stats(struct kbase_emu_stats *out);
    __attribute__((weak)) int mali_kbase_emu_open_direct(const char *pathname, int flags);
    __attribute__((weak)) int mali_kbase_emu_close_direct(int fd);
    __attribute__((weak)) int mali_kbase_emu_ioctl_direct(int fd, unsigned long request, void *arg);
    __attribute__((weak)) void *mali_kbase_emu_mmap_direct(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
    __attribute__((weak)) int mali_kbase_emu_munmap_direct(void *addr, size_t length);
}

struct KbaseBufferObject {
    uint64_t cookie_or_va = 0;
    uint64_t gpu_va       = 0;
    uint64_t cpu_va       = 0;
    uint64_t va_pages     = 0;
    uint64_t commit_pages = 0;
    uint64_t flags        = 0;
    bool     same_va      = false;
    bool     imported_umm = false;
    int      dmabuf_fd    = -1;
    void*    host_mapping = nullptr;
};

struct DmaBufObject {
    int      fd           = -1;
    uint64_t size_bytes   = 0;
    void*    host_mapping = nullptr;
};

struct KbaseCsfQueue {
    uint64_t buffer_gpu_addr = 0;
    uint32_t buffer_size     = 0;
    uint8_t  priority        = 0;
    uint8_t  group_handle    = 0;
    uint8_t  csi_index       = 0;
    bool     bound           = false;
    uint64_t mmap_handle     = 0;
    uint8_t* user_io_pages   = nullptr; // 3 * 4096B: [0]=Doorbell, [1]=CS_USER_INPUT, [2]=CS_USER_OUTPUT
};

struct KbaseCsfExecutedJob {
    uint8_t     cs_opcode = 0;
    uint64_t    srt_gpu_va = 0;
    uint64_t    spd_gpu_va = 0;
    uint64_t    tsd_gpu_va = 0;
    uint64_t    fau_gpu_va = 0;
    uint64_t    shader_objc_va = 0;
    uint32_t    shader_objc_bytes = 0;
    uint32_t    wg_x = 1, wg_y = 1, wg_z = 1;
    std::vector<ValhallDecodedInstr> disasm;
    std::vector<uint32_t>            lifted_spirv;
    bool        executed_on_lvp = false;
    bool        is_vkcube_graphics_job = false;
};

class MaliKbaseCsfKernelEmulator {
public:
    static constexpr uint32_t MALI_G615_PRODUCT_ID = 0xB883U;
    static constexpr uint64_t MALI_G615_RAW_GPU_ID = 0xB8830001ULL;

    int      virtual_fd     = 100; // /dev/mali0
    int      dma_heap_fd    = 101; // /dev/dma_heap/system
    int      next_dmabuf_fd = 200; // Exported DMA-BUF fds from DMA_HEAP_IOCTL_ALLOC
    uint32_t gpu_product_id = MALI_G615_PRODUCT_ID;
    uint64_t raw_gpu_id     = MALI_G615_RAW_GPU_ID;
    uint16_t uapi_major     = 1;
    uint16_t uapi_minor     = 36;
    bool     csf_mode_active = false;
    uint32_t context_flags   = 0;
    uint64_t jit_va_pages    = 0;
    uint64_t exec_va_pages   = 0;
    uint64_t next_cookie_index = 1;
    uint64_t next_tiler_heap_va = 0x710000000ULL;
    uint8_t  next_group_handle  = 1;
    uint8_t  next_kcpu_id       = 1;

    void*    tracking_page = nullptr;
    void*    user_reg_page = nullptr;

    std::map<int, DmaBufObject>           dmabuf_table;
    std::map<uint64_t, KbaseBufferObject> bos_by_cookie;
    std::map<uint64_t, KbaseBufferObject> bos_by_gpu_va;
    std::map<uint64_t, KbaseCsfQueue>     queues_by_buf_va;
    std::map<uint64_t, uint64_t>          mmap_handle_to_queue_va;

    struct KbaseJmExecutedAtom {
        uint8_t  atom_number = 0;
        uint32_t core_req = 0;
        uint8_t  jm_job_type = 0;
        bool     is_bifrost_clause_isa = false;
        bool     is_vkcube_graphics_atom = false;
        uint64_t jc_gpu_va = 0;
        uint64_t shader_objc_va = 0;
        uint32_t shader_objc_bytes = 0;
        std::vector<ValhallDecodedInstr>  valhall_disasm;
        std::vector<BifrostDecodedClause> bifrost_disasm;
        std::vector<uint32_t>             lifted_spirv;
        bool     executed_on_lvp = false;
    };

    std::vector<std::string>              ioctl_trace;
    std::vector<KbaseCsfExecutedJob>      executed_jobs;
    std::vector<KbaseJmExecutedAtom>      executed_jm_atoms;
    std::vector<VkCubeFrameStats>         executed_vkcube_frames;
    std::vector<BaseJdEventV2>            jm_event_queue;
    HostLvpipeVulkanExecutor              lvp_executor;

    void ensureLiftedKernelBound() {
        if (mali_kbase_emu_register_csf_job_hook) {
            mali_kbase_emu_register_csf_job_hook(&MaliKbaseCsfKernelEmulator::onLiftedCsfJobCallback, this);
        }
    }

    static void onLiftedCsfJobCallback(uint8_t cs_opcode, uint64_t insn, const uint64_t *cs_regs, void *user_data) {
        if (!user_data || !cs_regs) return;
        static_cast<MaliKbaseCsfKernelEmulator*>(user_data)->dispatchCsfJobFromRegs(cs_opcode, insn, cs_regs);
    }

    void dispatchCsfJobFromRegs(uint8_t opc, uint64_t insn, const uint64_t *cs_regs64) {
        auto getReg64 = [&](unsigned d_idx) -> uint64_t {
            if (d_idx + 1 >= 96) return 0;
            return (cs_regs64[d_idx] & 0xFFFFFFFFULL) | ((cs_regs64[d_idx + 1] & 0xFFFFFFFFULL) << 32);
        };
        uint8_t srt_sel = static_cast<uint8_t>((insn >> 40) & 0x3U);
        uint8_t spd_sel = static_cast<uint8_t>((insn >> 42) & 0x3U);
        uint8_t tsd_sel = static_cast<uint8_t>((insn >> 44) & 0x3U);
        uint8_t fau_sel = static_cast<uint8_t>((insn >> 46) & 0x3U);

        KbaseCsfExecutedJob job;
        job.cs_opcode  = opc;
        job.srt_gpu_va = getReg64(srt_sel * 2);
        job.spd_gpu_va = getReg64(8 + spd_sel * 2);
        job.tsd_gpu_va = getReg64(16 + tsd_sel * 2);
        job.fau_gpu_va = getReg64(24 + fau_sel * 2);
        job.wg_x       = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[32]));
        job.wg_y       = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[33]));
        job.wg_z       = std::max<uint32_t>(1U, static_cast<uint32_t>(cs_regs64[34]));

        if (job.spd_gpu_va >= 0x10000ULL) {
            const uint64_t* spd = reinterpret_cast<const uint64_t*>(job.spd_gpu_va);
            job.shader_objc_va    = spd[0];
            job.shader_objc_bytes = static_cast<uint32_t>(spd[1] & 0xFFFFFFFFULL);
        }

        const MaliIdvsDrawPacket* draw_pkt = nullptr;
        if (job.tsd_gpu_va >= 0x10000ULL && *reinterpret_cast<const uint32_t*>(job.tsd_gpu_va) == MALI_IDVS_MAGIC) {
            draw_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(job.tsd_gpu_va);
        } else if (job.srt_gpu_va >= 0x10000ULL && *reinterpret_cast<const uint32_t*>(job.srt_gpu_va) == MALI_IDVS_MAGIC) {
            draw_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(job.srt_gpu_va);
        }

        if (draw_pkt && draw_pkt->mfbd_gpu_va) {
            job.is_vkcube_graphics_job = true;
            if (job.shader_objc_va >= 0x10000ULL && job.shader_objc_bytes >= 8 && job.shader_objc_bytes <= 65536) {
                const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(job.shader_objc_va);
                size_t num_words = job.shader_objc_bytes / 8;
                job.disasm = ValhallISAToSpirvTranslator::disassembleObjc(objc_words, num_words);
            }
            if (opc == MALI_CS_OPCODE_RUN_IDVS) {
                job.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(
                    (job.shader_objc_va >= 0x10000ULL) ? reinterpret_cast<const uint64_t*>(job.shader_objc_va) : nullptr,
                    (job.shader_objc_va >= 0x10000ULL) ? (job.shader_objc_bytes / 8) : 0, false);
                job.executed_on_lvp = lvp_executor.validateShaderModuleOnLvpipe(job.lifted_spirv);
            } else if (opc == MALI_CS_OPCODE_RUN_FRAGMENT) {
                job.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftFragmentObjcToSpirv(
                    (job.shader_objc_va >= 0x10000ULL) ? reinterpret_cast<const uint64_t*>(job.shader_objc_va) : nullptr,
                    (job.shader_objc_va >= 0x10000ULL) ? (job.shader_objc_bytes / 8) : 0, false);
                auto vs_spv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(nullptr, 0, false);
                auto* mfbd  = reinterpret_cast<MaliMfbdDescriptor*>(draw_pkt->mfbd_gpu_va);
                VkCubeFrameStats fstats{};
                job.executed_on_lvp = lvp_executor.executeVkCubeGraphicsPass(
                    *draw_pkt, *mfbd, vs_spv, job.lifted_spirv, &fstats);
                executed_vkcube_frames.push_back(fstats);
            }
        } else if (job.shader_objc_va >= 0x10000ULL && job.shader_objc_bytes >= 8 && job.shader_objc_bytes <= 65536) {
            const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(job.shader_objc_va);
            size_t num_words = job.shader_objc_bytes / 8;
            job.lifted_spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(
                objc_words, num_words, &job.disasm);

            const void* in_ptr = nullptr;
            void* out_ptr = nullptr;
            size_t copy_bytes = 64;
            if (job.srt_gpu_va >= 0x10000ULL) {
                const uint64_t* srt = reinterpret_cast<const uint64_t*>(job.srt_gpu_va);
                in_ptr  = reinterpret_cast<const void*>(srt[0]);
                out_ptr = reinterpret_cast<void*>(srt[1]);
                if (srt[2] >= 64 && srt[2] <= 65536) {
                    copy_bytes = static_cast<size_t>(srt[2]);
                }
            }
            job.executed_on_lvp = lvp_executor.executeLiftedSpirv(
                job.lifted_spirv, in_ptr, out_ptr, copy_bytes, job.wg_x, job.wg_y, job.wg_z);
        }
        executed_jobs.push_back(std::move(job));
    }

    void setGpuProfile(MaliGpuProfileId prof) {
        gpu_product_id = static_cast<uint32_t>(prof);
        raw_gpu_id     = (static_cast<uint64_t>(gpu_product_id) << 16) | 1ULL;
    }

    // Build complete 85-property Mali TLV gpuprops stream matching
    // gpu_property_mapping[] in gpu/mali_kbase/mali_kbase_gpuprops.c
    std::vector<uint8_t> buildGpupropsTlv() const {
        std::vector<uint8_t> tlv;
        auto writeU8 = [&](uint32_t prop_id, uint8_t val) {
            uint32_t hdr = (prop_id << 2) | 0U;
            uint8_t b[5];
            std::memcpy(b, &hdr, 4);
            b[4] = val;
            tlv.insert(tlv.end(), b, b + 5);
        };
        auto writeU16 = [&](uint32_t prop_id, uint16_t val) {
            uint32_t hdr = (prop_id << 2) | 1U;
            uint8_t b[6];
            std::memcpy(b, &hdr, 4);
            std::memcpy(b + 4, &val, 2);
            tlv.insert(tlv.end(), b, b + 6);
        };
        auto writeU32 = [&](uint32_t prop_id, uint32_t val) {
            uint32_t hdr = (prop_id << 2) | 2U;
            uint8_t b[8];
            std::memcpy(b, &hdr, 4);
            std::memcpy(b + 4, &val, 4);
            tlv.insert(tlv.end(), b, b + 8);
        };
        auto writeU64 = [&](uint32_t prop_id, uint64_t val) {
            uint32_t hdr = (prop_id << 2) | 3U;
            uint8_t b[12];
            std::memcpy(b, &hdr, 4);
            std::memcpy(b + 4, &val, 8);
            tlv.insert(tlv.end(), b, b + 12);
        };

        writeU32(1,  gpu_product_id);
        writeU16(2,  1);
        writeU16(3,  0);
        writeU16(4,  0);
        writeU32(6,  850000U);
        writeU32(8,  24U);
        writeU32(9,  0x01FFFFFEU);
        writeU32(10, 0x0000FFFFU);
        writeU32(11, 0x9F81FFFFU);
        writeU32(80, 0x00000000U);
        writeU64(12, 8ULL << 30);
        writeU8 (82, 2);

        writeU8 (13, 6);
        writeU8 (14, 20);
        writeU8 (15, 2);

        writeU32(16, 512U);
        writeU32(17, 8U);

        writeU32(18, 2048U);
        writeU32(19, 1024U);
        writeU32(20, 1024U);
        writeU16(21, 65535U);
        writeU8 (22, 8);
        writeU8 (23, 0);
        writeU8 (24, 1);
        writeU32(84, 2048U);

        writeU64(25, 0x3FULL);
        writeU64(26, 0x1ULL);
        writeU64(27, 0x3ULL);
        writeU64(28, 0x3ULL);
        writeU64(29, 0x07140206ULL);
        writeU64(30, 0x00000002ULL);
        writeU64(31, 0x00000101ULL);
        writeU64(32, 0x00002830ULL);
        writeU32(33, 0xFFU);
        writeU32(34, csf_mode_active ? 0x0U : 0x7U);
        for (uint32_t js = 0; js < 16; ++js) {
            writeU32(35 + js, (!csf_mode_active && js < 3) ? 0x0000027EU : 0U);
        }
        writeU64(51, 0x00000809ULL);
        writeU32(52, 0x01FFFFFEU);
        writeU32(53, 0x0000FFFFU);
        writeU32(54, 0x9F81FFFFU);
        writeU32(81, 0x00000000U);
        writeU64(55, raw_gpu_id);
        writeU32(56, 2048U);
        writeU32(57, 1024U);
        writeU32(58, 1024U);
        writeU32(59, (8U << 24) | 65535U);
        writeU32(60, 1U);
        writeU32(83, 2048U);
        writeU64(85, 0x7ULL);

        writeU32(61, 1U);
        writeU32(62, 1U);
        writeU32(63, 1U);
        writeU64(64, 0x3FULL);
        for (uint32_t cg = 1; cg < 16; ++cg) {
            writeU64(64 + cg, 0ULL);
        }
        return tlv;
    }

    static std::vector<uint8_t> buildMaliG615GpupropsTlv() {
        MaliKbaseCsfKernelEmulator tmp;
        tmp.csf_mode_active = true;
        return tmp.buildGpupropsTlv();
    }

    uint64_t handleOpen(const char* path, uint64_t /*flags*/) {
        if (!path) return static_cast<uint64_t>(-1LL);
        if (std::strstr(path, "dma_heap") != nullptr) {
            ioctl_trace.push_back(std::string("open(") + path + ") -> fd=" + std::to_string(dma_heap_fd));
            return static_cast<uint64_t>(dma_heap_fd);
        }
        if (std::strstr(path, "mali") != nullptr) {
            fprintf(stderr, "MALI!!!!\n");
            ensureLiftedKernelBound();
            if (mali_kbase_emu_open_direct) {
                (void)mali_kbase_emu_open_direct(path, 0);
            }
            ioctl_trace.push_back(std::string("open(") + path + ") -> fd=" + std::to_string(virtual_fd));
            return static_cast<uint64_t>(virtual_fd);
        }
        return static_cast<uint64_t>(-1LL);
    }

    uint64_t handleClose(uint64_t fd) {
        if (mali_kbase_emu_close_direct && static_cast<int>(fd) == virtual_fd) {
            (void)mali_kbase_emu_close_direct(static_cast<int>(fd));
        }
        return 0;
    }

    uint64_t handleMmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
        ensureLiftedKernelBound();
        int ifd = static_cast<int>(fd);
        if (dmabuf_table.count(ifd)) {
            void* p = dmabuf_table[ifd].host_mapping;
            ioctl_trace.push_back("mmap(dmabuf_fd=" + std::to_string(ifd) + ")");
            return reinterpret_cast<uint64_t>(p);
        }
        if (mali_kbase_emu_mmap_direct && (ifd == virtual_fd || ifd == -1) &&
            !bos_by_cookie.count(offset) && !mmap_handle_to_queue_va.count(offset)) {
            void* p = mali_kbase_emu_mmap_direct(
                reinterpret_cast<void*>(addr),
                std::max<size_t>(length, KBASE_PAGE_SIZE),
                static_cast<int>(prot) | PROT_READ | PROT_WRITE,
                static_cast<int>(flags ? flags : MAP_SHARED),
                ifd,
                static_cast<off_t>(offset));
            if (p && p != MAP_FAILED) {
                ioctl_trace.push_back("mmap(lifted_kbase_ko offset=0x" + std::to_string(offset) + ")");
                return reinterpret_cast<uint64_t>(p);
            }
        }
        if (ifd != virtual_fd && ifd != -1) {
            void* p = mmap(reinterpret_cast<void*>(addr), length, static_cast<int>(prot) | PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return reinterpret_cast<uint64_t>(p);
        }
        if (offset == BASE_MEM_MAP_TRACKING_HANDLE) {
            if (!tracking_page) {
                tracking_page = mmap(nullptr, KBASE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            }
            ioctl_trace.push_back("mmap(BASE_MEM_MAP_TRACKING_HANDLE)");
            return reinterpret_cast<uint64_t>(tracking_page);
        }
        if (offset == BASEP_MEM_CSF_USER_REG_PAGE_HANDLE) {
            if (!user_reg_page) {
                user_reg_page = mmap(nullptr, KBASE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                *reinterpret_cast<uint32_t*>(user_reg_page) = 1U;
            }
            ioctl_trace.push_back("mmap(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE)");
            return reinterpret_cast<uint64_t>(user_reg_page);
        }
        if (mmap_handle_to_queue_va.count(offset)) {
            uint64_t qva = mmap_handle_to_queue_va[offset];
            auto& q = queues_by_buf_va[qva];
            if (!q.user_io_pages) {
                q.user_io_pages = static_cast<uint8_t*>(
                    mmap(nullptr, 3 * KBASE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
                std::memset(q.user_io_pages, 0, 3 * KBASE_PAGE_SIZE);
            }
            ioctl_trace.push_back("mmap(BASEP_MEM_CSF_USER_IO_PAGES_HANDLE, 3 pages)");
            return reinterpret_cast<uint64_t>(q.user_io_pages);
        }
        if (bos_by_cookie.count(offset)) {
            auto& bo = bos_by_cookie[offset];
            if (!bo.host_mapping) {
                size_t map_sz = std::max<size_t>(length, bo.va_pages * KBASE_PAGE_SIZE);
                bo.host_mapping = mmap(nullptr, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                std::memset(bo.host_mapping, 0, map_sz);
            }
            bo.cpu_va = reinterpret_cast<uint64_t>(bo.host_mapping);
            if (bo.same_va) {
                bo.gpu_va = bo.cpu_va;
            }
            bos_by_gpu_va[bo.gpu_va] = bo;
            std::ostringstream oss;
            oss << "mmap(BO cookie=0x" << std::hex << offset << " -> gpu_va=0x" << bo.gpu_va << std::dec << ")";
            ioctl_trace.push_back(oss.str());
            return bo.cpu_va;
        }
        if (bos_by_gpu_va.count(offset)) {
            return bos_by_gpu_va[offset].cpu_va;
        }
        void* p = mmap(nullptr, std::max<size_t>(length, KBASE_PAGE_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return reinterpret_cast<uint64_t>(p);
    }

    uint64_t handleMunmap(uint64_t addr, uint64_t length) {
        if (bos_by_gpu_va.count(addr)) {
            bool is_imported = bos_by_gpu_va[addr].imported_umm;
            bos_by_gpu_va.erase(addr);
            if (is_imported) return 0; // Keep underlying dma_buf alive for WSI consumer inspection
        }
        if (addr && length) {
            munmap(reinterpret_cast<void*>(addr), length);
        }
        return 0;
    }

    // CSF Command Stream Ringbuffer Interpreter:
    // Executes 64-bit CSF instructions from [CS_EXTRACT .. CS_INSERT) in queue's ringbuffer
    void executeCsfRingbuffer(KbaseCsfQueue& q) {
        if (!q.user_io_pages || !q.buffer_gpu_addr || q.buffer_size == 0) return;
        uint8_t* input_page  = q.user_io_pages + KBASE_PAGE_SIZE;
        uint8_t* output_page = q.user_io_pages + 2 * KBASE_PAGE_SIZE;

        uint64_t cs_insert  = *reinterpret_cast<uint64_t*>(input_page + CS_INSERT_LO);
        uint64_t cs_extract = *reinterpret_cast<uint64_t*>(output_page + CS_EXTRACT_LO);

        *reinterpret_cast<uint32_t*>(output_page + CS_ACTIVE) = 1U;

        uint32_t regs[96]{};
        auto getReg64 = [&](unsigned d_idx) -> uint64_t {
            if (d_idx + 1 >= 96) return 0;
            return static_cast<uint64_t>(regs[d_idx]) | (static_cast<uint64_t>(regs[d_idx + 1]) << 32);
        };
        auto setReg64 = [&](unsigned d_idx, uint64_t val) {
            if (d_idx + 1 < 96) {
                regs[d_idx]     = static_cast<uint32_t>(val & 0xFFFFFFFFULL);
                regs[d_idx + 1] = static_cast<uint32_t>(val >> 32);
            }
        };

        auto execStreamRange = [&](const uint64_t* stream_words, size_t word_count, auto& self_ref) -> void {
            for (size_t i = 0; i < word_count; ++i) {
                uint64_t insn = stream_words[i];
                uint8_t opc = static_cast<uint8_t>((insn >> 56) & 0xFFU);
                switch (opc) {
                    case MALI_CS_OPCODE_NOP:
                        break;
                    case MALI_CS_OPCODE_MOVE48: {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint64_t imm48 = insn & 0xFFFFFFFFFFFFULL;
                        setReg64(dst, imm48);
                        break;
                    }
                    case MALI_CS_OPCODE_MOVE32: {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint32_t imm32 = static_cast<uint32_t>(insn & 0xFFFFFFFFULL);
                        if (dst < 96) regs[dst] = imm32;
                        break;
                    }
                    case MALI_CS_OPCODE_WAIT:
                    case MALI_CS_OPCODE_SET_SB_ENTRY:
                    case MALI_CS_OPCODE_REQ_RESOURCE:
                    case MALI_CS_OPCODE_FLUSH_CACHE2:
                    case MALI_CS_OPCODE_FINISH_TILING:
                    case MALI_CS_OPCODE_FINISH_FRAGMENT:
                        break;
                    case MALI_CS_OPCODE_ADD_IMMEDIATE32: {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint8_t src = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint32_t imm = static_cast<uint32_t>(insn & 0xFFFFFFFFULL);
                        if (dst < 96 && src < 96) regs[dst] = regs[src] + imm;
                        break;
                    }
                    case MALI_CS_OPCODE_ADD_IMMEDIATE64: {
                        uint8_t dst = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint8_t src = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        int64_t imm = static_cast<int32_t>(insn & 0xFFFFFFFFULL);
                        setReg64(dst, static_cast<uint64_t>(static_cast<int64_t>(getReg64(src)) + imm));
                        break;
                    }
                    case MALI_CS_OPCODE_LOAD_MULTIPLE: {
                        uint8_t base_reg = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint8_t addr_reg = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint16_t mask    = static_cast<uint16_t>((insn >> 16) & 0xFFFFU);
                        int16_t offset   = static_cast<int16_t>(insn & 0xFFFFU);
                        uint64_t mem_va  = static_cast<uint64_t>(static_cast<int64_t>(getReg64(addr_reg)) + offset);
                        const uint32_t* ptr = reinterpret_cast<const uint32_t*>(mem_va);
                        unsigned k = 0;
                        for (unsigned b = 0; b < 16; ++b) {
                            if ((mask >> b) & 1U) {
                                if (base_reg + b < 96) regs[base_reg + b] = ptr[k];
                                k++;
                            }
                        }
                        break;
                    }
                    case MALI_CS_OPCODE_STORE_MULTIPLE: {
                        uint8_t base_reg = static_cast<uint8_t>((insn >> 48) & 0xFFU);
                        uint8_t addr_reg = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint16_t mask    = static_cast<uint16_t>((insn >> 16) & 0xFFFFU);
                        int16_t offset   = static_cast<int16_t>(insn & 0xFFFFU);
                        uint64_t mem_va  = static_cast<uint64_t>(static_cast<int64_t>(getReg64(addr_reg)) + offset);
                        uint32_t* ptr = reinterpret_cast<uint32_t*>(mem_va);
                        unsigned k = 0;
                        for (unsigned b = 0; b < 16; ++b) {
                            if ((mask >> b) & 1U) {
                                if (base_reg + b < 96) ptr[k] = regs[base_reg + b];
                                k++;
                            }
                        }
                        break;
                    }
                    case MALI_CS_OPCODE_CALL: {
                        uint8_t addr_reg = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint8_t len_reg  = static_cast<uint8_t>((insn >> 32) & 0xFFU);
                        uint64_t sub_va  = getReg64(addr_reg);
                        uint32_t sub_len = (len_reg < 96) ? regs[len_reg] : 0;
                        if (sub_va && sub_len >= 8) {
                            self_ref(reinterpret_cast<const uint64_t*>(sub_va), sub_len / 8, self_ref);
                        }
                        break;
                    }
                    case MALI_CS_OPCODE_SYNC_ADD32:
                    case MALI_CS_OPCODE_SYNC_SET32: {
                        uint8_t addr_reg = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint8_t data_reg = static_cast<uint8_t>((insn >> 32) & 0xFFU);
                        uint64_t sync_va = getReg64(addr_reg);
                        uint32_t val = (data_reg < 96) ? regs[data_reg] : 0;
                        if (sync_va) {
                            uint32_t* p = reinterpret_cast<uint32_t*>(sync_va);
                            if (opc == MALI_CS_OPCODE_SYNC_ADD32) *p += val;
                            else *p = val;
                        }
                        break;
                    }
                    case MALI_CS_OPCODE_SYNC_ADD64:
                    case MALI_CS_OPCODE_SYNC_SET64: {
                        uint8_t addr_reg = static_cast<uint8_t>((insn >> 40) & 0xFFU);
                        uint8_t data_reg = static_cast<uint8_t>((insn >> 32) & 0xFFU);
                        uint64_t sync_va = getReg64(addr_reg);
                        uint64_t val = getReg64(data_reg);
                        if (sync_va) {
                            uint64_t* p = reinterpret_cast<uint64_t*>(sync_va);
                            if (opc == MALI_CS_OPCODE_SYNC_ADD64) *p += val;
                            else *p = val;
                        }
                        break;
                    }
                    case MALI_CS_OPCODE_RUN_COMPUTE:
                    case MALI_CS_OPCODE_RUN_FRAGMENT:
                    case MALI_CS_OPCODE_RUN_IDVS: {
                        uint8_t srt_sel = static_cast<uint8_t>((insn >> 40) & 0x3U);
                        uint8_t spd_sel = static_cast<uint8_t>((insn >> 42) & 0x3U);
                        uint8_t tsd_sel = static_cast<uint8_t>((insn >> 44) & 0x3U);
                        uint8_t fau_sel = static_cast<uint8_t>((insn >> 46) & 0x3U);

                        KbaseCsfExecutedJob job;
                        job.cs_opcode  = opc;
                        job.srt_gpu_va = getReg64(srt_sel * 2);
                        job.spd_gpu_va = getReg64(8 + spd_sel * 2);
                        job.tsd_gpu_va = getReg64(16 + tsd_sel * 2);
                        job.fau_gpu_va = getReg64(24 + fau_sel * 2);
                        job.wg_x       = std::max<uint32_t>(1U, regs[32]);
                        job.wg_y       = std::max<uint32_t>(1U, regs[33]);
                        job.wg_z       = std::max<uint32_t>(1U, regs[34]);

                        if (job.spd_gpu_va) {
                            const uint64_t* spd = reinterpret_cast<const uint64_t*>(job.spd_gpu_va);
                            job.shader_objc_va    = spd[0];
                            job.shader_objc_bytes = static_cast<uint32_t>(spd[1] & 0xFFFFFFFFULL);
                        }

                        // Check if this is a 3D vkcube IDVS + Fragment graphics pipeline draw packet
                        const MaliIdvsDrawPacket* draw_pkt = nullptr;
                        if (job.tsd_gpu_va && *reinterpret_cast<const uint32_t*>(job.tsd_gpu_va) == MALI_IDVS_MAGIC) {
                            draw_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(job.tsd_gpu_va);
                        } else if (job.srt_gpu_va && *reinterpret_cast<const uint32_t*>(job.srt_gpu_va) == MALI_IDVS_MAGIC) {
                            draw_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(job.srt_gpu_va);
                        }

                        if (draw_pkt && draw_pkt->mfbd_gpu_va) {
                            job.is_vkcube_graphics_job = true;
                            if (job.shader_objc_va && job.shader_objc_bytes >= 8) {
                                const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(job.shader_objc_va);
                                size_t num_words = job.shader_objc_bytes / 8;
                                job.disasm = ValhallISAToSpirvTranslator::disassembleObjc(objc_words, num_words);
                            }
                            if (opc == MALI_CS_OPCODE_RUN_IDVS) {
                                job.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(
                                    reinterpret_cast<const uint64_t*>(job.shader_objc_va),
                                    job.shader_objc_bytes / 8, false);
                                job.executed_on_lvp = lvp_executor.validateShaderModuleOnLvpipe(job.lifted_spirv);
                            } else if (opc == MALI_CS_OPCODE_RUN_FRAGMENT) {
                                job.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftFragmentObjcToSpirv(
                                    reinterpret_cast<const uint64_t*>(job.shader_objc_va),
                                    job.shader_objc_bytes / 8, false);
                                auto vs_spv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(nullptr, 0, false);
                                auto* mfbd  = reinterpret_cast<MaliMfbdDescriptor*>(draw_pkt->mfbd_gpu_va);
                                VkCubeFrameStats fstats{};
                                job.executed_on_lvp = lvp_executor.executeVkCubeGraphicsPass(
                                    *draw_pkt, *mfbd, vs_spv, job.lifted_spirv, &fstats);
                                executed_vkcube_frames.push_back(fstats);
                            }
                        } else if (job.shader_objc_va && job.shader_objc_bytes >= 8) {
                            const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(job.shader_objc_va);
                            size_t num_words = job.shader_objc_bytes / 8;
                            job.lifted_spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(
                                objc_words, num_words, &job.disasm);

                            const void* in_ptr = nullptr;
                            void* out_ptr = nullptr;
                            size_t copy_bytes = 64;
                            if (job.srt_gpu_va) {
                                const uint64_t* srt = reinterpret_cast<const uint64_t*>(job.srt_gpu_va);
                                in_ptr  = reinterpret_cast<const void*>(srt[0]);
                                out_ptr = reinterpret_cast<void*>(srt[1]);
                                if (srt[2] >= 64 && srt[2] <= 65536) {
                                    copy_bytes = static_cast<size_t>(srt[2]);
                                }
                            }
                            job.executed_on_lvp = lvp_executor.executeLiftedSpirv(
                                job.lifted_spirv, in_ptr, out_ptr, copy_bytes, job.wg_x, job.wg_y, job.wg_z);
                        }
                        executed_jobs.push_back(std::move(job));
                        break;
                    }
                    default:
                        break;
                }
            }
        };

        const uint64_t* ring = reinterpret_cast<const uint64_t*>(q.buffer_gpu_addr);
        size_t ring_words = q.buffer_size / 8;
        if (cs_insert > cs_extract && ring_words > 0) {
            std::vector<uint64_t> cmds;
            for (uint64_t off = cs_extract; off < cs_insert; off += 8) {
                size_t idx = (off / 8) % ring_words;
                cmds.push_back(ring[idx]);
            }
            execStreamRange(cmds.data(), cmds.size(), execStreamRange);
        }

        *reinterpret_cast<uint64_t*>(output_page + CS_EXTRACT_LO) = cs_insert;
        *reinterpret_cast<uint32_t*>(output_page + CS_ACTIVE)     = 0U;
    }

    uint64_t handleRead(uint64_t /*fd*/, uint64_t buf_addr, uint64_t count) {
        if (!jm_event_queue.empty() && buf_addr != 0 && count >= sizeof(BaseJdEventV2)) {
            BaseJdEventV2 ev = jm_event_queue.front();
            jm_event_queue.erase(jm_event_queue.begin());
            std::memcpy(reinterpret_cast<void*>(buf_addr), &ev, sizeof(BaseJdEventV2));
            ioctl_trace.push_back("read(/dev/mali0) -> base_jd_event_v2(DONE, atom=" + std::to_string(ev.atom_number) + ")");
            return sizeof(BaseJdEventV2);
        }
        if (buf_addr && count > 0) {
            std::memset(reinterpret_cast<void*>(buf_addr), 0, count);
        }
        return count;
    }

    uint64_t handlePoll(uint64_t fds_addr, uint64_t nfds, uint64_t /*timeout*/) {
        if (fds_addr && nfds > 0) {
            for (uint64_t i = 0; i < nfds; ++i) {
                short* rev_ptr = reinterpret_cast<short*>(fds_addr + i * 8 + 6);
                *rev_ptr = 1; // POLLIN
            }
            return nfds;
        }
        return 1;
    }

    // Job Manager (JM) Job Descriptor Chain Walker & Bifrost/Valhall -> Lavapipe Executor
    void executeJmJobChain(const BaseJdAtomV2& atom) {
        uint64_t jc_va = atom.jc;
        while (jc_va != 0) {
            auto* hdr = reinterpret_cast<MaliJmJobHeader*>(jc_va);
            uint8_t jtype = static_cast<uint8_t>((hdr->flags_and_index >> 1) & 0x7FU);
            uint64_t next_va = hdr->next;

            if (jtype == MALI_JOB_TYPE_WRITE_VALUE) {
                uint64_t target_va = *reinterpret_cast<const uint64_t*>(jc_va + 0x20);
                uint64_t imm_val   = *reinterpret_cast<const uint64_t*>(jc_va + 0x30);
                if (target_va != 0) {
                    *reinterpret_cast<uint64_t*>(target_va) = imm_val;
                }
            } else if (jtype == MALI_JOB_TYPE_COMPUTE || jtype == MALI_JOB_TYPE_FRAGMENT ||
                       jtype == MALI_JOB_TYPE_VERTEX  || jtype == MALI_JOB_TYPE_TILER) {
                uint64_t rsd_va   = *reinterpret_cast<const uint64_t*>(jc_va + 0x20);
                uint64_t srt_va   = *reinterpret_cast<const uint64_t*>(jc_va + 0x28);
                uint32_t isa_mode = *reinterpret_cast<const uint32_t*>(jc_va + 0x30);
                uint32_t wg_x     = std::max<uint32_t>(1U, *reinterpret_cast<const uint32_t*>(jc_va + 0x34));

                KbaseJmExecutedAtom exec_atom{};
                exec_atom.atom_number           = atom.atom_number;
                exec_atom.core_req              = atom.core_req;
                exec_atom.jm_job_type           = jtype;
                exec_atom.is_bifrost_clause_isa = (isa_mode == 1U);
                exec_atom.jc_gpu_va             = jc_va;

                if (rsd_va != 0) {
                    const uint64_t* rsd = reinterpret_cast<const uint64_t*>(rsd_va);
                    exec_atom.shader_objc_va    = rsd[0] & ~0x7ULL;
                    exec_atom.shader_objc_bytes = static_cast<uint32_t>(rsd[1] & 0xFFFFFFFFULL);
                }

                // Check if srt_va points to a 3D vkcube MaliIdvsDrawPacket
                const MaliIdvsDrawPacket* draw_pkt = nullptr;
                if (srt_va != 0 && *reinterpret_cast<const uint32_t*>(srt_va) == MALI_IDVS_MAGIC) {
                    draw_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(srt_va);
                }

                if (draw_pkt && draw_pkt->mfbd_gpu_va) {
                    exec_atom.is_vkcube_graphics_atom = true;
                    if (exec_atom.shader_objc_va != 0 && exec_atom.shader_objc_bytes >= 8) {
                        const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(exec_atom.shader_objc_va);
                        size_t num_words = exec_atom.shader_objc_bytes / 8;
                        if (exec_atom.is_bifrost_clause_isa) {
                            exec_atom.bifrost_disasm = BifrostISAToSpirvTranslator::disassembleClauses(objc_words, num_words);
                        } else {
                            exec_atom.valhall_disasm = ValhallISAToSpirvTranslator::disassembleObjc(objc_words, num_words);
                        }
                    }
                    if (jtype == MALI_JOB_TYPE_TILER || jtype == MALI_JOB_TYPE_VERTEX) {
                        exec_atom.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(
                            reinterpret_cast<const uint64_t*>(exec_atom.shader_objc_va),
                            exec_atom.shader_objc_bytes / 8,
                            exec_atom.is_bifrost_clause_isa);
                        exec_atom.executed_on_lvp = lvp_executor.validateShaderModuleOnLvpipe(exec_atom.lifted_spirv);
                    } else if (jtype == MALI_JOB_TYPE_FRAGMENT) {
                        exec_atom.lifted_spirv = MaliVkCubeIsaPipelineTranslator::liftFragmentObjcToSpirv(
                            reinterpret_cast<const uint64_t*>(exec_atom.shader_objc_va),
                            exec_atom.shader_objc_bytes / 8,
                            exec_atom.is_bifrost_clause_isa);
                        auto vs_spv = MaliVkCubeIsaPipelineTranslator::liftVertexObjcToSpirv(
                            nullptr, 0, exec_atom.is_bifrost_clause_isa);
                        auto* mfbd = reinterpret_cast<MaliMfbdDescriptor*>(draw_pkt->mfbd_gpu_va);
                        VkCubeFrameStats fstats{};
                        exec_atom.executed_on_lvp = lvp_executor.executeVkCubeGraphicsPass(
                            *draw_pkt, *mfbd, vs_spv, exec_atom.lifted_spirv, &fstats);
                        executed_vkcube_frames.push_back(fstats);
                    }
                } else if (exec_atom.shader_objc_va != 0 && exec_atom.shader_objc_bytes >= 8) {
                    const uint64_t* objc_words = reinterpret_cast<const uint64_t*>(exec_atom.shader_objc_va);
                    size_t num_words = exec_atom.shader_objc_bytes / 8;
                    if (exec_atom.is_bifrost_clause_isa) {
                        exec_atom.lifted_spirv = BifrostISAToSpirvTranslator::translateClauseObjcToSpirv(
                            objc_words, num_words, &exec_atom.bifrost_disasm);
                    } else {
                        exec_atom.lifted_spirv = ValhallISAToSpirvTranslator::translateObjcToSpirv(
                            objc_words, num_words, &exec_atom.valhall_disasm);
                    }

                    const void* in_ptr = nullptr;
                    void* out_ptr = nullptr;
                    size_t copy_bytes = 64;
                    if (srt_va != 0) {
                        const uint64_t* srt = reinterpret_cast<const uint64_t*>(srt_va);
                        in_ptr  = reinterpret_cast<const void*>(srt[0]);
                        out_ptr = reinterpret_cast<void*>(srt[1]);
                        if (srt[2] >= 64 && srt[2] <= 65536) {
                            copy_bytes = static_cast<size_t>(srt[2]);
                        }
                    }
                    exec_atom.executed_on_lvp = lvp_executor.executeLiftedSpirv(
                        exec_atom.lifted_spirv, in_ptr, out_ptr, copy_bytes, wg_x, 1, 1);
                }
                executed_jm_atoms.push_back(std::move(exec_atom));
            }

            hdr->exception_status = 0x01U;
            jc_va = next_va;
        }

        BaseJdEventV2 ev{};
        ev.event_code  = BASE_JD_EVENT_DONE;
        ev.atom_number = atom.atom_number;
        ev.udata       = atom.udata;
        jm_event_queue.push_back(ev);
    }

    uint64_t handleIoctl(uint64_t fd, uint64_t cmd, uint64_t arg_addr) {
        ensureLiftedKernelBound();
        uint32_t ucmd = static_cast<uint32_t>(cmd);
        if (mali_kbase_emu_ioctl_direct && ((ucmd >> 8) & 0xFFU) == 0x80U) {
            unsigned int nr = ucmd & 0xFFU;
            // Track jit_va_pages and exec_va_pages for runner telemetry assertions
            if (ucmd == 0x4018800eU && arg_addr) jit_va_pages = *reinterpret_cast<uint64_t*>(arg_addr);
            if (ucmd == 0x40088026U && arg_addr) exec_va_pages = *reinterpret_cast<uint64_t*>(arg_addr);
            if (ucmd == 0xc0048034U) csf_mode_active = true;
            // Route CSF kernel UAPI ioctls (MEM_ALLOC/EX/QUERY/FREE/COMMIT/ALIAS/SYNC/FLAGS,
            // CS_QUEUE_REGISTER/BIND/KICK/TERMINATE, CS_QUEUE_GROUP_CREATE/TERMINATE,
            // CS_TILER_HEAP_INIT/TERM, KCPU_QUEUE_CREATE/DELETE/ENQUEUE, SET_FLAGS, VERSION_CHECK_CSF)
            // directly into our lifted /dev/mali0 kernel emulator (mali_kbase_emu.c + lifted_kbase_ko.ll + lifted_csffw.c)!
            // Keep GET_GPUPROPS (nr=3), CS_GET_GLB_IFACE (nr=51), JM JOB_SUBMIT (nr=2), and UMM MEM_IMPORT (nr=22)
            // augmented with the runner's 85-property TLV / 64-stream / virtual-dmabuf tables after running lifted_kbase_ko!
            if (nr != 0 && nr != 2 && nr != 3 && nr != 22 && nr != 28 && ucmd != 0xc0048036U && ucmd != 0xc0018036U) {
                int krc = mali_kbase_emu_ioctl_direct(static_cast<int>(fd), static_cast<unsigned long>(ucmd), reinterpret_cast<void*>(arg_addr));
                ioctl_trace.push_back("lifted_kbase_ko_ioctl(cmd=0x" + std::to_string(ucmd) + ")");
                return (krc == 0) ? 0ULL : static_cast<uint64_t>(static_cast<int64_t>(krc));
            }
        }

        switch (ucmd) {
            // 0. DMA_HEAP_IOCTL_ALLOC (0xc0184800) on /dev/dma_heap/system
            case DMA_HEAP_IOCTL_ALLOC: {
                if (!arg_addr) return static_cast<uint64_t>(-1LL);
                auto* req = reinterpret_cast<DmaHeapAllocationData*>(arg_addr);
                uint64_t alloc_len = std::max<uint64_t>(KBASE_PAGE_SIZE, (req->len + KBASE_PAGE_SIZE - 1) & ~(KBASE_PAGE_SIZE - 1));
                void* mem = mmap(nullptr, alloc_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                std::memset(mem, 0, alloc_len);
                int dfd = next_dmabuf_fd++;
                DmaBufObject dbo{};
                dbo.fd           = dfd;
                dbo.size_bytes   = alloc_len;
                dbo.host_mapping = mem;
                dmabuf_table[dfd] = dbo;
                req->fd = static_cast<uint32_t>(dfd);
                ioctl_trace.push_back("DMA_HEAP_IOCTL_ALLOC(len=" + std::to_string(alloc_len) + ", dmabuf_fd=" + std::to_string(dfd) + ")");
                return 0;
            }
            // 1. KBASE_IOCTL_VERSION_CHECK_CSF (0xc0048034) / JM (0xc0048000) / RESERVED (0xc0048036)
            case 0xc0048034U:
            case 0xc0048036U:
            case 0xc0018036U:
            case 0xc0048000U: {
                if (arg_addr) {
                    auto* ver = reinterpret_cast<KbaseIoctlVersionCheck*>(arg_addr);
                    if (ucmd == 0xc0048034U) {
                        ver->major = uapi_major;
                        ver->minor = uapi_minor;
                        csf_mode_active = true;
                        ioctl_trace.push_back("KBASE_IOCTL_VERSION_CHECK_CSF -> 1.36");
                    } else {
                        ver->major = 11;
                        ver->minor = 35;
                        ioctl_trace.push_back("KBASE_IOCTL_VERSION_CHECK_JM -> 11.35");
                    }
                }
                return 0;
            }
            // 2. KBASE_IOCTL_SET_FLAGS (0x40048001)
            case 0x40048001U: {
                if (arg_addr) {
                    context_flags = reinterpret_cast<KbaseIoctlSetFlags*>(arg_addr)->create_flags;
                }
                ioctl_trace.push_back("KBASE_IOCTL_SET_FLAGS");
                return 0;
            }
            // 2B. KBASE_IOCTL_JOB_SUBMIT (0x40108002 / 0x40188002) - Job Manager (JM) Atom Submission
            case 0x40108002U:
            case 0x40188002U: {
                if (arg_addr) {
                    auto* sub = reinterpret_cast<const KbaseIoctlJobSubmit*>(arg_addr);
                    uint32_t stride = sub->stride ? sub->stride : sizeof(BaseJdAtomV2);
                    for (uint32_t i = 0; i < sub->nr_atoms; ++i) {
                        const auto* atom = reinterpret_cast<const BaseJdAtomV2*>(sub->addr + i * stride);
                        executeJmJobChain(*atom);
                    }
                }
                ioctl_trace.push_back("KBASE_IOCTL_JOB_SUBMIT");
                return 0;
            }
            // 2C. KBASE_IOCTL_POST_TERM (0x00008004) & KBASE_IOCTL_SOFT_EVENT_UPDATE (0x4010801c)
            case 0x00008004U: {
                ioctl_trace.push_back("KBASE_IOCTL_POST_TERM");
                return 0;
            }
            case 0x4010801cU: {
                if (arg_addr) {
                    auto* sev = reinterpret_cast<const KbaseIoctlSoftEventUpdate*>(arg_addr);
                    if (sev->event != 0) {
                        *reinterpret_cast<uint32_t*>(sev->event) = sev->new_status;
                    }
                }
                ioctl_trace.push_back("KBASE_IOCTL_SOFT_EVENT_UPDATE");
                return 0;
            }
            // 3. KBASE_IOCTL_GET_GPUPROPS (0x40108003)
            case 0x40108003U: {
                if (!arg_addr) return 0;
                auto* gp = reinterpret_cast<KbaseIoctlGetGpuprops*>(arg_addr);
                auto tlv = buildGpupropsTlv();
                if (gp->size == 0 || gp->buffer == 0) {
                    ioctl_trace.push_back("KBASE_IOCTL_GET_GPUPROPS(probe) -> " + std::to_string(tlv.size()) + "B");
                    return tlv.size();
                }
                uint32_t copy_len = std::min<uint32_t>(gp->size, static_cast<uint32_t>(tlv.size()));
                std::memcpy(reinterpret_cast<void*>(gp->buffer), tlv.data(), copy_len);
                ioctl_trace.push_back("KBASE_IOCTL_GET_GPUPROPS(fill, Mali-G615 0xb8830001)");
                return csf_mode_active ? static_cast<uint64_t>(copy_len) : 0ULL;
            }
            // 4. KBASE_IOCTL_MEM_JIT_INIT (0x4018800e)
            case 0x4018800eU: {
                if (arg_addr) jit_va_pages = *reinterpret_cast<uint64_t*>(arg_addr);
                ioctl_trace.push_back("KBASE_IOCTL_MEM_JIT_INIT");
                return 0;
            }
            // 5. KBASE_IOCTL_MEM_EXEC_INIT (0x40088026)
            case 0x40088026U: {
                if (arg_addr) exec_va_pages = *reinterpret_cast<uint64_t*>(arg_addr);
                ioctl_trace.push_back("KBASE_IOCTL_MEM_EXEC_INIT");
                return 0;
            }
            // 6. KBASE_IOCTL_MEM_ALLOC (0xc0208005) & KBASE_IOCTL_MEM_ALLOC_EX (0xc040803b)
            case 0xc0208005U:
            case 0xc040803bU: {
                if (!arg_addr) return static_cast<uint64_t>(-1LL);
                uint64_t va_pages     = *reinterpret_cast<uint64_t*>(arg_addr + 0);
                uint64_t commit_pages = *reinterpret_cast<uint64_t*>(arg_addr + 8);
                uint64_t in_flags     = *reinterpret_cast<uint64_t*>(arg_addr + 24);
                if (va_pages == 0) va_pages = 1;

                KbaseBufferObject bo{};
                bo.va_pages     = va_pages;
                bo.commit_pages = commit_pages ? commit_pages : va_pages;
                bo.same_va      = (in_flags & BASE_MEM_SAME_VA) != 0 && (in_flags & BASE_MEM_PROT_GPU_EX) == 0;

                if (bo.same_va) {
                    uint64_t cookie = BASE_MEM_COOKIE_BASE + (next_cookie_index++ << KBASE_PAGE_SHIFT);
                    bo.cookie_or_va = cookie;
                    bo.flags        = in_flags | BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP;
                    bos_by_cookie[cookie] = bo;
                    *reinterpret_cast<uint64_t*>(arg_addr + 0) = bo.flags;
                    *reinterpret_cast<uint64_t*>(arg_addr + 8) = cookie;
                    ioctl_trace.push_back("KBASE_IOCTL_MEM_ALLOC_EX(SAME_VA, pages=" + std::to_string(va_pages) + ")");
                } else {
                    size_t map_sz = va_pages * KBASE_PAGE_SIZE;
                    bo.host_mapping = mmap(nullptr, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    std::memset(bo.host_mapping, 0, map_sz);
                    bo.cpu_va       = reinterpret_cast<uint64_t>(bo.host_mapping);
                    bo.gpu_va       = bo.cpu_va;
                    bo.cookie_or_va = bo.gpu_va;
                    bo.flags        = in_flags & ~BASE_MEM_SAME_VA;
                    bos_by_gpu_va[bo.gpu_va] = bo;
                    *reinterpret_cast<uint64_t*>(arg_addr + 0) = bo.flags;
                    *reinterpret_cast<uint64_t*>(arg_addr + 8) = bo.gpu_va;
                    ioctl_trace.push_back("KBASE_IOCTL_MEM_ALLOC_EX(EXEC_VA/CUSTOM_VA, pages=" + std::to_string(va_pages) + ")");
                }
                return 0;
            }
            // 7. KBASE_IOCTL_MEM_QUERY (0xc0108006)
            case 0xc0108006U: {
                if (arg_addr) {
                    auto* q = reinterpret_cast<KbaseIoctlMemQuery*>(arg_addr);
                    uint64_t addr = q->in.gpu_addr;
                    uint64_t qtype = q->in.query;
                    uint64_t val = 1;
                    if (bos_by_gpu_va.count(addr)) {
                        const auto& bo = bos_by_gpu_va[addr];
                        if (qtype == 1) val = bo.commit_pages;
                        else if (qtype == 2) val = bo.va_pages;
                        else if (qtype == 3) val = bo.flags;
                    }
                    *reinterpret_cast<uint64_t*>(arg_addr + 0) = val;
                    *reinterpret_cast<uint64_t*>(arg_addr + 8) = val;
                }
                ioctl_trace.push_back("KBASE_IOCTL_MEM_QUERY");
                return 0;
            }
            // 8. KBASE_IOCTL_MEM_FREE (0x40088007)
            case 0x40088007U: {
                if (arg_addr) {
                    uint64_t addr = *reinterpret_cast<uint64_t*>(arg_addr);
                    if (bos_by_gpu_va.count(addr)) {
                        auto bo = bos_by_gpu_va[addr];
                        if (bo.host_mapping && !bo.imported_umm) {
                            munmap(bo.host_mapping, bo.va_pages * KBASE_PAGE_SIZE);
                        }
                        bos_by_gpu_va.erase(addr);
                    }
                    bos_by_cookie.erase(addr);
                }
                ioctl_trace.push_back("KBASE_IOCTL_MEM_FREE");
                return 0;
            }
            // 9. KBASE_IOCTL_MEM_COMMIT (0x40108014), ALIAS (0xc0208015), IMPORT (0xc0188016), FLAGS_CHANGE (0x40188017), SYNC (0x4020800f)
            case 0x40108014U:
                ioctl_trace.push_back("KBASE_IOCTL_MEM_COMMIT");
                return 0;
            case 0xc0208015U: {
                if (arg_addr) {
                    auto* al = reinterpret_cast<KbaseIoctlMemAlias*>(arg_addr);
                    uint64_t pages = std::max<uint64_t>(1ULL, al->in.stride * al->in.nents);
                    uint64_t cookie = BASE_MEM_COOKIE_BASE + (next_cookie_index++ << KBASE_PAGE_SHIFT);
                    KbaseBufferObject bo{};
                    bo.cookie_or_va = cookie;
                    bo.va_pages     = pages;
                    bo.commit_pages = pages;
                    bo.flags        = al->in.flags | BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP;
                    bo.same_va      = true;
                    bos_by_cookie[cookie] = bo;
                    al->out.flags    = bo.flags;
                    al->out.gpu_va   = cookie;
                    al->out.va_pages = pages;
                }
                ioctl_trace.push_back("KBASE_IOCTL_MEM_ALIAS");
                return 0;
            }
            case 0xc0188016U: {
                if (arg_addr) {
                    auto* imp = reinterpret_cast<KbaseIoctlMemImport*>(arg_addr);
                    uint64_t in_flags = imp->in.flags;
                    uint64_t phandle  = imp->in.phandle;
                    int dfd = -1;
                    if (phandle > 0 && phandle < 65536ULL) {
                        dfd = static_cast<int>(phandle);
                    } else if (phandle >= 65536ULL) {
                        dfd = *reinterpret_cast<const int*>(phandle);
                    }
                    KbaseBufferObject bo{};
                    bo.imported_umm = true;
                    bo.dmabuf_fd    = dfd;
                    if (dmabuf_table.count(dfd)) {
                        const auto& dbo = dmabuf_table[dfd];
                        bo.va_pages     = std::max<uint64_t>(1ULL, dbo.size_bytes / KBASE_PAGE_SIZE);
                        bo.commit_pages = bo.va_pages;
                        bo.host_mapping = dbo.host_mapping;
                    } else {
                        bo.va_pages     = 4ULL;
                        bo.commit_pages = 4ULL;
                        bo.host_mapping = mmap(nullptr, 4 * KBASE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                        std::memset(bo.host_mapping, 0, 4 * KBASE_PAGE_SIZE);
                    }
                    bo.cpu_va       = reinterpret_cast<uint64_t>(bo.host_mapping);
                    bo.gpu_va       = bo.cpu_va;
                    bo.same_va      = true;
                    uint64_t cookie = BASE_MEM_COOKIE_BASE + (next_cookie_index++ << KBASE_PAGE_SHIFT);
                    bo.cookie_or_va = cookie;
                    bo.flags        = in_flags | BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP | BASE_MEM_COHERENT_SYSTEM;
                    bos_by_cookie[cookie]    = bo;
                    bos_by_gpu_va[bo.gpu_va] = bo;
                    imp->out.flags    = bo.flags;
                    imp->out.gpu_va   = cookie;
                    imp->out.va_pages = bo.va_pages;
                }
                ioctl_trace.push_back("KBASE_IOCTL_MEM_IMPORT");
                return 0;
            }
            case 0x40188017U:
                ioctl_trace.push_back("KBASE_IOCTL_MEM_FLAGS_CHANGE");
                return 0;
            case 0x4020800fU:
                ioctl_trace.push_back("KBASE_IOCTL_MEM_SYNC");
                return 0;
            // 10. KBASE_IOCTL_CS_GET_GLB_IFACE (0xc0188033)
            case 0xc0188033U: {
                if (!arg_addr) return 0;
                auto* glb = reinterpret_cast<KbaseIoctlCsGetGlbIface*>(arg_addr);
                uint32_t max_groups  = glb->in.max_group_num;
                uint32_t max_streams = glb->in.max_total_stream_num;
                uint64_t groups_ptr  = glb->in.groups_ptr;
                uint64_t streams_ptr = glb->in.streams_ptr;

                constexpr uint32_t kNumGroups  = 8;
                constexpr uint32_t kStreamsPerGroup = 8;
                constexpr uint32_t kTotalStreams = kNumGroups * kStreamsPerGroup;

                if (max_groups > 0 && groups_ptr != 0) {
                    auto* g_out = reinterpret_cast<BasepCsGroupControl*>(groups_ptr);
                    uint32_t n = std::min<uint32_t>(max_groups, kNumGroups);
                    for (uint32_t i = 0; i < n; ++i) {
                        g_out[i].features     = 0x7U;
                        g_out[i].stream_num   = kStreamsPerGroup;
                        g_out[i].suspend_size = 256U;
                        g_out[i].padding      = 0U;
                    }
                }
                if (max_streams > 0 && streams_ptr != 0) {
                    auto* s_out = reinterpret_cast<BasepCsStreamControl*>(streams_ptr);
                    uint32_t n = std::min<uint32_t>(max_streams, kTotalStreams);
                    for (uint32_t i = 0; i < n; ++i) {
                        s_out[i].features = (8U << 8) | 96U;
                        s_out[i].padding  = 0U;
                    }
                }
                glb->out.glb_version          = (1U << 24) | (1U << 16);
                glb->out.features             = 0x7U;
                glb->out.group_num            = kNumGroups;
                glb->out.max_total_stream_num = kTotalStreams;
                glb->out.total_stream_num     = kTotalStreams;
                glb->out.instr_features       = 0x71U;
                ioctl_trace.push_back(max_groups == 0 ? "KBASE_IOCTL_CS_GET_GLB_IFACE(probe)" : "KBASE_IOCTL_CS_GET_GLB_IFACE(fill)");
                return 0;
            }
            // 11. KBASE_IOCTL_CS_QUEUE_GROUP_CREATE (_1_6=0xc020802a, _1_18=0xc028803a, current=0xc078803f)
            case 0xc020802aU:
            case 0xc028803aU:
            case 0xc078803fU: {
                if (arg_addr) {
                    auto* csg = reinterpret_cast<KbaseIoctlCsQueueGroupCreate*>(arg_addr);
                    uint8_t gh = next_group_handle++;
                    std::memset(csg, 0, sizeof(KbaseIoctlCsQueueGroupCreate));
                    csg->out.group_handle   = gh;
                    csg->out.group_uid      = 1000U + gh;
                    csg->out.user_io_gpu_va = 0ULL;
                }
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_GROUP_CREATE");
                return 0;
            }
            case 0x4008802bU:
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE");
                return 0;
            // 12. KBASE_IOCTL_CS_QUEUE_REGISTER (0x40108024) & REGISTER_EX (0x40288028)
            case 0x40108024U:
            case 0x40288028U: {
                if (arg_addr) {
                    auto* reg = reinterpret_cast<KbaseIoctlCsQueueRegister*>(arg_addr);
                    KbaseCsfQueue q{};
                    q.buffer_gpu_addr = reg->buffer_gpu_addr & ~0xFFFULL;
                    q.buffer_size     = reg->buffer_size;
                    q.priority        = reg->priority;
                    queues_by_buf_va[q.buffer_gpu_addr] = q;
                }
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_REGISTER");
                return 0;
            }
            case 0x40088029U:
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_TERMINATE");
                return 0;
            // 13. KBASE_IOCTL_CS_QUEUE_BIND (0xc0108027)
            case 0xc0108027U: {
                if (arg_addr) {
                    auto* bind = reinterpret_cast<KbaseIoctlCsQueueBind*>(arg_addr);
                    uint64_t buf_va = bind->in.buffer_gpu_addr & ~0xFFFULL;
                    uint8_t  gh     = bind->in.group_handle;
                    uint8_t  csi    = bind->in.csi_index;
                    uint64_t handle = BASEP_MEM_CSF_USER_IO_PAGES_HANDLE + ((uint64_t(gh) * 8ULL + csi) * 3ULL * KBASE_PAGE_SIZE);
                    auto& q = queues_by_buf_va[buf_va];
                    q.buffer_gpu_addr = buf_va;
                    q.group_handle    = gh;
                    q.csi_index       = csi;
                    q.bound           = true;
                    q.mmap_handle     = handle;
                    mmap_handle_to_queue_va[handle] = buf_va;
                    bind->out.mmap_handle = handle;
                }
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_BIND");
                return 0;
            }
            // 14. KBASE_IOCTL_CS_QUEUE_KICK (0x40088025)
            case 0x40088025U: {
                if (arg_addr) {
                    auto* kick = reinterpret_cast<KbaseIoctlCsQueueKick*>(arg_addr);
                    uint64_t buf_va = kick->buffer_gpu_addr & ~0xFFFULL;
                    if (queues_by_buf_va.count(buf_va)) {
                        executeCsfRingbuffer(queues_by_buf_va[buf_va]);
                    }
                }
                ioctl_trace.push_back("KBASE_IOCTL_CS_QUEUE_KICK");
                return 0;
            }
            // 15. KBASE_IOCTL_CS_TILER_HEAP_INIT (0xc0188030 / 0xc0108030) & TERM (0x40088031)
            case 0xc0188030U:
            case 0xc0108030U: {
                if (arg_addr) {
                    auto* th = reinterpret_cast<KbaseIoctlCsTilerHeapInit*>(arg_addr);
                    uint64_t hva = next_tiler_heap_va;
                    next_tiler_heap_va += 0x200000ULL;
                    th->out.gpu_heap_va    = hva;
                    th->out.first_chunk_va = hva + KBASE_PAGE_SIZE;
                }
                ioctl_trace.push_back("KBASE_IOCTL_CS_TILER_HEAP_INIT");
                return 0;
            }
            case 0x40088031U:
                ioctl_trace.push_back("KBASE_IOCTL_CS_TILER_HEAP_TERM");
                return 0;
            // 16. KBASE_IOCTL_KCPU_QUEUE_CREATE (0x8008802d), DELETE (0x4008802e), ENQUEUE (0x4010802f)
            case 0x8008802dU: {
                if (arg_addr) {
                    reinterpret_cast<KbaseIoctlKcpuQueueNew*>(arg_addr)->id = next_kcpu_id++;
                }
                ioctl_trace.push_back("KBASE_IOCTL_KCPU_QUEUE_CREATE");
                return 0;
            }
            case 0x4008802eU:
                ioctl_trace.push_back("KBASE_IOCTL_KCPU_QUEUE_DELETE");
                return 0;
            case 0x4010802fU: {
                if (arg_addr) {
                    auto* enq = reinterpret_cast<KbaseIoctlKcpuQueueEnqueue*>(arg_addr);
                    if (enq->addr && enq->nr_commands > 0) {
                        const uint8_t* cmd_bytes = reinterpret_cast<const uint8_t*>(enq->addr);
                        uint8_t ctype = cmd_bytes[0];
                        uint64_t objs_ptr = *reinterpret_cast<const uint64_t*>(cmd_bytes + 8);
                        uint32_t nr_objs  = *reinterpret_cast<const uint32_t*>(cmd_bytes + 16);
                        if (ctype == 3 && objs_ptr && nr_objs > 0) {
                            uint64_t cqs_addr = *reinterpret_cast<const uint64_t*>(objs_ptr);
                            if (cqs_addr) *reinterpret_cast<uint32_t*>(cqs_addr) += 1U;
                        } else if (ctype == 5 && objs_ptr && nr_objs > 0) {
                            uint64_t cqs_addr = *reinterpret_cast<const uint64_t*>(objs_ptr + 0);
                            uint64_t cqs_val  = *reinterpret_cast<const uint64_t*>(objs_ptr + 8);
                            if (cqs_addr) *reinterpret_cast<uint64_t*>(cqs_addr) = cqs_val;
                        }
                    }
                }
                ioctl_trace.push_back("KBASE_IOCTL_KCPU_QUEUE_ENQUEUE");
                return 0;
            }
            case 0x802cU:
                ioctl_trace.push_back("KBASE_IOCTL_CS_EVENT_SIGNAL");
                return 0;
            default:
                ioctl_trace.push_back("KBASE_IOCTL_OTHER");
                return 0;
        }
    }
};
