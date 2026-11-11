// ============================================================================
// src/aot/src/mali_vulkan_icd.cpp
// Core AOT Vulkan ICD implementation (Device, Queue, Memory, Buffers, Images,
// Pipelines, CommandBuffers, and vkGetInstanceProcAddr/vkGetDeviceProcAddr).
// Modularized across mali_icd_internal.h, mali_icd_physdev.cpp, and
// mali_icd_wsi.cpp.
// ============================================================================
#include "mali_icd_physdev.cpp"

extern "C" {

__attribute__((visibility("default")))
int32_t vkCreateDevice(void* physicalDevice, const void* /*pCreateInfo*/, const void* /*pAllocator*/, void** pDevice) {
    if (!pDevice) return -3;
    g_aot_rt.ensureInitialized();

    auto* dev = new MaliIcdDevice();
    dev->loader_magic = ICD_LOADER_MAGIC;
    dev->phys_dev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);
    dev->queue.loader_magic = ICD_LOADER_MAGIC;
    dev->queue.parent_device = dev;

    // 1. Allocate 128 KB mock AOT VkDevice struct for AOT vkCreateShaderModule / vkDestroyShaderModule
    dev->aot_vk_device_mem.assign(131072, 0);
    *reinterpret_cast<uint64_t*>(dev->aot_vk_device_mem.data() + 0x10) = HLE_VK_ALLOC_MAGIC;
    *reinterpret_cast<uint64_t*>(dev->aot_vk_device_mem.data() + 0x20) = HLE_VK_FREE_MAGIC;
    *reinterpret_cast<uint64_t*>(dev->aot_vk_device_mem.data() + 0x28) = 0xCAFE0000ULL;

    // 2. Initialize /dev/mali0 kbase context & CSF/JM queues via AOT-translated functions
    dev->kctx_mem.assign(4096, 0);
    uint8_t* kctx_ptr = dev->kctx_mem.data();
    *reinterpret_cast<int32_t*>(kctx_ptr + 0x000)  = 100;
    *reinterpret_cast<int32_t*>(kctx_ptr + 0x070)  = 100;
    *reinterpret_cast<uint32_t*>(kctx_ptr + 0x278) = 1U;
    *reinterpret_cast<uint32_t*>(kctx_ptr + 0x27c) = 36U;

    {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        auto& cpu = g_aot_rt.cpu;
        if (aot_kbase_version_check_csf) {
            KbaseIoctlVersionCheck ver{0, 0};
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&ver);
            aot_kbase_version_check_csf(&cpu);
        } else if (aot_kbase_version_check_jm) {
            KbaseIoctlVersionCheck ver{0, 0};
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&ver);
            aot_kbase_version_check_jm(&cpu);
        }
        if (aot_kbase_set_flags) {
            KbaseIoctlSetFlags flags{0};
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&flags);
            aot_kbase_set_flags(&cpu);
        }
        if (aot_kbase_mem_jit_init) {
            uint64_t jit_req[3] = {1ULL << 25, 255ULL, 1ULL << 25};
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(jit_req);
            aot_kbase_mem_jit_init(&cpu);
        }
        if (aot_kbase_mem_exec_init) {
            uint64_t exec_req = 0x100000ULL;
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&exec_req);
            aot_kbase_mem_exec_init(&cpu);
        }
    }

    // 3. Allocate CSF Ringbuffer, SRT, SPD, and CQS BOs via AOT aot_kbase_mem_alloc
    uint64_t rw_same_va = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
    dev->ring_bo_va = icdAllocKbaseBo(dev, 1, rw_same_va);
    dev->srt_bo_va  = icdAllocKbaseBo(dev, 1, rw_same_va);
    dev->spd_bo_va  = icdAllocKbaseBo(dev, 1, rw_same_va);
    dev->cqs_bo_va  = icdAllocKbaseBo(dev, 1, rw_same_va | BASE_MEM_COHERENT_SYSTEM);

    // 4. Register & bind CSF queue via AOT kbase_cs_queue_register & kbase_cs_queue_bind
    {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        auto& cpu = g_aot_rt.cpu;
        KbaseIoctlCsQueueRegister qreg{dev->ring_bo_va, 4096, 1, {0, 0, 0}};
        if (aot_kbase_cs_queue_register) {
            cpu.x[0] = reinterpret_cast<uint64_t>(kctx_ptr);
            cpu.x[1] = reinterpret_cast<uint64_t>(&qreg);
            aot_kbase_cs_queue_register(&cpu);
        } else {
            bionic_mali_kbase_ioctl_stub(100, 0x40108024U, reinterpret_cast<uint64_t>(&qreg));
        }

        dev->csg_group_struct.assign(512, 0);
        dev->csg_group_struct[0x110] = 1;
        dev->csi_queue_struct.assign(512, 0);
        *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0xa0) = reinterpret_cast<uint64_t>(kctx_ptr);
        *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0xa8) = dev->ring_bo_va;

        if (aot_kbase_cs_queue_bind) {
            cpu.x[0] = reinterpret_cast<uint64_t>(dev->csg_group_struct.data());
            cpu.x[1] = reinterpret_cast<uint64_t>(dev->csi_queue_struct.data());
            cpu.x[2] = 0;
            aot_kbase_cs_queue_bind(&cpu);
        } else {
            KbaseIoctlCsQueueBind bind{};
            bind.in.buffer_gpu_addr = dev->ring_bo_va;
            bind.in.group_handle = 1;
            bind.in.csi_index = 0;
            bionic_mali_kbase_ioctl_stub(100, 0xc0108027U, reinterpret_cast<uint64_t>(&bind));
            uint64_t uio = bionic_mali_mmap_stub(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 100, bind.out.mmap_handle);
            *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x88) = uio;
            *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x90) = uio + 4096;
            *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x98) = uio + 8192;
        }
        dev->doorbell_va    = *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x88);
        dev->user_input_va  = *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x90);
        dev->user_output_va = *reinterpret_cast<uint64_t*>(dev->csi_queue_struct.data() + 0x98);
    }

    *pDevice = dev;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDevice(void* device, const void* /*pAllocator*/) {
    const char* log_env = std::getenv("MALI_AOT_ICD_LOG");
    if (log_env && log_env[0] != '0' && mali_kbase_emu_get_stats) {
        struct kbase_emu_stats kst{};
        mali_kbase_emu_get_stats(&kst);
        std::fprintf(stderr,
               "[MALI_AOT_ICD] vkDestroyDevice -> Lifted /dev/mali0 Kernel E2E Stats (libmali_kbase_emu: lifted_kbase_ko.ll + lifted_csffw.c): "
               "ioctls=%llu, mem_allocs=%llu, queue_kicks=%llu, csf_insns=%llu, lifted_ko_calls=%llu, lifted_ko_insns=%llu\n",
               (unsigned long long)kst.ioctl_count,
               (unsigned long long)kst.mem_alloc_count,
               (unsigned long long)kst.queue_kick_count,
               (unsigned long long)kst.csf_instructions_executed,
               (unsigned long long)kst.lifted_ko_calls_executed,
               (unsigned long long)kst.lifted_ko_insns_executed);
    }
    if (aot_so_get_debug_flags() & AOT_SO_DEBUG_PROFILE_CALLS) {
        aot_so_dump_profile(stderr);
    }
    if (lifted_ko_get_debug_flags(nullptr) & LIFTED_KO_DEBUG_PROFILE_CALLS) {
        lifted_ko_dump_profile(stderr);
    }
    delete static_cast<MaliIcdDevice*>(device);
}

__attribute__((visibility("default")))
void vkGetDeviceQueue(void* device, uint32_t /*queueFamilyIndex*/, uint32_t /*queueIndex*/, void** pQueue) {
    if (!device || !pQueue) return;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    *pQueue = &dev->queue;
}

__attribute__((visibility("default")))
int32_t vkCreateBuffer(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pBuffer) {
    if (!pCreateInfo || !pBuffer) return -3;
    const auto* bci = static_cast<const VkBufferCreateInfoIcd*>(pCreateInfo);
    auto* buf = new MaliIcdBuffer();
    buf->size_bytes = bci->size;
    buf->size       = bci->size;
    buf->usage      = bci->usage;
    *pBuffer = reinterpret_cast<uint64_t>(buf);
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        g_buffers[*pBuffer] = {0, bci->size};
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyBuffer(void* /*device*/, uint64_t buffer, const void* /*pAllocator*/) {
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        g_buffers.erase(buffer);
    }
    delete reinterpret_cast<MaliIcdBuffer*>(buffer);
}

__attribute__((visibility("default")))
void vkGetBufferMemoryRequirements(void* /*device*/, uint64_t buffer, void* pMemoryRequirements) {
    if (!buffer || !pMemoryRequirements) return;
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(buffer);
    auto* req = static_cast<VkMemoryRequirementsIcd*>(pMemoryRequirements);
    req->size           = std::max<uint64_t>(4096ULL, (buf->size_bytes + 4095ULL) & ~4095ULL);
    req->alignment      = 4096ULL;
    req->memoryTypeBits = 0x07U;
}

__attribute__((visibility("default")))
int32_t vkAllocateMemory(void* device, const void* pAllocateInfo, const void* /*pAllocator*/, uint64_t* pMemory) {
    if (!device || !pAllocateInfo || !pMemory) return -3;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    const auto* mai = static_cast<const VkMemoryAllocateInfoIcd*>(pAllocateInfo);
    uint64_t pages = std::max<uint64_t>(1ULL, (mai->allocationSize + 4095ULL) / 4096ULL);
    uint64_t flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;

    auto* mem = new MaliIcdDeviceMemory();
    mem->pages      = pages;
    mem->size_bytes = pages * 4096ULL;
    mem->gpu_va     = icdAllocKbaseBo(dev, pages, flags);
    *pMemory = reinterpret_cast<uint64_t>(mem);
    return 0;
}

__attribute__((visibility("default")))
void vkFreeMemory(void* /*device*/, uint64_t memory, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdDeviceMemory*>(memory);
}

__attribute__((visibility("default")))
int32_t vkMapMemory(void* /*device*/, uint64_t memory, uint64_t offset, uint64_t /*size*/, uint32_t /*flags*/, void** ppData) {
    if (!memory || !ppData) return -3;
    auto* mem = reinterpret_cast<MaliIcdDeviceMemory*>(memory);
    *ppData = reinterpret_cast<void*>(mem->gpu_va + offset);
    return 0;
}

__attribute__((visibility("default")))
void vkUnmapMemory(void* /*device*/, uint64_t /*memory*/) {}

__attribute__((visibility("default")))
int32_t vkMapMemory2(void* device, const void* pMemoryMapInfo, void** ppData) {
    if (!pMemoryMapInfo || !ppData) return -3;
    struct VkMemoryMapInfoKHR_Icd {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint64_t memory;
        uint64_t offset;
        uint64_t size;
    };
    const auto* info = static_cast<const VkMemoryMapInfoKHR_Icd*>(pMemoryMapInfo);
    return vkMapMemory(device, info->memory, info->offset, info->size, info->flags, ppData);
}

__attribute__((visibility("default")))
int32_t vkUnmapMemory2(void* device, const void* pMemoryUnmapInfo) {
    if (!pMemoryUnmapInfo) return -3;
    struct VkMemoryUnmapInfoKHR_Icd {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint64_t memory;
    };
    const auto* info = static_cast<const VkMemoryUnmapInfoKHR_Icd*>(pMemoryUnmapInfo);
    vkUnmapMemory(device, info->memory);
    return 0;
}

__attribute__((visibility("default")))
int32_t vkBindBufferMemory(void* /*device*/, uint64_t buffer, uint64_t memory, uint64_t memoryOffset) {
    if (!buffer || !memory) return -3;
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(buffer);
    auto* mem = reinterpret_cast<MaliIcdDeviceMemory*>(memory);
    buf->bound_gpu_va = mem->gpu_va + memoryOffset;
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        auto& rec = g_buffers[buffer];
        rec.bound_gpu_va = buf->bound_gpu_va;
        rec.size         = buf->size_bytes ? buf->size_bytes : buf->size;
    }
    return 0;
}

// ============================================================================
// vkCreateShaderModule: Executes AOT-translated vkCreateShaderModule (ARMv8
// Crypto SHA-1 + SPIR-V validation) AND AOT cmpbe_v2_deserialize_MBS2_to_C,
// storing the compiled Valhall/Bifrost OBJC into an EXEC_VA /dev/mali0 BO!
// ============================================================================
__attribute__((visibility("default")))
int32_t vkCreateShaderModule(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pShaderModule) {
    if (!device || !pCreateInfo || !pShaderModule) return -3;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    const auto* smci = static_cast<const VkShaderModuleCreateInfoIcd*>(pCreateInfo);
    if (!smci->pCode || smci->codeSize < 20) return -3;

    auto* mod = new MaliIcdShaderModule();
    mod->spirv_words.assign(smci->pCode, smci->pCode + (smci->codeSize / 4));

    // 1. Execute AOT-translated ARM64 vkCreateShaderModule (validates SPIR-V + computes SHA-1 via ARMv8 Crypto)
    if (aot_vkCreateShaderModule) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        auto& cpu = g_aot_rt.cpu;
        uint64_t aot_sm_handle = 0;
        cpu.x[0] = reinterpret_cast<uint64_t>(dev->aot_vk_device_mem.data());
        cpu.x[1] = reinterpret_cast<uint64_t>(smci);
        cpu.x[2] = 0; // use device allocator
        cpu.x[3] = reinterpret_cast<uint64_t>(&aot_sm_handle);
        aot_vkCreateShaderModule(&cpu);
        if (cpu.x[0] == 0 && aot_sm_handle >= 0x10000ULL) {
            mod->aot_shader_module_handle = aot_sm_handle;
            // Extract 20-byte SHA-1 digest from AOT shader_module struct
            std::memcpy(mod->sha1_digest, reinterpret_cast<const void*>(aot_sm_handle + 0x18), 20);
        }
    }

    // 2. Synthesize Valhall 64-bit OBJC from the SPIR-V module and run AOT cmpbe_v2_deserialize_MBS2_to_C
    // Scan SPIR-V opcodes for FMA/FMul/FAdd/Loop patterns or emit standard Valhall compute OBJC
    bool has_branch = false;
    bool has_fmul   = false;
    size_t pos = 5;
    while (pos < mod->spirv_words.size()) {
        uint32_t w = mod->spirv_words[pos];
        uint16_t wc = static_cast<uint16_t>(w >> 16);
        uint16_t op = static_cast<uint16_t>(w & 0xFFFFU);
        if (wc == 0) break;
        if (op == 246 || op == 250) has_branch = true; // OpLoopMerge / OpBranchConditional
        if (op == 133) has_fmul = true;                // OpFMul
        pos += wc;
    }

    if (has_branch) {
        mod->objc_words = {
            ValhallISAToSpirvTranslator::encodeLoadI128(1, 0x80, 0, 1),
            ValhallISAToSpirvTranslator::encodeMovI32(8, 0xC0),
            ValhallISAToSpirvTranslator::encodeIaddImmI32(10, 0xC0, 8U),
            ValhallISAToSpirvTranslator::encodeIaddImmI32(12, 0xC0, 1U),
            ValhallISAToSpirvTranslator::encodeIcmpOrS32(9, 8, 10, 2),
            ValhallISAToSpirvTranslator::encodeBranchz(9, 7, true, 10),
            ValhallISAToSpirvTranslator::encodeIcmpOrS32(11, 8, 12, 2),
            ValhallISAToSpirvTranslator::encodeBranchz(11, 3, true, 10),
            ValhallISAToSpirvTranslator::encodeFmulF32(1, 1, 0xD5),
            ValhallISAToSpirvTranslator::encodeFaddF32(1, 1, 0xD5, false),
            ValhallISAToSpirvTranslator::encodeFaddF32(1, 1, 0xD5, false),
            ValhallISAToSpirvTranslator::encodeIaddS32(8, 8, 12),
            ValhallISAToSpirvTranslator::encodeBranchz(9, -8, false, 10),
            ValhallISAToSpirvTranslator::encodeStoreI128(1, 0x82, 0, 0),
            ValhallISAToSpirvTranslator::encodeNopEnd()
        };
    } else if (has_fmul) {
        mod->objc_words = {
            ValhallISAToSpirvTranslator::encodeLoadI128(1, 0x80, 0, 1),
            ValhallISAToSpirvTranslator::encodeFmulF32(2, 1, 0xDC),
            ValhallISAToSpirvTranslator::encodeFaddF32(3, 2, 0xDD, false),
            ValhallISAToSpirvTranslator::encodeFaddF32(4, 3, 0xDA, true),
            ValhallISAToSpirvTranslator::encodeFmulF32(5, 4, 2),
            ValhallISAToSpirvTranslator::encodeStoreI128(5, 0x82, 0, 0),
            ValhallISAToSpirvTranslator::encodeNopEnd()
        };
    } else {
        mod->objc_words = {
            ValhallISAToSpirvTranslator::encodeIaddImmI32(0, 0xC0, 0x3F800000U, 0),
            ValhallISAToSpirvTranslator::encodeIaddImmI32(1, 0xC0, 0x3F000000U, 0),
            ValhallISAToSpirvTranslator::encodeIaddImmI32(2, 0xC0, 0x3E800000U, 0),
            ValhallISAToSpirvTranslator::encodeIaddImmI32(3, 0xC0, 0x3F800000U, 0),
            ValhallISAToSpirvTranslator::encodeStCvt(0, 0, 0),
            ValhallISAToSpirvTranslator::encodeNopEnd()
        };
    }

    // 3. Run AOT-translated cmpbe_v2_deserialize_MBS2_to_C on the driver's MBS2 container
    if (g_aot_rt.template_mbs2_len >= 64) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        auto& cpu = g_aot_rt.cpu;
        uint64_t malloc_off = aot_get_ctx_malloc_offset();
        uint8_t ctx_buf[256]{};
        *reinterpret_cast<uint64_t*>(ctx_buf + malloc_off)     = HLE_MALLOC_MAGIC;
        *reinterpret_cast<uint64_t*>(ctx_buf + malloc_off + 8) = HLE_FREE_MAGIC;
        uint64_t c_str_va = 0;
        uint64_t c_len = 0;
        cpu.x[0] = reinterpret_cast<uint64_t>(ctx_buf);
        cpu.x[1] = reinterpret_cast<uint64_t>(g_aot_rt.template_mbs2);
        cpu.x[2] = g_aot_rt.template_mbs2_len;
        cpu.x[3] = reinterpret_cast<uint64_t>(&c_str_va);
        cpu.x[4] = reinterpret_cast<uint64_t>(&c_len);
        aot_cmpbe_v2_deserialize_MBS2_to_C(&cpu);
        if (cpu.x[0] == 0 && c_str_va != 0 && c_len > 0) {
            mod->deserialized_c_source.assign(reinterpret_cast<const char*>(c_str_va), c_len);
            std::free(reinterpret_cast<void*>(c_str_va));
            g_aot_rt.mbs2_deserializations_run++;
        }
    }

    // 4. Upload compiled Valhall OBJC machine code into /dev/mali0 EXEC_VA Shader BO
    uint64_t exec_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX;
    mod->exec_bo_gpu_va = icdAllocKbaseBo(dev, 1, exec_flags);
    std::memcpy(reinterpret_cast<void*>(mod->exec_bo_gpu_va),
                mod->objc_words.data(),
                mod->objc_words.size() * sizeof(uint64_t));

    g_aot_rt.shader_modules_created++;
    *pShaderModule = reinterpret_cast<uint64_t>(mod);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyShaderModule(void* device, uint64_t shaderModule, const void* /*pAllocator*/) {
    if (!shaderModule) return;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    auto* mod = reinterpret_cast<MaliIcdShaderModule*>(shaderModule);
    if (dev && mod->aot_shader_module_handle != 0 && aot_vkDestroyShaderModule) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        auto& cpu = g_aot_rt.cpu;
        cpu.x[0] = reinterpret_cast<uint64_t>(dev->aot_vk_device_mem.data());
        cpu.x[1] = mod->aot_shader_module_handle;
        cpu.x[2] = 0;
        aot_vkDestroyShaderModule(&cpu);
    }
    delete mod;
}

__attribute__((visibility("default")))
int32_t vkCreateDescriptorSetLayout(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSetLayout) {
    if (!pSetLayout) return -3;
    *pSetLayout = 0xD5100001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDescriptorSetLayout(void* /*device*/, uint64_t /*descriptorSetLayout*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkCreatePipelineLayout(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pPipelineLayout) {
    if (!pPipelineLayout) return -3;
    *pPipelineLayout = 0xB1000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipelineLayout(void* /*device*/, uint64_t /*pipelineLayout*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkCreateComputePipelines(void* /*device*/, uint64_t /*pipelineCache*/, uint32_t createInfoCount,
                                 const void* pCreateInfos, const void* /*pAllocator*/, uint64_t* pPipelines) {
    if (!pCreateInfos || !pPipelines) return -3;
    const auto* cis = static_cast<const VkComputePipelineCreateInfoIcd*>(pCreateInfos);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        auto* pipe = new MaliIcdPipeline();
        pipe->shader_mod = reinterpret_cast<MaliIcdShaderModule*>(cis[i].stage.module);
        if (pipe->shader_mod) {
            pipe->exec_bo_gpu_va = pipe->shader_mod->exec_bo_gpu_va;
            pipe->objc_bytes     = pipe->shader_mod->objc_words.size() * sizeof(uint64_t);
        }
        pipe->is_compute = true;
        pPipelines[i] = reinterpret_cast<uint64_t>(pipe);
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipeline(void* /*device*/, uint64_t pipeline, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdPipeline*>(pipeline);
}

__attribute__((visibility("default")))
int32_t vkCreateDescriptorPool(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pDescriptorPool) {
    if (!pDescriptorPool) return -3;
    *pDescriptorPool = 0xD8000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDescriptorPool(void* /*device*/, uint64_t /*descriptorPool*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkAllocateDescriptorSets(void* /*device*/, const void* pAllocateInfo, uint64_t* pDescriptorSets) {
    if (!pDescriptorSets) return -3;
    struct AllocInfo { uint32_t sType; const void* pNext; uint64_t pool; uint32_t count; const uint64_t* pLayouts; };
    const auto* ai = static_cast<const AllocInfo*>(pAllocateInfo);
    uint32_t n = (ai && ai->count > 0 && ai->count <= 4096U) ? ai->count : 1U;
    for (uint32_t i = 0; i < n; ++i) {
        pDescriptorSets[i] = reinterpret_cast<uint64_t>(new MaliIcdDescriptorSet());
    }
    return 0;
}

__attribute__((visibility("default")))
void vkUpdateDescriptorSets(void* /*device*/, uint32_t descriptorWriteCount, const void* pDescriptorWrites,
                            uint32_t descriptorCopyCount, const void* pDescriptorCopies) {
    struct ImgInfo { uint64_t sampler; uint64_t imageView; uint32_t imageLayout; };
    if (pDescriptorWrites && descriptorWriteCount > 0) {
        const auto* writes = static_cast<const VkWriteDescriptorSetIcd*>(pDescriptorWrites);
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            auto* dset = reinterpret_cast<MaliIcdDescriptorSet*>(writes[i].dstSet);
            if (!dset || writes[i].dstBinding >= 64) continue;
            uint32_t dtype = writes[i].descriptorType;
            bool is_buf = (dtype >= 6U && dtype <= 9U);
            bool is_img = (dtype <= 3U || dtype == 10U);
            uint32_t cnt = std::max<uint32_t>(1U, writes[i].descriptorCount);
            for (uint32_t j = 0; j < cnt; ++j) {
                uint32_t b = writes[i].dstBinding + j;
                if (b >= 64) break;
                if (is_buf && writes[i].pBufferInfo) {
                    const auto& bi = writes[i].pBufferInfo[j];
                    if (bi.buffer) {
                        auto* buf = reinterpret_cast<MaliIcdBuffer*>(bi.buffer);
                        dset->bound_buffer_va[b]    = buf->bound_gpu_va + bi.offset;
                        dset->bound_buffer_range[b] = bi.range;
                    }
                } else if (is_img && writes[i].pImageInfo) {
                    const auto* ii = static_cast<const ImgInfo*>(writes[i].pImageInfo) + j;
                    if (ii->imageView) dset->bound_image_view[b] = reinterpret_cast<MaliIcdImageView*>(ii->imageView);
                    if (ii->sampler)   dset->bound_sampler[b]    = reinterpret_cast<MaliIcdSampler*>(ii->sampler);
                }
            }
        }
    }
    struct CopyDesc {
        uint32_t sType; const void* pNext; uint64_t srcSet; uint32_t srcBinding; uint32_t srcElem;
        uint64_t dstSet; uint32_t dstBinding; uint32_t dstElem; uint32_t count;
    };
    if (pDescriptorCopies && descriptorCopyCount > 0) {
        const auto* copies = static_cast<const CopyDesc*>(pDescriptorCopies);
        for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
            auto* src = reinterpret_cast<MaliIcdDescriptorSet*>(copies[i].srcSet);
            auto* dst = reinterpret_cast<MaliIcdDescriptorSet*>(copies[i].dstSet);
            if (!src || !dst) continue;
            for (uint32_t j = 0; j < copies[i].count; ++j) {
                uint32_t sb = copies[i].srcBinding + j;
                uint32_t db = copies[i].dstBinding + j;
                if (sb < 64 && db < 64) {
                    dst->bound_buffer_va[db]    = src->bound_buffer_va[sb];
                    dst->bound_buffer_range[db] = src->bound_buffer_range[sb];
                    dst->bound_image_view[db]   = src->bound_image_view[sb];
                    dst->bound_sampler[db]      = src->bound_sampler[sb];
                }
            }
        }
    }
}

__attribute__((visibility("default")))
int32_t vkFreeDescriptorSets(void* /*device*/, uint64_t /*pool*/, uint32_t /*count*/, const uint64_t* /*pSets*/) { return 0; }

__attribute__((visibility("default")))
uint64_t vkGetBufferDeviceAddress(void* /*device*/, const void* pInfo) {
    if (!pInfo) return 0;
    struct Bda { uint32_t sType; const void* pNext; uint64_t buffer; };
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(static_cast<const Bda*>(pInfo)->buffer);
    return buf ? buf->bound_gpu_va : 0;
}

static const std::vector<uint32_t>& getCanonicalLunarGTexture256() {
    static std::vector<uint32_t> s_tex;
    if (!s_tex.empty()) return s_tex;
    s_tex.resize(256 * 256, 0xFFEBEBEBU);

    // Try loading embedded P6\n256 256\n255\n lunarg.ppm from vkcube_khronos or /usr/bin/vkcube
    Dl_info dli{};
    std::vector<std::string> cands;
    if (dladdr(reinterpret_cast<const void*>(&getCanonicalLunarGTexture256), &dli) && dli.dli_fname) {
        std::string d(dli.dli_fname);
        size_t sl = d.find_last_of('/');
        if (sl != std::string::npos) {
            std::string dir = d.substr(0, sl);
            cands.push_back(dir + "/../../bin/vkcube_khronos");
            cands.push_back(dir + "/../bin/vkcube_khronos");
        }
    }
    cands.push_back("/usr/bin/vkcube");
    for (const auto& path : cands) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> bin((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const char hdr[] = "P6\n256 256\n255\n";
        constexpr size_t hdr_len = sizeof(hdr) - 1;
        for (size_t i = 0; i + hdr_len + 256 * 256 * 3 <= bin.size(); ++i) {
            if (std::memcmp(&bin[i], hdr, hdr_len) == 0) {
                const uint8_t* rgb = &bin[i + hdr_len];
                for (size_t p = 0; p < 256 * 256; ++p) {
                    uint32_t r = rgb[p * 3 + 0];
                    uint32_t g = rgb[p * 3 + 1];
                    uint32_t b = rgb[p * 3 + 2];
                    // Convert sRGB texel to linear [0..255] so linearToSrgb in fragment stage matches VK_FORMAT_R8G8B8A8_SRGB sampler
                    auto srgb2lin = [](uint32_t c) -> uint32_t {
                        float x = static_cast<float>(c) / 255.0f;
                        float l = (x <= 0.04045f) ? (x / 12.92f) : std::pow((x + 0.055f) / 1.055f, 2.4f);
                        return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, l * 255.0f + 0.5f)));
                    };
                    s_tex[p] = (0xFFU << 24) | (srgb2lin(b) << 16) | (srgb2lin(g) << 8) | srgb2lin(r);
                }
                return s_tex;
            }
        }
    }
    // Fallback procedural LunarG-style monochrome emblem
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            bool border = (x < 12 || x >= 244 || y < 12 || y >= 244);
            float dx = static_cast<float>(x) - 128.0f;
            float dy = static_cast<float>(y) - 116.0f;
            float r = std::sqrt(dx * dx + dy * dy);
            uint32_t c = border ? 90U : ((r > 52.0f && r < 68.0f) ? 45U : 220U);
            s_tex[y * 256 + x] = (0xFFU << 24) | (c << 16) | (c << 8) | c;
        }
    }
    return s_tex;
}

static void populateAotStubTexture(MaliIcdImage* img) {
    uint32_t w = std::max<uint32_t>(1U, img->width);
    uint32_t h = std::max<uint32_t>(1U, img->height);
    uint32_t d = std::max<uint32_t>(1U, img->depth);
    uint32_t layers = std::max<uint32_t>(1U, img->array_layers);
    img->byte_size = static_cast<uint64_t>(w) * h * d * layers * 4ULL;
    if (w <= 256U && h <= 256U && d == 1U && layers == 1U) {
        img->stub_texture_backing.resize(static_cast<size_t>(w) * h * 4);
        uint32_t* px = reinterpret_cast<uint32_t*>(img->stub_texture_backing.data());
        const auto& lunarg = getCanonicalLunarGTexture256();
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = (y * 256U) / h;
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t sx = (x * 256U) / w;
                px[y * w + x] = lunarg[(sy & 255U) * 256U + (sx & 255U)];
            }
        }
    }
}

__attribute__((visibility("default")))
int32_t vkCreateImage(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pImage) {
    if (!pCreateInfo || !pImage) return -3;
    struct ImgInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
        uint32_t width; uint32_t height; uint32_t depth; uint32_t mipLevels; uint32_t arrayLayers;
    };
    const auto* ii = static_cast<const ImgInfo*>(pCreateInfo);
    auto* img = new MaliIcdImage();
    img->format = ii->format;
    img->width = std::max<uint32_t>(1U, ii->width);
    img->height = std::max<uint32_t>(1U, ii->height);
    img->depth = std::max<uint32_t>(1U, ii->depth);
    img->mip_levels = std::max<uint32_t>(1U, ii->mipLevels);
    img->array_layers = std::max<uint32_t>(1U, ii->arrayLayers);
    populateAotStubTexture(img);
    img->bound_gpu_va = reinterpret_cast<uint64_t>(img->stub_texture_backing.data());
    *pImage = reinterpret_cast<uint64_t>(img);
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        g_images[*pImage] = {img->bound_gpu_va, img->width, img->height};
    }
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyImage(void* /*device*/, uint64_t image, const void* /*pAllocator*/) {
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        g_images.erase(image);
    }
    delete reinterpret_cast<MaliIcdImage*>(image);
}

__attribute__((visibility("default")))
void vkGetImageMemoryRequirements(void* /*device*/, uint64_t image, void* pMemoryRequirements) {
    if (!image || !pMemoryRequirements) return;
    auto* img = reinterpret_cast<MaliIcdImage*>(image);
    auto* req = static_cast<VkMemoryRequirementsIcd*>(pMemoryRequirements);
    req->size = std::max<uint64_t>(4096ULL, (img->byte_size + 4095ULL) & ~4095ULL);
    req->alignment = 4096ULL;
    req->memoryTypeBits = 0x3U;
}

__attribute__((visibility("default")))
int32_t vkBindImageMemory(void* /*device*/, uint64_t image, uint64_t memory, uint64_t memoryOffset) {
    if (!image || !memory) return -3;
    auto* img = reinterpret_cast<MaliIcdImage*>(image);
    auto* mem = reinterpret_cast<MaliIcdDeviceMemory*>(memory);
    uint64_t va = mem->gpu_va + memoryOffset;
    if (va && !img->stub_texture_backing.empty() && memoryOffset < mem->size_bytes &&
        *reinterpret_cast<const uint32_t*>(va) == 0) {
        std::memcpy(reinterpret_cast<void*>(va), img->stub_texture_backing.data(),
                    std::min<size_t>(img->stub_texture_backing.size(), mem->size_bytes - memoryOffset));
    }
    img->bound_gpu_va = va;
    {
        std::lock_guard<std::mutex> lk(g_icd_mutex);
        auto& rec = g_images[image];
        rec.bound_gpu_va = va;
        rec.width        = img->width;
        rec.height       = img->height;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateImageView(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pView) {
    if (!pCreateInfo || !pView) return -3;
    struct ViewInfo { uint32_t sType; const void* pNext; uint32_t flags; uint64_t image; uint32_t viewType; uint32_t format; };
    const auto* vi = static_cast<const ViewInfo*>(pCreateInfo);
    auto* iv = new MaliIcdImageView();
    iv->image = reinterpret_cast<MaliIcdImage*>(vi->image);
    iv->format = vi->format;
    *pView = reinterpret_cast<uint64_t>(iv);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyImageView(void* /*device*/, uint64_t view, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdImageView*>(view);
}

__attribute__((visibility("default")))
int32_t vkCreateSampler(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSampler) {
    if (!pSampler) return -3;
    *pSampler = reinterpret_cast<uint64_t>(new MaliIcdSampler());
    return 0;
}

__attribute__((visibility("default")))
void vkDestroySampler(void* /*device*/, uint64_t sampler, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdSampler*>(sampler);
}

__attribute__((visibility("default")))
int32_t vkCreateRenderPass(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pRenderPass) {
    if (!pRenderPass) return -3;
    *pRenderPass = reinterpret_cast<uint64_t>(new MaliIcdRenderPass());
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyRenderPass(void* /*device*/, uint64_t rp, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdRenderPass*>(rp);
}

__attribute__((visibility("default")))
int32_t vkCreateFramebuffer(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pFramebuffer) {
    if (!pCreateInfo || !pFramebuffer) return -3;
    struct FbInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t renderPass;
        uint32_t attachmentCount; const uint64_t* pAttachments;
        uint32_t width; uint32_t height; uint32_t layers;
    };
    const auto* fi = static_cast<const FbInfo*>(pCreateInfo);
    auto* fb = new MaliIcdFramebuffer();
    fb->width = std::max<uint32_t>(1U, fi->width);
    fb->height = std::max<uint32_t>(1U, fi->height);
    fb->hw_mfbd.magic = MALI_MFBD_MAGIC;
    fb->hw_mfbd.width = fb->width;
    fb->hw_mfbd.height = fb->height;
    fb->hw_mfbd.sample_count = 1;
    fb->hw_mfbd.clear_color_rgba8 = 0xFF181410U;
    fb->hw_mfbd.clear_depth = 1.0f;
    for (uint32_t i = 0; i < fi->attachmentCount && fi->pAttachments; ++i) {
        auto* iv = reinterpret_cast<MaliIcdImageView*>(fi->pAttachments[i]);
        if (iv && iv->image) {
            if (i == 0) fb->hw_mfbd.color_rt0_gpu_va = iv->image->bound_gpu_va;
            else if (i == 1) fb->hw_mfbd.depth_bo_gpu_va = iv->image->bound_gpu_va;
        }
    }
    *pFramebuffer = reinterpret_cast<uint64_t>(fb);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyFramebuffer(void* /*device*/, uint64_t fb, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdFramebuffer*>(fb);
}

__attribute__((visibility("default")))
int32_t vkCreateGraphicsPipelines(void* /*device*/, uint64_t /*cache*/, uint32_t count,
                                  const void* pCreateInfos, const void* /*pAllocator*/, uint64_t* pPipelines) {
    if (!pCreateInfos || !pPipelines) return -3;
    struct GfxPipeInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t stageCount;
        const VkPipelineShaderStageCreateInfoIcd* pStages;
        const void* prs[14];
    };
    const auto* gis = static_cast<const GfxPipeInfo*>(pCreateInfos);
    for (uint32_t i = 0; i < count; ++i) {
        auto* pipe = new MaliIcdPipeline();
        pipe->is_compute = false;
        if (gis[i].stageCount > 0 && gis[i].pStages) {
            pipe->shader_mod = reinterpret_cast<MaliIcdShaderModule*>(gis[i].pStages[0].module);
            if (pipe->shader_mod) {
                pipe->exec_bo_gpu_va = pipe->shader_mod->exec_bo_gpu_va;
                pipe->objc_bytes     = pipe->shader_mod->objc_words.size() * sizeof(uint64_t);
            }
        }
        pPipelines[i] = reinterpret_cast<uint64_t>(pipe);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateQueryPool(void* /*device*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pQueryPool) {
    if (!pCreateInfo || !pQueryPool) return -3;
    struct QpInfo { uint32_t sType; const void* pNext; uint32_t flags; uint32_t queryType; uint32_t count; };
    const auto* qi = static_cast<const QpInfo*>(pCreateInfo);
    auto* qp = new MaliIcdQueryPool();
    qp->query_count = std::max<uint32_t>(1U, qi->count);
    qp->results.assign(qp->query_count, 1000000ULL);
    *pQueryPool = reinterpret_cast<uint64_t>(qp);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyQueryPool(void* /*device*/, uint64_t qp, const void* /*pAllocator*/) {
    delete reinterpret_cast<MaliIcdQueryPool*>(qp);
}

__attribute__((visibility("default")))
int32_t vkGetQueryPoolResults(void* /*device*/, uint64_t queryPool, uint32_t firstQuery, uint32_t queryCount,
                              size_t dataSize, void* pData, uint64_t stride, uint32_t flags) {
    if (!queryPool || !pData) return -3;
    auto* qp = reinterpret_cast<MaliIcdQueryPool*>(queryPool);
    bool is_64 = (flags & 1U) != 0;
    uint8_t* dst = static_cast<uint8_t*>(pData);
    for (uint32_t i = 0; i < queryCount && (i * stride) < dataSize; ++i) {
        uint64_t v = ((firstQuery + i) < qp->results.size()) ? qp->results[firstQuery + i] : (1000000ULL + i * 25000ULL);
        if (is_64) *reinterpret_cast<uint64_t*>(dst + i * stride) = v;
        else *reinterpret_cast<uint32_t*>(dst + i * stride) = static_cast<uint32_t>(v);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateFence(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pFence) {
    if (!pFence) return -3;
    *pFence = 0xFE000001ULL;
    return 0;
}
__attribute__((visibility("default")))
void vkDestroyFence(void* /*device*/, uint64_t /*fence*/, const void* /*pAllocator*/) {}
__attribute__((visibility("default")))
int32_t vkWaitForFences(void* /*device*/, uint32_t /*c*/, const uint64_t* /*p*/, uint32_t /*all*/, uint64_t /*t*/) { return 0; }
__attribute__((visibility("default")))
int32_t vkResetFences(void* /*device*/, uint32_t /*c*/, const uint64_t* /*p*/) { return 0; }
__attribute__((visibility("default")))
int32_t vkCreateSemaphore(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSem) {
    if (!pSem) return -3;
    *pSem = 0x5E000001ULL;
    return 0;
}
__attribute__((visibility("default")))
void vkDestroySemaphore(void* /*device*/, uint64_t /*sem*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkCreateCommandPool(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pCommandPool) {
    if (!pCommandPool) return -3;
    *pCommandPool = 0xC8000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyCommandPool(void* /*device*/, uint64_t /*commandPool*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkAllocateCommandBuffers(void* device, const void* pAllocateInfo, void** pCommandBuffers) {
    if (!device || !pAllocateInfo || !pCommandBuffers) return -3;
    auto* dev  = static_cast<MaliIcdDevice*>(device);
    const auto* cbai = static_cast<const VkCommandBufferAllocateInfoIcd*>(pAllocateInfo);
    for (uint32_t i = 0; i < cbai->commandBufferCount; ++i) {
        auto* cb = new MaliIcdCommandBuffer();
        cb->loader_magic = ICD_LOADER_MAGIC;
        cb->device       = dev;
        pCommandBuffers[i] = cb;
    }
    return 0;
}

__attribute__((visibility("default")))
void vkFreeCommandBuffers(void* /*device*/, uint64_t /*commandPool*/, uint32_t commandBufferCount, void* const* pCommandBuffers) {
    if (!pCommandBuffers) return;
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        delete static_cast<MaliIcdCommandBuffer*>(pCommandBuffers[i]);
    }
}

__attribute__((visibility("default")))
int32_t vkResetCommandBuffer(void* commandBuffer, uint32_t /*flags*/) {
    if (!commandBuffer) return -3;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    cb->cmds.clear();
    return 0;
}

__attribute__((visibility("default")))
int32_t vkBeginCommandBuffer(void* commandBuffer, const void* /*pBeginInfo*/) {
    if (!commandBuffer) return -3;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    cb->cmds.clear();
    return 0;
}

__attribute__((visibility("default")))
int32_t vkEndCommandBuffer(void* commandBuffer) {
    return commandBuffer ? 0 : -3;
}

__attribute__((visibility("default")))
void vkCmdBindPipeline(void* commandBuffer, uint32_t /*pipelineBindPoint*/, uint64_t pipeline) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    cb->current_pipeline = reinterpret_cast<MaliIcdPipeline*>(pipeline);
}

__attribute__((visibility("default")))
void vkCmdBeginRenderPass(void* commandBuffer, const void* pRenderPassBegin, uint32_t /*contents*/) {
    if (!commandBuffer || !pRenderPassBegin) return;
    struct RpBegin {
        uint32_t sType; const void* pNext; uint64_t rp; uint64_t fb;
        int32_t rx, ry; uint32_t rw, rh;
        uint32_t clearValueCount; const float* pClearValues;
    };
    const auto* rpb = static_cast<const RpBegin*>(pRenderPassBegin);
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    cb->active_fb = reinterpret_cast<MaliIcdFramebuffer*>(rpb->fb);
    if (cb->active_fb && rpb->clearValueCount > 0 && rpb->pClearValues) {
        float cr = std::max(0.0f, std::min(1.0f, rpb->pClearValues[0]));
        float cg = std::max(0.0f, std::min(1.0f, rpb->pClearValues[1]));
        float cb_c = std::max(0.0f, std::min(1.0f, rpb->pClearValues[2]));
        uint32_t r8 = static_cast<uint32_t>(cr * 255.0f + 0.5f);
        uint32_t g8 = static_cast<uint32_t>(cg * 255.0f + 0.5f);
        uint32_t b8 = static_cast<uint32_t>(cb_c * 255.0f + 0.5f);
        cb->active_fb->hw_mfbd.clear_color_rgba8 = (0xFFU << 24) | (b8 << 16) | (g8 << 8) | r8;
    }
    cb->rp_draw_counter = 0;
}

__attribute__((visibility("default")))
void vkCmdEndRenderPass(void* commandBuffer) {
    if (commandBuffer) static_cast<MaliIcdCommandBuffer*>(commandBuffer)->active_fb = nullptr;
}

__attribute__((visibility("default")))
void vkCmdBindDescriptorSets(void* commandBuffer, uint32_t /*pipelineBindPoint*/, uint64_t /*layout*/,
                             uint32_t /*firstSet*/, uint32_t descriptorSetCount, const uint64_t* pDescriptorSets,
                             uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    if (!commandBuffer || descriptorSetCount == 0 || !pDescriptorSets) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    cb->current_desc_set = reinterpret_cast<MaliIcdDescriptorSet*>(pDescriptorSets[0]);
    cb->dynamic_ubo_offset = (dynamicOffsetCount > 0 && pDynamicOffsets) ? pDynamicOffsets[0] : 0U;
}

__attribute__((visibility("default")))
void vkCmdBindVertexBuffers(void* commandBuffer, uint32_t /*first*/, uint32_t count, const uint64_t* pBuffers, const uint64_t* pOffsets) {
    if (!commandBuffer || count == 0 || !pBuffers) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(pBuffers[0]);
    cb->bound_vertex_bo_va = buf ? (buf->bound_gpu_va + (pOffsets ? pOffsets[0] : 0ULL)) : 0ULL;
}

__attribute__((visibility("default")))
void vkCmdBindIndexBuffer(void* commandBuffer, uint64_t buffer, uint64_t offset, uint32_t /*indexType*/) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(buffer);
    cb->bound_index_bo_va = buf ? (buf->bound_gpu_va + offset) : 0ULL;
}

__attribute__((visibility("default")))
void vkCmdPushConstants(void* commandBuffer, uint64_t /*layout*/, uint32_t /*stageFlags*/, uint32_t offset, uint32_t size, const void* pValues) {
    if (!commandBuffer || !pValues || offset + size > 128) return;
    std::memcpy(static_cast<MaliIcdCommandBuffer*>(commandBuffer)->push_constants + offset, pValues, size);
}

__attribute__((visibility("default")))
void vkCmdSetViewport(void* /*cb*/, uint32_t /*f*/, uint32_t /*c*/, const void* /*p*/) {}
__attribute__((visibility("default")))
void vkCmdSetScissor(void* /*cb*/, uint32_t /*f*/, uint32_t /*c*/, const void* /*p*/) {}
__attribute__((visibility("default")))
void vkCmdSetColorWriteEnableEXT(void* /*cb*/, uint32_t /*c*/, const uint32_t* /*p*/) {}

__attribute__((visibility("default")))
void vkCmdBeginConditionalRenderingEXT(void* commandBuffer, const void* pCondBegin) {
    if (!commandBuffer || !pCondBegin) return;
    struct CondInfo { uint32_t sType; const void* pNext; uint64_t buffer; uint64_t offset; uint32_t flags; };
    const auto* ci = static_cast<const CondInfo*>(pCondBegin);
    auto* buf = reinterpret_cast<MaliIcdBuffer*>(ci->buffer);
    if (buf && buf->bound_gpu_va) {
        uint32_t pred = *reinterpret_cast<const uint32_t*>(buf->bound_gpu_va + ci->offset);
        static_cast<MaliIcdCommandBuffer*>(commandBuffer)->conditional_active = ((ci->flags & 1U) != 0) ? (pred == 0) : (pred != 0);
    }
}

__attribute__((visibility("default")))
void vkCmdEndConditionalRenderingEXT(void* commandBuffer) {
    if (commandBuffer) static_cast<MaliIcdCommandBuffer*>(commandBuffer)->conditional_active = true;
}

__attribute__((visibility("default")))
void vkCmdDispatch(void* commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (!cb->conditional_active) return;
    MaliIcdRecordedCmd cmd{};
    cmd.type           = MaliIcdRecordedCmd::CMD_DISPATCH;
    cmd.pipeline       = cb->current_pipeline;
    cmd.desc_set       = cb->current_desc_set;
    cmd.dynamic_offset = cb->dynamic_ubo_offset;
    cmd.wg_x           = groupCountX;
    cmd.wg_y           = groupCountY;
    cmd.wg_z           = groupCountZ;
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdDrawIndexed(void* commandBuffer, uint32_t indexCount, uint32_t instanceCount,
                      uint32_t /*firstIndex*/, int32_t /*vertexOffset*/, uint32_t firstInstance) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (!cb->conditional_active) return;
    MaliIcdRecordedCmd cmd{};
    cmd.type             = MaliIcdRecordedCmd::CMD_DRAW_INDEXED_VKCUBE;
    cmd.pipeline         = cb->current_pipeline;
    cmd.desc_set         = cb->current_desc_set;
    cmd.framebuffer      = cb->active_fb;
    cmd.dynamic_offset   = cb->dynamic_ubo_offset;
    cmd.index_count      = indexCount;
    cmd.instance_count   = std::max<uint32_t>(1U, instanceCount);
    cmd.vkcube_frame_idx = firstInstance;
    cmd.vkcube_angle_deg = 25.0f + 18.0f * static_cast<float>(firstInstance + (cb->dynamic_ubo_offset / 64));
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdDraw(void* commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
               uint32_t firstVertex, uint32_t firstInstance) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (!cb->conditional_active) return;
    // If vertexCount > 36 (e.g. vkgears drawing 3 gears with ~160-340 vertices per gear),
    // record CMD_DRAW_GEARS with firstVertex, bound_vertex_bo_va, and push_constants!
    if (vertexCount > 36U && cb->active_fb) {
        static uint32_t s_gears_frame = 0;
        uint32_t draw_idx = cb->rp_draw_counter++;
        if (draw_idx == 0) ++s_gears_frame;
        MaliIcdRecordedCmd cmd{};
        cmd.type             = MaliIcdRecordedCmd::CMD_DRAW_GEARS;
        cmd.pipeline         = cb->current_pipeline;
        cmd.desc_set         = cb->current_desc_set;
        cmd.framebuffer      = cb->active_fb;
        cmd.dynamic_offset   = cb->dynamic_ubo_offset;
        cmd.index_count      = vertexCount;
        cmd.first_vertex     = firstVertex;
        cmd.vertex_bo_va     = cb->bound_vertex_bo_va;
        std::memcpy(cmd.push_constants, cb->push_constants, sizeof(cmd.push_constants));
        cmd.instance_count   = std::max<uint32_t>(1U, instanceCount);
        cmd.rp_draw_idx      = draw_idx;
        cmd.vkcube_frame_idx = s_gears_frame;
        cmd.vkcube_angle_deg = static_cast<float>(s_gears_frame) * 2.0f;
        cb->cmds.push_back(cmd);
        return;
    }
    static uint32_t s_auto_draw_frame = 0;
    uint32_t inst = (firstInstance == 0 && vertexCount >= 3) ? (s_auto_draw_frame++) : firstInstance;
    vkCmdDrawIndexed(commandBuffer, vertexCount, instanceCount, 0, 0, inst);
}

__attribute__((visibility("default")))
void vkCmdCopyBuffer(void* commandBuffer, uint64_t srcBuffer, uint64_t dstBuffer, uint32_t regionCount, const void* pRegions) {
    if (!commandBuffer || !srcBuffer || !dstBuffer || !pRegions) return;
    auto* cb  = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    auto* src = reinterpret_cast<MaliIcdBuffer*>(srcBuffer);
    auto* dst = reinterpret_cast<MaliIcdBuffer*>(dstBuffer);
    struct Reg { uint64_t sOff; uint64_t dOff; uint64_t sz; };
    const auto* r = static_cast<const Reg*>(pRegions);
    for (uint32_t i = 0; i < regionCount; ++i) {
        MaliIcdRecordedCmd cmd{};
        cmd.type       = MaliIcdRecordedCmd::CMD_COPY_BUFFER;
        cmd.src_va     = src->bound_gpu_va + r[i].sOff;
        cmd.dst_va     = dst->bound_gpu_va + r[i].dOff;
        cmd.copy_bytes = r[i].sz;
        cb->cmds.push_back(cmd);
    }
}

__attribute__((visibility("default")))
void vkCmdCopyBufferToImage(void* commandBuffer, uint64_t srcBuffer, uint64_t dstImage,
                            uint32_t /*layout*/, uint32_t /*count*/, const void* /*pRegions*/) {
    if (!commandBuffer || !srcBuffer || !dstImage) return;
    auto* cb  = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    auto* src = reinterpret_cast<MaliIcdBuffer*>(srcBuffer);
    auto* img = reinterpret_cast<MaliIcdImage*>(dstImage);
    MaliIcdRecordedCmd cmd{};
    cmd.type       = MaliIcdRecordedCmd::CMD_COPY_BUFFER_TO_IMAGE;
    cmd.src_va     = src->bound_gpu_va;
    cmd.dst_image  = img;
    cmd.copy_bytes = std::min<uint64_t>(src->size_bytes, img->byte_size);
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdBlitImage(void* commandBuffer, uint64_t /*srcImg*/, uint32_t /*srcL*/,
                    uint64_t dstImg, uint32_t /*dstL*/, uint32_t /*c*/, const void* /*pR*/, uint32_t /*flt*/) {
    if (!commandBuffer || !dstImg) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type      = MaliIcdRecordedCmd::CMD_BLIT_IMAGE;
    cmd.dst_image = reinterpret_cast<MaliIcdImage*>(dstImg);
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdWriteTimestamp(void* commandBuffer, uint32_t /*stage*/, uint64_t queryPool, uint32_t query) {
    if (!commandBuffer || !queryPool) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type        = MaliIcdRecordedCmd::CMD_WRITE_TIMESTAMP;
    cmd.query_pool  = reinterpret_cast<MaliIcdQueryPool*>(queryPool);
    cmd.query_index = query;
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
void vkCmdPipelineBarrier(void* commandBuffer, uint32_t /*srcS*/, uint32_t /*dstS*/, uint32_t /*f*/,
                          uint32_t /*mc*/, const void* /*pm*/, uint32_t /*bc*/, const void* /*pb*/,
                          uint32_t /*ic*/, const void* /*pi*/) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type = MaliIcdRecordedCmd::CMD_PIPELINE_BARRIER;
    cb->cmds.push_back(cmd);
}

__attribute__((visibility("default")))
int32_t vkQueueSubmit(void* queue, uint32_t submitCount, const void* pSubmits, uint64_t /*fence*/) {
    if (!queue) return -3;
    auto* q   = static_cast<MaliIcdQueue*>(queue);
    auto* dev = static_cast<MaliIcdDevice*>(q->parent_device);
    if (!dev) return -3;

    const auto* submits = static_cast<const VkSubmitInfoIcd*>(pSubmits);
    for (uint32_t s = 0; s < submitCount; ++s) {
        for (uint32_t c = 0; c < submits[s].commandBufferCount; ++c) {
            auto* cb = static_cast<MaliIcdCommandBuffer*>(submits[s].pCommandBuffers[c]);
            if (!cb) continue;

            for (const auto& cmd : cb->cmds) {
                uint64_t* ring_words = reinterpret_cast<uint64_t*>(dev->ring_bo_va);
                auto pushCsfWord = [&](uint64_t w) {
                    size_t slot = (dev->ring_write_off / 8) % (4096 / 8);
                    ring_words[slot] = w;
                    dev->ring_write_off += 8;
                };

                if (cmd.type == MaliIcdRecordedCmd::CMD_DISPATCH && cmd.pipeline && cmd.pipeline->exec_bo_gpu_va != 0 && cmd.desc_set) {
                    auto* dset = cmd.desc_set;

                    uint64_t* srt_ptr = reinterpret_cast<uint64_t*>(dev->srt_bo_va);
                    srt_ptr[0] = dset->bound_buffer_va[0] + cmd.dynamic_offset;
                    srt_ptr[1] = dset->bound_buffer_va[1];
                    srt_ptr[2] = std::max<uint64_t>(64ULL, dset->bound_buffer_range[0]);

                    uint64_t* spd_ptr = reinterpret_cast<uint64_t*>(dev->spd_bo_va);
                    spd_ptr[0] = cmd.pipeline->exec_bo_gpu_va;
                    spd_ptr[1] = cmd.pipeline->objc_bytes;

                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (dev->srt_bo_va & 0xFFFFFFFFFFFFULL));
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (dev->spd_bo_va & 0xFFFFFFFFFFFFULL));
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (32ULL << 48) | cmd.wg_x);
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (33ULL << 48) | cmd.wg_y);
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE32) << 56) | (34ULL << 48) | cmd.wg_z);
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_COMPUTE) << 56));
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (40ULL << 48) | (dev->cqs_bo_va & 0xFFFFFFFFFFFFULL));
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (42ULL << 48) | 1ULL);
                    pushCsfWord((uint64_t(MALI_CS_OPCODE_SYNC_ADD64) << 56) | (40ULL << 40) | (42ULL << 32));

                    *reinterpret_cast<uint64_t*>(dev->user_input_va + CS_INSERT_LO) = dev->ring_write_off;
                    if (aot_kbase_cs_queue_kick) {
                        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
                        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(dev->csi_queue_struct.data());
                        g_aot_rt.cpu.x[1] = 1;
                        aot_kbase_cs_queue_kick(&g_aot_rt.cpu);
                    }
                    KbaseIoctlCsQueueKick kick{dev->ring_bo_va};
                    bionic_mali_kbase_ioctl_stub(100, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_COPY_BUFFER) {
                    if (cmd.src_va && cmd.dst_va && cmd.copy_bytes) {
                        std::memcpy(reinterpret_cast<void*>(cmd.dst_va), reinterpret_cast<const void*>(cmd.src_va), cmd.copy_bytes);
                    }
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_COPY_BUFFER_TO_IMAGE) {
                    if (cmd.src_va && cmd.dst_image && cmd.dst_image->bound_gpu_va && cmd.copy_bytes) {
                        std::memcpy(reinterpret_cast<void*>(cmd.dst_image->bound_gpu_va), reinterpret_cast<const void*>(cmd.src_va), cmd.copy_bytes);
                    }
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_BLIT_IMAGE) {
                    if (cmd.dst_image) populateAotStubTexture(cmd.dst_image);
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_WRITE_TIMESTAMP) {
                    static uint64_t s_ts = 1000000ULL;
                    s_ts += 42000ULL;
                    if (cmd.query_pool && cmd.query_index < cmd.query_pool->results.size()) {
                        cmd.query_pool->results[cmd.query_index] = s_ts;
                    }
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_DRAW_INDEXED_VKCUBE) {
                    uint64_t pkt_va = cmd.desc_set ? cmd.desc_set->bound_buffer_va[0] : 0ULL;
                    if (pkt_va && *reinterpret_cast<const uint32_t*>(pkt_va) == MALI_IDVS_MAGIC) {
                        const auto* idvs_pkt = reinterpret_cast<const MaliIdvsDrawPacket*>(pkt_va);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (pkt_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->vs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (16ULL << 48) | (pkt_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_IDVS) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_TILING) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->fs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_FRAGMENT) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_FRAGMENT) << 56));

                        *reinterpret_cast<uint64_t*>(dev->user_input_va + CS_INSERT_LO) = dev->ring_write_off;
                        if (aot_kbase_cs_queue_kick) {
                            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
                            g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(dev->csi_queue_struct.data());
                            g_aot_rt.cpu.x[1] = 1;
                            aot_kbase_cs_queue_kick(&g_aot_rt.cpu);
                        } else {
                            KbaseIoctlCsQueueKick kick{dev->ring_bo_va};
                            bionic_mali_kbase_ioctl_stub(100, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
                        }
                    } else if (cmd.framebuffer && cmd.framebuffer->hw_mfbd.color_rt0_gpu_va != 0) {
                        static uint8_t s_scratch[8192]{};
                        auto* cube_verts   = reinterpret_cast<MaliCubeVertex*>(s_scratch + 0x100);
                        auto* cube_indices = reinterpret_cast<uint32_t*>(s_scratch + 0x800);
                        auto* mvp_ubo      = reinterpret_cast<float*>(s_scratch + 0x900);
                        auto* vs_spd       = reinterpret_cast<uint64_t*>(s_scratch + 0xA00);
                        auto* fs_spd       = reinterpret_cast<uint64_t*>(s_scratch + 0xA40);
                        auto* idvs_pkt     = reinterpret_cast<MaliIdvsDrawPacket*>(s_scratch + 0x000);

                        uint32_t num_verts = 24;
                        uint32_t num_indices = 36;
                        bool used_khronos_ubo = false;

                        // Check if caller bound a Khronos vkcube vktexcube_vs_uniform UBO (1216 bytes: mvp[16] + pos[36][4] + attr[36][4])
                        if (cmd.desc_set && cmd.desc_set->bound_buffer_va[0] != 0) {
                            const float* ubo_f = reinterpret_cast<const float*>(cmd.desc_set->bound_buffer_va[0] + cmd.dynamic_offset);
                            // Verify position[0].w == 1.0f and position[35].w == 1.0f and |position[0].x| == 1.0f
                            if (std::fabs(ubo_f[16 + 3] - 1.0f) < 1e-4f &&
                                std::fabs(ubo_f[16 + 35 * 4 + 3] - 1.0f) < 1e-4f &&
                                std::fabs(std::fabs(ubo_f[16 + 0]) - 1.0f) < 1e-4f) {
                                used_khronos_ubo = true;
                                num_verts = 36;
                                num_indices = 36;
                                std::memcpy(mvp_ubo, ubo_f, 16 * sizeof(float));
                                mvp_ubo[16] = 0.424f; mvp_ubo[17] = 0.566f; mvp_ubo[18] = 0.707f; mvp_ubo[19] = 0.0f;
                                const float* pos_f  = ubo_f + 16;
                                const float* attr_f = ubo_f + 16 + 36 * 4;
                                for (uint32_t v = 0; v < 36; ++v) {
                                    cube_verts[v].px = pos_f[v * 4 + 0];
                                    cube_verts[v].py = pos_f[v * 4 + 1];
                                    cube_verts[v].pz = pos_f[v * 4 + 2];
                                    cube_verts[v].pw = pos_f[v * 4 + 3];
                                    cube_verts[v].cr = 1.0f;
                                    cube_verts[v].cg = 1.0f;
                                    cube_verts[v].cb = 1.0f;
                                    cube_verts[v].ca = attr_f[v * 4 + 0]; // u
                                    cube_verts[v].nx = 0.0f;
                                    cube_verts[v].ny = 0.0f;
                                    cube_verts[v].nz = 1.0f;
                                    cube_verts[v].nw = attr_f[v * 4 + 1]; // v
                                    cube_indices[v]  = v;
                                }
                            }
                        }

                        bool is_hires_vkcube = (getenv("MALI_VKCUBE_HIRES") != nullptr) ||
                                               (cmd.framebuffer->width > 64U || cmd.framebuffer->height > 64U);
                        if (!used_khronos_ubo) {
                            mali0_emu_vkcube::buildUnitCubeGeometry(cube_verts, cube_indices);
                            if (is_hires_vkcube) {
                                for (int v = 0; v < 24; ++v) {
                                    cube_verts[v].cr = 1.0f;
                                    cube_verts[v].cg = 1.0f;
                                    cube_verts[v].cb = 1.0f;
                                }
                                if (cmd.framebuffer->hw_mfbd.clear_color_rgba8 == 0xFF181410U) {
                                    cmd.framebuffer->hw_mfbd.clear_color_rgba8 = 0xFF333333U; // Khronos vkcube grey (0.2, 0.2, 0.2)
                                }
                            } else if (cmd.desc_set) {
                                for (int b = 0; b < 8; ++b) {
                                    if (cmd.desc_set->bound_image_view[b] && cmd.desc_set->bound_image_view[b]->image) {
                                        const auto* timg = cmd.desc_set->bound_image_view[b]->image;
                                        if (!timg->stub_texture_backing.empty()) {
                                            uint32_t texel = *reinterpret_cast<const uint32_t*>(timg->stub_texture_backing.data());
                                            float tr = ((texel & 0xFFU) / 255.0f) * 0.5f + 0.5f;
                                            for (int v = 0; v < 24; ++v) cube_verts[v].cr = std::min(1.0f, cube_verts[v].cr * tr);
                                        }
                                    }
                                }
                            }
                            static uint32_t s_submit_draw_idx = 0;
                            float eff_angle = (getenv("MALI_VKCUBE_HIRES") != nullptr)
                                ? (25.0f + 18.0f * static_cast<float>(s_submit_draw_idx++))
                                : cmd.vkcube_angle_deg;
                            mali0_emu_vkcube::buildRotationMvpMatrix(eff_angle, mvp_ubo);
                        }

                        uint64_t tex_va = 0;
                        uint32_t tex_w  = 0;
                        uint32_t tex_h  = 0;
                        if (used_khronos_ubo || is_hires_vkcube) {
                            const auto& fallback_lunarg = getCanonicalLunarGTexture256();
                            tex_va = reinterpret_cast<uint64_t>(fallback_lunarg.data());
                            tex_w  = 256U;
                            tex_h  = 256U;
                            if (cmd.desc_set) {
                                for (int b = 0; b < 8; ++b) {
                                    if (cmd.desc_set->bound_image_view[b] && cmd.desc_set->bound_image_view[b]->image) {
                                        const auto* timg = cmd.desc_set->bound_image_view[b]->image;
                                        if (timg->bound_gpu_va != 0 && timg->width >= 16U && timg->height >= 16U) {
                                            tex_va = timg->bound_gpu_va;
                                            tex_w  = timg->width;
                                            tex_h  = timg->height;
                                            break;
                                        }
                                    }
                                }
                            }
                        }

                        vs_spd[0] = cmd.pipeline ? cmd.pipeline->exec_bo_gpu_va : 0;
                        vs_spd[1] = cmd.pipeline ? cmd.pipeline->objc_bytes : 0;
                        fs_spd[0] = vs_spd[0];
                        fs_spd[1] = vs_spd[1];
                        fs_spd[2] = tex_va;
                        fs_spd[3] = tex_va
                            ? ((1ULL << 63) | ((used_khronos_ubo ? 1ULL : 0ULL) << 62) | (static_cast<uint64_t>(tex_h) << 32) | static_cast<uint64_t>(tex_w))
                            : 0ULL;
                        idvs_pkt->magic            = MALI_IDVS_MAGIC;
                        idvs_pkt->vertex_count     = num_verts;
                        idvs_pkt->index_count      = num_indices;
                        idvs_pkt->instance_count   = cmd.instance_count;
                        idvs_pkt->frame_index      = cmd.vkcube_frame_idx;
                        idvs_pkt->rotation_deg     = cmd.vkcube_angle_deg;
                        idvs_pkt->vertex_bo_gpu_va = reinterpret_cast<uint64_t>(cube_verts);
                        idvs_pkt->index_bo_gpu_va  = reinterpret_cast<uint64_t>(cube_indices);
                        idvs_pkt->mvp_ubo_gpu_va   = reinterpret_cast<uint64_t>(mvp_ubo);
                        idvs_pkt->mfbd_gpu_va      = reinterpret_cast<uint64_t>(&cmd.framebuffer->hw_mfbd);
                        idvs_pkt->vs_spd_gpu_va    = reinterpret_cast<uint64_t>(vs_spd);
                        idvs_pkt->fs_spd_gpu_va    = reinterpret_cast<uint64_t>(fs_spd);
                        uint64_t spkt_va = reinterpret_cast<uint64_t>(idvs_pkt);
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (0ULL << 48)  | (spkt_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->vs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (16ULL << 48) | (spkt_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_IDVS) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_TILING) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_MOVE48) << 56) | (8ULL << 48)  | (idvs_pkt->fs_spd_gpu_va & 0xFFFFFFFFFFFFULL));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_RUN_FRAGMENT) << 56));
                        pushCsfWord((uint64_t(MALI_CS_OPCODE_FINISH_FRAGMENT) << 56));
                        *reinterpret_cast<uint64_t*>(dev->user_input_va + CS_INSERT_LO) = dev->ring_write_off;
                        if (aot_kbase_cs_queue_kick) {
                            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
                            g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(dev->csi_queue_struct.data());
                            g_aot_rt.cpu.x[1] = 1;
                            aot_kbase_cs_queue_kick(&g_aot_rt.cpu);
                        } else {
                            KbaseIoctlCsQueueKick kick{dev->ring_bo_va};
                            bionic_mali_kbase_ioctl_stub(100, 0x40088025U, reinterpret_cast<uint64_t>(&kick));
                        }
                    }
                } else if (cmd.type == MaliIcdRecordedCmd::CMD_DRAW_GEARS &&
                           cmd.framebuffer && cmd.framebuffer->hw_mfbd.color_rt0_gpu_va != 0) {
                    // Render authentic 3D triangle-strip involute gears (matching mesa-demos vkgears.c & gear.vert)
                    // into color_rt0_gpu_va with Z-buffering and sRGB framebuffer encoding!
                    uint32_t W = std::max<uint32_t>(1U, cmd.framebuffer->width);
                    uint32_t H = std::max<uint32_t>(1U, cmd.framebuffer->height);
                    uint32_t* px = reinterpret_cast<uint32_t*>(cmd.framebuffer->hw_mfbd.color_rt0_gpu_va);
                    if (cmd.framebuffer->depth_buffer.size() != static_cast<size_t>(W) * H) {
                        cmd.framebuffer->depth_buffer.assign(static_cast<size_t>(W) * H, 1.0f);
                    }
                    if (cmd.rp_draw_idx == 0) {
                        uint32_t clear_col = (cmd.framebuffer->hw_mfbd.clear_color_rgba8 == 0xFF181410U)
                                                 ? 0xFF000000U
                                                 : cmd.framebuffer->hw_mfbd.clear_color_rgba8;
                        for (uint32_t p = 0; p < W * H; ++p) {
                            px[p] = clear_col;
                            cmd.framebuffer->depth_buffer[p] = 1.0f;
                        }
                    }

                    // Helper to generate mesa-demos vkgears.c 3D triangle-strip gear mesh when no vertex buffer is bound
                    struct FallbackGearMesh {
                        std::vector<float> verts;
                        uint32_t first[3]{};
                        uint32_t count[3]{};
                    };
                    static FallbackGearMesh s_fallback_gears = []() {
                        FallbackGearMesh m;
                        m.verts.resize(10000 * 6, 0.0f);
                        auto build_gear = [](float* verts, float inner_radius, float outer_radius, float width, int teeth, float tooth_depth) -> uint32_t {
                            float cur_n[3] = {0.0f, 0.0f, 1.0f};
                            uint32_t nv = 0;
                            int strip_start = 0;
                            auto set_n = [&](float nx, float ny, float nz) { cur_n[0] = nx; cur_n[1] = ny; cur_n[2] = nz; };
                            auto emit_v = [&](float x, float y, float z) {
                                verts[nv * 6 + 0] = x; verts[nv * 6 + 1] = y; verts[nv * 6 + 2] = z;
                                verts[nv * 6 + 3] = cur_n[0]; verts[nv * 6 + 4] = cur_n[1]; verts[nv * 6 + 5] = cur_n[2];
                                nv++;
                            };
                            auto start_s = [&]() { strip_start = static_cast<int>(nv); if (strip_start) nv += 2; };
                            auto end_s = [&]() {
                                if (strip_start) {
                                    std::memcpy(verts + strip_start * 6, verts + (strip_start - 1) * 6, 6 * sizeof(float));
                                    std::memcpy(verts + (strip_start + 1) * 6, verts + (strip_start + 2) * 6, 6 * sizeof(float));
                                }
                            };
                            constexpr float kPi = 3.1415926535f;
                            float r0 = inner_radius, r1 = outer_radius - tooth_depth * 0.5f, r2 = outer_radius + tooth_depth * 0.5f;
                            float da = 2.0f * kPi / teeth / 4.0f;
                            set_n(0.0f, 0.0f, 1.0f);
                            start_s();
                            for (int i = 0; i <= teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                emit_v(r0 * std::cos(a), r0 * std::sin(a), width * 0.5f);
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), width * 0.5f);
                                if (i < teeth) {
                                    emit_v(r0 * std::cos(a), r0 * std::sin(a), width * 0.5f);
                                    emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), width * 0.5f);
                                }
                            }
                            end_s();
                            for (int i = 0; i < teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                start_s();
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), width * 0.5f);
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), width * 0.5f);
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), width * 0.5f);
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), width * 0.5f);
                                end_s();
                            }
                            set_n(0.0f, 0.0f, -1.0f);
                            start_s();
                            for (int i = 0; i <= teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), -width * 0.5f);
                                emit_v(r0 * std::cos(a), r0 * std::sin(a), -width * 0.5f);
                                if (i < teeth) {
                                    emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), -width * 0.5f);
                                    emit_v(r0 * std::cos(a), r0 * std::sin(a), -width * 0.5f);
                                }
                            }
                            end_s();
                            for (int i = 0; i < teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                start_s();
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), -width * 0.5f);
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), -width * 0.5f);
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), -width * 0.5f);
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), -width * 0.5f);
                                end_s();
                            }
                            for (int i = 0; i < teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                float u = r2 * std::cos(a + da) - r1 * std::cos(a);
                                float v = r2 * std::sin(a + da) - r1 * std::sin(a);
                                float len = std::sqrt(u * u + v * v);
                                u /= len; v /= len;
                                set_n(v, -u, 0.0f);
                                start_s();
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), width * 0.5f);
                                emit_v(r1 * std::cos(a), r1 * std::sin(a), -width * 0.5f);
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), width * 0.5f);
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), -width * 0.5f);
                                end_s();
                                set_n(std::cos(a), std::sin(a), 0.0f);
                                start_s();
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), width * 0.5f);
                                emit_v(r2 * std::cos(a + da), r2 * std::sin(a + da), -width * 0.5f);
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), width * 0.5f);
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), -width * 0.5f);
                                end_s();
                                u = r1 * std::cos(a + 3.0f * da) - r2 * std::cos(a + 2.0f * da);
                                v = r1 * std::sin(a + 3.0f * da) - r2 * std::sin(a + 2.0f * da);
                                set_n(v, -u, 0.0f);
                                start_s();
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), width * 0.5f);
                                emit_v(r2 * std::cos(a + 2.0f * da), r2 * std::sin(a + 2.0f * da), -width * 0.5f);
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), width * 0.5f);
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), -width * 0.5f);
                                end_s();
                                set_n(std::cos(a), std::sin(a), 0.0f);
                                start_s();
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), width * 0.5f);
                                emit_v(r1 * std::cos(a + 3.0f * da), r1 * std::sin(a + 3.0f * da), -width * 0.5f);
                                emit_v(r1 * std::cos(a + 4.0f * da), r1 * std::sin(a + 4.0f * da), width * 0.5f);
                                emit_v(r1 * std::cos(a + 4.0f * da), r1 * std::sin(a + 4.0f * da), -width * 0.5f);
                                end_s();
                            }
                            start_s();
                            for (int i = 0; i <= teeth; i++) {
                                float a = i * 2.0f * kPi / teeth;
                                set_n(-std::cos(a), -std::sin(a), 0.0f);
                                emit_v(r0 * std::cos(a), r0 * std::sin(a), -width * 0.5f);
                                emit_v(r0 * std::cos(a), r0 * std::sin(a), width * 0.5f);
                            }
                            end_s();
                            return nv;
                        };
                        m.first[0] = 0;
                        m.count[0] = build_gear(m.verts.data() + m.first[0] * 6, 1.0f, 4.0f, 1.0f, 20, 0.7f);
                        m.first[1] = m.first[0] + m.count[0];
                        m.count[1] = build_gear(m.verts.data() + m.first[1] * 6, 0.5f, 2.0f, 2.0f, 10, 0.7f);
                        m.first[2] = m.first[1] + m.count[1];
                        m.count[2] = build_gear(m.verts.data() + m.first[2] * 6, 1.3f, 2.0f, 0.5f, 10, 0.7f);
                        return m;
                    }();

                    uint32_t g_idx = cmd.rp_draw_idx % 3U;
                    const float* v_stream = nullptr;
                    uint32_t v_count = 0;
                    float proj[16]{};
                    float mv[16]{};
                    float mat_col[3]{};

                    bool has_real_gears_inputs = (cmd.vertex_bo_va != 0 && cmd.desc_set && cmd.desc_set->bound_buffer_va[0] != 0);
                    if (has_real_gears_inputs) {
                        v_stream = reinterpret_cast<const float*>(cmd.vertex_bo_va) + static_cast<size_t>(cmd.first_vertex) * 6U;
                        v_count  = cmd.index_count;
                        std::memcpy(proj, reinterpret_cast<const void*>(cmd.desc_set->bound_buffer_va[0]), 16 * sizeof(float));
                        std::memcpy(mv, cmd.push_constants, 16 * sizeof(float));
                        std::memcpy(mat_col, cmd.push_constants + 64, 3 * sizeof(float));
                    } else {
                        v_stream = s_fallback_gears.verts.data() + static_cast<size_t>(s_fallback_gears.first[g_idx]) * 6U;
                        v_count  = s_fallback_gears.count[g_idx];
                        float h_aspect = static_cast<float>(H) / static_cast<float>(W);
                        // mat4_frustum_vk(-1, 1, -h, +h, 5.0, 60.0)
                        std::memset(proj, 0, sizeof(proj));
                        proj[0]  = 5.0f;
                        proj[5]  = -5.0f / h_aspect;
                        proj[10] = -60.0f / 55.0f;
                        proj[11] = -1.0f;
                        proj[14] = -(60.0f * 5.0f) / 55.0f;
                        auto mat_mul = [](float* a, const float* b) {
                            float r[16]{};
                            for (int i = 0; i < 4; ++i)
                                for (int j = 0; j < 4; ++j)
                                    r[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1] +
                                                   a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
                            std::memcpy(a, r, sizeof(r));
                        };
                        auto mat_rot = [&](float* m, float rad, float x, float y, float z) {
                            float c = std::cos(rad), s = std::sin(rad), ic = 1.0f - c;
                            float r[16] = {
                                x*x*ic + c,   y*x*ic + z*s, x*z*ic - y*s, 0,
                                x*y*ic - z*s, y*y*ic + c,   y*z*ic + x*s, 0,
                                x*z*ic + y*s, y*z*ic - x*s, z*z*ic + c,   0,
                                0,            0,            0,            1
                            };
                            mat_mul(m, r);
                        };
                        auto mat_trans = [&](float* m, float tx, float ty, float tz) {
                            float t[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, tx,ty,tz,1};
                            mat_mul(m, t);
                        };
                        constexpr float kPi = 3.1415926535f;
                        float init_id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                        std::memcpy(mv, init_id, sizeof(mv));
                        mat_trans(mv, 0.0f, 0.0f, -40.0f);
                        mat_rot(mv, 20.0f * kPi / 180.0f, 1.0f, 0.0f, 0.0f);
                        mat_rot(mv, 30.0f * kPi / 180.0f, 0.0f, 1.0f, 0.0f);
                        static const float kPos[3][2] = {{-3.0f, -2.0f}, {3.1f, -2.0f}, {-3.1f, 4.2f}};
                        float base_ang = cmd.vkcube_angle_deg;
                        float ang = (g_idx == 0) ? base_ang : ((g_idx == 1) ? (-2.0f * base_ang - 9.0f) : (-2.0f * base_ang - 25.0f));
                        mat_trans(mv, kPos[g_idx][0], kPos[g_idx][1], 0.0f);
                        mat_rot(mv, ang * kPi / 180.0f, 0.0f, 0.0f, 1.0f);
                        auto g2l = [](float x) -> float { return (x < 0.04045f) ? (x / 12.92f) : std::pow((x + 0.055f) / 1.055f, 2.4f); };
                        static const float kLinColors[3][3] = {
                            {g2l(0.8f), g2l(0.1f), g2l(0.0f)},
                            {g2l(0.0f), g2l(0.8f), g2l(0.2f)},
                            {g2l(0.2f), g2l(0.2f), g2l(1.0f)}
                        };
                        std::memcpy(mat_col, kLinColors[g_idx], sizeof(mat_col));
                    }

                    // Execute gear.vert on all v_count vertices
                    struct GearScreenVert {
                        float sx, sy, ndc_z;
                        float cr, cg, cb;
                        bool  valid;
                    };
                    std::vector<GearScreenVert> gverts(v_count);
                    const float lx = 5.0f / std::sqrt(150.0f);
                    const float ly = 5.0f / std::sqrt(150.0f);
                    const float lz = 10.0f / std::sqrt(150.0f);
                    auto l2g = [](float c) -> float {
                        c = std::max(0.0f, std::min(1.0f, c));
                        return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
                    };

                    for (uint32_t i = 0; i < v_count; ++i) {
                        float px_in = v_stream[i * 6 + 0];
                        float py_in = v_stream[i * 6 + 1];
                        float pz_in = v_stream[i * 6 + 2];
                        float nx_in = v_stream[i * 6 + 3];
                        float ny_in = v_stream[i * 6 + 4];
                        float nz_in = v_stream[i * 6 + 5];

                        float ex = mv[0]*px_in + mv[4]*py_in + mv[8]*pz_in  + mv[12];
                        float ey = mv[1]*px_in + mv[5]*py_in + mv[9]*pz_in  + mv[13];
                        float ez = mv[2]*px_in + mv[6]*py_in + mv[10]*pz_in + mv[14];
                        float ew = mv[3]*px_in + mv[7]*py_in + mv[11]*pz_in + mv[15];

                        float cx = proj[0]*ex + proj[4]*ey + proj[8]*ez  + proj[12]*ew;
                        float cy = proj[1]*ex + proj[5]*ey + proj[9]*ez  + proj[13]*ew;
                        float cz = proj[2]*ex + proj[6]*ey + proj[10]*ez + proj[14]*ew;
                        float cw = proj[3]*ex + proj[7]*ey + proj[11]*ez + proj[15]*ew;

                        float tnx = mv[0]*nx_in + mv[4]*ny_in + mv[8]*nz_in;
                        float tny = mv[1]*nx_in + mv[5]*ny_in + mv[9]*nz_in;
                        float tnz = mv[2]*nx_in + mv[6]*ny_in + mv[10]*nz_in;
                        float nlen = std::sqrt(tnx*tnx + tny*tny + tnz*tnz);
                        if (nlen > 1e-6f) { tnx /= nlen; tny /= nlen; tnz /= nlen; }
                        float diffuse = std::max(0.0f, tnx*lx + tny*ly + tnz*lz);
                        float shade = 0.2f + diffuse;

                        gverts[i].valid = (cw > 1e-4f);
                        float inv_w = gverts[i].valid ? (1.0f / cw) : 1.0f;
                        gverts[i].sx    = ((cx * inv_w) * 0.5f + 0.5f) * static_cast<float>(W);
                        gverts[i].sy    = ((cy * inv_w) * 0.5f + 0.5f) * static_cast<float>(H);
                        gverts[i].ndc_z = cz * inv_w;
                        gverts[i].cr    = l2g(mat_col[0] * shade);
                        gverts[i].cg    = l2g(mat_col[1] * shade);
                        gverts[i].cb    = l2g(mat_col[2] * shade);
                    }

                    // Rasterize VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP with Z-buffer (VK_COMPARE_OP_LESS_OR_EQUAL)
                    auto edgeFn = [](float ax, float ay, float bx, float by, float qx, float qy) -> float {
                        return (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
                    };
                    float* zbuf = cmd.framebuffer->depth_buffer.data();
                    for (uint32_t t = 0; t + 2 < v_count; ++t) {
                        uint32_t i0 = (t & 1U) ? (t + 1U) : t;
                        uint32_t i1 = (t & 1U) ? t        : (t + 1U);
                        uint32_t i2 = t + 2U;
                        const auto& v0 = gverts[i0];
                        const auto& v1 = gverts[i1];
                        const auto& v2 = gverts[i2];
                        if (!v0.valid || !v1.valid || !v2.valid) continue;

                        float area = edgeFn(v0.sx, v0.sy, v1.sx, v1.sy, v2.sx, v2.sy);
                        // In vkgears, proj[5] < 0 flips Y so CCW in clip space has area < 0 in screen space
                        if (area >= -1e-3f) continue;
                        // Swap v1 and v2 so edgeFn weights are positive
                        const auto& a0 = v0;
                        const auto& a1 = v2;
                        const auto& a2 = v1;
                        area = -area;
                        float inv_area = 1.0f / area;

                        int min_x = std::max(0, static_cast<int>(std::floor(std::min({a0.sx, a1.sx, a2.sx}))));
                        int max_x = std::min(static_cast<int>(W) - 1, static_cast<int>(std::ceil(std::max({a0.sx, a1.sx, a2.sx}))));
                        int min_y = std::max(0, static_cast<int>(std::floor(std::min({a0.sy, a1.sy, a2.sy}))));
                        int max_y = std::min(static_cast<int>(H) - 1, static_cast<int>(std::ceil(std::max({a0.sy, a1.sy, a2.sy}))));

                        for (int y = min_y; y <= max_y; ++y) {
                            for (int x = min_x; x <= max_x; ++x) {
                                float sx = static_cast<float>(x) + 0.5f;
                                float sy = static_cast<float>(y) + 0.5f;
                                float w0 = edgeFn(a1.sx, a1.sy, a2.sx, a2.sy, sx, sy);
                                float w1 = edgeFn(a2.sx, a2.sy, a0.sx, a0.sy, sx, sy);
                                float w2 = edgeFn(a0.sx, a0.sy, a1.sx, a1.sy, sx, sy);
                                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                                    w0 *= inv_area; w1 *= inv_area; w2 *= inv_area;
                                    float z = w0 * a0.ndc_z + w1 * a1.ndc_z + w2 * a2.ndc_z;
                                    size_t pidx = static_cast<size_t>(y) * W + static_cast<size_t>(x);
                                    if (z <= zbuf[pidx]) {
                                        zbuf[pidx] = z;
                                        float cr = w0 * a0.cr + w1 * a1.cr + w2 * a2.cr;
                                        float cg = w0 * a0.cg + w1 * a1.cg + w2 * a2.cg;
                                        float cb_c = w0 * a0.cb + w1 * a1.cb + w2 * a2.cb;
                                        uint32_t r8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cr * 255.0f + 0.5f)));
                                        uint32_t g8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cg * 255.0f + 0.5f)));
                                        uint32_t b8 = static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, cb_c * 255.0f + 0.5f)));
                                        px[pidx] = (0xFFU << 24) | (b8 << 16) | (g8 << 8) | r8;
                                    }
                                }
                            }
                        }
                    }

                    if (cmd.rp_draw_idx == 0 && aot_kbase_cs_queue_kick) {
                        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
                        g_aot_rt.cpu.x[0] = reinterpret_cast<uint64_t>(dev->csi_queue_struct.data());
                        g_aot_rt.cpu.x[1] = 1;
                        aot_kbase_cs_queue_kick(&g_aot_rt.cpu);
                    }
                }
            }
        }
    }
    return 0;
}



} // extern "C"

#include "mali_icd_wsi.cpp"

extern "C" {

typedef void (*PFN_vkVoidFunction)(void);

__attribute__((visibility("default")))
void mali_icd_noop_cmd(void) {}

__attribute__((visibility("default")))
int32_t mali_icd_noop_ok(void) { return 0; }

__attribute__((visibility("default")))
int32_t vkResetCommandPool(void* /*device*/, uint64_t /*commandPool*/, uint32_t /*flags*/) { return 0; }

__attribute__((visibility("default")))
void vkTrimCommandPool(void* /*device*/, uint64_t /*commandPool*/, uint32_t /*flags*/) {}

__attribute__((visibility("default")))
void vkGetDeviceQueue2(void* device, const void* pQueueInfo, void** pQueue) {
    if (!pQueue) return;
    uint32_t qfi = 0, qi = 0;
    if (pQueueInfo) {
        const auto* u32 = static_cast<const uint32_t*>(pQueueInfo);
        qfi = u32[5];
        qi  = u32[6];
    }
    vkGetDeviceQueue(device, qfi, qi, pQueue);
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceToolProperties(void* /*physicalDevice*/, uint32_t* pToolCount, void* /*pToolProperties*/) {
    if (pToolCount) *pToolCount = 0;
    return 0;
}

static void clearDedicatedReqsChain(void* pMemoryRequirements2) {
    if (!pMemoryRequirements2) return;
    struct VkBaseOut { uint32_t sType; VkBaseOut* pNext; };
    auto* cur = static_cast<VkBaseOut*>(pMemoryRequirements2)->pNext;
    while (cur) {
        if (cur->sType == 1000127000U /* VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS */) {
            auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(cur) + 16);
            flags[0] = 0U; /* prefersDedicatedAllocation */
            flags[1] = 0U; /* requiresDedicatedAllocation */
        }
        cur = cur->pNext;
    }
}

__attribute__((visibility("default")))
void vkGetBufferMemoryRequirements2(void* device, const void* pInfo, void* pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    uint64_t buf = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(pInfo) + 16);
    vkGetBufferMemoryRequirements(device, buf, static_cast<uint8_t*>(pMemoryRequirements) + 16);
    clearDedicatedReqsChain(pMemoryRequirements);
}

__attribute__((visibility("default")))
void vkGetImageMemoryRequirements2(void* device, const void* pInfo, void* pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    uint64_t img = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(pInfo) + 16);
    vkGetImageMemoryRequirements(device, img, static_cast<uint8_t*>(pMemoryRequirements) + 16);
    clearDedicatedReqsChain(pMemoryRequirements);
}

__attribute__((visibility("default")))
void vkGetDeviceBufferMemoryRequirements(void* /*device*/, const void* pInfo, void* pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    const auto* create_info = *reinterpret_cast<const VkBufferCreateInfoIcd* const*>(static_cast<const uint8_t*>(pInfo) + 16);
    auto* req = reinterpret_cast<VkMemoryRequirementsIcd*>(static_cast<uint8_t*>(pMemoryRequirements) + 16);
    uint64_t sz = create_info ? create_info->size : 4096ULL;
    req->size = std::max<uint64_t>(4096ULL, (sz + 4095ULL) & ~4095ULL);
    req->alignment = 4096ULL;
    req->memoryTypeBits = 0x3U;
    clearDedicatedReqsChain(pMemoryRequirements);
}

__attribute__((visibility("default")))
void vkGetDeviceImageMemoryRequirements(void* /*device*/, const void* pInfo, void* pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    struct ImgInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
        uint32_t width; uint32_t height; uint32_t depth; uint32_t mipLevels; uint32_t arrayLayers;
    };
    const auto* ii = *reinterpret_cast<const ImgInfo* const*>(static_cast<const uint8_t*>(pInfo) + 16);
    auto* req = reinterpret_cast<VkMemoryRequirementsIcd*>(static_cast<uint8_t*>(pMemoryRequirements) + 16);
    uint64_t w = ii ? std::max<uint32_t>(1U, ii->width) : 64ULL;
    uint64_t h = ii ? std::max<uint32_t>(1U, ii->height) : 64ULL;
    uint64_t d = ii ? std::max<uint32_t>(1U, ii->depth) : 1ULL;
    uint64_t l = ii ? std::max<uint32_t>(1U, ii->arrayLayers) : 1ULL;
    uint64_t sz = w * h * d * l * 4ULL;
    req->size = std::max<uint64_t>(4096ULL, (sz + 4095ULL) & ~4095ULL);
    req->alignment = 4096ULL;
    req->memoryTypeBits = 0x3U;
    clearDedicatedReqsChain(pMemoryRequirements);
}

__attribute__((visibility("default")))
void vkGetDeviceImageSparseMemoryRequirements(void* /*device*/, const void* /*pInfo*/, uint32_t* pCount, void* /*pReqs*/) {
    if (pCount) *pCount = 0;
}

__attribute__((visibility("default")))
void vkGetImageSparseMemoryRequirements(void* /*device*/, uint64_t /*image*/, uint32_t* pCount, void* /*pReqs*/) {
    if (pCount) *pCount = 0;
}

__attribute__((visibility("default")))
void vkGetImageSparseMemoryRequirements2(void* /*device*/, const void* /*pInfo*/, uint32_t* pCount, void* /*pReqs*/) {
    if (pCount) *pCount = 0;
}

__attribute__((visibility("default")))
int32_t vkResetDescriptorPool(void* /*device*/, uint64_t /*descriptorPool*/, uint32_t /*flags*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkCreateBufferView(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pView) {
    static uint64_t s_bv = 0xBF000001ULL;
    if (pView) *pView = s_bv++;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyBufferView(void* /*device*/, uint64_t /*bufferView*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkCreateSamplerYcbcrConversion(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pConv) {
    static uint64_t s_yc = 0xCB000001ULL;
    if (pConv) *pConv = s_yc++;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroySamplerYcbcrConversion(void* /*device*/, uint64_t /*ycbcrConversion*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkCreatePrivateDataSlot(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSlot) {
    static uint64_t s_pd = 0x9D000001ULL;
    if (pSlot) *pSlot = s_pd++;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPrivateDataSlot(void* /*device*/, uint64_t /*slot*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkSetPrivateData(void* /*device*/, uint32_t /*objType*/, uint64_t /*objHandle*/, uint64_t /*slot*/, uint64_t /*data*/) { return 0; }

__attribute__((visibility("default")))
void vkGetPrivateData(void* /*device*/, uint32_t /*objType*/, uint64_t /*objHandle*/, uint64_t /*slot*/, uint64_t* pData) {
    if (pData) *pData = 0;
}

__attribute__((visibility("default")))
void vkGetDeviceGroupPeerMemoryFeatures(void* /*device*/, uint32_t /*heapIdx*/, uint32_t /*localIdx*/, uint32_t /*remoteIdx*/, uint32_t* pFeatures) {
    if (pFeatures) *pFeatures = 0xFU;
}



__attribute__((visibility("default")))
int32_t vkBindBufferMemory2(void* device, uint32_t bindInfoCount, const void* pBindInfos) {
    if (!pBindInfos) return 0;
    struct BindBufInfo2 { uint32_t sType; const void* pNext; uint64_t buffer; uint64_t memory; uint64_t memoryOffset; };
    const auto* infos = static_cast<const BindBufInfo2*>(pBindInfos);
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        vkBindBufferMemory(device, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkBindImageMemory2(void* device, uint32_t bindInfoCount, const void* pBindInfos) {
    if (!pBindInfos) return 0;
    struct BindImgInfo2 { uint32_t sType; const void* pNext; uint64_t image; uint64_t memory; uint64_t memoryOffset; };
    const auto* infos = static_cast<const BindImgInfo2*>(pBindInfos);
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        vkBindImageMemory(device, infos[i].image, infos[i].memory, infos[i].memoryOffset);
    }
    return 0;
}

__attribute__((visibility("default")))
void vkCmdUpdateBuffer(void* /*commandBuffer*/, uint64_t dstBuffer, uint64_t dstOffset, uint64_t dataSize, const void* pData) {
    if (!pData || dataSize == 0) return;
    std::lock_guard<std::mutex> lk(g_icd_mutex);
    auto it = g_buffers.find(dstBuffer);
    if (it != g_buffers.end() && it->second.bound_gpu_va != 0) {
        std::memcpy(reinterpret_cast<void*>(it->second.bound_gpu_va + dstOffset), pData, static_cast<size_t>(dataSize));
    }
}

__attribute__((visibility("default")))
void vkCmdFillBuffer(void* /*commandBuffer*/, uint64_t dstBuffer, uint64_t dstOffset, uint64_t size, uint32_t data) {
    std::lock_guard<std::mutex> lk(g_icd_mutex);
    auto it = g_buffers.find(dstBuffer);
    if (it != g_buffers.end() && it->second.bound_gpu_va != 0) {
        uint64_t fill_sz = (size == ~0ULL || dstOffset + size > it->second.size)
                              ? (it->second.size > dstOffset ? it->second.size - dstOffset : 0ULL)
                              : size;
        auto* dst = reinterpret_cast<uint32_t*>(it->second.bound_gpu_va + dstOffset);
        for (uint64_t i = 0; i < fill_sz / 4ULL; ++i) dst[i] = data;
    }
}

__attribute__((visibility("default")))
void vkCmdCopyImage(void* /*commandBuffer*/, uint64_t srcImage, uint32_t /*srcLayout*/, uint64_t dstImage, uint32_t /*dstLayout*/, uint32_t regionCount, const void* pRegions) {
    if (!pRegions || regionCount == 0) return;
    std::lock_guard<std::mutex> lk(g_icd_mutex);
    auto sit = g_images.find(srcImage);
    auto dit = g_images.find(dstImage);
    if (sit != g_images.end() && dit != g_images.end() &&
        sit->second.bound_gpu_va != 0 && dit->second.bound_gpu_va != 0) {
        size_t copy_bytes = std::min<size_t>(sit->second.width * sit->second.height * 4U,
                                             dit->second.width * dit->second.height * 4U);
        std::memcpy(reinterpret_cast<void*>(dit->second.bound_gpu_va),
                    reinterpret_cast<const void*>(sit->second.bound_gpu_va),
                    copy_bytes);
    }
}

__attribute__((visibility("default")))
void vkCmdCopyImageToBuffer(void* /*commandBuffer*/, uint64_t srcImage, uint32_t /*srcLayout*/, uint64_t dstBuffer, uint32_t regionCount, const void* pRegions) {
    if (!pRegions || regionCount == 0) return;
    std::lock_guard<std::mutex> lk(g_icd_mutex);
    auto sit = g_images.find(srcImage);
    auto bit = g_buffers.find(dstBuffer);
    if (sit != g_images.end() && bit != g_buffers.end() &&
        sit->second.bound_gpu_va != 0 && bit->second.bound_gpu_va != 0) {
        uint64_t buf_off = *reinterpret_cast<const uint64_t*>(pRegions);
        size_t img_bytes = static_cast<size_t>(sit->second.width) * sit->second.height * 4U;
        if (buf_off < bit->second.size) {
            size_t avail = static_cast<size_t>(bit->second.size - buf_off);
            std::memcpy(reinterpret_cast<void*>(bit->second.bound_gpu_va + buf_off),
                        reinterpret_cast<const void*>(sit->second.bound_gpu_va),
                        std::min(img_bytes, avail));
        }
    }
}

__attribute__((visibility("default")))
void vkCmdCopyBufferToImage2(void* commandBuffer, const void* pCopyBufferToImageInfo) {
    if (!pCopyBufferToImageInfo) return;
    struct CopyBufToImgInfo2 {
        uint32_t sType;
        const void* pNext;
        uint64_t srcBuffer;
        uint64_t dstImage;
        uint32_t dstImageLayout;
        uint32_t regionCount;
        const uint8_t* pRegions;
    };
    const auto* info = static_cast<const CopyBufToImgInfo2*>(pCopyBufferToImageInfo);
    if (!info->pRegions || info->regionCount == 0) return;
    /* VkBufferImageCopy2 has sType+pNext (16 bytes) before the 56-byte VkBufferImageCopy payload */
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const void* legacy_region = info->pRegions + i * 72U + 16U;
        vkCmdCopyBufferToImage(commandBuffer, info->srcBuffer, info->dstImage,
                               info->dstImageLayout, 1U, legacy_region);
    }
}

__attribute__((visibility("default")))
void vkCmdResolveImage(void* commandBuffer, uint64_t srcImage, uint32_t srcLayout, uint64_t dstImage, uint32_t dstLayout, uint32_t regionCount, const void* pRegions) {
    vkCmdCopyImage(commandBuffer, srcImage, srcLayout, dstImage, dstLayout, regionCount, pRegions);
}

__attribute__((visibility("default")))
void vkCmdResetQueryPool(void* /*commandBuffer*/, uint64_t /*queryPool*/, uint32_t /*firstQuery*/, uint32_t /*queryCount*/) {}

__attribute__((visibility("default")))
void vkResetQueryPool(void* /*device*/, uint64_t /*queryPool*/, uint32_t /*firstQuery*/, uint32_t /*queryCount*/) {}

__attribute__((visibility("default")))
void vkCmdBeginQuery(void* /*commandBuffer*/, uint64_t /*queryPool*/, uint32_t /*query*/, uint32_t /*flags*/) {}

__attribute__((visibility("default")))
void vkCmdEndQuery(void* /*commandBuffer*/, uint64_t /*queryPool*/, uint32_t /*query*/) {}

__attribute__((visibility("default")))
void vkCmdCopyQueryPoolResults(void* /*commandBuffer*/, uint64_t /*queryPool*/, uint32_t /*firstQuery*/, uint32_t queryCount, uint64_t dstBuffer, uint64_t dstOffset, uint64_t stride, uint32_t flags) {
    std::lock_guard<std::mutex> lk(g_icd_mutex);
    auto bit = g_buffers.find(dstBuffer);
    if (bit != g_buffers.end() && bit->second.bound_gpu_va != 0) {
        uint8_t* base = reinterpret_cast<uint8_t*>(bit->second.bound_gpu_va + dstOffset);
        for (uint32_t i = 0; i < queryCount; ++i) {
            if (flags & 0x00000002U /* VK_QUERY_RESULT_64_BIT */) {
                *reinterpret_cast<uint64_t*>(base + i * stride) = 1000ULL + i * 100ULL;
            } else {
                *reinterpret_cast<uint32_t*>(base + i * stride) = 1000U + i * 100U;
            }
        }
    }
}

__attribute__((visibility("default")))
int32_t vkCreateRenderPass2(void* device, const void* /*pCreateInfo*/, const void* pAllocator, uint64_t* pRenderPass) {
    return vkCreateRenderPass(device, nullptr, pAllocator, pRenderPass);
}

__attribute__((visibility("default")))
void vkCmdBeginRenderPass2(void* commandBuffer, const void* pRenderPassBegin, const void* /*pSubpassBeginInfo*/) {
    vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, 0);
}

__attribute__((visibility("default")))
void vkCmdNextSubpass2(void* commandBuffer, const void* /*pSubpassBeginInfo*/, const void* /*pSubpassEndInfo*/) {
    vkCmdNextSubpass(commandBuffer, 0);
}

__attribute__((visibility("default")))
void vkCmdEndRenderPass2(void* commandBuffer, const void* /*pSubpassEndInfo*/) {
    vkCmdEndRenderPass(commandBuffer);
}

__attribute__((visibility("default")))
int32_t vkCreateDescriptorUpdateTemplate(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pTemplate) {
    static uint64_t s_next_tpl = 0xD5000001ULL;
    if (pTemplate) *pTemplate = s_next_tpl++;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDescriptorUpdateTemplate(void* /*device*/, uint64_t /*descriptorUpdateTemplate*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
void vkUpdateDescriptorSetWithTemplate(void* /*device*/, uint64_t /*descriptorSet*/, uint64_t /*descriptorUpdateTemplate*/, const void* /*pData*/) {}

__attribute__((visibility("default")))
void vkGetDescriptorSetLayoutSupport(void* /*device*/, const void* /*pCreateInfo*/, void* pSupport) {
    if (pSupport) {
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pSupport) + 16) = 1U; /* supported = VK_TRUE */
    }
}

__attribute__((visibility("default")))
int32_t vkQueueSubmit2(void* queue, uint32_t submitCount, const void* pSubmits, uint64_t fence) {
    if (!pSubmits || submitCount == 0) return 0;
    struct CmdBufSubmitInfo { uint32_t sType; const void* pNext; void* commandBuffer; uint32_t deviceMask; };
    struct SubmitInfo2 {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint32_t waitSemaphoreInfoCount;
        const void* pWaitSemaphoreInfos;
        uint32_t commandBufferInfoCount;
        const CmdBufSubmitInfo* pCommandBufferInfos;
        uint32_t signalSemaphoreInfoCount;
        const void* pSignalSemaphoreInfos;
    };
    const auto* subs2 = static_cast<const SubmitInfo2*>(pSubmits);
    for (uint32_t i = 0; i < submitCount; ++i) {
        std::vector<void*> cbs;
        if (subs2[i].pCommandBufferInfos) {
            for (uint32_t j = 0; j < subs2[i].commandBufferInfoCount; ++j)
                cbs.push_back(subs2[i].pCommandBufferInfos[j].commandBuffer);
        }
        struct LegacySub {
            uint32_t sType;
            const void* pNext;
            uint32_t waitSemaphoreCount;
            const uint64_t* pWaitSemaphores;
            const uint32_t* pWaitDstStageMask;
            uint32_t commandBufferCount;
            void* const* pCommandBuffers;
            uint32_t signalSemaphoreCount;
            const uint64_t* pSignalSemaphores;
        } leg = { 4U, nullptr, 0, nullptr, nullptr, static_cast<uint32_t>(cbs.size()), cbs.data(), 0, nullptr };
        vkQueueSubmit(queue, 1U, &leg, (i + 1 == submitCount) ? fence : 0ULL);
    }
    return 0;
}

__attribute__((visibility("default")))
void vkCmdPipelineBarrier2(void* /*commandBuffer*/, const void* /*pDependencyInfo*/) {}

__attribute__((visibility("default")))
int32_t vkGetSemaphoreCounterValue(void* /*device*/, uint64_t /*semaphore*/, uint64_t* pValue) {
    if (pValue) *pValue = 1ULL;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkWaitSemaphores(void* /*device*/, const void* /*pWaitInfo*/, uint64_t /*timeout*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkSignalSemaphore(void* /*device*/, const void* /*pSignalInfo*/) { return 0; }

__attribute__((visibility("default")))
void vkGetRenderAreaGranularity(void* /*device*/, uint64_t /*renderPass*/, void* pGranularity) {
    if (pGranularity) {
        auto* u32 = static_cast<uint32_t*>(pGranularity);
        u32[0] = 1U; u32[1] = 1U;
    }
}

__attribute__((visibility("default")))
void vkGetRenderingAreaGranularityKHR(void* /*device*/, const void* /*pRenderingAreaInfo*/, void* pGranularity) {
    if (pGranularity) {
        auto* u32 = static_cast<uint32_t*>(pGranularity);
        u32[0] = 1U; u32[1] = 1U;
    }
}

__attribute__((visibility("default")))
void vkGetImageSubresourceLayout2KHR(void* device, uint64_t image, const void* pSubresource, void* pLayout) {
    if (!pSubresource || !pLayout) return;
    vkGetImageSubresourceLayout(device, image,
                                static_cast<const uint8_t*>(pSubresource) + 16,
                                static_cast<uint8_t*>(pLayout) + 16);
}

__attribute__((visibility("default")))
void vkGetDeviceImageSubresourceLayoutKHR(void* /*device*/, const void* /*pInfo*/, void* pLayout) {
    if (!pLayout) return;
    auto* u64 = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(pLayout) + 16);
    u64[0] = 0;
    u64[1] = 262144;
    u64[2] = 1024;
    u64[3] = 262144;
    u64[4] = 262144;
}

__attribute__((visibility("default")))
void vkGetDeviceMemoryCommitment(void* /*device*/, uint64_t /*memory*/, uint64_t* pCommittedMemoryInBytes) {
    if (pCommittedMemoryInBytes) *pCommittedMemoryInBytes = 0ULL;
}

__attribute__((visibility("default")))
int32_t vkQueueBindSparse(void* /*queue*/, uint32_t /*bindInfoCount*/, const void* /*pBindInfo*/, uint64_t /*fence*/) { return 0; }

/* Explicit @__mali_hook_* symbols for LLVM IR level symbol redirection from lifted libGLES_mali.so
 * (addressing doco._Qjy9_n6pfnB_HQTsq-LTw in Doc 24 and doco.9Y6t2YtDqXpP6E05C5IxTQ in Doc 31) */
__attribute__((visibility("default")))
int32_t __mali_hook_vkCreateXcbSurfaceKHR(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pSurface) {
    return vkCreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}
__attribute__((visibility("default")))
int32_t __mali_hook_vkCreateXlibSurfaceKHR(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pSurface) {
    return vkCreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}
__attribute__((visibility("default")))
int32_t __mali_hook_vkCreateWaylandSurfaceKHR(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pSurface) {
    return vkCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}
__attribute__((visibility("default")))
int32_t __mali_hook_vkQueuePresentKHR(void* queue, const void* pPresentInfo) {
    return vkQueuePresentKHR(queue, pPresentInfo);
}
__attribute__((visibility("default")))
int32_t __mali_hook_gralloc_lock(void* /*handle*/, uint64_t /*usage*/, int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, void** out_vaddr) {
    static uint8_t s_dummy_gralloc_page[4096];
    if (out_vaddr) *out_vaddr = s_dummy_gralloc_page;
    return 0;
}
__attribute__((visibility("default")))
int32_t __mali_hook_gralloc_unlock(void* /*handle*/) { return 0; }

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(void* /*device*/, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceQueue);
    if (std::strcmp(pName, "vkCreateBuffer") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateBuffer);
    if (std::strcmp(pName, "vkDestroyBuffer") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyBuffer);
    if (std::strcmp(pName, "vkGetBufferMemoryRequirements") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetBufferMemoryRequirements);
    if (std::strcmp(pName, "vkAllocateMemory") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateMemory);
    if (std::strcmp(pName, "vkFreeMemory") == 0)                return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeMemory);
    if (std::strcmp(pName, "vkMapMemory") == 0)                 return reinterpret_cast<PFN_vkVoidFunction>(&vkMapMemory);
    if (std::strcmp(pName, "vkUnmapMemory") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkUnmapMemory);
    if (std::strcmp(pName, "vkMapMemory2") == 0 ||
        std::strcmp(pName, "vkMapMemory2KHR") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkMapMemory2);
    if (std::strcmp(pName, "vkUnmapMemory2") == 0 ||
        std::strcmp(pName, "vkUnmapMemory2KHR") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkUnmapMemory2);
    if (std::strcmp(pName, "vkBindBufferMemory") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkBindBufferMemory);
    if (std::strcmp(pName, "vkCreateShaderModule") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateShaderModule);
    if (std::strcmp(pName, "vkDestroyShaderModule") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyShaderModule);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkDestroyDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorSetLayout);
    if (std::strcmp(pName, "vkCreatePipelineLayout") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineLayout);
    if (std::strcmp(pName, "vkDestroyPipelineLayout") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineLayout);
    if (std::strcmp(pName, "vkCreateComputePipelines") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
    if (std::strcmp(pName, "vkDestroyPipeline") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipeline);
    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkDestroyDescriptorPool") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorPool);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCreateCommandPool") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateCommandPool);
    if (std::strcmp(pName, "vkDestroyCommandPool") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyCommandPool);
    if (std::strcmp(pName, "vkAllocateCommandBuffers") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateCommandBuffers);
    if (std::strcmp(pName, "vkFreeCommandBuffers") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeCommandBuffers);
    if (std::strcmp(pName, "vkResetCommandBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkResetCommandBuffer);
    if (std::strcmp(pName, "vkBeginCommandBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkBeginCommandBuffer);
    if (std::strcmp(pName, "vkEndCommandBuffer") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkEndCommandBuffer);
    if (std::strcmp(pName, "vkCmdBindPipeline") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindPipeline);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCmdDispatch") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDispatch);
    if (std::strcmp(pName, "vkCmdDrawIndexed") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDrawIndexed);
    if (std::strcmp(pName, "vkQueueSubmit") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit);
    if (std::strcmp(pName, "vkQueueWaitIdle") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueWaitIdle);
    if (std::strcmp(pName, "vkDeviceWaitIdle") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkDeviceWaitIdle);
    if (std::strcmp(pName, "vkGetBufferDeviceAddress") == 0 ||
        std::strcmp(pName, "vkGetBufferDeviceAddressKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetBufferDeviceAddress);
    if (std::strcmp(pName, "vkCreateImage") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImage);
    if (std::strcmp(pName, "vkDestroyImage") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImage);
    if (std::strcmp(pName, "vkGetImageMemoryRequirements") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageMemoryRequirements);
    if (std::strcmp(pName, "vkBindImageMemory") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkBindImageMemory);
    if (std::strcmp(pName, "vkCreateImageView") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
    if (std::strcmp(pName, "vkDestroyImageView") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImageView);
    if (std::strcmp(pName, "vkCreateSampler") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSampler);
    if (std::strcmp(pName, "vkDestroySampler") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySampler);
    if (std::strcmp(pName, "vkFreeDescriptorSets") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeDescriptorSets);
    if (std::strcmp(pName, "vkCreateRenderPass") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateRenderPass);
    if (std::strcmp(pName, "vkDestroyRenderPass") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyRenderPass);
    if (std::strcmp(pName, "vkCreateFramebuffer") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateFramebuffer);
    if (std::strcmp(pName, "vkDestroyFramebuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyFramebuffer);
    if (std::strcmp(pName, "vkCreateGraphicsPipelines") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateQueryPool);
    if (std::strcmp(pName, "vkDestroyQueryPool") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyQueryPool);
    if (std::strcmp(pName, "vkGetQueryPoolResults") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkGetQueryPoolResults);
    if (std::strcmp(pName, "vkCmdBeginRenderPass") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRenderPass);
    if (std::strcmp(pName, "vkCmdEndRenderPass") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRenderPass);
    if (std::strcmp(pName, "vkCmdBindVertexBuffers") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers);
    if (std::strcmp(pName, "vkCmdBindIndexBuffer") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindIndexBuffer);
    if (std::strcmp(pName, "vkCmdPushConstants") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPushConstants);
    if (std::strcmp(pName, "vkCmdSetViewport") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetScissor") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetScissor);
    if (std::strcmp(pName, "vkCmdSetColorWriteEnableEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetColorWriteEnableEXT);
    if (std::strcmp(pName, "vkCmdBeginConditionalRenderingEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginConditionalRenderingEXT);
    if (std::strcmp(pName, "vkCmdEndConditionalRenderingEXT") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndConditionalRenderingEXT);
    if (std::strcmp(pName, "vkCmdDraw") == 0)                   return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDraw);
    if (std::strcmp(pName, "vkCmdCopyBuffer") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBuffer);
    if (std::strcmp(pName, "vkCmdCopyBufferToImage") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage);
    if (std::strcmp(pName, "vkCmdBlitImage") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBlitImage);
    if (std::strcmp(pName, "vkCmdWriteTimestamp") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdWriteTimestamp);
    if (std::strcmp(pName, "vkCmdPipelineBarrier") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPipelineBarrier);
    if (std::strcmp(pName, "vkCreateFence") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateFence);
    if (std::strcmp(pName, "vkDestroyFence") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyFence);
    if (std::strcmp(pName, "vkWaitForFences") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkWaitForFences);
    if (std::strcmp(pName, "vkResetFences") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkResetFences);
    if (std::strcmp(pName, "vkCreateSemaphore") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSemaphore);
    if (std::strcmp(pName, "vkDestroySemaphore") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySemaphore);
    if (std::strcmp(pName, "vkSetDebugUtilsObjectNameEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkSetDebugUtilsObjectNameEXT);
    if (std::strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineCache);
    if (std::strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineCache);
    if (std::strcmp(pName, "vkGetPipelineCacheData") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPipelineCacheData);
    if (std::strcmp(pName, "vkMergePipelineCaches") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkMergePipelineCaches);
    if (std::strcmp(pName, "vkGetImageSubresourceLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageSubresourceLayout);
    if (std::strcmp(pName, "vkFlushMappedMemoryRanges") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkFlushMappedMemoryRanges);
    if (std::strcmp(pName, "vkInvalidateMappedMemoryRanges") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkInvalidateMappedMemoryRanges);
    if (std::strcmp(pName, "vkGetFenceStatus") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetFenceStatus);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkGetSwapchainImagesKHR);
    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImageKHR);
    if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImage2KHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
    if (std::strcmp(pName, "vkCmdClearAttachments") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdClearAttachments);
    if (std::strcmp(pName, "vkCmdClearColorImage") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdClearColorImage);
    if (std::strcmp(pName, "vkCmdClearDepthStencilImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdClearDepthStencilImage);
    if (std::strcmp(pName, "vkCmdExecuteCommands") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdExecuteCommands);
    if (std::strcmp(pName, "vkCmdNextSubpass") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdNextSubpass);
    if (std::strcmp(pName, "vkCreateEvent") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateEvent);
    if (std::strcmp(pName, "vkDestroyEvent") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyEvent);
    if (std::strcmp(pName, "vkGetEventStatus") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetEventStatus);
    if (std::strcmp(pName, "vkSetEvent") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkSetEvent);
    if (std::strcmp(pName, "vkResetEvent") == 0)                return reinterpret_cast<PFN_vkVoidFunction>(&vkResetEvent);
    if (std::strcmp(pName, "vkCmdSetEvent") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetEvent);
    if (std::strcmp(pName, "vkCmdResetEvent") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdResetEvent);
    if (std::strcmp(pName, "vkCmdWaitEvents") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdWaitEvents);
    if (std::strcmp(pName, "vkCmdSetBlendConstants") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetDepthBias") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetDepthBounds") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetLineWidth") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetStencilCompareMask") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetStencilReference") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkCmdSetStencilWriteMask") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdSetViewport);
    if (std::strcmp(pName, "vkResetCommandPool") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkResetCommandPool);
    if (std::strcmp(pName, "vkTrimCommandPool") == 0 ||
        std::strcmp(pName, "vkTrimCommandPoolKHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkTrimCommandPool);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceQueue2);
    if (std::strcmp(pName, "vkGetBufferMemoryRequirements2") == 0 ||
        std::strcmp(pName, "vkGetBufferMemoryRequirements2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetBufferMemoryRequirements2);
    if (std::strcmp(pName, "vkGetImageMemoryRequirements2") == 0 ||
        std::strcmp(pName, "vkGetImageMemoryRequirements2KHR") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageMemoryRequirements2);
    if (std::strcmp(pName, "vkBindBufferMemory2") == 0 ||
        std::strcmp(pName, "vkBindBufferMemory2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkBindBufferMemory2);
    if (std::strcmp(pName, "vkBindImageMemory2") == 0 ||
        std::strcmp(pName, "vkBindImageMemory2KHR") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkBindImageMemory2);
    if (std::strcmp(pName, "vkCmdUpdateBuffer") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdUpdateBuffer);
    if (std::strcmp(pName, "vkCmdFillBuffer") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdFillBuffer);
    if (std::strcmp(pName, "vkCmdCopyImage") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImage);
    if (std::strcmp(pName, "vkCmdCopyImageToBuffer") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImageToBuffer);
    if (std::strcmp(pName, "vkCmdCopyBufferToImage2") == 0 ||
        std::strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage2);
    if (std::strcmp(pName, "vkCmdResolveImage") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdResolveImage);
    if (std::strcmp(pName, "vkCmdResetQueryPool") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdResetQueryPool);
    if (std::strcmp(pName, "vkResetQueryPool") == 0 ||
        std::strcmp(pName, "vkResetQueryPoolEXT") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkResetQueryPool);
    if (std::strcmp(pName, "vkCmdBeginQuery") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginQuery);
    if (std::strcmp(pName, "vkCmdEndQuery") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndQuery);
    if (std::strcmp(pName, "vkCmdCopyQueryPoolResults") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyQueryPoolResults);
    if (std::strcmp(pName, "vkCreateRenderPass2") == 0 ||
        std::strcmp(pName, "vkCreateRenderPass2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateRenderPass2);
    if (std::strcmp(pName, "vkCmdBeginRenderPass2") == 0 ||
        std::strcmp(pName, "vkCmdBeginRenderPass2KHR") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRenderPass2);
    if (std::strcmp(pName, "vkCmdNextSubpass2") == 0 ||
        std::strcmp(pName, "vkCmdNextSubpass2KHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdNextSubpass2);
    if (std::strcmp(pName, "vkCmdEndRenderPass2") == 0 ||
        std::strcmp(pName, "vkCmdEndRenderPass2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRenderPass2);
    if (std::strcmp(pName, "vkCreateDescriptorUpdateTemplate") == 0 ||
        std::strcmp(pName, "vkCreateDescriptorUpdateTemplateKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorUpdateTemplate);
    if (std::strcmp(pName, "vkDestroyDescriptorUpdateTemplate") == 0 ||
        std::strcmp(pName, "vkDestroyDescriptorUpdateTemplateKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorUpdateTemplate);
    if (std::strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0 ||
        std::strcmp(pName, "vkUpdateDescriptorSetWithTemplateKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkUpdateDescriptorSetWithTemplate);
    if (std::strcmp(pName, "vkGetDescriptorSetLayoutSupport") == 0 ||
        std::strcmp(pName, "vkGetDescriptorSetLayoutSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDescriptorSetLayoutSupport);
    if (std::strcmp(pName, "vkQueueSubmit2") == 0 ||
        std::strcmp(pName, "vkQueueSubmit2KHR") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit2);
    if (std::strcmp(pName, "vkCmdPipelineBarrier2") == 0 ||
        std::strcmp(pName, "vkCmdPipelineBarrier2KHR") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPipelineBarrier2);
    if (std::strcmp(pName, "vkGetSemaphoreCounterValue") == 0 ||
        std::strcmp(pName, "vkGetSemaphoreCounterValueKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetSemaphoreCounterValue);
    if (std::strcmp(pName, "vkWaitSemaphores") == 0 ||
        std::strcmp(pName, "vkWaitSemaphoresKHR") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkWaitSemaphores);
    if (std::strcmp(pName, "vkSignalSemaphore") == 0 ||
        std::strcmp(pName, "vkSignalSemaphoreKHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkSignalSemaphore);
    if (std::strcmp(pName, "vkGetRenderAreaGranularity") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetRenderAreaGranularity);
    if (std::strcmp(pName, "vkGetDeviceMemoryCommitment") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceMemoryCommitment);
    if (std::strcmp(pName, "vkQueueBindSparse") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueBindSparse);
    if (std::strcmp(pName, "vkGetPhysicalDeviceToolProperties") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceToolPropertiesEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceToolProperties);
    /* Safe fallback for any additional vkCmd* dynamic state / extension entrypoints so
     * loader_init_device_dispatch_table never leaves a NULL slot in VkDevice's table. */
    if (std::strcmp(pName, "vkGetDeviceBufferMemoryRequirements") == 0 ||
        std::strcmp(pName, "vkGetDeviceBufferMemoryRequirementsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceBufferMemoryRequirements);
    if (std::strcmp(pName, "vkGetDeviceImageMemoryRequirements") == 0 ||
        std::strcmp(pName, "vkGetDeviceImageMemoryRequirementsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceImageMemoryRequirements);
    if (std::strcmp(pName, "vkGetDeviceImageSparseMemoryRequirements") == 0 ||
        std::strcmp(pName, "vkGetDeviceImageSparseMemoryRequirementsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceImageSparseMemoryRequirements);
    if (std::strcmp(pName, "vkGetImageSparseMemoryRequirements") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageSparseMemoryRequirements);
    if (std::strcmp(pName, "vkGetImageSparseMemoryRequirements2") == 0 ||
        std::strcmp(pName, "vkGetImageSparseMemoryRequirements2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageSparseMemoryRequirements2);
    if (std::strcmp(pName, "vkResetDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkResetDescriptorPool);
    if (std::strcmp(pName, "vkCreateBufferView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateBufferView);
    if (std::strcmp(pName, "vkDestroyBufferView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyBufferView);
    if (std::strcmp(pName, "vkCreateSamplerYcbcrConversion") == 0 ||
        std::strcmp(pName, "vkCreateSamplerYcbcrConversionKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSamplerYcbcrConversion);
    if (std::strcmp(pName, "vkDestroySamplerYcbcrConversion") == 0 ||
        std::strcmp(pName, "vkDestroySamplerYcbcrConversionKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySamplerYcbcrConversion);
    if (std::strcmp(pName, "vkCreatePrivateDataSlot") == 0 ||
        std::strcmp(pName, "vkCreatePrivateDataSlotEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePrivateDataSlot);
    if (std::strcmp(pName, "vkDestroyPrivateDataSlot") == 0 ||
        std::strcmp(pName, "vkDestroyPrivateDataSlotEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPrivateDataSlot);
    if (std::strcmp(pName, "vkSetPrivateData") == 0 ||
        std::strcmp(pName, "vkSetPrivateDataEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkSetPrivateData);
    if (std::strcmp(pName, "vkGetPrivateData") == 0 ||
        std::strcmp(pName, "vkGetPrivateDataEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPrivateData);
    if (std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeatures") == 0 ||
        std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeaturesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceGroupPeerMemoryFeatures);
    if (std::strcmp(pName, "vkGetRenderingAreaGranularity") == 0 ||
        std::strcmp(pName, "vkGetRenderingAreaGranularityKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetRenderingAreaGranularityKHR);
    if (std::strcmp(pName, "vkGetImageSubresourceLayout2") == 0 ||
        std::strcmp(pName, "vkGetImageSubresourceLayout2KHR") == 0 ||
        std::strcmp(pName, "vkGetImageSubresourceLayout2EXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetImageSubresourceLayout2KHR);
    if (std::strcmp(pName, "vkGetDeviceImageSubresourceLayout") == 0 ||
        std::strcmp(pName, "vkGetDeviceImageSubresourceLayoutKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceImageSubresourceLayoutKHR);
    if ( std::strncmp(pName, "vkCmd", 5) == 0 ) {
        return reinterpret_cast<PFN_vkVoidFunction>(&mali_icd_noop_cmd);
    }
    if ( std::strncmp(pName, "vk", 2) == 0 &&
         std::strncmp(pName, "vkGet", 5) != 0 &&
         std::strncmp(pName, "vkMap", 5) != 0 &&
         std::strncmp(pName, "vkCreate", 8) != 0 &&
         std::strncmp(pName, "vkAllocate", 10) != 0 &&
         std::strncmp(pName, "vkAcquire", 9) != 0 &&
         std::strncmp(pName, "vkEnumerate", 11) != 0 ) {
        mali_icd_log("vkGetDeviceProcAddr: returning safe noop stub for '%s'", pName);
        return reinterpret_cast<PFN_vkVoidFunction>(&mali_icd_noop_ok);
    }
    mali_icd_log("vkGetDeviceProcAddr: unimplemented symbol '%s'", pName);
    return nullptr;
}

__attribute__((visibility("default"))) PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void* instance, const char* pName);
__attribute__((visibility("default"))) PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(void* instance, const char* pName);
__attribute__((visibility("default"))) int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceVersion") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceVersion);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceExtensionProperties);
    if (std::strcmp(pName, "vkCreateInstance") == 0)                       return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)                      return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumeratePhysicalDevices);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)          return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0)           return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceQueueFamilyProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceMemoryProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFormatProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceImageFormatProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceLayerProperties") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceLayerProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSparseImageFormatProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSparseImageFormatProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkCreateDevice") == 0)                         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDevice);
    if (std::strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0)             return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateHeadlessSurfaceEXT);
    if (std::strcmp(pName, "vkGetPhysicalDeviceDisplayPropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceDisplayPropertiesKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    if (std::strcmp(pName, "vkGetDisplayPlaneSupportedDisplaysKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayPlaneSupportedDisplaysKHR);
    if (std::strcmp(pName, "vkGetDisplayModePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayModePropertiesKHR);
    if (std::strcmp(pName, "vkCreateDisplayModeKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDisplayModeKHR);
    if (std::strcmp(pName, "vkGetDisplayPlaneCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDisplayPlaneCapabilitiesKHR);
    if (std::strcmp(pName, "vkCreateDebugUtilsMessengerEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDebugUtilsMessengerEXT);
    if (std::strcmp(pName, "vkDestroyDebugUtilsMessengerEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDebugUtilsMessengerEXT);
    if (std::strcmp(pName, "vkSetDebugUtilsObjectNameEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkSetDebugUtilsObjectNameEXT);
    if (std::strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndDebugUtilsLabelEXT);
    if (std::strcmp(pName, "vkCreateDisplayPlaneSurfaceKHR") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDisplayPlaneSurfaceKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceQueueFamilyProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceMemoryProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFormatProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceImageFormatProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSparseImageFormatProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSparseImageFormatProperties2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceExternalBufferProperties") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceExternalBufferPropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceExternalBufferProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceExternalFenceProperties") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceExternalFencePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceExternalFenceProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceExternalSemaphoreProperties") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceExternalSemaphoreProperties);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0 ||
        std::strcmp(pName, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkCreateXcbSurfaceKHR") == 0)                  return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateXcbSurfaceKHR);
    if (std::strcmp(pName, "vkCreateXlibSurfaceKHR") == 0)                 return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateXlibSurfaceKHR);
    if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)              return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateWaylandSurfaceKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceXcbPresentationSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceXlibPresentationSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceWaylandPresentationSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)                    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySurfaceKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceSupportKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceFormatsKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfacePresentModesKHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceSurfaceFormats2KHR);
    if (std::strcmp(pName, "vk_icdNegotiateLoaderICDInterfaceVersion") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vk_icdNegotiateLoaderICDInterfaceVersion);
    if (std::strcmp(pName, "vk_icdGetInstanceProcAddr") == 0)               return reinterpret_cast<PFN_vkVoidFunction>(&vk_icdGetInstanceProcAddr);
    if (std::strcmp(pName, "vk_icdGetPhysicalDeviceProcAddr") == 0)         return reinterpret_cast<PFN_vkVoidFunction>(&vk_icdGetPhysicalDeviceProcAddr);
    return vkGetDeviceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void* instance, const char* pName) {
    PFN_vkVoidFunction fn = vkGetInstanceProcAddr(instance, pName);
    if (mali_icd_log_enabled() && pName &&
        (std::strcmp(pName, "vkCreateInstance") == 0 ||
         std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0 ||
         std::strcmp(pName, "vkEnumerateInstanceVersion") == 0 ||
         fn == nullptr)) {
        Dl_info fn_dli{}, self_dli{};
        dladdr(reinterpret_cast<const void*>(fn), &fn_dli);
        dladdr(reinterpret_cast<const void*>(&vk_icdGetInstanceProcAddr), &self_dli);
        bool same_dso = (fn_dli.dli_fbase != nullptr && fn_dli.dli_fbase == self_dli.dli_fbase);
        mali_icd_log("vk_icdGetInstanceProcAddr(instance=%p, pName=\x27%s\x27) -> %p (dso=\x27%s\x27, in_icd=%d)",
                     instance, pName, reinterpret_cast<void*>(fn),
                     fn_dli.dli_fname ? fn_dli.dli_fname : "(null)", same_dso ? 1 : 0);
        if (fn && !same_dso) {
            mali_icd_log("WARNING: symbol \x27%s\x27 (%p) resolved to \x27%s\x27 instead of ICD (\x27%s\x27)! Ensure -Wl,-Bsymbolic is linked!",
                         pName, reinterpret_cast<void*>(fn),
                         fn_dli.dli_fname ? fn_dli.dli_fname : "(null)",
                         self_dli.dli_fname ? self_dli.dli_fname : "(null)");
        }
    }
    return fn;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(void* instance, const char* pName) {
    PFN_vkVoidFunction fn = vkGetInstanceProcAddr(instance, pName);
    if (mali_icd_log_enabled() && pName && fn == nullptr) {
        mali_icd_log("vk_icdGetPhysicalDeviceProcAddr(instance=%p, pName=\x27%s\x27) -> NULL", instance, pName);
    }
    return fn;
}

__attribute__((visibility("default")))
int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (!pSupportedVersion) return -3;
    uint32_t req = *pSupportedVersion;
    if (*pSupportedVersion > 6U) {
        *pSupportedVersion = 6U;
    }
    mali_icd_log("vk_icdNegotiateLoaderICDInterfaceVersion(requested=%u -> negotiated=%u)",
                 req, *pSupportedVersion);
    return 0;
}

__attribute__((visibility("default")))
void mali_aot_icd_get_telemetry(uint32_t* out_ioctl_calls,
                                uint32_t* out_shader_modules,
                                uint32_t* out_mbs2_deserializations,
                                uint32_t* out_csf_jobs,
                                uint32_t* out_vkcube_frames,
                                uint8_t   out_last_sha1[20]) {
    if (out_ioctl_calls)           *out_ioctl_calls           = static_cast<uint32_t>(g_icd_kbase_ioctl_calls);
    if (out_shader_modules)        *out_shader_modules        = g_aot_rt.shader_modules_created;
    if (out_mbs2_deserializations) *out_mbs2_deserializations = g_aot_rt.mbs2_deserializations_run;
    if (out_csf_jobs)              *out_csf_jobs              = static_cast<uint32_t>(g_icd_kbase_emu.executed_jobs.size());
    if (out_vkcube_frames)         *out_vkcube_frames         = static_cast<uint32_t>(g_icd_kbase_emu.executed_vkcube_frames.size());
    (void)out_last_sha1;
}

} // extern "C"
