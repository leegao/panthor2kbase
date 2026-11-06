/*
 * SPDX-License-Identifier: MIT
 *
 * PanVK Android & Linux Vulkan ICD Driver Front-End (Patched & Unpatched Modes)
 *
 * Built in two configurations from the same source + Mesa 26.3.0 pan_kmod /
 * panthor_kmod backend:
 *   1. Variant A (libvulkan_panfrost_patched.so, -DPANTHOR_KBASE_PATCHED_KMOD=1):
 *      Applies 0001-panfrost-kmod-minimal-uapi-adapter-hook.patch so pan_kmod
 *      and panthor_kmod route open/ioctl/mmap/munmap directly to
 *      panthor_kbase_adapter_*.
 *   2. Variant B (libvulkan_panfrost_unpatched.so, -DPANTHOR_KBASE_PATCHED_KMOD=0):
 *      Uses 100% unmodified upstream pan_kmod/panthor_kmod calls (drmGetDevices2,
 *      stat, open, drmGetVersion, drmGetCap, drmIoctl, mmap, munmap, drmSyncobj*,
 *      drmPrime*) and relies on libpanthor_kbase_runtime_hook.so (dl_iterate_phdr
 *      ELF GOT/PLT patching) to redirect calls to /dev/mali0 at runtime.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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

/* ============================================================================
 * Upstream Mesa 26.3.0 pan_kmod & panthor_kmod Execution Path
 * ============================================================================ */

#if PANTHOR_KBASE_PATCHED_KMOD
/* Variant A: Patched pan_kmod / panthor_kmod hook macros */
#define KMOD_OPEN(path, flags)                 panthor_kbase_adapter_open(path, flags)
#define KMOD_CLOSE(fd)                         panthor_kbase_adapter_close(fd)
#define KMOD_IOCTL(fd, req, arg)               panthor_kbase_adapter_ioctl(fd, req, arg)
#define KMOD_MMAP(addr, len, prot, fl, fd, of) panthor_kbase_adapter_mmap(addr, len, prot, fl, fd, of)
#define KMOD_MUNMAP(addr, len)                 panthor_kbase_adapter_munmap(addr, len)
#else
/* Variant B: Unpatched upstream Mesa pan_kmod / panthor_kmod calls (GOT-hooked at runtime) */
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
};

struct panvk_icd_memory {
   uint32_t bo_handle;
   uint64_t size;
   uint64_t gpu_va;
   void *cpu_ptr;
};

struct panvk_icd_cmd_buffer {
   VK_LOADER_DATA loader_magic;
   struct panvk_icd_device *dev;
   uint32_t recorded_dwords;
   bool is_compute;
};

static struct panvk_icd_instance g_default_instance = {0};
static struct panvk_icd_device   g_default_device   = {0};

static VkResult panvk_probe_and_init_kmod(struct panvk_icd_instance *inst)
{
   if (inst->drm_fd > 0 && inst->physical_device_enumerated)
      return VK_SUCCESS;

   const char *render_path = "/dev/dri/renderD128";
#if !PANTHOR_KBASE_PATCHED_KMOD
   /* Unpatched upstream Mesa panvk_physical_device.c enumerates via drmGetDevices2 + stat */
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

   /* Query GPU_INFO, CSIF_INFO, MMU_INFO exactly as upstream panthor_kmod.c does */
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

/* ============================================================================
 * Exported Vulkan ICD & Core Entrypoints
 * ============================================================================ */

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
   pProperties->vendorID = 0x13B5; /* ARM */
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

   /* Exercise panvk_vX_device.c:396 os_dupfd_cloexec -> panthor_kbase_adapter_dup */
   int dev_fd = panthor_kbase_adapter_dup(inst->drm_fd);
   if (dev_fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* Exercise PAN_KMOD_BO_FLAG_EXECUTABLE (1 << 0) shader heap allocation in EXEC_VA_ZONE */
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

   /* Allocate CS command stream BO + subqueue context BO via Panthor DRM UAPI */
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
   if (g_default_device.subq_bo_cpu && g_default_device.subq_bo_cpu != MAP_FAILED)
      g_default_device.subq_bo_cpu[6] = g_default_device.subq_bo_gpu_va + 256ULL;

   /* Create tiler heap and CS group (3 queues: vertex/tiler, fragment, compute) */
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
      struct drm_panthor_tiler_heap_destroy hd = {
         .handle = dev->heap_handle,
      };
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
      /* Calls munmap() - in unpatched mode, the runtime hook intercepts munmap()
       * and prevents premature SAME_VA GPU page table teardown! */
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

   /* Emit canonical PanVK CSF stream (MOVE48 r48=subq_ctx_va, HEAP_OPERATION,
    * RUN_IDVS, FINISH_TILING, SYNC_ADD64 scope=CSG) into cs_bo_cpu */
   uint64_t *cs = dev->cs_bo_cpu;
   cs[0] = (CSF_OP_MOVE48 << 56) | (48ULL << 48) | (dev->subq_bo_gpu_va & 0xffffffffffffULL);
   cs[1] = (CSF_OP_HEAP_OPERATION << 56) | (1ULL << 48);
   cs[2] = (CSF_OP_RUN_IDVS << 56) | (1ULL << 32);
   cs[3] = (CSF_OP_FINISH_TILING << 56);
   cs[4] = (CSF_OP_SYNC_ADD64 << 56) | (CSF_SYNC_SCOPE_CSG << 28) | (48ULL << 16) | 1ULL;
   cs[5] = (CSF_OP_RUN_COMPUTE << 56) | (1ULL << 32);

   struct drm_panthor_sync_op sync_sig = {
      .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ,
      .handle = dev->syncobj_handle,
      .timeline_value = 1ULL,
   };
   struct drm_panthor_queue_submit qsub = {
      .queue_index = 0,
      .stream_size = 6 * sizeof(uint64_t),
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
   return VK_SUCCESS;
}

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
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   (void)instance;
   if (!pName)
      return NULL;
   if (strcmp(pName, "vkCreateInstance") == 0)
      return (PFN_vkVoidFunction)vkCreateInstance;
   if (strcmp(pName, "vkDestroyInstance") == 0)
      return (PFN_vkVoidFunction)vkDestroyInstance;
   if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
      return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
   if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)
      return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;
   if (strcmp(pName, "vkCreateDevice") == 0)
      return (PFN_vkVoidFunction)vkCreateDevice;
   if (strcmp(pName, "vkDestroyDevice") == 0)
      return (PFN_vkVoidFunction)vkDestroyDevice;
   if (strcmp(pName, "vkAllocateMemory") == 0)
      return (PFN_vkVoidFunction)vkAllocateMemory;
   if (strcmp(pName, "vkMapMemory") == 0)
      return (PFN_vkVoidFunction)vkMapMemory;
   if (strcmp(pName, "vkUnmapMemory") == 0)
      return (PFN_vkVoidFunction)vkUnmapMemory;
   if (strcmp(pName, "vkFreeMemory") == 0)
      return (PFN_vkVoidFunction)vkFreeMemory;
   if (strcmp(pName, "vkQueueSubmit") == 0)
      return (PFN_vkVoidFunction)vkQueueSubmit;
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
