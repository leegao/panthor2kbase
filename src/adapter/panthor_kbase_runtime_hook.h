/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor-to-Kbase Runtime Hook Engine (Unpatched Mesa PanVK Mode)
 *
 * Enables an UNPATCHED upstream Mesa 26.3.0 libvulkan_panfrost.so build to run
 * on ARM Mali Kbase CSF devices (/dev/mali0) by:
 *   1. Patching ELF .got.plt / .got (DT_JMPREL & DT_RELA) entries across loaded
 *      shared objects via dl_iterate_phdr() for open/open64/__open_2/close/dup/
 *      ioctl/mmap/mmap64/munmap and all drm* entrypoints.
 *   2. Synthesizing a virtual DRM platform device (/dev/dri/renderD128,
 *      compatible="arm,mali-valhall-csf") in drmGetDevices2() / drmGetDevice2()
 *      when running on a kbase host that lacks /dev/dri/renderD128.
 */
#ifndef PANTHOR_KBASE_RUNTIME_HOOK_H
#define PANTHOR_KBASE_RUNTIME_HOOK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct panthor_kbase_hook_stats {
   uint32_t modules_scanned;
   uint32_t modules_hooked;
   uint32_t got_slots_patched;
   uint32_t open_intercepts;
   uint32_t ioctl_intercepts;
   uint32_t mmap_intercepts;
   uint32_t munmap_intercepts;
   uint32_t drm_devices_enumerated;
};

/*
 * Scans all currently loaded ELF modules via dl_iterate_phdr() and redirects
 * GOT/PLT entries for libc file/memory/ioctl syscalls and libdrm symbols to
 * the Panthor-to-Kbase UAPI adapter.
 *
 * Returns the number of GOT slots patched across all matching modules.
 */
int panthor_kbase_install_runtime_hooks(void);

/*
 * Patches a specific named module (or all matching modules if module_substr is NULL).
 */
int panthor_kbase_patch_module_got(const char *module_substr);

/*
 * Retrieves cumulative runtime hook statistics for verification and testing.
 */
void panthor_kbase_get_hook_stats(struct panthor_kbase_hook_stats *out_stats);

/*
 * Resets runtime hook call counters (while keeping GOT patches installed).
 */
void panthor_kbase_reset_hook_counters(void);

/*
 * Synthetic DRM device enumeration entrypoints for unpatched Mesa panvk.
 */
int panthor_kbase_drmGetDevices2(uint32_t flags, void **devices, int max_devices);
void panthor_kbase_drmFreeDevices(void **devices, int count);

#ifdef __cplusplus
}
#endif

#endif /* PANTHOR_KBASE_RUNTIME_HOOK_H */
