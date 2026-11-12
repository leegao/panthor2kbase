/*
 * SPDX-License-Identifier: MIT
 *
 * PanVK Android & Linux Vulkan ICD Driver Front-End (Patched & Unpatched Modes)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "xf86drm.h"
#include "panthor_drm.h"
#include "panthor_kbase_adapter.h"
#include "vulkan/vulkan.h"
#include "vulkan/vk_icd.h"

#ifndef PANTHOR_USER_VA_AUTO
#define PANTHOR_USER_VA_AUTO 0ULL
#endif
#ifndef PANTHOR_KBASE_PATCHED_KMOD
#define PANTHOR_KBASE_PATCHED_KMOD 0
#endif

#if PANTHOR_KBASE_PATCHED_KMOD
#define KMOD_OPEN(path, flags)                 panthor_kbase_adapter_open(path, flags)
#define KMOD_CLOSE(fd)                         panthor_kbase_adapter_close(fd)
#define KMOD_IOCTL(fd, req, arg)               panthor_kbase_adapter_ioctl(fd, req, arg)
#define KMOD_MMAP(addr, len, prot, fl, fd, of) panthor_kbase_adapter_mmap(addr, len, prot, fl, fd, of)
#define KMOD_MUNMAP(addr, len)                 panthor_kbase_adapter_munmap(addr, len)
#else
#define KMOD_OPEN(path, flags)                 open(path, flags)
#define KMOD_CLOSE(fd)                         close(fd)
#define KMOD_IOCTL(fd, req, arg)               drmIoctl(fd, req, arg)
#define KMOD_MMAP(addr, len, prot, fl, fd, of) mmap(addr, len, prot, fl, fd, of)
#define KMOD_MUNMAP(addr, len)                 munmap(addr, len)
#endif

struct panvk_icd_instance {
   VK_LOADER_DATA loader_magic;
   int drm_fd;
   uint32_t vm_id;
   struct drm_panthor_gpu_info gpu_info;
   struct drm_panthor_csif_info csif_info;
   struct drm_panthor_mmu_info mmu_info;
   void *flush_id_mmio;
   bool physical_device_enumerated;
};

struct panvk_icd_device {
   VK_LOADER_DATA loader_magic;
   struct panvk_icd_instance *instance;
   uint32_t group_handle;
   uint32_t heap_handle;
   uint64_t heap_ctx_gpu_va;
   uint32_t syncobj_handle;
   uint32_t cs_bo_handle;
   uint64_t cs_bo_gpu_va;
   uint64_t *cs_bo_cpu;
   uint32_t subq_bo_handle;
   uint64_t subq_bo_gpu_va;
   uint64_t *subq_bo_cpu;
   uint32_t ringbuf_bo_handle;
   uint64_t ringbuf_gpu_va;
   void *ringbuf_cpu;
   uint32_t frame_index;
};

struct panvk_icd_memory {
   uint32_t bo_handle;
   uint64_t size;
   uint64_t gpu_va;
   void *cpu_ptr;
};

struct panvk_icd_image {
   uint64_t magic;
   uint32_t width;
   uint32_t height;
   uint32_t format;
   uint32_t pad;
   uint64_t bound_gpu_va;
   uint32_t *pixels;
   size_t byte_size;
};

struct panvk_icd_swapchain {
   uint32_t width;
   uint32_t height;
   uint32_t image_count;
   struct panvk_icd_image images[4];
};

struct panvk_icd_cmd_buffer {
   VK_LOADER_DATA loader_magic;
   struct panvk_icd_device *dev;
   uint32_t recorded_dwords;
   bool is_compute;
};

static struct panvk_icd_instance g_default_instance = {0};
static struct panvk_icd_device   g_default_device   = {0};
static struct panvk_icd_cmd_buffer g_default_cmdbuf = {0};
static uint64_t g_next_dummy_handle = 0x5000ULL;

static VkResult panvk_probe_and_init_kmod(struct panvk_icd_instance *inst)
{
   if (inst->drm_fd > 0 && inst->physical_device_enumerated)
      return VK_SUCCESS;

   const char *render_path = "/dev/dri/renderD128";
#if !PANTHOR_KBASE_PATCHED_KMOD
   drmDevicePtr devs[8] = {0};
   int num_devs = drmGetDevices2(0, devs, 8);
   if (num_devs > 0 && devs[0] && (devs[0]->available_nodes & (1 << DRM_NODE_RENDER))) {
      struct stat st;
      if (stat(devs[0]->nodes[DRM_NODE_RENDER], &st) == 0)
         render_path = strdup(devs[0]->nodes[DRM_NODE_RENDER]);
      drmFreeDevices(devs, num_devs);
   }
#endif

   int fd = KMOD_OPEN(render_path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   drmVersionPtr ver = drmGetVersion(fd);
   if (!ver || !ver->name ||
       (strcmp(ver->name, "panthor") != 0 && strcmp(ver->name, "panfrost") != 0)) {
      if (ver)
         drmFreeVersion(ver);
      KMOD_CLOSE(fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   drmFreeVersion(ver);

   uint64_t timeline_cap = 0;
   if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline_cap) != 0 || !timeline_cap) {
      KMOD_CLOSE(fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct drm_panthor_dev_query q_gpu = {
      .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO,
      .size = sizeof(inst->gpu_info),
      .pointer = (uintptr_t)&inst->gpu_info,
   };
   struct drm_panthor_dev_query q_csif = {
      .type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
      .size = sizeof(inst->csif_info),
      .pointer = (uintptr_t)&inst->csif_info,
   };
   struct drm_panthor_dev_query q_mmu = {
      .type = DRM_PANTHOR_DEV_QUERY_MMU_INFO,
      .size = sizeof(inst->mmu_info),
      .pointer = (uintptr_t)&inst->mmu_info,
   };
   if (KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q_gpu) != 0 ||
       KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q_csif) != 0 ||
       KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q_mmu) != 0) {
      KMOD_CLOSE(fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   struct drm_panthor_set_user_mmio_offset mmio = {
      .offset = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT,
   };
   KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &mmio);

   inst->flush_id_mmio = KMOD_MMAP(NULL, 4096, PROT_READ, MAP_SHARED,
                                   fd, (off_t)DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);

   struct drm_panthor_vm_create vmc = { .user_va_range = (1ULL << 39) };
   if (KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vmc) != 0) {
      KMOD_CLOSE(fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   inst->drm_fd = fd;
   inst->vm_id = vmc.id;
   inst->physical_device_enumerated = true;
   return VK_SUCCESS;
}

static void panvk_shade_swapchain_image(struct panvk_icd_image *img, uint32_t frame_idx)
{
   if (!img || !img->pixels)
      return;
   uint32_t w = img->width ? img->width : 64;
   uint32_t h = img->height ? img->height : 64;
   float angle = (25.0f + 18.0f * (float)frame_idx) * 0.0174532925f;
   float ca = cosf(angle), sa = sinf(angle);
   for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
         float nx = ((float)x / (float)w) * 2.0f - 1.0f;
         float ny = ((float)y / (float)h) * 2.0f - 1.0f;
         float rx = nx * ca - ny * sa;
         float ry = nx * sa + ny * ca;
         if (fabsf(rx) < 0.62f && fabsf(ry) < 0.62f) {
            uint8_t r = (uint8_t)((rx + 0.62f) * 200.0f) ^ (uint8_t)(frame_idx * 37U);
            uint8_t g = (uint8_t)((ry + 0.62f) * 200.0f) ^ (uint8_t)(frame_idx * 73U);
            uint8_t b = (uint8_t)(((rx + ry + 1.24f) * 100.0f) + frame_idx * 19U);
            img->pixels[y * w + x] = 0xFF000000U | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
         } else {
            img->pixels[y * w + x] = 0xFF181410U;
         }
      }
   }
}

static struct panvk_icd_swapchain *g_active_swapchain = NULL;

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties)
{
   (void)pLayerName;
   static const VkExtensionProperties exts[] = {
      { "VK_KHR_surface", 25 },
      { "VK_EXT_headless_surface", 1 },
      { "VK_KHR_xcb_surface", 6 },
      { "VK_KHR_xlib_surface", 6 },
      { "VK_KHR_wayland_surface", 6 },
      { "VK_KHR_get_physical_device_properties2", 2 },
   };
   uint32_t total = sizeof(exts) / sizeof(exts[0]);
   if (!pProperties) {
      *pPropertyCount = total;
      return VK_SUCCESS;
   }
   uint32_t copy_cnt = *pPropertyCount < total ? *pPropertyCount : total;
   memcpy(pProperties, exts, copy_cnt * sizeof(VkExtensionProperties));
   *pPropertyCount = copy_cnt;
   return copy_cnt < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                     const char *pLayerName,
                                     uint32_t *pPropertyCount,
                                     VkExtensionProperties *pProperties)
{
   (void)physicalDevice;
   (void)pLayerName;
   static const VkExtensionProperties exts[] = {
      { "VK_KHR_swapchain", 70 },
      { "VK_KHR_maintenance1", 2 },
      { "VK_KHR_dynamic_rendering", 1 },
   };
   uint32_t total = sizeof(exts) / sizeof(exts[0]);
   if (!pProperties) {
      *pPropertyCount = total;
      return VK_SUCCESS;
   }
   uint32_t copy_cnt = *pPropertyCount < total ? *pPropertyCount : total;
   memcpy(pProperties, exts, copy_cnt * sizeof(VkExtensionProperties));
   *pPropertyCount = copy_cnt;
   return copy_cnt < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkInstance *pInstance)
{
   (void)pCreateInfo;
   (void)pAllocator;
   if (!pInstance)
      return VK_ERROR_INITIALIZATION_FAILED;
   memset(&g_default_instance, 0, sizeof(g_default_instance));
   set_loader_magic_value(&g_default_instance);
   *pInstance = (VkInstance)&g_default_instance;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
   (void)pAllocator;
   struct panvk_icd_instance *inst = (struct panvk_icd_instance *)instance;
   if (inst && inst->drm_fd > 0) {
      if (inst->flush_id_mmio && inst->flush_id_mmio != MAP_FAILED)
         KMOD_MUNMAP(inst->flush_id_mmio, 4096);
      struct drm_panthor_vm_destroy vmd = { .id = inst->vm_id };
      KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vmd);
      KMOD_CLOSE(inst->drm_fd);
      inst->drm_fd = -1;
      inst->physical_device_enumerated = false;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance,
                           uint32_t *pPhysicalDeviceCount,
                           VkPhysicalDevice *pPhysicalDevices)
{
   struct panvk_icd_instance *inst =
      instance ? (struct panvk_icd_instance *)instance : &g_default_instance;
   VkResult res = panvk_probe_and_init_kmod(inst);
   if (res != VK_SUCCESS)
      return res;

   if (!pPhysicalDevices) {
      *pPhysicalDeviceCount = 1;
      return VK_SUCCESS;
   }
   if (*pPhysicalDeviceCount == 0)
      return VK_INCOMPLETE;

   *pPhysicalDeviceCount = 1;
   pPhysicalDevices[0] = (VkPhysicalDevice)inst;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceProperties *pProperties)
{
   struct panvk_icd_instance *inst = (struct panvk_icd_instance *)physicalDevice;
   if (!pProperties)
      return;
   memset(pProperties, 0, sizeof(*pProperties));
   pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 296);
   pProperties->driverVersion = 1;
   pProperties->vendorID = 0x13B5;
   pProperties->deviceID = inst ? inst->gpu_info.gpu_id : 0xa8670000;
   pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
#if PANTHOR_KBASE_PATCHED_KMOD
   snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
            "Mali-G715 (PanVK 26.3.0 Patched-Kmod Adapter 0x%x)", pProperties->deviceID);
#else
   snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
            "Mali-G715 (PanVK 26.3.0 Unpatched + Runtime-Hook 0x%x)", pProperties->deviceID);
#endif
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                            VkPhysicalDeviceFeatures *pFeatures)
{
   (void)physicalDevice;
   if (pFeatures)
      memset(pFeatures, 0, sizeof(*pFeatures));
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   (void)physicalDevice;
   if (!pMemoryProperties)
      return;
   memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
   pMemoryProperties->memoryTypeCount = 1;
   pMemoryProperties->memoryTypes[0].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   pMemoryProperties->memoryHeapCount = 1;
   pMemoryProperties->memoryHeaps[0].size = 2ULL * 1024 * 1024 * 1024;
   pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                         uint32_t *pQueueFamilyPropertyCount,
                                         VkQueueFamilyProperties *pQueueFamilyProperties)
{
   (void)physicalDevice;
   if (!pQueueFamilyProperties) {
      *pQueueFamilyPropertyCount = 1;
      return;
   }
   if (*pQueueFamilyPropertyCount > 0) {
      memset(&pQueueFamilyProperties[0], 0, sizeof(VkQueueFamilyProperties));
      pQueueFamilyProperties[0].queueFlags =
         VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
      pQueueFamilyProperties[0].queueCount = 1;
      pQueueFamilyProperties[0].timestampValidBits = 64;
      *pQueueFamilyPropertyCount = 1;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkDevice *pDevice)
{
   (void)pCreateInfo;
   (void)pAllocator;
   struct panvk_icd_instance *inst =
      physicalDevice ? (struct panvk_icd_instance *)physicalDevice : &g_default_instance;
   VkResult res = panvk_probe_and_init_kmod(inst);
   if (res != VK_SUCCESS)
      return res;

   memset(&g_default_device, 0, sizeof(g_default_device));
   set_loader_magic_value(&g_default_device);
   g_default_device.instance = inst;

   int dev_fd = panthor_kbase_adapter_dup(inst->drm_fd);
   if (dev_fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   panthor_kbase_adapter_hint_bo_flags(1U /* PAN_KMOD_BO_FLAG_EXECUTABLE */);
   struct drm_panthor_bo_create exec_bo = { .size = 65536, .flags = 0 };
   if (KMOD_IOCTL(dev_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &exec_bo) != 0) {
      panthor_kbase_adapter_close(dev_fd);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   struct drm_panthor_vm_bind_op bind_exec = {
      .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP | DRM_PANTHOR_VM_BIND_OP_MAP_READONLY | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC,
      .bo_handle = exec_bo.handle,
      .va = PANTHOR_USER_VA_AUTO,
      .size = 65536,
   };
   bind_exec.flags &= ~DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC;
   struct drm_panthor_vm_bind vb_exec = {
      .vm_id = inst->vm_id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bind_exec),
   };
   KMOD_IOCTL(dev_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb_exec);
   drmCloseBufferHandle(dev_fd, exec_bo.handle);
   panthor_kbase_adapter_close(dev_fd);

   panthor_kbase_adapter_hint_bo_flags(1U << 5 /* PAN_KMOD_BO_FLAG_WB_MMAP */);
   struct drm_panthor_bo_create cs_bo = { .size = 16384, .flags = 0 };
   if (KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &cs_bo) != 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct drm_panthor_bo_mmap_offset cs_mo = { .handle = cs_bo.handle };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &cs_mo);
   g_default_device.cs_bo_handle = cs_bo.handle;
   g_default_device.cs_bo_cpu = KMOD_MMAP(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED,
                                          inst->drm_fd, (off_t)cs_mo.offset);

   struct drm_panthor_vm_bind_op bind_cs = {
      .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
      .bo_handle = cs_bo.handle,
      .va = PANTHOR_USER_VA_AUTO,
      .size = 16384,
   };
   struct drm_panthor_vm_bind vb_cs = {
      .vm_id = inst->vm_id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bind_cs),
   };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb_cs);
   g_default_device.cs_bo_gpu_va = bind_cs.va;

   /* Allocate PanVK 2x aliased render_desc_ringbuf BO (256 KiB -> 512 KiB alias) */
   struct drm_panthor_bo_create rb_bo = { .size = 262144, .flags = DRM_PANTHOR_BO_WB_MMAP };
   if (KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &rb_bo) == 0) {
      g_default_device.ringbuf_bo_handle = rb_bo.handle;
      struct drm_panthor_vm_bind_op rb_ops[2] = {
         {
            .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC,
            .bo_handle = rb_bo.handle,
            .bo_offset = 0,
            .va = PANTHOR_USER_VA_AUTO,
            .size = 262144,
         },
         {
            .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC,
            .bo_handle = rb_bo.handle,
            .bo_offset = 0,
            .va = PANTHOR_USER_VA_AUTO,
            .size = 262144,
         },
      };
      struct drm_panthor_vm_bind vb_rb = {
         .vm_id = inst->vm_id,
         .flags = 0,
         .ops = DRM_PANTHOR_OBJ_ARRAY(2, rb_ops),
      };
      if (KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb_rb) == 0)
         g_default_device.ringbuf_gpu_va = rb_ops[0].va;
   }

   struct drm_panthor_bo_create sq_bo = { .size = 4096, .flags = DRM_PANTHOR_BO_WB_MMAP };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &sq_bo);
   struct drm_panthor_bo_mmap_offset sq_mo = { .handle = sq_bo.handle };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &sq_mo);
   g_default_device.subq_bo_handle = sq_bo.handle;
   g_default_device.subq_bo_cpu = KMOD_MMAP(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                                            inst->drm_fd, (off_t)sq_mo.offset);
   struct drm_panthor_vm_bind_op bind_sq = {
      .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
      .bo_handle = sq_bo.handle,
      .va = PANTHOR_USER_VA_AUTO,
      .size = 4096,
   };
   struct drm_panthor_vm_bind vb_sq = {
      .vm_id = inst->vm_id,
      .flags = 0,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bind_sq),
   };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb_sq);
   g_default_device.subq_bo_gpu_va = bind_sq.va;
   if (g_default_device.subq_bo_cpu && g_default_device.subq_bo_cpu != MAP_FAILED) {
      g_default_device.subq_bo_cpu[4] = g_default_device.ringbuf_gpu_va ? g_default_device.ringbuf_gpu_va : (g_default_device.subq_bo_gpu_va + 512ULL);
      g_default_device.subq_bo_cpu[6] = g_default_device.subq_bo_gpu_va + 256ULL;
   }

   struct drm_panthor_tiler_heap_create hc = {
      .vm_id = inst->vm_id,
      .initial_chunk_count = 4,
      .chunk_size = 2 * 1024 * 1024,
      .max_chunks = 64,
      .target_in_flight = 65535,
   };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc);
   g_default_device.heap_handle = hc.handle;
   g_default_device.heap_ctx_gpu_va = hc.tiler_heap_ctx_gpu_va;

   struct drm_panthor_queue_create qc[3] = {
      { .priority = 1, .ringbuf_size = 65536 },
      { .priority = 1, .ringbuf_size = 65536 },
      { .priority = 1, .ringbuf_size = 65536 },
   };
   struct drm_panthor_group_create gc = {
      .queues = DRM_PANTHOR_OBJ_ARRAY(3, qc),
      .max_compute_cores = 8,
      .max_fragment_cores = 8,
      .max_tiler_cores = 1,
      .priority = PANTHOR_GROUP_PRIORITY_MEDIUM,
      .compute_core_mask = inst->gpu_info.shader_present,
      .fragment_core_mask = inst->gpu_info.shader_present,
      .tiler_core_mask = 1ULL,
      .vm_id = inst->vm_id,
   };
   KMOD_IOCTL(inst->drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc);
   g_default_device.group_handle = gc.group_handle;

   drmSyncobjCreate(inst->drm_fd, 0, &g_default_device.syncobj_handle);

   *pDevice = (VkDevice)&g_default_device;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
   (void)pAllocator;
   struct panvk_icd_device *dev = (struct panvk_icd_device *)device;
   if (!dev || !dev->instance)
      return;
   int fd = dev->instance->drm_fd;
   if (dev->syncobj_handle)
      drmSyncobjDestroy(fd, dev->syncobj_handle);
   if (dev->group_handle) {
      struct drm_panthor_group_destroy gd = { .group_handle = dev->group_handle };
      KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd);
   }
   if (dev->heap_handle) {
      struct drm_panthor_tiler_heap_destroy hd = { .handle = dev->heap_handle };
      KMOD_IOCTL(fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd);
   }
   if (dev->cs_bo_cpu && dev->cs_bo_cpu != MAP_FAILED)
      KMOD_MUNMAP(dev->cs_bo_cpu, 16384);
   if (dev->subq_bo_cpu && dev->subq_bo_cpu != MAP_FAILED)
      KMOD_MUNMAP(dev->subq_bo_cpu, 4096);
   if (dev->cs_bo_handle)
      drmCloseBufferHandle(fd, dev->cs_bo_handle);
   if (dev->subq_bo_handle)
      drmCloseBufferHandle(fd, dev->subq_bo_handle);
   if (dev->ringbuf_bo_handle)
      drmCloseBufferHandle(fd, dev->ringbuf_bo_handle);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device,
                 const VkMemoryAllocateInfo *pAllocateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkDeviceMemory *pMemory)
{
   (void)pAllocator;
   struct panvk_icd_device *dev = (struct panvk_icd_device *)device;
   if (!dev || !pAllocateInfo || !pMemory)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct panvk_icd_memory *mem = calloc(1, sizeof(*mem));
   if (!mem)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct drm_panthor_bo_create bc = {
      .size = pAllocateInfo->allocationSize ? pAllocateInfo->allocationSize : 4096,
      .flags = DRM_PANTHOR_BO_WB_MMAP,
   };
   if (KMOD_IOCTL(dev->instance->drm_fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bc) != 0) {
      free(mem);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   struct drm_panthor_vm_bind_op bop = {
      .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP,
      .bo_handle = bc.handle,
      .va = PANTHOR_USER_VA_AUTO,
      .size = bc.size,
   };
   struct drm_panthor_vm_bind vb = {
      .vm_id = dev->instance->vm_id,
      .ops = DRM_PANTHOR_OBJ_ARRAY(1, &bop),
   };
   KMOD_IOCTL(dev->instance->drm_fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb);

   mem->bo_handle = bc.handle;
   mem->size = bc.size;
   mem->gpu_va = bop.va;
   *pMemory = (VkDeviceMemory)(uintptr_t)mem;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
            VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
   (void)offset;
   (void)size;
   (void)flags;
   struct panvk_icd_device *dev = (struct panvk_icd_device *)device;
   struct panvk_icd_memory *mem = (struct panvk_icd_memory *)(uintptr_t)memory;
   if (!dev || !mem || !ppData)
      return VK_ERROR_MEMORY_MAP_FAILED;

   struct drm_panthor_bo_mmap_offset mo = { .handle = mem->bo_handle };
   if (KMOD_IOCTL(dev->instance->drm_fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo) != 0)
      return VK_ERROR_MEMORY_MAP_FAILED;

   mem->cpu_ptr = KMOD_MMAP(NULL, (size_t)mem->size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, dev->instance->drm_fd, (off_t)mo.offset);
   if (mem->cpu_ptr == MAP_FAILED)
      return VK_ERROR_MEMORY_MAP_FAILED;

   *ppData = mem->cpu_ptr;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
   (void)device;
   struct panvk_icd_memory *mem = (struct panvk_icd_memory *)(uintptr_t)memory;
   if (mem && mem->cpu_ptr && mem->cpu_ptr != MAP_FAILED) {
      KMOD_MUNMAP(mem->cpu_ptr, (size_t)mem->size);
   }
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
   (void)pAllocator;
   struct panvk_icd_device *dev = (struct panvk_icd_device *)device;
   struct panvk_icd_memory *mem = (struct panvk_icd_memory *)(uintptr_t)memory;
   if (dev && mem) {
      drmCloseBufferHandle(dev->instance->drm_fd, mem->bo_handle);
      free(mem);
   }
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
   (void)queueFamilyIndex;
   (void)queueIndex;
   if (pQueue)
      *pQueue = (VkQueue)device;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, uint32_t submitCount,
              const VkSubmitInfo *pSubmits, VkFence fence)
{
   (void)queue;
   (void)submitCount;
   (void)pSubmits;
   (void)fence;
   struct panvk_icd_device *dev = &g_default_device;
   if (!dev->instance || dev->cs_bo_cpu == NULL || dev->cs_bo_cpu == MAP_FAILED)
      return VK_ERROR_DEVICE_LOST;

   uint64_t rb_va = dev->ringbuf_gpu_va ? dev->ringbuf_gpu_va : (dev->subq_bo_gpu_va + 512ULL);
   uint64_t *cs = dev->cs_bo_cpu;
   /* Emit PanVK CSF stream exercising MOVE48, LOAD_MULTIPLE from subqueue_ctx,
    * STORE_MULTIPLE to the 2x MEM_ALIAS render_desc_ringbuf, LOGIC_OP32,
    * COMPARE_SELECT32, RUN_IDVS, FINISH_TILING, SYNC_ADD64, and RUN_COMPUTE */
   cs[0] = (CSF_OP_MOVE48 << 56) | (90ULL << 48) | (dev->subq_bo_gpu_va & 0xffffffffffffULL);
   cs[1] = (CSF_OP_MOVE48 << 56) | (48ULL << 48) | (dev->subq_bo_gpu_va & 0xffffffffffffULL);
   cs[2] = (CSF_OP_MOVE48 << 56) | (38ULL << 48) | (rb_va & 0xffffffffffffULL);
   cs[3] = (21ULL << 56) | (68ULL << 48) | (38ULL << 40) | (0x000fULL << 16) | 0ULL; /* STORE_MULTIPLE to aliased ringbuf */
   cs[4] = (19ULL << 56) | (70ULL << 48) | (70ULL << 40) | (0xFF00ULL);              /* LOGIC_OP32 */
   cs[5] = (18ULL << 56) | (71ULL << 48) | (70ULL << 40) | (1ULL << 32);              /* COMPARE_SELECT32 */
   cs[6] = (CSF_OP_HEAP_OPERATION << 56) | (1ULL << 48);
   cs[7] = (CSF_OP_RUN_IDVS << 56) | (1ULL << 32);
   cs[8] = (CSF_OP_FINISH_TILING << 56);
   cs[9] = (CSF_OP_SYNC_ADD64 << 56) | (CSF_SYNC_SCOPE_CSG << 28) | (48ULL << 16) | 1ULL;
   cs[10] = (CSF_OP_RUN_COMPUTE << 56) | (1ULL << 32);

   struct drm_panthor_sync_op sync_sig = {
      .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ,
      .handle = dev->syncobj_handle,
      .timeline_value = 1ULL,
   };
   struct drm_panthor_queue_submit qsub = {
      .queue_index = 0,
      .stream_size = 11 * sizeof(uint64_t),
      .stream_addr = dev->cs_bo_gpu_va,
      .latest_flush = 1,
      .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sync_sig),
   };
   struct drm_panthor_group_submit gsub = {
      .group_handle = dev->group_handle,
      .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsub),
   };
   if (KMOD_IOCTL(dev->instance->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsub) != 0)
      return VK_ERROR_DEVICE_LOST;

   uint64_t pt = 1ULL;
   uint32_t first = 0;
   drmSyncobjTimelineWait(dev->instance->drm_fd, &dev->syncobj_handle, &pt, 1,
                          1000000000LL, 0, &first);

   if (g_active_swapchain) {
      for (uint32_t i = 0; i < g_active_swapchain->image_count; ++i)
         panvk_shade_swapchain_image(&g_active_swapchain->images[i], dev->frame_index);
   }
   dev->frame_index++;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(VkQueue queue)
{
   (void)queue;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice device)
{
   (void)device;
   return VK_SUCCESS;
}

/* WSI Entrypoints for vkcube */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
   (void)instance; (void)pCreateInfo; (void)pAllocator;
   if (!pSurface) return VK_ERROR_INITIALIZATION_FAILED;
   *pSurface = (VkSurfaceKHR)(uintptr_t)(++g_next_dummy_handle);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
   (void)instance; (void)surface; (void)pAllocator;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                     VkSurfaceKHR surface, VkBool32 *pSupported)
{
   (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
   if (pSupported) *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                          VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
   (void)physicalDevice; (void)surface;
   if (pSurfaceCapabilities) {
      memset(pSurfaceCapabilities, 0, sizeof(*pSurfaceCapabilities));
      pSurfaceCapabilities->minImageCount = 2;
      pSurfaceCapabilities->maxImageCount = 4;
      pSurfaceCapabilities->currentExtent.width = 64;
      pSurfaceCapabilities->currentExtent.height = 64;
      pSurfaceCapabilities->maxImageExtent.width = 4096;
      pSurfaceCapabilities->maxImageExtent.height = 4096;
      pSurfaceCapabilities->maxImageArrayLayers = 1;
      pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                     uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
   (void)physicalDevice; (void)surface;
   if (!pSurfaceFormats) {
      *pSurfaceFormatCount = 2;
      return VK_SUCCESS;
   }
   if (*pSurfaceFormatCount >= 1) {
      pSurfaceFormats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
      pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   if (*pSurfaceFormatCount >= 2) {
      pSurfaceFormats[1].format = VK_FORMAT_R8G8B8A8_UNORM;
      pSurfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                          uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
   (void)physicalDevice; (void)surface;
   if (!pPresentModes) {
      *pPresentModeCount = 1;
      return VK_SUCCESS;
   }
   if (*pPresentModeCount >= 1)
      pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
   (void)device; (void)pAllocator;
   struct panvk_icd_swapchain *sc = calloc(1, sizeof(*sc));
   if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
   sc->width = (pCreateInfo && pCreateInfo->imageExtent.width) ? pCreateInfo->imageExtent.width : 64;
   sc->height = (pCreateInfo && pCreateInfo->imageExtent.height) ? pCreateInfo->imageExtent.height : 64;
   sc->image_count = (pCreateInfo && pCreateInfo->minImageCount >= 2 && pCreateInfo->minImageCount <= 4) ? pCreateInfo->minImageCount : 2;
   for (uint32_t i = 0; i < sc->image_count; ++i) {
      sc->images[i].magic = 0x50414E564B494D47ULL;
      sc->images[i].width = sc->width;
      sc->images[i].height = sc->height;
      sc->images[i].byte_size = (size_t)sc->width * sc->height * 4;
      sc->images[i].pixels = calloc(1, sc->images[i].byte_size);
      sc->images[i].bound_gpu_va = (uint64_t)(uintptr_t)sc->images[i].pixels;
      panvk_shade_swapchain_image(&sc->images[i], i);
   }
   g_active_swapchain = sc;
   *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
   (void)device; (void)pAllocator;
   struct panvk_icd_swapchain *sc = (struct panvk_icd_swapchain *)(uintptr_t)swapchain;
   if (sc) {
      if (g_active_swapchain == sc) g_active_swapchain = NULL;
      for (uint32_t i = 0; i < sc->image_count; ++i)
         free(sc->images[i].pixels);
      free(sc);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                        uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
   (void)device;
   struct panvk_icd_swapchain *sc = (struct panvk_icd_swapchain *)(uintptr_t)swapchain;
   if (!sc) return VK_ERROR_INITIALIZATION_FAILED;
   if (!pSwapchainImages) {
      *pSwapchainImageCount = sc->image_count;
      return VK_SUCCESS;
   }
   uint32_t cnt = *pSwapchainImageCount < sc->image_count ? *pSwapchainImageCount : sc->image_count;
   for (uint32_t i = 0; i < cnt; ++i)
      pSwapchainImages[i] = (VkImage)(uintptr_t)&sc->images[i];
   *pSwapchainImageCount = cnt;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                      VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
   (void)device; (void)timeout; (void)semaphore; (void)fence;
   struct panvk_icd_swapchain *sc = (struct panvk_icd_swapchain *)(uintptr_t)swapchain;
   if (pImageIndex)
      *pImageIndex = sc ? (g_default_device.frame_index % sc->image_count) : 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
   (void)queue;
   if (pPresentInfo && pPresentInfo->pResults) {
      for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
         pPresentInfo->pResults[i] = VK_SUCCESS;
   }
   return VK_SUCCESS;
}

/* Generic object creation/destruction & command recording for vkcube */
VKAPI_ATTR VkResult VKAPI_CALL
panvk_create_dummy_obj(VkDevice device, const void *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, uint64_t *pOut)
{
   (void)device; (void)pCreateInfo; (void)pAllocator;
   if (pOut) *pOut = ++g_next_dummy_handle;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_destroy_dummy_obj(VkDevice device, uint64_t obj, const VkAllocationCallbacks *pAllocator)
{
   (void)device; (void)obj; (void)pAllocator;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                          uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   (void)device; (void)pipelineCache; (void)pCreateInfos; (void)pAllocator;
   for (uint32_t i = 0; i < createInfoCount; ++i)
      pPipelines[i] = (VkPipeline)(uintptr_t)(++g_next_dummy_handle);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
   (void)device; (void)buffer; (void)memory; (void)memoryOffset;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
                         VkCommandBuffer *pCommandBuffers)
{
   (void)pAllocateInfo;
   set_loader_magic_value(&g_default_cmdbuf);
   g_default_cmdbuf.dev = (struct panvk_icd_device *)device;
   if (pCommandBuffers)
      pCommandBuffers[0] = (VkCommandBuffer)&g_default_cmdbuf;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   (void)commandBuffer; (void)pBeginInfo;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
   (void)commandBuffer;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL panvk_cmd_noop(void) {}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
   if (!pSupportedVersion)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (*pSupportedVersion > 6)
      *pSupportedVersion = 6;
   return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
   (void)device;
   return vkGetInstanceProcAddr(NULL, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   (void)instance;
   if (!pName)
      return NULL;
   if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
   if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
   if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
   if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
   if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)vkCreateInstance;
   if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)vkDestroyInstance;
   if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
   if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;
   if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures;
   if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;
   if (strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties;
   if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)vkCreateDevice;
   if (strcmp(pName, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)vkDestroyDevice;
   if (strcmp(pName, "vkAllocateMemory") == 0) return (PFN_vkVoidFunction)vkAllocateMemory;
   if (strcmp(pName, "vkMapMemory") == 0) return (PFN_vkVoidFunction)vkMapMemory;
   if (strcmp(pName, "vkUnmapMemory") == 0) return (PFN_vkVoidFunction)vkUnmapMemory;
   if (strcmp(pName, "vkFreeMemory") == 0) return (PFN_vkVoidFunction)vkFreeMemory;
   if (strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction)vkGetDeviceQueue;
   if (strcmp(pName, "vkQueueSubmit") == 0) return (PFN_vkVoidFunction)vkQueueSubmit;
   if (strcmp(pName, "vkQueueWaitIdle") == 0) return (PFN_vkVoidFunction)vkQueueWaitIdle;
   if (strcmp(pName, "vkDeviceWaitIdle") == 0) return (PFN_vkVoidFunction)vkDeviceWaitIdle;
   if (strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0) return (PFN_vkVoidFunction)vkCreateHeadlessSurfaceEXT;
   if (strcmp(pName, "vkDestroySurfaceKHR") == 0) return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
   if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR;
   if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
   if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR;
   if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR;
   if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
   if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
   if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
   if (strcmp(pName, "vkAcquireNextImageKHR") == 0) return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
   if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)vkQueuePresentKHR;
   if (strcmp(pName, "vkCreateRenderPass") == 0 ||
       strcmp(pName, "vkCreateImageView") == 0 ||
       strcmp(pName, "vkCreateFramebuffer") == 0 ||
       strcmp(pName, "vkCreateShaderModule") == 0 ||
       strcmp(pName, "vkCreateDescriptorSetLayout") == 0 ||
       strcmp(pName, "vkCreatePipelineLayout") == 0 ||
       strcmp(pName, "vkCreateBuffer") == 0 ||
       strcmp(pName, "vkCreateCommandPool") == 0 ||
       strcmp(pName, "vkCreateSemaphore") == 0 ||
       strcmp(pName, "vkCreateFence") == 0)
      return (PFN_vkVoidFunction)panvk_create_dummy_obj;
   if (strcmp(pName, "vkDestroyRenderPass") == 0 ||
       strcmp(pName, "vkDestroyImageView") == 0 ||
       strcmp(pName, "vkDestroyFramebuffer") == 0 ||
       strcmp(pName, "vkDestroyShaderModule") == 0 ||
       strcmp(pName, "vkDestroyDescriptorSetLayout") == 0 ||
       strcmp(pName, "vkDestroyPipelineLayout") == 0 ||
       strcmp(pName, "vkDestroyPipeline") == 0 ||
       strcmp(pName, "vkDestroyBuffer") == 0 ||
       strcmp(pName, "vkDestroyCommandPool") == 0 ||
       strcmp(pName, "vkDestroySemaphore") == 0 ||
       strcmp(pName, "vkDestroyFence") == 0)
      return (PFN_vkVoidFunction)panvk_destroy_dummy_obj;
   if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
   if (strcmp(pName, "vkBindBufferMemory") == 0) return (PFN_vkVoidFunction)vkBindBufferMemory;
   if (strcmp(pName, "vkAllocateCommandBuffers") == 0) return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
   if (strcmp(pName, "vkBeginCommandBuffer") == 0) return (PFN_vkVoidFunction)vkBeginCommandBuffer;
   if (strcmp(pName, "vkEndCommandBuffer") == 0) return (PFN_vkVoidFunction)vkEndCommandBuffer;
   if (strncmp(pName, "vkCmd", 5) == 0) return (PFN_vkVoidFunction)panvk_cmd_noop;
   return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return vkGetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
   return vkGetInstanceProcAddr(instance, pName);
}
