#pragma once
// ============================================================================
// r54p3_icd_types.hpp
// Decompiled Internal Structures of the r54p3-00eac0-6eda07 Vulkan 1.3 ICD
// (`vulkan::*`, `gfx::*`, `kbase::*`, `osu::*`) with exact RVA cross-references.
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "r54p3_dispatch_table.hpp"
#include "r54p3_cmpbe_boundary.hpp"

namespace mali_r54p3 {

static constexpr uintptr_t kIcdLoaderMagic = 0x01CDC0DEULL;

namespace osu {
// RVA 0x008bfaa0: ARMv8 Crypto Extension SHA-1 hardware-acceleratedSPIR-V digest
void computeSpirvSha1(const uint32_t* words, size_t byte_len, uint8_t out_sha1[20]);
} // namespace osu

namespace kbase {

struct Context {
    int32_t  fd            = 100;
    uint16_t uapi_major    = 1;
    uint16_t uapi_minor    = 36;
    uint32_t create_flags  = 0;
    void*    tracking_page = nullptr;
    void*    user_reg_page = nullptr;
    uint64_t tiler_heap_va = 0;
};

struct CsiQueue {
    Context* kctx           = nullptr;
    uint8_t  group_handle   = 1;
    uint8_t  csi_index      = 0;
    uint64_t ring_bo_gpu_va = 0;
    uint32_t ring_size      = 4096;
    uint64_t doorbell_va    = 0;
    uint64_t user_input_va  = 0;
    uint64_t user_output_va = 0;
    uint64_t write_offset   = 0;
};

} // namespace kbase

namespace gfx {

// 32-byte Valhall Texture Descriptor (built by gfx::image_view::init at RVA 0x01b05630)
struct ValhallTextureDescriptor {
    uint32_t format_and_swizzle = 0;
    uint32_t dimension_and_type = 0;
    uint32_t width_minus_1      = 0;
    uint32_t height_minus_1     = 0;
    uint64_t surface_gpu_va     = 0;
    uint32_t row_stride_bytes   = 0;
    uint32_t layer_stride_bytes = 0;
};

// 32-byte Valhall Sampler Descriptor (built by vkCreateSampler at RVA 0x0098d500)
struct ValhallSamplerDescriptor {
    uint32_t filter_mode        = 1;
    uint32_t wrap_modes         = 0;
    float    min_lod            = 0.0f;
    float    max_lod            = 15.0f;
    float    lod_bias           = 0.0f;
    float    max_anisotropy     = 16.0f;
    uint32_t compare_op         = 0;
    uint32_t border_color       = 0;
};

} // namespace gfx

namespace vulkan {

struct PhysicalDevice;
struct Device;

struct Instance {
    uintptr_t      loader_magic = kIcdLoaderMagic;
    uint32_t       api_version  = (1U << 22) | (3U << 12) | 250U;
    PhysicalDevice* phys_dev    = nullptr;
};

struct PhysicalDevice {
    uintptr_t loader_magic    = kIcdLoaderMagic;
    Instance* parent_instance = nullptr;
    uint32_t  vendor_id       = 0x13B5U;
    uint32_t  device_id       = 0xB8830001U;
    char      device_name[256] = "Arm Mali-G615 (r54p3 Decompiled C++17 ICD via mali0_emu)";
};

struct Queue {
    uintptr_t loader_magic = kIcdLoaderMagic;
    Device*   device       = nullptr;
    uint32_t  family_index = 0;
    uint32_t  queue_index  = 0;
};

struct Device {
    uintptr_t            loader_magic = kIcdLoaderMagic;
    PhysicalDevice*      phys_dev     = nullptr;
    Queue                queues[4]{};
    kbase::Context       kctx{};
    kbase::CsiQueue      csi_queue{};
    cmpbe::CmpbeV2Context cmpbe_ctx{};
    uint64_t             srt_bo_va    = 0;
    uint64_t             spd_bo_va    = 0;
    uint64_t             tsd_bo_va    = 0;
    uint64_t             fau_bo_va    = 0;
    uint64_t             cqs_bo_va    = 0;
    std::mutex           submit_mtx;
};

struct DeviceMemory {
    uint64_t gpu_va      = 0;
    uint64_t size_bytes  = 0;
    uint64_t va_pages    = 0;
    uint32_t type_index  = 0;
};

struct Buffer {
    uint64_t size_bytes   = 0;
    uint32_t usage        = 0;
    uint64_t bound_gpu_va = 0;
};

struct Image {
    uint32_t             width        = 64;
    uint32_t             height       = 64;
    uint32_t             depth        = 1;
    uint32_t             mip_levels   = 1;
    uint32_t             array_layers = 1;
    uint32_t             format       = 37;
    uint64_t             bound_gpu_va = 0;
    uint64_t             byte_size    = 16384;
    uint32_t             image_type   = 1;
    uint32_t             samples      = 1;
    uint32_t             usage        = 0;
    std::vector<uint8_t> stub_texture_backing;
};

struct ImageView {
    Image*                        image     = nullptr;
    uint32_t                      view_type = 1;
    uint32_t                      format    = 37;
    gfx::ValhallTextureDescriptor hw_tex_desc{};
};

struct Sampler {
    gfx::ValhallSamplerDescriptor hw_sampler_desc{};
};

struct ShaderModule {
    std::vector<uint32_t> spirv_words;
    uint8_t               sha1_digest[20]{};
};

struct DescriptorSetLayout {
    uint32_t binding_count = 0;
};

struct PipelineLayout {
    uint32_t set_layout_count   = 0;
    uint32_t push_constant_size = 128;
};

struct RenderPass {
    uint32_t attachment_count = 1;
    uint32_t color_format     = 37;
    uint32_t depth_format     = 126;
};

struct Framebuffer {
    RenderPass*             render_pass = nullptr;
    uint32_t                width       = 64;
    uint32_t                height      = 64;
    uint32_t                layers      = 1;
    std::vector<ImageView*> attachments;
    std::vector<float>      depth_buffer;
    MaliMfbdDescriptor      hw_mfbd{};
};

struct Pipeline {
    bool                           is_compute    = false;
    PipelineLayout*                layout        = nullptr;
    RenderPass*                    render_pass   = nullptr;
    cmpbe::CmpbeV2CompilationResult* vs_compiled = nullptr;
    cmpbe::CmpbeV2CompilationResult* fs_compiled = nullptr;
    cmpbe::CmpbeV2CompilationResult* cs_compiled = nullptr;
    uint64_t                       vs_exec_bo_va = 0;
    uint64_t                       fs_exec_bo_va = 0;
    uint64_t                       cs_exec_bo_va = 0;
};

struct DescriptorSlot {
    uint64_t                      buffer_gpu_va = 0;
    uint64_t                      buffer_offset = 0;
    uint64_t                      buffer_range  = 0;
    ImageView*                    image_view    = nullptr;
    Sampler*                      sampler       = nullptr;
    gfx::ValhallTextureDescriptor tex_desc{};
    gfx::ValhallSamplerDescriptor samp_desc{};
};

struct DescriptorSet {
    DescriptorSlot bindings[32]{};
};

struct QueryPool {
    uint32_t              query_type  = 2;
    uint32_t              query_count = 8;
    std::vector<uint64_t> results;
};

struct RecordedCommand {
    enum Type {
        CMD_DISPATCH,
        CMD_DRAW,
        CMD_DRAW_INDEXED,
        CMD_COPY_BUFFER,
        CMD_COPY_BUFFER_TO_IMAGE,
        CMD_BLIT_IMAGE,
        CMD_WRITE_TIMESTAMP,
        CMD_PIPELINE_BARRIER,
    } type = CMD_DISPATCH;

    Pipeline*      pipeline        = nullptr;
    DescriptorSet* desc_set        = nullptr;
    Framebuffer*   framebuffer     = nullptr;
    uint32_t       dynamic_offset  = 0;
    uint32_t       wg_x = 1, wg_y = 1, wg_z = 1;
    uint32_t       index_count     = 0;
    uint32_t       vertex_count    = 0;
    uint32_t       first_vertex    = 0;
    uint32_t       instance_count  = 1;
    uint32_t       first_instance  = 0;
    uint32_t       rp_draw_idx     = 0;
    uint64_t       vertex_bo_va    = 0;
    uint8_t        push_constants[128]{};
    uint64_t       src_va          = 0;
    uint64_t       dst_va          = 0;
    uint64_t       copy_bytes      = 0;
    Image*         dst_image       = nullptr;
    QueryPool*     query_pool      = nullptr;
    uint32_t       query_index     = 0;
};

struct CommandBuffer {
    uintptr_t                    loader_magic = kIcdLoaderMagic;
    Device*                      device       = nullptr;
    Pipeline*                    bound_pipeline = nullptr;
    DescriptorSet*               bound_desc_set = nullptr;
    RenderPass*                  active_rp      = nullptr;
    Framebuffer*                 active_fb      = nullptr;
    uint64_t                     bound_vbo_va[8]{};
    uint64_t                     bound_ibo_va   = 0;
    uint32_t                     dynamic_ubo_offset = 0;
    uint32_t                     rp_draw_counter = 0;
    uint8_t                      push_constants[128]{};
    bool                         conditional_rendering_active = true;
    std::vector<RecordedCommand> commands;
};

} // namespace vulkan
} // namespace mali_r54p3
