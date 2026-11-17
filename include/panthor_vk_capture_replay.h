/*
 * SPDX-License-Identifier: MIT
 *
 * Vulkan API Capture Layer (VK_LAYER_PANTHOR_capture) & Local Replay Engine
 * Binary Trace Format (.panvkcap) & Public Replay API
 */
#ifndef PANTHOR_VK_CAPTURE_REPLAY_H
#define PANTHOR_VK_CAPTURE_REPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANVK_CAP_MAGIC           "PANVKCP1"
#define PANVK_CAP_VERSION         1U

#define PANVK_CAP_FLAG_STUB_TEXTURES (1U << 0)

enum panvk_cap_record_type {
   PANVK_REC_NOP                    = 0,
   PANVK_REC_CREATE_INSTANCE        = 1,
   PANVK_REC_CREATE_DEVICE          = 2,
   PANVK_REC_CREATE_BUFFER          = 3,
   PANVK_REC_CREATE_IMAGE           = 4,
   PANVK_REC_ALLOCATE_MEMORY        = 5,
   PANVK_REC_BIND_BUFFER_MEMORY     = 6,
   PANVK_REC_BIND_IMAGE_MEMORY      = 7,
   PANVK_REC_MEMORY_SNAPSHOT        = 8,
   PANVK_REC_CREATE_SHADER_MODULE   = 9,
   PANVK_REC_CREATE_RENDER_PASS     = 10,
   PANVK_REC_CREATE_PIPELINE        = 11,
   PANVK_REC_UPDATE_DESCRIPTORS     = 12,
   PANVK_REC_CMD_BUFFER_RECORD      = 13,
   PANVK_REC_QUEUE_SUBMIT           = 14,
   PANVK_REC_QUEUE_PRESENT          = 15,
};

enum panvk_cmd_op_type {
   PANVK_CMD_BEGIN_RENDER_PASS      = 1,
   PANVK_CMD_END_RENDER_PASS        = 2,
   PANVK_CMD_BIND_PIPELINE          = 3,
   PANVK_CMD_BIND_VERTEX_BUFFERS    = 4,
   PANVK_CMD_BIND_INDEX_BUFFER      = 5,
   PANVK_CMD_BIND_DESCRIPTOR_SETS   = 6,
   PANVK_CMD_PUSH_CONSTANTS         = 7,
   PANVK_CMD_SET_VIEWPORT           = 8,
   PANVK_CMD_SET_SCISSOR            = 9,
   PANVK_CMD_COPY_BUFFER            = 10,
   PANVK_CMD_COPY_BUFFER_TO_IMAGE   = 11,
   PANVK_CMD_DRAW                   = 12,
   PANVK_CMD_DRAW_INDEXED           = 13,
   PANVK_CMD_DISPATCH               = 14,
};

struct panvk_cap_file_header {
   char     magic[8];              /* "PANVKCP1" */
   uint32_t version;               /* 1 */
   uint32_t flags;                 /* PANVK_CAP_FLAG_* */
   uint32_t api_version;           /* e.g. VK_API_VERSION_1_1 */
   uint32_t captured_vendor_id;    /* 0x13B5 (ARM) */
   uint32_t captured_device_id;    /* e.g. 0xa8670000 */
   uint32_t reserved0;
   uint64_t capture_timestamp_ns;
   uint64_t reserved[3];
};

struct panvk_cap_record_header {
   uint32_t record_type;           /* enum panvk_cap_record_type */
   uint32_t payload_bytes;
   uint64_t seq_id;
   uint64_t fnv1a64_checksum;
};

struct panvk_rec_create_instance {
   uint32_t api_version;
   uint32_t enabled_ext_count;
   char     app_name[64];
};

struct panvk_rec_create_device {
   uint32_t queue_family_index;
   uint32_t enabled_ext_count;
};

struct panvk_rec_create_buffer {
   uint64_t buffer_id;
   uint64_t size;
   uint32_t usage;
   uint32_t sharing_mode;
};

struct panvk_rec_create_image {
   uint64_t image_id;
   uint32_t image_type;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t mip_levels;
   uint32_t array_layers;
   uint32_t samples;
   uint32_t tiling;
   uint32_t usage;
};

struct panvk_rec_allocate_memory {
   uint64_t memory_id;
   uint64_t allocation_size;
   uint32_t memory_type_index;
   uint32_t pad;
};

struct panvk_rec_bind_memory {
   uint64_t resource_id; /* buffer_id or image_id */
   uint64_t memory_id;
   uint64_t memory_offset;
};

struct panvk_rec_memory_snapshot {
   uint64_t memory_id;
   uint64_t offset;
   uint64_t data_size;
   uint64_t content_fnv1a64;
   uint32_t is_stubbed_tex;
   uint32_t tex_width;
   uint32_t tex_height;
   uint32_t tex_format;
   /* Followed by (is_stubbed_tex ? 0 : data_size) bytes */
};

struct panvk_rec_create_shader_module {
   uint64_t shader_id;
   uint32_t code_size;
   uint32_t pad;
   uint64_t spirv_fnv1a64;
   /* Followed by code_size bytes of SPIR-V */
};

struct panvk_rec_create_pipeline {
   uint64_t pipeline_id;
   uint32_t bind_point; /* 0 = GRAPHICS, 1 = COMPUTE */
   uint32_t vertex_stride;
   uint64_t vs_shader_id;
   uint64_t fs_shader_id;
   uint64_t cs_shader_id;
   uint32_t topology;
   uint32_t pad;
};

struct panvk_rec_update_descriptors {
   uint64_t dst_set_id;
   uint32_t dst_binding;
   uint32_t descriptor_type;
   uint64_t buffer_id;
   uint64_t buffer_offset;
   uint64_t buffer_range;
   uint64_t image_id;
};

struct panvk_cmd_entry {
   uint32_t op; /* enum panvk_cmd_op_type */
   uint32_t flags_or_bind_point;
   uint64_t handle0;
   uint64_t handle1;
   uint64_t offset0;
   uint64_t size0;
   uint32_t u32_args[8];
   float    f32_args[20]; /* clear values or push constants (up to 80 bytes) */
};

struct panvk_rec_cmd_buffer {
   uint64_t cmd_buffer_id;
   uint32_t cmd_count;
   uint32_t pad;
   /* Followed by cmd_count * struct panvk_cmd_entry */
};

struct panvk_rec_queue_submit {
   uint64_t queue_id;
   uint64_t fence_id;
   uint32_t cmd_buffer_count;
   uint32_t pad;
   /* Followed by cmd_buffer_count * uint64_t cmd_buffer_ids */
};

struct panvk_rec_queue_present {
   uint64_t swapchain_id;
   uint32_t image_index;
   uint32_t width;
   uint32_t height;
   uint32_t format;
};

struct panvk_replay_options {
   const char *icd_json_path;
   const char *dump_ppm_path;
   bool        verbose;
};

struct panvk_replay_summary {
   uint64_t total_records_replayed;
   uint64_t total_buffers_created;
   uint64_t total_images_created;
   uint64_t total_memories_allocated;
   uint64_t total_memory_snapshots_restored;
   uint64_t total_stubbed_textures_synthesized;
   uint64_t total_shaders_created;
   uint64_t total_pipelines_created;
   uint64_t total_cmd_buffers_replayed;
   uint64_t total_queue_submits;
   uint64_t total_presents;
   uint32_t final_frame_crc32;
   int      final_status;
};

/* Programmatic Capture Control */
int  panthor_vk_capture_start(const char *filepath, uint32_t flags);
void panthor_vk_capture_stop(void);
bool panthor_vk_capture_is_active(void);

/* Local Vulkan Trace Replayer */
int  panthor_vk_replay_trace_file(const char *filepath,
                                  const struct panvk_replay_options *opts,
                                  struct panvk_replay_summary *out_summary);

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_VK_CAPTURE_REPLAY_H */
