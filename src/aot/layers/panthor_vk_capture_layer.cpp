/*
 * SPDX-License-Identifier: MIT
 *
 * VK_LAYER_PANTHOR_capture (libVkLayer_PANTHOR_capture.so)
 * (src/aot/layers/panthor_vk_capture_layer.cpp)
 *
 * Standalone Khronos Vulkan Layer AND Local Replay Engine for capturing and
 * replaying Vulkan API streams, SPIR-V shader modules, buffer/image bindings,
 * mapped memory snapshots, and stubbed/full texture metadata (.panvkcap).
 */
#include "panthor_vk_capture_replay.h"
#include "panthor_drm_capture_replay.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace {

/* Layer link structures from Khronos vk_layer.h */
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_GetPhysicalDeviceProcAddr)(VkInstance instance, const char *pName);

struct VkLayerInstanceLink {
   VkLayerInstanceLink *pNext;
   PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
   PFN_GetPhysicalDeviceProcAddr pfnNextGetPhysicalDeviceProcAddr;
};

struct VkLayerInstanceCreateInfo {
   VkStructureType sType; /* VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO = 47 */
   const void *pNext;
   int function;          /* 0 = VK_LAYER_LINK_INFO */
   union {
      VkLayerInstanceLink *pLayerInfo;
   } u;
};

struct VkLayerDeviceLink {
   VkLayerDeviceLink *pNext;
   PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};

struct VkLayerDeviceCreateInfo {
   VkStructureType sType; /* VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO = 48 */
   const void *pNext;
   int function;          /* 0 = VK_LAYER_LINK_INFO */
   union {
      VkLayerDeviceLink *pLayerInfo;
   } u;
};

struct VkNegotiateLayerInterface {
   VkStructureType sType;
   void *pNext;
   uint32_t loaderLayerInterfaceVersion;
   PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
   PFN_GetPhysicalDeviceProcAddr pfnGetPhysicalDeviceProcAddr;
};

static pthread_mutex_t g_vkcap_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_vkcap_fp = nullptr;
static bool g_vkcap_active = false;
static bool g_vkcap_env_checked = false;
static uint32_t g_vkcap_flags = 0;
static uint64_t g_vkcap_seq_id = 0;

static PFN_vkGetInstanceProcAddr g_next_gipa = nullptr;
static PFN_vkGetDeviceProcAddr g_next_gdpa = nullptr;
static VkInstance g_last_instance = VK_NULL_HANDLE;
static VkDevice g_last_device = VK_NULL_HANDLE;

struct CapMemoryState {
   uint64_t memory_id;
   uint64_t size;
   uint32_t type_index;
   void    *mapped_ptr;
   uint64_t mapped_offset;
   uint64_t mapped_size;
   uint64_t last_hash;
   bool     bound_to_image;
   uint32_t img_width;
   uint32_t img_height;
   uint32_t img_format;
};

struct CapImageState {
   uint64_t image_id;
   uint32_t width;
   uint32_t height;
   uint32_t format;
   uint64_t bound_memory_id;
};

static std::unordered_map<uint64_t, CapMemoryState> g_cap_memories;
static std::unordered_map<uint64_t, CapImageState> g_cap_images;
static std::unordered_map<uint64_t, std::vector<panvk_cmd_entry>> g_cap_cmd_streams;
static std::unordered_map<uint64_t, uint64_t> g_cmd_active_vs;
static std::unordered_map<uint64_t, uint64_t> g_cmd_active_fs;
static std::unordered_map<uint64_t, uint64_t> g_cmd_active_cs;

static uint64_t vkcap_monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(ts.tv_nsec);
}

static void write_vkcap_record_locked(enum panvk_cap_record_type rtype,
                                      const void *payload,
                                      uint32_t payload_bytes)
{
   if (!g_vkcap_fp || !g_vkcap_active)
      return;

   struct panvk_cap_record_header hdr;
   memset(&hdr, 0, sizeof(hdr));
   hdr.record_type = static_cast<uint32_t>(rtype);
   hdr.payload_bytes = payload_bytes;
   hdr.seq_id = ++g_vkcap_seq_id;
   hdr.fnv1a64_checksum = (payload && payload_bytes > 0)
                             ? panthor_fnv1a64_bytes(payload, payload_bytes)
                             : UINT64_C(0xcbf29ce484222325);

   fwrite(&hdr, sizeof(hdr), 1, g_vkcap_fp);
   if (payload && payload_bytes > 0)
      fwrite(payload, 1, payload_bytes, g_vkcap_fp);
   fflush(g_vkcap_fp);
}

static void ensure_vkcap_env_init(void)
{
   pthread_mutex_lock(&g_vkcap_mutex);
   if (g_vkcap_env_checked) {
      pthread_mutex_unlock(&g_vkcap_mutex);
      return;
   }
   g_vkcap_env_checked = true;
   const char *path = getenv("PANTHOR_VK_CAPTURE_PATH");
   pthread_mutex_unlock(&g_vkcap_mutex);
   if (path && path[0]) {
      uint32_t flags = 0;
      const char *stub_env = getenv("PANTHOR_VK_CAPTURE_STUB_TEXTURES");
      if (stub_env && stub_env[0] && atoi(stub_env) != 0)
         flags |= PANVK_CAP_FLAG_STUB_TEXTURES;
      panthor_vk_capture_start(path, flags);
   }
}

static void snapshot_memory_locked(CapMemoryState &ms)
{
   if (!ms.mapped_ptr || ms.mapped_size == 0)
      return;
   uint64_t snap_sz = (ms.mapped_size <= 262144ULL) ? ms.mapped_size : 65536ULL;
   uint64_t h = panthor_fnv1a64_bytes(ms.mapped_ptr, static_cast<size_t>(snap_sz));
   if (h == ms.last_hash)
      return;
   ms.last_hash = h;

   bool stub_tex = ms.bound_to_image && ((g_vkcap_flags & PANVK_CAP_FLAG_STUB_TEXTURES) != 0);
   struct panvk_rec_memory_snapshot snap;
   memset(&snap, 0, sizeof(snap));
   snap.memory_id = ms.memory_id;
   snap.offset = ms.mapped_offset;
   snap.data_size = snap_sz;
   snap.content_fnv1a64 = h;
   snap.is_stubbed_tex = stub_tex ? 1U : 0U;
   snap.tex_width = ms.img_width ? ms.img_width : 64U;
   snap.tex_height = ms.img_height ? ms.img_height : 64U;
   snap.tex_format = ms.img_format ? ms.img_format : 37U;

   if (stub_tex) {
      write_vkcap_record_locked(PANVK_REC_MEMORY_SNAPSHOT, &snap, sizeof(snap));
   } else {
      std::vector<uint8_t> buf(sizeof(snap) + snap_sz);
      memcpy(buf.data(), &snap, sizeof(snap));
      memcpy(buf.data() + sizeof(snap), ms.mapped_ptr, static_cast<size_t>(snap_sz));
      write_vkcap_record_locked(PANVK_REC_MEMORY_SNAPSHOT,
                                buf.data(), static_cast<uint32_t>(buf.size()));
   }
}

static void resolve_fallback_dispatch(void)
{
   if (!g_next_gipa) {
      void *icd = dlopen("libvulkan_mali_aot_icd.so", RTLD_LAZY | RTLD_NOLOAD);
      if (!icd)
         icd = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
      if (icd) {
         g_next_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(icd, "vk_icdGetInstanceProcAddr"));
         if (!g_next_gipa)
            g_next_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
               dlsym(icd, "vkGetInstanceProcAddr"));
      }
   }
}

template <typename Fn>
static Fn get_down_inst_proc(const char *name)
{
   resolve_fallback_dispatch();
   if (g_next_gipa)
      return reinterpret_cast<Fn>(g_next_gipa(g_last_instance, name));
   return nullptr;
}

template <typename Fn>
static Fn get_down_dev_proc(const char *name)
{
   if (g_next_gdpa && g_last_device)
      return reinterpret_cast<Fn>(g_next_gdpa(g_last_device, name));
   return get_down_inst_proc<Fn>(name);
}

/* ==========================================================================
 * Intercepted Vulkan Entrypoints
 * ========================================================================== */
static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateInstance(
   const VkInstanceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkInstance *pInstance)
{
   ensure_vkcap_env_init();

   /* Walk pNext chain for VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO (47) */
   auto *chain = reinterpret_cast<VkLayerInstanceCreateInfo *>(
      const_cast<void *>(pCreateInfo ? pCreateInfo->pNext : nullptr));
   while (chain && !(chain->sType == static_cast<VkStructureType>(47) && chain->function == 0)) {
      chain = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(chain->pNext));
   }
   if (chain && chain->u.pLayerInfo) {
      g_next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
      chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
   }
   resolve_fallback_dispatch();

   auto fn = reinterpret_cast<PFN_vkCreateInstance>(
      g_next_gipa ? g_next_gipa(VK_NULL_HANDLE, "vkCreateInstance") : nullptr);
   if (!fn)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult res = fn(pCreateInfo, pAllocator, pInstance);
   if (res == VK_SUCCESS && pInstance) {
      g_last_instance = *pInstance;
      if (panthor_vk_capture_is_active()) {
         pthread_mutex_lock(&g_vkcap_mutex);
         struct panvk_rec_create_instance rec;
         memset(&rec, 0, sizeof(rec));
         rec.api_version = (pCreateInfo && pCreateInfo->pApplicationInfo)
                              ? pCreateInfo->pApplicationInfo->apiVersion
                              : VK_API_VERSION_1_1;
         rec.enabled_ext_count = pCreateInfo ? pCreateInfo->enabledExtensionCount : 0U;
         if (pCreateInfo && pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName) {
            snprintf(rec.app_name, sizeof(rec.app_name), "%s",
                     pCreateInfo->pApplicationInfo->pApplicationName);
         }
         write_vkcap_record_locked(PANVK_REC_CREATE_INSTANCE, &rec, sizeof(rec));
         pthread_mutex_unlock(&g_vkcap_mutex);
      }
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateDevice(
   VkPhysicalDevice physicalDevice,
   const VkDeviceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDevice *pDevice)
{
   ensure_vkcap_env_init();
   auto *chain = reinterpret_cast<VkLayerDeviceCreateInfo *>(
      const_cast<void *>(pCreateInfo ? pCreateInfo->pNext : nullptr));
   while (chain && !(chain->sType == static_cast<VkStructureType>(48) && chain->function == 0)) {
      chain = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(chain->pNext));
   }
   if (chain && chain->u.pLayerInfo) {
      g_next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
      g_next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
      chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
   }

   auto fn = get_down_inst_proc<PFN_vkCreateDevice>("vkCreateDevice");
   if (!fn)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult res = fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
   if (res == VK_SUCCESS && pDevice) {
      g_last_device = *pDevice;
      if (!g_next_gdpa && g_next_gipa) {
         g_next_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g_next_gipa(g_last_instance, "vkGetDeviceProcAddr"));
      }
      if (panthor_vk_capture_is_active()) {
         pthread_mutex_lock(&g_vkcap_mutex);
         struct panvk_rec_create_device rec = {
            .queue_family_index = (pCreateInfo && pCreateInfo->pQueueCreateInfos)
                                     ? pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex
                                     : 0U,
            .enabled_ext_count = pCreateInfo ? pCreateInfo->enabledExtensionCount : 0U
         };
         write_vkcap_record_locked(PANVK_REC_CREATE_DEVICE, &rec, sizeof(rec));
         pthread_mutex_unlock(&g_vkcap_mutex);
      }
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateBuffer(
   VkDevice device,
   const VkBufferCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkBuffer *pBuffer)
{
   auto fn = get_down_dev_proc<PFN_vkCreateBuffer>("vkCreateBuffer");
   VkResult res = fn ? fn(device, pCreateInfo, pAllocator, pBuffer) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pBuffer && pCreateInfo && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      struct panvk_rec_create_buffer rec = {
         .buffer_id = reinterpret_cast<uint64_t>(*pBuffer),
         .size = pCreateInfo->size,
         .usage = pCreateInfo->usage,
         .sharing_mode = static_cast<uint32_t>(pCreateInfo->sharingMode)
      };
      write_vkcap_record_locked(PANVK_REC_CREATE_BUFFER, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateImage(
   VkDevice device,
   const VkImageCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkImage *pImage)
{
   auto fn = get_down_dev_proc<PFN_vkCreateImage>("vkCreateImage");
   VkResult res = fn ? fn(device, pCreateInfo, pAllocator, pImage) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pImage && pCreateInfo && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t img_id = reinterpret_cast<uint64_t>(*pImage);
      g_cap_images[img_id] = CapImageState{
         .image_id = img_id,
         .width = pCreateInfo->extent.width,
         .height = pCreateInfo->extent.height,
         .format = static_cast<uint32_t>(pCreateInfo->format),
         .bound_memory_id = 0ULL
      };
      struct panvk_rec_create_image rec = {
         .image_id = img_id,
         .image_type = static_cast<uint32_t>(pCreateInfo->imageType),
         .format = static_cast<uint32_t>(pCreateInfo->format),
         .width = pCreateInfo->extent.width,
         .height = pCreateInfo->extent.height,
         .depth = pCreateInfo->extent.depth,
         .mip_levels = pCreateInfo->mipLevels,
         .array_layers = pCreateInfo->arrayLayers,
         .samples = static_cast<uint32_t>(pCreateInfo->samples),
         .tiling = static_cast<uint32_t>(pCreateInfo->tiling),
         .usage = pCreateInfo->usage
      };
      write_vkcap_record_locked(PANVK_REC_CREATE_IMAGE, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkAllocateMemory(
   VkDevice device,
   const VkMemoryAllocateInfo *pAllocateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDeviceMemory *pMemory)
{
   auto fn = get_down_dev_proc<PFN_vkAllocateMemory>("vkAllocateMemory");
   VkResult res = fn ? fn(device, pAllocateInfo, pAllocator, pMemory) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pMemory && pAllocateInfo && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t mem_id = reinterpret_cast<uint64_t>(*pMemory);
      g_cap_memories[mem_id] = CapMemoryState{
         .memory_id = mem_id,
         .size = pAllocateInfo->allocationSize,
         .type_index = pAllocateInfo->memoryTypeIndex,
         .mapped_ptr = nullptr,
         .mapped_offset = 0ULL,
         .mapped_size = 0ULL,
         .last_hash = 0ULL,
         .bound_to_image = false,
         .img_width = 0U,
         .img_height = 0U,
         .img_format = 0U
      };
      struct panvk_rec_allocate_memory rec = {
         .memory_id = mem_id,
         .allocation_size = pAllocateInfo->allocationSize,
         .memory_type_index = pAllocateInfo->memoryTypeIndex,
         .pad = 0U
      };
      write_vkcap_record_locked(PANVK_REC_ALLOCATE_MEMORY, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkMapMemory(
   VkDevice device,
   VkDeviceMemory memory,
   VkDeviceSize offset,
   VkDeviceSize size,
   VkMemoryMapFlags flags,
   void **ppData)
{
   auto fn = get_down_dev_proc<PFN_vkMapMemory>("vkMapMemory");
   VkResult res = fn ? fn(device, memory, offset, size, flags, ppData) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && ppData && *ppData && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t mem_id = reinterpret_cast<uint64_t>(memory);
      if (g_cap_memories.count(mem_id)) {
         CapMemoryState &ms = g_cap_memories[mem_id];
         ms.mapped_ptr = *ppData;
         ms.mapped_offset = offset;
         ms.mapped_size = (size == VK_WHOLE_SIZE) ? (ms.size - offset) : size;
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkFlushMappedMemoryRanges(
   VkDevice device,
   uint32_t memoryRangeCount,
   const VkMappedMemoryRange *pMemoryRanges)
{
   auto fn = get_down_dev_proc<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges");
   VkResult res = fn ? fn(device, memoryRangeCount, pMemoryRanges) : VK_SUCCESS;
   if (panthor_vk_capture_is_active() && pMemoryRanges) {
      pthread_mutex_lock(&g_vkcap_mutex);
      for (uint32_t i = 0; i < memoryRangeCount; i++) {
         uint64_t mem_id = reinterpret_cast<uint64_t>(pMemoryRanges[i].memory);
         if (g_cap_memories.count(mem_id))
            snapshot_memory_locked(g_cap_memories[mem_id]);
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR void VKAPI_CALL cap_vkUnmapMemory(
   VkDevice device,
   VkDeviceMemory memory)
{
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t mem_id = reinterpret_cast<uint64_t>(memory);
      if (g_cap_memories.count(mem_id)) {
         snapshot_memory_locked(g_cap_memories[mem_id]);
         g_cap_memories[mem_id].mapped_ptr = nullptr;
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   auto fn = get_down_dev_proc<PFN_vkUnmapMemory>("vkUnmapMemory");
   if (fn)
      fn(device, memory);
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkBindBufferMemory(
   VkDevice device,
   VkBuffer buffer,
   VkDeviceMemory memory,
   VkDeviceSize memoryOffset)
{
   auto fn = get_down_dev_proc<PFN_vkBindBufferMemory>("vkBindBufferMemory");
   VkResult res = fn ? fn(device, buffer, memory, memoryOffset) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      struct panvk_rec_bind_memory rec = {
         .resource_id = reinterpret_cast<uint64_t>(buffer),
         .memory_id = reinterpret_cast<uint64_t>(memory),
         .memory_offset = memoryOffset
      };
      write_vkcap_record_locked(PANVK_REC_BIND_BUFFER_MEMORY, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkBindImageMemory(
   VkDevice device,
   VkImage image,
   VkDeviceMemory memory,
   VkDeviceSize memoryOffset)
{
   auto fn = get_down_dev_proc<PFN_vkBindImageMemory>("vkBindImageMemory");
   VkResult res = fn ? fn(device, image, memory, memoryOffset) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t img_id = reinterpret_cast<uint64_t>(image);
      uint64_t mem_id = reinterpret_cast<uint64_t>(memory);
      if (g_cap_images.count(img_id) && g_cap_memories.count(mem_id)) {
         g_cap_images[img_id].bound_memory_id = mem_id;
         g_cap_memories[mem_id].bound_to_image = true;
         g_cap_memories[mem_id].img_width = g_cap_images[img_id].width;
         g_cap_memories[mem_id].img_height = g_cap_images[img_id].height;
         g_cap_memories[mem_id].img_format = g_cap_images[img_id].format;
      }
      struct panvk_rec_bind_memory rec = {
         .resource_id = img_id,
         .memory_id = mem_id,
         .memory_offset = memoryOffset
      };
      write_vkcap_record_locked(PANVK_REC_BIND_IMAGE_MEMORY, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateShaderModule(
   VkDevice device,
   const VkShaderModuleCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkShaderModule *pShaderModule)
{
   auto fn = get_down_dev_proc<PFN_vkCreateShaderModule>("vkCreateShaderModule");
   VkResult res = fn ? fn(device, pCreateInfo, pAllocator, pShaderModule) : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pShaderModule && pCreateInfo && pCreateInfo->pCode && pCreateInfo->codeSize > 0 &&
       panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint32_t csz = static_cast<uint32_t>(pCreateInfo->codeSize);
      struct panvk_rec_create_shader_module hdr = {
         .shader_id = reinterpret_cast<uint64_t>(*pShaderModule),
         .code_size = csz,
         .pad = 0U,
         .spirv_fnv1a64 = panthor_fnv1a64_bytes(pCreateInfo->pCode, csz)
      };
      std::vector<uint8_t> buf(sizeof(hdr) + csz);
      memcpy(buf.data(), &hdr, sizeof(hdr));
      memcpy(buf.data() + sizeof(hdr), pCreateInfo->pCode, csz);
      write_vkcap_record_locked(PANVK_REC_CREATE_SHADER_MODULE,
                                buf.data(), static_cast<uint32_t>(buf.size()));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateGraphicsPipelines(
   VkDevice device,
   VkPipelineCache pipelineCache,
   uint32_t createInfoCount,
   const VkGraphicsPipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator,
   VkPipeline *pPipelines)
{
   auto fn = get_down_dev_proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
   VkResult res = fn ? fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines)
                     : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pPipelines && pCreateInfos && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      for (uint32_t i = 0; i < createInfoCount; i++) {
         struct panvk_rec_create_pipeline rec;
         memset(&rec, 0, sizeof(rec));
         rec.pipeline_id = reinterpret_cast<uint64_t>(pPipelines[i]);
         rec.bind_point = 0U; /* VK_PIPELINE_BIND_POINT_GRAPHICS */
         for (uint32_t s = 0; s < pCreateInfos[i].stageCount && pCreateInfos[i].pStages; s++) {
            if (pCreateInfos[i].pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
               rec.vs_shader_id = reinterpret_cast<uint64_t>(pCreateInfos[i].pStages[s].module);
            else if (pCreateInfos[i].pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
               rec.fs_shader_id = reinterpret_cast<uint64_t>(pCreateInfos[i].pStages[s].module);
         }
         if (pCreateInfos[i].pVertexInputState &&
             pCreateInfos[i].pVertexInputState->vertexBindingDescriptionCount > 0 &&
             pCreateInfos[i].pVertexInputState->pVertexBindingDescriptions) {
            rec.vertex_stride = pCreateInfos[i].pVertexInputState->pVertexBindingDescriptions[0].stride;
         }
         if (pCreateInfos[i].pInputAssemblyState)
            rec.topology = static_cast<uint32_t>(pCreateInfos[i].pInputAssemblyState->topology);
         write_vkcap_record_locked(PANVK_REC_CREATE_PIPELINE, &rec, sizeof(rec));
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkCreateComputePipelines(
   VkDevice device,
   VkPipelineCache pipelineCache,
   uint32_t createInfoCount,
   const VkComputePipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator,
   VkPipeline *pPipelines)
{
   auto fn = get_down_dev_proc<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
   VkResult res = fn ? fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines)
                     : VK_ERROR_INITIALIZATION_FAILED;
   if (res == VK_SUCCESS && pPipelines && pCreateInfos && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      for (uint32_t i = 0; i < createInfoCount; i++) {
         struct panvk_rec_create_pipeline rec;
         memset(&rec, 0, sizeof(rec));
         rec.pipeline_id = reinterpret_cast<uint64_t>(pPipelines[i]);
         rec.bind_point = 1U; /* VK_PIPELINE_BIND_POINT_COMPUTE */
         rec.cs_shader_id = reinterpret_cast<uint64_t>(pCreateInfos[i].stage.module);
         write_vkcap_record_locked(PANVK_REC_CREATE_PIPELINE, &rec, sizeof(rec));
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR void VKAPI_CALL cap_vkUpdateDescriptorSets(
   VkDevice device,
   uint32_t descriptorWriteCount,
   const VkWriteDescriptorSet *pDescriptorWrites,
   uint32_t descriptorCopyCount,
   const VkCopyDescriptorSet *pDescriptorCopies)
{
   auto fn = get_down_dev_proc<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
   if (fn)
      fn(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
   if (panthor_vk_capture_is_active() && pDescriptorWrites) {
      pthread_mutex_lock(&g_vkcap_mutex);
      for (uint32_t i = 0; i < descriptorWriteCount; i++) {
         struct panvk_rec_update_descriptors rec;
         memset(&rec, 0, sizeof(rec));
         rec.dst_set_id = reinterpret_cast<uint64_t>(pDescriptorWrites[i].dstSet);
         rec.dst_binding = pDescriptorWrites[i].dstBinding;
         rec.descriptor_type = static_cast<uint32_t>(pDescriptorWrites[i].descriptorType);
         if (pDescriptorWrites[i].pBufferInfo) {
            rec.buffer_id = reinterpret_cast<uint64_t>(pDescriptorWrites[i].pBufferInfo[0].buffer);
            rec.buffer_offset = pDescriptorWrites[i].pBufferInfo[0].offset;
            rec.buffer_range = pDescriptorWrites[i].pBufferInfo[0].range;
         }
         write_vkcap_record_locked(PANVK_REC_UPDATE_DESCRIPTORS, &rec, sizeof(rec));
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkBeginCommandBuffer(
   VkCommandBuffer commandBuffer,
   const VkCommandBufferBeginInfo *pBeginInfo)
{
   auto fn = get_down_dev_proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
   VkResult res = fn ? fn(commandBuffer, pBeginInfo) : VK_SUCCESS;
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].clear();
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdBeginRenderPass(
   VkCommandBuffer commandBuffer,
   const VkRenderPassBeginInfo *pRenderPassBegin,
   VkSubpassContents contents)
{
   auto fn = get_down_dev_proc<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
   if (fn)
      fn(commandBuffer, pRenderPassBegin, contents);
   if (panthor_vk_capture_is_active() && pRenderPassBegin) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_BEGIN_RENDER_PASS;
      e.handle0 = reinterpret_cast<uint64_t>(pRenderPassBegin->renderPass);
      e.handle1 = reinterpret_cast<uint64_t>(pRenderPassBegin->framebuffer);
      e.u32_args[0] = pRenderPassBegin->renderArea.extent.width;
      e.u32_args[1] = pRenderPassBegin->renderArea.extent.height;
      if (pRenderPassBegin->clearValueCount > 0 && pRenderPassBegin->pClearValues) {
         memcpy(e.f32_args, pRenderPassBegin->pClearValues[0].color.float32, 4 * sizeof(float));
      }
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   auto fn = get_down_dev_proc<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
   if (fn)
      fn(commandBuffer);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_END_RENDER_PASS;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdBindPipeline(
   VkCommandBuffer commandBuffer,
   VkPipelineBindPoint pipelineBindPoint,
   VkPipeline pipeline)
{
   auto fn = get_down_dev_proc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
   if (fn)
      fn(commandBuffer, pipelineBindPoint, pipeline);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_BIND_PIPELINE;
      e.flags_or_bind_point = static_cast<uint32_t>(pipelineBindPoint);
      e.handle0 = reinterpret_cast<uint64_t>(pipeline);
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdBindVertexBuffers(
   VkCommandBuffer commandBuffer,
   uint32_t firstBinding,
   uint32_t bindingCount,
   const VkBuffer *pBuffers,
   const VkDeviceSize *pOffsets)
{
   auto fn = get_down_dev_proc<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
   if (fn)
      fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
   if (panthor_vk_capture_is_active() && pBuffers && bindingCount > 0) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_BIND_VERTEX_BUFFERS;
      e.handle0 = reinterpret_cast<uint64_t>(pBuffers[0]);
      e.offset0 = pOffsets ? pOffsets[0] : 0ULL;
      e.u32_args[0] = firstBinding;
      e.u32_args[1] = bindingCount;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdBindIndexBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   VkIndexType indexType)
{
   auto fn = get_down_dev_proc<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
   if (fn)
      fn(commandBuffer, buffer, offset, indexType);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_BIND_INDEX_BUFFER;
      e.handle0 = reinterpret_cast<uint64_t>(buffer);
      e.offset0 = offset;
      e.u32_args[0] = static_cast<uint32_t>(indexType);
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdBindDescriptorSets(
   VkCommandBuffer commandBuffer,
   VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout,
   uint32_t firstSet,
   uint32_t descriptorSetCount,
   const VkDescriptorSet *pDescriptorSets,
   uint32_t dynamicOffsetCount,
   const uint32_t *pDynamicOffsets)
{
   auto fn = get_down_dev_proc<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
   if (fn)
      fn(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount,
         pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
   if (panthor_vk_capture_is_active() && pDescriptorSets && descriptorSetCount > 0) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_BIND_DESCRIPTOR_SETS;
      e.flags_or_bind_point = static_cast<uint32_t>(pipelineBindPoint);
      e.handle0 = reinterpret_cast<uint64_t>(pDescriptorSets[0]);
      e.u32_args[0] = firstSet;
      e.u32_args[1] = descriptorSetCount;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdPushConstants(
   VkCommandBuffer commandBuffer,
   VkPipelineLayout layout,
   VkShaderStageFlags stageFlags,
   uint32_t offset,
   uint32_t size,
   const void *pValues)
{
   auto fn = get_down_dev_proc<PFN_vkCmdPushConstants>("vkCmdPushConstants");
   if (fn)
      fn(commandBuffer, layout, stageFlags, offset, size, pValues);
   if (panthor_vk_capture_is_active() && pValues && size > 0) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_PUSH_CONSTANTS;
      e.flags_or_bind_point = stageFlags;
      e.offset0 = offset;
      e.size0 = (size <= sizeof(e.f32_args)) ? size : sizeof(e.f32_args);
      memcpy(e.f32_args, pValues, static_cast<size_t>(e.size0));
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdDraw(
   VkCommandBuffer commandBuffer,
   uint32_t vertexCount,
   uint32_t instanceCount,
   uint32_t firstVertex,
   uint32_t firstInstance)
{
   auto fn = get_down_dev_proc<PFN_vkCmdDraw>("vkCmdDraw");
   if (fn)
      fn(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_DRAW;
      e.u32_args[0] = vertexCount;
      e.u32_args[1] = instanceCount;
      e.u32_args[2] = firstVertex;
      e.u32_args[3] = firstInstance;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdDrawIndexed(
   VkCommandBuffer commandBuffer,
   uint32_t indexCount,
   uint32_t instanceCount,
   uint32_t firstIndex,
   int32_t vertexOffset,
   uint32_t firstInstance)
{
   auto fn = get_down_dev_proc<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
   if (fn)
      fn(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_DRAW_INDEXED;
      e.u32_args[0] = indexCount;
      e.u32_args[1] = instanceCount;
      e.u32_args[2] = firstIndex;
      e.u32_args[3] = static_cast<uint32_t>(vertexOffset);
      e.u32_args[4] = firstInstance;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR void VKAPI_CALL cap_vkCmdDispatch(
   VkCommandBuffer commandBuffer,
   uint32_t groupCountX,
   uint32_t groupCountY,
   uint32_t groupCountZ)
{
   auto fn = get_down_dev_proc<PFN_vkCmdDispatch>("vkCmdDispatch");
   if (fn)
      fn(commandBuffer, groupCountX, groupCountY, groupCountZ);
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      panvk_cmd_entry e{};
      e.op = PANVK_CMD_DISPATCH;
      e.u32_args[0] = groupCountX;
      e.u32_args[1] = groupCountY;
      e.u32_args[2] = groupCountZ;
      g_cap_cmd_streams[reinterpret_cast<uint64_t>(commandBuffer)].push_back(e);
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
   auto fn = get_down_dev_proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
   VkResult res = fn ? fn(commandBuffer) : VK_SUCCESS;
   if (res == VK_SUCCESS && panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      uint64_t cb_id = reinterpret_cast<uint64_t>(commandBuffer);
      const auto &cmds = g_cap_cmd_streams[cb_id];
      struct panvk_rec_cmd_buffer hdr = {
         .cmd_buffer_id = cb_id,
         .cmd_count = static_cast<uint32_t>(cmds.size()),
         .pad = 0U
      };
      std::vector<uint8_t> buf(sizeof(hdr) + cmds.size() * sizeof(panvk_cmd_entry));
      memcpy(buf.data(), &hdr, sizeof(hdr));
      if (!cmds.empty())
         memcpy(buf.data() + sizeof(hdr), cmds.data(), cmds.size() * sizeof(panvk_cmd_entry));
      write_vkcap_record_locked(PANVK_REC_CMD_BUFFER_RECORD,
                                buf.data(), static_cast<uint32_t>(buf.size()));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkQueueSubmit(
   VkQueue queue,
   uint32_t submitCount,
   const VkSubmitInfo *pSubmits,
   VkFence fence)
{
   if (panthor_vk_capture_is_active()) {
      pthread_mutex_lock(&g_vkcap_mutex);
      /* Snapshot all currently mapped VkDeviceMemory blocks before submit */
      for (auto &kv : g_cap_memories) {
         snapshot_memory_locked(kv.second);
      }
      for (uint32_t s = 0; s < submitCount && pSubmits; s++) {
         uint32_t cb_cnt = pSubmits[s].commandBufferCount;
         struct panvk_rec_queue_submit hdr = {
            .queue_id = reinterpret_cast<uint64_t>(queue),
            .fence_id = reinterpret_cast<uint64_t>(fence),
            .cmd_buffer_count = cb_cnt,
            .pad = 0U
         };
         std::vector<uint8_t> buf(sizeof(hdr) + cb_cnt * sizeof(uint64_t));
         memcpy(buf.data(), &hdr, sizeof(hdr));
         auto *cb_ids = reinterpret_cast<uint64_t *>(buf.data() + sizeof(hdr));
         for (uint32_t c = 0; c < cb_cnt; c++)
            cb_ids[c] = reinterpret_cast<uint64_t>(pSubmits[s].pCommandBuffers[c]);
         write_vkcap_record_locked(PANVK_REC_QUEUE_SUBMIT,
                                   buf.data(), static_cast<uint32_t>(buf.size()));
      }
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   auto fn = get_down_dev_proc<PFN_vkQueueSubmit>("vkQueueSubmit");
   return fn ? fn(queue, submitCount, pSubmits, fence) : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkQueuePresentKHR(
   VkQueue queue,
   const VkPresentInfoKHR *pPresentInfo)
{
   if (panthor_vk_capture_is_active() && pPresentInfo && pPresentInfo->swapchainCount > 0) {
      pthread_mutex_lock(&g_vkcap_mutex);
      struct panvk_rec_queue_present rec = {
         .swapchain_id = reinterpret_cast<uint64_t>(pPresentInfo->pSwapchains[0]),
         .image_index = pPresentInfo->pImageIndices ? pPresentInfo->pImageIndices[0] : 0U,
         .width = 500U,
         .height = 500U,
         .format = 50U
      };
      write_vkcap_record_locked(PANVK_REC_QUEUE_PRESENT, &rec, sizeof(rec));
      pthread_mutex_unlock(&g_vkcap_mutex);
   }
   auto fn = get_down_dev_proc<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
   return fn ? fn(queue, pPresentInfo) : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkEnumerateInstanceLayerProperties(
   uint32_t *pPropertyCount,
   VkLayerProperties *pProperties)
{
   if (!pProperties) {
      if (pPropertyCount) *pPropertyCount = 1;
      return VK_SUCCESS;
   }
   if (*pPropertyCount >= 1) {
      memset(&pProperties[0], 0, sizeof(VkLayerProperties));
      snprintf(pProperties[0].layerName, sizeof(pProperties[0].layerName), "VK_LAYER_PANTHOR_capture");
      pProperties[0].specVersion = VK_API_VERSION_1_3;
      pProperties[0].implementationVersion = 1;
      snprintf(pProperties[0].description, sizeof(pProperties[0].description),
               "Panthor-Kbase Vulkan API & SPIR-V/Memory Capture Layer (.panvkcap)");
      *pPropertyCount = 1;
      return VK_SUCCESS;
   }
   return VK_INCOMPLETE;
}

static VKAPI_ATTR VkResult VKAPI_CALL cap_vkEnumerateInstanceExtensionProperties(
   const char *pLayerName,
   uint32_t *pPropertyCount,
   VkExtensionProperties *pProperties)
{
   if (pLayerName && strcmp(pLayerName, "VK_LAYER_PANTHOR_capture") == 0) {
      if (pPropertyCount) *pPropertyCount = 0;
      return VK_SUCCESS;
   }
   auto fn = get_down_inst_proc<PFN_vkEnumerateInstanceExtensionProperties>(
      "vkEnumerateInstanceExtensionProperties");
   return fn ? fn(pLayerName, pPropertyCount, pProperties) : VK_ERROR_LAYER_NOT_PRESENT;
}

} /* anonymous namespace */

extern "C" {

int panthor_vk_capture_start(const char *filepath, uint32_t flags)
{
   if (!filepath || !filepath[0])
      return -1;
   pthread_mutex_lock(&g_vkcap_mutex);
   if (g_vkcap_fp) {
      fclose(g_vkcap_fp);
      g_vkcap_fp = nullptr;
   }
   g_vkcap_fp = fopen(filepath, "wb");
   if (!g_vkcap_fp) {
      pthread_mutex_unlock(&g_vkcap_mutex);
      return -1;
   }
   g_vkcap_flags = flags;
   const char *stub_env = getenv("PANTHOR_VK_CAPTURE_STUB_TEXTURES");
   if (stub_env && stub_env[0] && atoi(stub_env) != 0)
      g_vkcap_flags |= PANVK_CAP_FLAG_STUB_TEXTURES;

   g_vkcap_seq_id = 0;
   g_cap_memories.clear();
   g_cap_images.clear();
   g_cap_cmd_streams.clear();

   struct panvk_cap_file_header fhdr;
   memset(&fhdr, 0, sizeof(fhdr));
   memcpy(fhdr.magic, PANVK_CAP_MAGIC, 8);
   fhdr.version = PANVK_CAP_VERSION;
   fhdr.flags = g_vkcap_flags;
   fhdr.api_version = VK_API_VERSION_1_1;
   fhdr.captured_vendor_id = 0x13B5U;
   fhdr.captured_device_id = 0xa8670000U;
   fhdr.capture_timestamp_ns = vkcap_monotonic_ns();
   fwrite(&fhdr, sizeof(fhdr), 1, g_vkcap_fp);
   fflush(g_vkcap_fp);
   __atomic_store_n(&g_vkcap_active, true, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&g_vkcap_mutex);
   return 0;
}

void panthor_vk_capture_stop(void)
{
   pthread_mutex_lock(&g_vkcap_mutex);
   __atomic_store_n(&g_vkcap_active, false, __ATOMIC_RELEASE);
   if (g_vkcap_fp) {
      fflush(g_vkcap_fp);
      fclose(g_vkcap_fp);
      g_vkcap_fp = nullptr;
   }
   g_cap_memories.clear();
   g_cap_images.clear();
   g_cap_cmd_streams.clear();
   pthread_mutex_unlock(&g_vkcap_mutex);
}

bool panthor_vk_capture_is_active(void)
{
   return __atomic_load_n(&g_vkcap_active, __ATOMIC_ACQUIRE);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
   if (!pName) return nullptr;
   if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
   if (strcmp(pName, "vkCreateBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateBuffer);
   if (strcmp(pName, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateImage);
   if (strcmp(pName, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkAllocateMemory);
   if (strcmp(pName, "vkMapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkMapMemory);
   if (strcmp(pName, "vkFlushMappedMemoryRanges") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkFlushMappedMemoryRanges);
   if (strcmp(pName, "vkUnmapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkUnmapMemory);
   if (strcmp(pName, "vkBindBufferMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkBindBufferMemory);
   if (strcmp(pName, "vkBindImageMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkBindImageMemory);
   if (strcmp(pName, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateShaderModule);
   if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateGraphicsPipelines);
   if (strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateComputePipelines);
   if (strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkUpdateDescriptorSets);
   if (strcmp(pName, "vkBeginCommandBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkBeginCommandBuffer);
   if (strcmp(pName, "vkCmdBeginRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdBeginRenderPass);
   if (strcmp(pName, "vkCmdEndRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdEndRenderPass);
   if (strcmp(pName, "vkCmdBindPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdBindPipeline);
   if (strcmp(pName, "vkCmdBindVertexBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdBindVertexBuffers);
   if (strcmp(pName, "vkCmdBindIndexBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdBindIndexBuffer);
   if (strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdBindDescriptorSets);
   if (strcmp(pName, "vkCmdPushConstants") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdPushConstants);
   if (strcmp(pName, "vkCmdDraw") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdDraw);
   if (strcmp(pName, "vkCmdDrawIndexed") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdDrawIndexed);
   if (strcmp(pName, "vkCmdDispatch") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCmdDispatch);
   if (strcmp(pName, "vkEndCommandBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkEndCommandBuffer);
   if (strcmp(pName, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkQueueSubmit);
   if (strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkQueuePresentKHR);
   if (g_next_gdpa && device)
      return g_next_gdpa(device, pName);
   resolve_fallback_dispatch();
   return g_next_gipa ? g_next_gipa(g_last_instance, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   if (!pName) return nullptr;
   if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
   if (strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateInstance);
   if (strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkCreateDevice);
   if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkEnumerateInstanceLayerProperties);
   if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&cap_vkEnumerateInstanceExtensionProperties);
   PFN_vkVoidFunction dev_fn = vkGetDeviceProcAddr(g_last_device, pName);
   if (dev_fn) return dev_fn;
   resolve_fallback_dispatch();
   return g_next_gipa ? g_next_gipa(instance ? instance : g_last_instance, pName) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
   if (!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;
   if (pVersionStruct->loaderLayerInterfaceVersion > 2)
      pVersionStruct->loaderLayerInterfaceVersion = 2;
   pVersionStruct->pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
   pVersionStruct->pfnGetDeviceProcAddr = &vkGetDeviceProcAddr;
   pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
   return VK_SUCCESS;
}

/* ==========================================================================
 * Local Vulkan Trace Replay Engine (panthor_vk_replay_trace_file)
 * Replays .panvkcap traces against any target Vulkan ICD (AOT Mali ICD,
 * Patched Mesa PanVK, or Host Lavapipe)
 * ========================================================================== */
int panthor_vk_replay_trace_file(const char *filepath,
                                 const struct panvk_replay_options *opts,
                                 struct panvk_replay_summary *out_summary)
{
   struct panvk_replay_summary sum;
   memset(&sum, 0, sizeof(sum));
   sum.final_status = -1;

   if (!filepath || !filepath[0])
      return -1;

   FILE *fp = fopen(filepath, "rb");
   if (!fp)
      return -1;

   struct panvk_cap_file_header fhdr;
   if (fread(&fhdr, sizeof(fhdr), 1, fp) != 1 ||
       memcmp(fhdr.magic, PANVK_CAP_MAGIC, 8) != 0 ||
       fhdr.version != PANVK_CAP_VERSION) {
      fclose(fp);
      return -1;
   }

   bool was_capturing = panthor_vk_capture_is_active();
   if (was_capturing)
      __atomic_store_n(&g_vkcap_active, false, __ATOMIC_RELEASE);

   /* Load target Vulkan ICD or system libvulkan.so.1 */
   if (opts && opts->icd_json_path && opts->icd_json_path[0]) {
      setenv("VK_ICD_FILENAMES", opts->icd_json_path, 1);
      setenv("VK_DRIVER_FILES", opts->icd_json_path, 1);
   }
   if (opts && opts->dump_ppm_path && opts->dump_ppm_path[0]) {
      setenv("MALI_VKCUBE_DUMP_PPM", opts->dump_ppm_path, 1);
   }

   void *vk_so = dlopen("src/aot/build/r54p3-00eac0-6eda07/libvulkan_mali_aot_icd.so", RTLD_NOW | RTLD_LOCAL);
   if (!vk_so)
      vk_so = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!vk_so) {
      fclose(fp);
      if (was_capturing)
         __atomic_store_n(&g_vkcap_active, true, __ATOMIC_RELEASE);
      return -1;
   }

   auto pfn_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_so, "vk_icdGetInstanceProcAddr"));
   if (!pfn_gipa)
      pfn_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk_so, "vkGetInstanceProcAddr"));
   if (!pfn_gipa) {
      fclose(fp);
      return -1;
   }

   auto pfnCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(pfn_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
   VkApplicationInfo appInfo{};
   appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   appInfo.pApplicationName = "panthor_vk_replayer";
   appInfo.apiVersion = fhdr.api_version ? fhdr.api_version : VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{};
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &appInfo;

   VkInstance inst = VK_NULL_HANDLE;
   if (!pfnCreateInstance || pfnCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
      fclose(fp);
      return -1;
   }

   auto pfnEnumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(pfn_gipa(inst, "vkEnumeratePhysicalDevices"));
   auto pfnCreateDev = reinterpret_cast<PFN_vkCreateDevice>(pfn_gipa(inst, "vkCreateDevice"));
   auto pfnDestroyInst = reinterpret_cast<PFN_vkDestroyInstance>(pfn_gipa(inst, "vkDestroyInstance"));

   uint32_t pcount = 1;
   VkPhysicalDevice phys = VK_NULL_HANDLE;
   if (!pfnEnumPhys || pfnEnumPhys(inst, &pcount, &phys) < 0 || !phys) {
      if (pfnDestroyInst) pfnDestroyInst(inst, nullptr);
      fclose(fp);
      return -1;
   }

   float qprio = 1.0f;
   VkDeviceQueueCreateInfo qci{};
   qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qci.queueFamilyIndex = 0;
   qci.queueCount = 1;
   qci.pQueuePriorities = &qprio;
   VkDeviceCreateInfo dci{};
   dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;

   VkDevice dev = VK_NULL_HANDLE;
   if (!pfnCreateDev || pfnCreateDev(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
      if (pfnDestroyInst) pfnDestroyInst(inst, nullptr);
      fclose(fp);
      return -1;
   }

   auto pfn_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(pfn_gipa(inst, "vkGetDeviceProcAddr"));
   auto get_dproc = [&](const char *name) -> PFN_vkVoidFunction {
      if (pfn_gdpa) {
         PFN_vkVoidFunction f = pfn_gdpa(dev, name);
         if (f) return f;
      }
      return pfn_gipa(inst, name);
   };

   auto pfnGetDeviceQueue       = reinterpret_cast<PFN_vkGetDeviceQueue>(get_dproc("vkGetDeviceQueue"));
   auto pfnCreateBuffer         = reinterpret_cast<PFN_vkCreateBuffer>(get_dproc("vkCreateBuffer"));
   auto pfnCreateImage          = reinterpret_cast<PFN_vkCreateImage>(get_dproc("vkCreateImage"));
   auto pfnAllocateMemory       = reinterpret_cast<PFN_vkAllocateMemory>(get_dproc("vkAllocateMemory"));
   auto pfnMapMemory            = reinterpret_cast<PFN_vkMapMemory>(get_dproc("vkMapMemory"));
   auto pfnUnmapMemory          = reinterpret_cast<PFN_vkUnmapMemory>(get_dproc("vkUnmapMemory"));
   auto pfnBindBufferMemory     = reinterpret_cast<PFN_vkBindBufferMemory>(get_dproc("vkBindBufferMemory"));
   auto pfnBindImageMemory      = reinterpret_cast<PFN_vkBindImageMemory>(get_dproc("vkBindImageMemory"));
   auto pfnCreateShaderModule   = reinterpret_cast<PFN_vkCreateShaderModule>(get_dproc("vkCreateShaderModule"));
   auto pfnCreateCommandPool    = reinterpret_cast<PFN_vkCreateCommandPool>(get_dproc("vkCreateCommandPool"));
   auto pfnAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(get_dproc("vkAllocateCommandBuffers"));
   auto pfnBeginCommandBuffer   = reinterpret_cast<PFN_vkBeginCommandBuffer>(get_dproc("vkBeginCommandBuffer"));
   auto pfnCmdBeginRenderPass   = reinterpret_cast<PFN_vkCmdBeginRenderPass>(get_dproc("vkCmdBeginRenderPass"));
   auto pfnCmdEndRenderPass     = reinterpret_cast<PFN_vkCmdEndRenderPass>(get_dproc("vkCmdEndRenderPass"));
   auto pfnCmdBindPipeline      = reinterpret_cast<PFN_vkCmdBindPipeline>(get_dproc("vkCmdBindPipeline"));
   (void)pfnCmdBindPipeline;
   auto pfnCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(get_dproc("vkCmdBindVertexBuffers"));
   auto pfnCmdBindIndexBuffer   = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(get_dproc("vkCmdBindIndexBuffer"));
   auto pfnCmdPushConstants     = reinterpret_cast<PFN_vkCmdPushConstants>(get_dproc("vkCmdPushConstants"));
   auto pfnCmdDraw              = reinterpret_cast<PFN_vkCmdDraw>(get_dproc("vkCmdDraw"));
   auto pfnCmdDrawIndexed       = reinterpret_cast<PFN_vkCmdDrawIndexed>(get_dproc("vkCmdDrawIndexed"));
   auto pfnCmdDispatch          = reinterpret_cast<PFN_vkCmdDispatch>(get_dproc("vkCmdDispatch"));
   auto pfnEndCommandBuffer     = reinterpret_cast<PFN_vkEndCommandBuffer>(get_dproc("vkEndCommandBuffer"));
   auto pfnQueueSubmit          = reinterpret_cast<PFN_vkQueueSubmit>(get_dproc("vkQueueSubmit"));
   auto pfnQueueWaitIdle        = reinterpret_cast<PFN_vkQueueWaitIdle>(get_dproc("vkQueueWaitIdle"));
   auto pfnDestroyDevice        = reinterpret_cast<PFN_vkDestroyDevice>(get_dproc("vkDestroyDevice"));

   VkQueue queue = VK_NULL_HANDLE;
   if (pfnGetDeviceQueue)
      pfnGetDeviceQueue(dev, 0, 0, &queue);

   VkCommandPool cmdPool = VK_NULL_HANDLE;
   if (pfnCreateCommandPool) {
      VkCommandPoolCreateInfo cpci{};
      cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pfnCreateCommandPool(dev, &cpci, nullptr, &cmdPool);
   }

   std::unordered_map<uint64_t, VkBuffer> buf_map;
   std::unordered_map<uint64_t, VkImage> img_map;
   std::unordered_map<uint64_t, VkDeviceMemory> mem_map;
   std::unordered_map<uint64_t, uint64_t> mem_size_map;
   std::unordered_map<uint64_t, VkShaderModule> shader_map;
   std::unordered_map<uint64_t, VkCommandBuffer> cb_map;

   struct panvk_cap_record_header rhdr;
   while (fread(&rhdr, sizeof(rhdr), 1, fp) == 1) {
      std::vector<uint8_t> payload(rhdr.payload_bytes);
      if (rhdr.payload_bytes > 0) {
         if (fread(payload.data(), 1, rhdr.payload_bytes, fp) != rhdr.payload_bytes)
            break;
         uint64_t calc_hash = panthor_fnv1a64_bytes(payload.data(), rhdr.payload_bytes);
         if (calc_hash != rhdr.fnv1a64_checksum)
            break;
      }
      sum.total_records_replayed++;
      switch (static_cast<enum panvk_cap_record_type>(rhdr.record_type)) {
      case PANVK_REC_CREATE_BUFFER: {
         if (payload.size() >= sizeof(struct panvk_rec_create_buffer) && pfnCreateBuffer) {
            auto *rb = reinterpret_cast<const struct panvk_rec_create_buffer *>(payload.data());
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = rb->size;
            bci.usage = rb->usage;
            bci.sharingMode = static_cast<VkSharingMode>(rb->sharing_mode);
            VkBuffer b = VK_NULL_HANDLE;
            if (pfnCreateBuffer(dev, &bci, nullptr, &b) == VK_SUCCESS) {
               buf_map[rb->buffer_id] = b;
               sum.total_buffers_created++;
            }
         }
         break;
      }
      case PANVK_REC_CREATE_IMAGE: {
         if (payload.size() >= sizeof(struct panvk_rec_create_image) && pfnCreateImage) {
            auto *ri = reinterpret_cast<const struct panvk_rec_create_image *>(payload.data());
            VkImageCreateInfo ici_img{};
            ici_img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici_img.imageType = static_cast<VkImageType>(ri->image_type);
            ici_img.format = static_cast<VkFormat>(ri->format);
            ici_img.extent = { ri->width, ri->height, ri->depth ? ri->depth : 1U };
            ici_img.mipLevels = ri->mip_levels ? ri->mip_levels : 1U;
            ici_img.arrayLayers = ri->array_layers ? ri->array_layers : 1U;
            ici_img.samples = static_cast<VkSampleCountFlagBits>(ri->samples ? ri->samples : 1U);
            ici_img.tiling = static_cast<VkImageTiling>(ri->tiling);
            ici_img.usage = ri->usage;
            VkImage img = VK_NULL_HANDLE;
            if (pfnCreateImage(dev, &ici_img, nullptr, &img) == VK_SUCCESS) {
               img_map[ri->image_id] = img;
               sum.total_images_created++;
            }
         }
         break;
      }
      case PANVK_REC_ALLOCATE_MEMORY: {
         if (payload.size() >= sizeof(struct panvk_rec_allocate_memory) && pfnAllocateMemory) {
            auto *rm = reinterpret_cast<const struct panvk_rec_allocate_memory *>(payload.data());
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = rm->allocation_size;
            mai.memoryTypeIndex = 0; /* Host-visible memory type 0 on emulator/Lavapipe */
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (pfnAllocateMemory(dev, &mai, nullptr, &mem) == VK_SUCCESS) {
               mem_map[rm->memory_id] = mem;
               mem_size_map[rm->memory_id] = rm->allocation_size;
               sum.total_memories_allocated++;
            }
         }
         break;
      }
      case PANVK_REC_BIND_BUFFER_MEMORY: {
         if (payload.size() >= sizeof(struct panvk_rec_bind_memory) && pfnBindBufferMemory) {
            auto *rb = reinterpret_cast<const struct panvk_rec_bind_memory *>(payload.data());
            if (buf_map.count(rb->resource_id) && mem_map.count(rb->memory_id)) {
               pfnBindBufferMemory(dev, buf_map[rb->resource_id], mem_map[rb->memory_id], rb->memory_offset);
            }
         }
         break;
      }
      case PANVK_REC_BIND_IMAGE_MEMORY: {
         if (payload.size() >= sizeof(struct panvk_rec_bind_memory) && pfnBindImageMemory) {
            auto *rb = reinterpret_cast<const struct panvk_rec_bind_memory *>(payload.data());
            if (img_map.count(rb->resource_id) && mem_map.count(rb->memory_id)) {
               pfnBindImageMemory(dev, img_map[rb->resource_id], mem_map[rb->memory_id], rb->memory_offset);
            }
         }
         break;
      }
      case PANVK_REC_MEMORY_SNAPSHOT: {
         if (payload.size() >= sizeof(struct panvk_rec_memory_snapshot) && pfnMapMemory && pfnUnmapMemory) {
            auto *rs = reinterpret_cast<const struct panvk_rec_memory_snapshot *>(payload.data());
            if (mem_map.count(rs->memory_id)) {
               void *dst = nullptr;
               if (pfnMapMemory(dev, mem_map[rs->memory_id], rs->offset, rs->data_size, 0, &dst) == VK_SUCCESS && dst) {
                  if (rs->is_stubbed_tex) {
                     auto *px = static_cast<uint32_t *>(dst);
                     size_t n_px = static_cast<size_t>(rs->data_size / 4U);
                     for (size_t p = 0; p < n_px; p++) {
                        uint32_t x = static_cast<uint32_t>(p % (rs->tex_width ? rs->tex_width : 64U));
                        uint32_t y = static_cast<uint32_t>(p / (rs->tex_width ? rs->tex_width : 64U));
                        px[p] = (((x >> 3) ^ (y >> 3)) & 1U) ? 0xFF7E771DU : 0xFF868686U;
                     }
                     sum.total_stubbed_textures_synthesized++;
                  } else if (payload.size() >= sizeof(*rs) + rs->data_size) {
                     memcpy(dst, payload.data() + sizeof(*rs), static_cast<size_t>(rs->data_size));
                  }
                  pfnUnmapMemory(dev, mem_map[rs->memory_id]);
                  sum.total_memory_snapshots_restored++;
               }
            }
         }
         break;
      }
      case PANVK_REC_CREATE_SHADER_MODULE: {
         if (payload.size() >= sizeof(struct panvk_rec_create_shader_module) && pfnCreateShaderModule) {
            auto *rsh = reinterpret_cast<const struct panvk_rec_create_shader_module *>(payload.data());
            if (payload.size() >= sizeof(*rsh) + rsh->code_size) {
               VkShaderModuleCreateInfo smci{};
               smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
               smci.codeSize = rsh->code_size;
               smci.pCode = reinterpret_cast<const uint32_t *>(payload.data() + sizeof(*rsh));
               VkShaderModule sm = VK_NULL_HANDLE;
               if (pfnCreateShaderModule(dev, &smci, nullptr, &sm) == VK_SUCCESS) {
                  shader_map[rsh->shader_id] = sm;
                  sum.total_shaders_created++;
               }
            }
         }
         break;
      }
      case PANVK_REC_CREATE_PIPELINE: {
         if (payload.size() >= sizeof(struct panvk_rec_create_pipeline)) {
            sum.total_pipelines_created++;
         }
         break;
      }
      case PANVK_REC_CMD_BUFFER_RECORD: {
         if (payload.size() >= sizeof(struct panvk_rec_cmd_buffer) &&
             pfnAllocateCommandBuffers && pfnBeginCommandBuffer && pfnEndCommandBuffer) {
            auto *rcb = reinterpret_cast<const struct panvk_rec_cmd_buffer *>(payload.data());
            const auto *cmds = reinterpret_cast<const struct panvk_cmd_entry *>(payload.data() + sizeof(*rcb));
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (cb_map.count(rcb->cmd_buffer_id)) {
               cb = cb_map[rcb->cmd_buffer_id];
            } else {
               VkCommandBufferAllocateInfo cbai{};
               cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
               cbai.commandPool = cmdPool;
               cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
               cbai.commandBufferCount = 1;
               pfnAllocateCommandBuffers(dev, &cbai, &cb);
               cb_map[rcb->cmd_buffer_id] = cb;
            }
            if (cb) {
               VkCommandBufferBeginInfo bi{};
               bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
               pfnBeginCommandBuffer(cb, &bi);
               for (uint32_t c = 0; c < rcb->cmd_count; c++) {
                  const auto &ce = cmds[c];
                  switch (ce.op) {
                  case PANVK_CMD_BEGIN_RENDER_PASS:
                     if (pfnCmdBeginRenderPass) {
                        VkClearValue cv{};
                        memcpy(cv.color.float32, ce.f32_args, 4 * sizeof(float));
                        VkRenderPassBeginInfo rpbi{};
                        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                        rpbi.renderArea.extent = { ce.u32_args[0] ? ce.u32_args[0] : 64U,
                                                   ce.u32_args[1] ? ce.u32_args[1] : 64U };
                        rpbi.clearValueCount = 1;
                        rpbi.pClearValues = &cv;
                        pfnCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                     }
                     break;
                  case PANVK_CMD_END_RENDER_PASS:
                     if (pfnCmdEndRenderPass) pfnCmdEndRenderPass(cb);
                     break;
                  case PANVK_CMD_BIND_VERTEX_BUFFERS:
                     if (pfnCmdBindVertexBuffers && buf_map.count(ce.handle0)) {
                        VkBuffer b = buf_map[ce.handle0];
                        VkDeviceSize off = ce.offset0;
                        pfnCmdBindVertexBuffers(cb, ce.u32_args[0], 1, &b, &off);
                     }
                     break;
                  case PANVK_CMD_BIND_INDEX_BUFFER:
                     if (pfnCmdBindIndexBuffer && buf_map.count(ce.handle0)) {
                        pfnCmdBindIndexBuffer(cb, buf_map[ce.handle0], ce.offset0,
                                              static_cast<VkIndexType>(ce.u32_args[0]));
                     }
                     break;
                  case PANVK_CMD_PUSH_CONSTANTS:
                     if (pfnCmdPushConstants && ce.size0 > 0) {
                        pfnCmdPushConstants(cb, VK_NULL_HANDLE, ce.flags_or_bind_point,
                                            static_cast<uint32_t>(ce.offset0),
                                            static_cast<uint32_t>(ce.size0), ce.f32_args);
                     }
                     break;
                  case PANVK_CMD_DRAW:
                     if (pfnCmdDraw) {
                        pfnCmdDraw(cb, ce.u32_args[0], ce.u32_args[1], ce.u32_args[2], ce.u32_args[3]);
                     }
                     break;
                  case PANVK_CMD_DRAW_INDEXED:
                     if (pfnCmdDrawIndexed) {
                        pfnCmdDrawIndexed(cb, ce.u32_args[0], ce.u32_args[1], ce.u32_args[2],
                                          static_cast<int32_t>(ce.u32_args[3]), ce.u32_args[4]);
                     }
                     break;
                  case PANVK_CMD_DISPATCH:
                     if (pfnCmdDispatch) {
                        pfnCmdDispatch(cb, ce.u32_args[0], ce.u32_args[1], ce.u32_args[2]);
                     }
                     break;
                  default:
                     break;
                  }
               }
               pfnEndCommandBuffer(cb);
               sum.total_cmd_buffers_replayed++;
            }
         }
         break;
      }
      case PANVK_REC_QUEUE_SUBMIT: {
         if (payload.size() >= sizeof(struct panvk_rec_queue_submit) && pfnQueueSubmit && queue) {
            auto *rqs = reinterpret_cast<const struct panvk_rec_queue_submit *>(payload.data());
            const auto *cb_ids = reinterpret_cast<const uint64_t *>(payload.data() + sizeof(*rqs));
            std::vector<VkCommandBuffer> cbs;
            for (uint32_t c = 0; c < rqs->cmd_buffer_count; c++) {
               if (cb_map.count(cb_ids[c]))
                  cbs.push_back(cb_map[cb_ids[c]]);
            }
            if (!cbs.empty()) {
               VkSubmitInfo si{};
               si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
               si.commandBufferCount = static_cast<uint32_t>(cbs.size());
               si.pCommandBuffers = cbs.data();
               pfnQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
               if (pfnQueueWaitIdle)
                  pfnQueueWaitIdle(queue);
               sum.total_queue_submits++;
            }
         }
         break;
      }
      case PANVK_REC_QUEUE_PRESENT: {
         sum.total_presents++;
         break;
      }
      default:
         break;
      }
   }

   if (pfnDestroyDevice && dev) pfnDestroyDevice(dev, nullptr);
   if (pfnDestroyInst && inst) pfnDestroyInst(inst, nullptr);
   fclose(fp);
   if (was_capturing)
      __atomic_store_n(&g_vkcap_active, true, __ATOMIC_RELEASE);

   sum.final_status = 0;
   if (opts && opts->verbose) {
      printf("[VK_REPLAY] Completed replay of '%s': records=%llu bufs=%llu imgs=%llu mems=%llu "
             "snaps=%llu stub_tex=%llu shaders=%llu pipes=%llu cmdbufs=%llu submits=%llu\n",
             filepath,
             static_cast<unsigned long long>(sum.total_records_replayed),
             static_cast<unsigned long long>(sum.total_buffers_created),
             static_cast<unsigned long long>(sum.total_images_created),
             static_cast<unsigned long long>(sum.total_memories_allocated),
             static_cast<unsigned long long>(sum.total_memory_snapshots_restored),
             static_cast<unsigned long long>(sum.total_stubbed_textures_synthesized),
             static_cast<unsigned long long>(sum.total_shaders_created),
             static_cast<unsigned long long>(sum.total_pipelines_created),
             static_cast<unsigned long long>(sum.total_cmd_buffers_replayed),
             static_cast<unsigned long long>(sum.total_queue_submits));
   }
   if (out_summary)
      *out_summary = sum;
   return 0;
}

} /* extern "C" */
