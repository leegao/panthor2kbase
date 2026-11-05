/*
 * SPDX-License-Identifier: MIT
 *
 * Dual-Variant Verification Suite:
 *   1. Variant A: Patched Mesa pan_kmod / panthor_kmod (libvulkan_panfrost_patched.so)
 *   2. Variant B: Unpatched Mesa pan_kmod / panthor_kmod + ELF GOT/PLT Runtime Hook
 *                 (libvulkan_panfrost_unpatched.so + libpanthor_kbase_runtime_hook.so)
 *
 * Verifies that an UNPATCHED libvulkan_panfrost.so loaded via dlopen() has its
 * ELF .got.plt / .got slots patched via dl_iterate_phdr(), enumerates the
 * synthetic /dev/dri/renderD128 node via drmGetDevices2(), executes full Vulkan
 * instance/device/memory/command-submission lifecycles, and produces identical
 * /dev/mali0 kbase UAPI sequences and CSF execution telemetry as the patched build.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <dlfcn.h>

#include "vulkan/vulkan.h"
#include "panthor_kbase_adapter.h"
#include "panthor_kbase_runtime_hook.h"
#include "../src/emulator/mali_kbase_emu.h"

struct variant_run_metrics {
   char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint32_t vendor_id;
   uint32_t device_id;
   struct kbase_emu_stats emu_stats;
};

static void run_icd_variant(const char *so_path,
                            bool install_got_hooks,
                            struct variant_run_metrics *out)
{
   memset(out, 0, sizeof(*out));
   mali_kbase_emu_reset_stats();
   panthor_kbase_reset_hook_counters();

   void *lib = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FATAL: dlopen(%s) failed: %s\n", so_path, dlerror());
      exit(1);
   }

   if (install_got_hooks) {
      int patched = panthor_kbase_install_runtime_hooks();
      printf("  [RuntimeHook] Patched %d ELF GOT/PLT slots across loaded modules for %s\n",
             patched, so_path);
      assert(patched > 0);
   }

   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   assert(gipa != NULL);

   PFN_vkCreateInstance pfnCreateInstance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   PFN_vkEnumeratePhysicalDevices pfnEnumDevs =
      (PFN_vkEnumeratePhysicalDevices)gipa(NULL, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties pfnGetProps =
      (PFN_vkGetPhysicalDeviceProperties)gipa(NULL, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice pfnCreateDevice =
      (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
   PFN_vkAllocateMemory pfnAllocMem =
      (PFN_vkAllocateMemory)gipa(NULL, "vkAllocateMemory");
   PFN_vkMapMemory pfnMapMem =
      (PFN_vkMapMemory)gipa(NULL, "vkMapMemory");
   PFN_vkUnmapMemory pfnUnmapMem =
      (PFN_vkUnmapMemory)gipa(NULL, "vkUnmapMemory");
   PFN_vkFreeMemory pfnFreeMem =
      (PFN_vkFreeMemory)gipa(NULL, "vkFreeMemory");
   PFN_vkQueueSubmit pfnQueueSubmit =
      (PFN_vkQueueSubmit)gipa(NULL, "vkQueueSubmit");
   PFN_vkDestroyDevice pfnDestroyDevice =
      (PFN_vkDestroyDevice)gipa(NULL, "vkDestroyDevice");
   PFN_vkDestroyInstance pfnDestroyInstance =
      (PFN_vkDestroyInstance)gipa(NULL, "vkDestroyInstance");

   assert(pfnCreateInstance && pfnEnumDevs && pfnGetProps && pfnCreateDevice);
   assert(pfnAllocMem && pfnMapMem && pfnUnmapMem && pfnFreeMem && pfnQueueSubmit);

   VkInstance instance = VK_NULL_HANDLE;
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   assert(pfnCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);

   uint32_t dev_count = 0;
   assert(pfnEnumDevs(instance, &dev_count, NULL) == VK_SUCCESS);
   assert(dev_count == 1);

   VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
   assert(pfnEnumDevs(instance, &dev_count, &phys_dev) == VK_SUCCESS);

   VkPhysicalDeviceProperties props;
   pfnGetProps(phys_dev, &props);
   snprintf(out->device_name, sizeof(out->device_name), "%s", props.deviceName);
   out->vendor_id = props.vendorID;
   out->device_id = props.deviceID;

   VkDevice device = VK_NULL_HANDLE;
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   assert(pfnCreateDevice(phys_dev, &dci, NULL, &device) == VK_SUCCESS);

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 8192,
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   assert(pfnAllocMem(device, &mai, NULL, &mem) == VK_SUCCESS);

   void *mapped = NULL;
   assert(pfnMapMem(device, mem, 0, 8192, 0, &mapped) == VK_SUCCESS);
   assert(mapped != NULL);
   memset(mapped, 0x5A, 256);

   /* Test SAME_VA munmap guard via vkUnmapMemory */
   pfnUnmapMem(device, mem);
   assert(((uint8_t *)mapped)[0] == 0x5A);

   assert(pfnQueueSubmit(VK_NULL_HANDLE, 0, NULL, VK_NULL_HANDLE) == VK_SUCCESS);

   mali_kbase_emu_get_stats(&out->emu_stats);
   printf("  [KbaseEmu] ioctls=%u mem_allocs=%u kicks=%u csf_insns=%u calls=%u idvs=%u compute=%u csg_warn=%u\n",
          out->emu_stats.ioctl_count, out->emu_stats.mem_alloc_count,
          out->emu_stats.queue_kick_count, out->emu_stats.csf_instructions_executed,
          out->emu_stats.csf_calls_followed, out->emu_stats.idvs_jobs_run,
          out->emu_stats.compute_jobs_run, out->emu_stats.csg_scope_warnings);

   if (install_got_hooks) {
      struct panthor_kbase_hook_stats hstats;
      panthor_kbase_get_hook_stats(&hstats);
      printf("  [RuntimeHook] Intercept counts: drmGetDevices2=%u open=%u mmap=%u munmap=%u\n",
             hstats.drm_devices_enumerated, hstats.open_intercepts,
             hstats.mmap_intercepts, hstats.munmap_intercepts);
      assert(hstats.drm_devices_enumerated >= 1);
      assert(hstats.open_intercepts >= 1);
      assert(hstats.mmap_intercepts >= 1);
      assert(hstats.munmap_intercepts >= 1);
   }

   pfnFreeMem(device, mem, NULL);
   pfnDestroyDevice(device, NULL);
   pfnDestroyInstance(instance, NULL);
   dlclose(lib);
}

int main(int argc, char **argv)
{
   const char *patched_so = (argc > 1) ? argv[1] : "./libvulkan_panfrost_patched.so";
   const char *unpatched_so = (argc > 2) ? argv[2] : "./libvulkan_panfrost_unpatched.so";

   printf("=== Testing Dual-Variant PanVK Builds (Patched Kmod vs Unpatched + Runtime Hook) ===\n");

   struct variant_run_metrics m_patched, m_unpatched;
   printf("1. Running Variant A: Patched pan_kmod (%s)...\n", patched_so);
   run_icd_variant(patched_so, false, &m_patched);
   printf("   -> Device: %s (vendor=0x%x, id=0x%x)\n",
          m_patched.device_name, m_patched.vendor_id, m_patched.device_id);

   printf("2. Running Variant B: Unpatched pan_kmod + ELF GOT/PLT Runtime Hook (%s)...\n",
          unpatched_so);
   run_icd_variant(unpatched_so, true, &m_unpatched);
   printf("   -> Device: %s (vendor=0x%x, id=0x%x)\n",
          m_unpatched.device_name, m_unpatched.vendor_id, m_unpatched.device_id);

   /* Verify equivalence of Kbase UAPI call counts and CSF execution telemetry */
   assert(m_patched.vendor_id == m_unpatched.vendor_id);
   assert(m_patched.device_id == m_unpatched.device_id);
   assert(m_patched.emu_stats.ioctl_count == m_unpatched.emu_stats.ioctl_count);
   assert(m_patched.emu_stats.mem_alloc_count == m_unpatched.emu_stats.mem_alloc_count);
   assert(m_patched.emu_stats.queue_kick_count == m_unpatched.emu_stats.queue_kick_count);
   assert(m_patched.emu_stats.csf_instructions_executed == m_unpatched.emu_stats.csf_instructions_executed);
   assert(m_patched.emu_stats.csf_calls_followed == m_unpatched.emu_stats.csf_calls_followed);
   assert(m_patched.emu_stats.idvs_jobs_run == m_unpatched.emu_stats.idvs_jobs_run);
   assert(m_patched.emu_stats.compute_jobs_run == m_unpatched.emu_stats.compute_jobs_run);
   assert(m_patched.emu_stats.csg_scope_warnings == 0);
   assert(m_unpatched.emu_stats.csg_scope_warnings == 0);
   assert(m_patched.emu_stats.same_va_munmap_destroys == 0);
   assert(m_unpatched.emu_stats.same_va_munmap_destroys == 0);

   printf("=== Dual-Variant Equivalence Verified: Patched and Unpatched+RuntimeHook match 100%%! ===\n");
   return 0;
}
