/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor-to-Kbase Runtime Hook Engine (Unpatched Mesa PanVK Mode)
 *
 * Allows an UNPATCHED upstream Mesa 26.3.0 libvulkan_panfrost.so binary to run
 * directly on ARM Mali Kbase CSF devices (/dev/mali0) without modifying Mesa:
 *   1. Uses dl_iterate_phdr() to walk loaded ELF images and rewrite .got.plt /
 *      .got (DT_JMPREL & DT_RELA) slots for open, open64, __open_2, close, dup,
 *      ioctl, mmap, mmap64, munmap, stat, and all libdrm functions.
 *   2. Synthesizes a virtual Panthor DRM platform device (/dev/dri/renderD128,
 *      compatible = "arm,mali-valhall-csf") in drmGetDevices2() / drmGetDevice2()
 *      so unpatched panvk_physical_device.c enumerates the kbase GPU seamlessly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>

#include "panthor_kbase_adapter.h"
#include "panthor_kbase_runtime_hook.h"

/* ELF Relocation types for AArch64 and x86_64 */
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT  1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_X86_64_GLOB_DAT
#define R_X86_64_GLOB_DAT   6
#endif
#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT  7
#endif

/* Standard libdrm constants & structures from xf86drm.h */
#define HOOK_DRM_NODE_PRIMARY 0
#define HOOK_DRM_NODE_CONTROL 1
#define HOOK_DRM_NODE_RENDER  2
#define HOOK_DRM_NODE_MAX     3

#define HOOK_DRM_BUS_PCI      0
#define HOOK_DRM_BUS_USB      1
#define HOOK_DRM_BUS_PLATFORM 2
#define HOOK_DRM_BUS_HOST1X   3

#define HOOK_DRM_PLATFORM_DEVICE_NAME_LEN 512

struct hook_drmPlatformBusInfo {
   char fullname[HOOK_DRM_PLATFORM_DEVICE_NAME_LEN];
};

struct hook_drmPlatformDeviceInfo {
   char **compatible;
};

struct hook_drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      struct hook_drmPlatformBusInfo *platform;
      void *pci;
      void *usb;
      void *host1x;
   } businfo;
   union {
      struct hook_drmPlatformDeviceInfo *platform;
      void *pci;
      void *usb;
      void *host1x;
   } deviceinfo;
};

static pthread_mutex_t g_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static struct panthor_kbase_hook_stats g_stats = {0};

static int (*orig_open)(const char *, int, ...) = NULL;
static int (*orig_close)(int) = NULL;
static int (*orig_ioctl)(int, unsigned long, ...) = NULL;
static void *(*orig_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*orig_munmap)(void *, size_t) = NULL;
static int (*orig_stat)(const char *, struct stat *) = NULL;
static void *(*orig_dlopen)(const char *, int) = NULL;

static void ensure_orig_resolved(void)
{
   if (!orig_open) {
      orig_open = dlsym(RTLD_NEXT, "open");
      if (!orig_open) orig_open = dlsym(RTLD_DEFAULT, "open");
   }
   if (!orig_close) {
      orig_close = dlsym(RTLD_NEXT, "close");
      if (!orig_close) orig_close = dlsym(RTLD_DEFAULT, "close");
   }
   if (!orig_ioctl) {
      orig_ioctl = dlsym(RTLD_NEXT, "ioctl");
      if (!orig_ioctl) orig_ioctl = dlsym(RTLD_DEFAULT, "ioctl");
   }
   if (!orig_mmap) {
      orig_mmap = dlsym(RTLD_NEXT, "mmap");
      if (!orig_mmap) orig_mmap = dlsym(RTLD_DEFAULT, "mmap");
   }
   if (!orig_munmap) {
      orig_munmap = dlsym(RTLD_NEXT, "munmap");
      if (!orig_munmap) orig_munmap = dlsym(RTLD_DEFAULT, "munmap");
   }
   if (!orig_stat) {
      orig_stat = dlsym(RTLD_NEXT, "stat");
      if (!orig_stat) orig_stat = dlsym(RTLD_DEFAULT, "stat");
   }
   if (!orig_dlopen) {
      orig_dlopen = dlsym(RTLD_NEXT, "dlopen");
      if (!orig_dlopen) orig_dlopen = dlsym(RTLD_DEFAULT, "dlopen");
   }
}

/* ============================================================================
 * Hooked Syscall & Libc Entrypoints for Unpatched libvulkan_panfrost.so
 * ============================================================================ */

static bool is_intercepted_drm_path(const char *path)
{
   if (!path)
      return false;
   return (strstr(path, "/dev/dri/renderD") != NULL ||
           strstr(path, "/dev/dri/card") != NULL ||
           strstr(path, "/dev/mali") != NULL);
}

int panthor_kbase_hook_open(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap;
      va_start(ap, flags);
      mode = (mode_t)va_arg(ap, int);
      va_end(ap);
   }
   if (is_intercepted_drm_path(path)) {
      __sync_fetch_and_add(&g_stats.open_intercepts, 1);
      return panthor_kbase_adapter_open(path, flags);
   }
   ensure_orig_resolved();
   return orig_open ? orig_open(path, flags, mode) : -1;
}

int panthor_kbase_hook_open2(const char *path, int oflag)
{
   return panthor_kbase_hook_open(path, oflag, 0);
}

int panthor_kbase_hook_close(int fd)
{
   return panthor_kbase_adapter_close(fd);
}

int panthor_kbase_hook_dup(int fd)
{
   return panthor_kbase_adapter_dup(fd);
}

int panthor_kbase_hook_fcntl(int fd, int cmd, ...)
{
   va_list ap;
   va_start(ap, cmd);
   long arg = va_arg(ap, long);
   va_end(ap);
   if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) &&
       panthor_kbase_adapter_is_virtual_drm_fd(fd)) {
      return panthor_kbase_adapter_dup(fd);
   }
   int (*real_fcntl)(int, int, ...) = dlsym(RTLD_NEXT, "fcntl");
   return real_fcntl ? real_fcntl(fd, cmd, arg) : 0;
}

int panthor_kbase_hook_ioctl(int fd, unsigned long request, ...)
{
   va_list ap;
   va_start(ap, request);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   if (panthor_kbase_adapter_is_virtual_drm_fd(fd))
      __sync_fetch_and_add(&g_stats.ioctl_intercepts, 1);
   return panthor_kbase_adapter_ioctl(fd, request, arg);
}

void *panthor_kbase_hook_mmap(void *addr, size_t length, int prot, int flags,
                              int fd, off_t offset)
{
   if (panthor_kbase_adapter_is_virtual_drm_fd(fd))
      __sync_fetch_and_add(&g_stats.mmap_intercepts, 1);
   return panthor_kbase_adapter_mmap(addr, length, prot, flags, fd, offset);
}

int panthor_kbase_hook_munmap(void *addr, size_t length)
{
   __sync_fetch_and_add(&g_stats.munmap_intercepts, 1);
   return panthor_kbase_adapter_munmap(addr, length);
}

int panthor_kbase_hook_stat(const char *path, struct stat *st)
{
   if (path && st && is_intercepted_drm_path(path)) {
      memset(st, 0, sizeof(*st));
      st->st_mode = S_IFCHR | 0666;
      if (strstr(path, "card") != NULL) {
         st->st_rdev = makedev(226, 0);
         st->st_ino = 10001;
      } else {
         st->st_rdev = makedev(226, 128);
         st->st_ino = 10128;
      }
      return 0;
   }
   ensure_orig_resolved();
   return orig_stat ? orig_stat(path, st) : -1;
}

/* ============================================================================
 * Synthetic DRM Device Enumeration (drmGetDevices2 / drmGetDevice2)
 * ============================================================================ */

static struct hook_drmDevice *alloc_synthetic_panthor_drm_device(void)
{
   /* Allocate a single contiguous block or structured allocation matching libdrm */
   struct hook_drmDevice *dev = calloc(1, sizeof(struct hook_drmDevice));
   if (!dev)
      return NULL;

   dev->nodes = calloc(HOOK_DRM_NODE_MAX, sizeof(char *));
   dev->nodes[HOOK_DRM_NODE_PRIMARY] = strdup("/dev/dri/card0");
   dev->nodes[HOOK_DRM_NODE_CONTROL] = strdup("/dev/dri/controlD64");
   dev->nodes[HOOK_DRM_NODE_RENDER]  = strdup("/dev/dri/renderD128");
   dev->available_nodes = (1 << HOOK_DRM_NODE_RENDER) | (1 << HOOK_DRM_NODE_PRIMARY);
   dev->bustype = HOOK_DRM_BUS_PLATFORM;

   dev->businfo.platform = calloc(1, sizeof(struct hook_drmPlatformBusInfo));
   strncpy(dev->businfo.platform->fullname, "/soc/mali",
           HOOK_DRM_PLATFORM_DEVICE_NAME_LEN - 1);

   dev->deviceinfo.platform = calloc(1, sizeof(struct hook_drmPlatformDeviceInfo));
   dev->deviceinfo.platform->compatible = calloc(2, sizeof(char *));
   dev->deviceinfo.platform->compatible[0] = strdup("arm,mali-valhall-csf");
   dev->deviceinfo.platform->compatible[1] = NULL;

   return dev;
}

static void free_synthetic_panthor_drm_device(struct hook_drmDevice *dev)
{
   if (!dev)
      return;
   if (dev->nodes) {
      for (int i = 0; i < HOOK_DRM_NODE_MAX; i++)
         free(dev->nodes[i]);
      free(dev->nodes);
   }
   if (dev->businfo.platform)
      free(dev->businfo.platform);
   if (dev->deviceinfo.platform) {
      if (dev->deviceinfo.platform->compatible) {
         for (int i = 0; dev->deviceinfo.platform->compatible[i]; i++)
            free(dev->deviceinfo.platform->compatible[i]);
         free(dev->deviceinfo.platform->compatible);
      }
      free(dev->deviceinfo.platform);
   }
   free(dev);
}

int panthor_kbase_drmGetDevices2(uint32_t flags, void **devices, int max_devices)
{
   (void)flags;
   __sync_fetch_and_add(&g_stats.drm_devices_enumerated, 1);
   if (!devices)
      return 1;
   if (max_devices <= 0)
      return 0;
   struct hook_drmDevice *dev = alloc_synthetic_panthor_drm_device();
   if (!dev)
      return -ENOMEM;
   devices[0] = dev;
   return 1;
}

int drmGetDevices2(uint32_t flags, void **devices, int max_devices)
{
   return panthor_kbase_drmGetDevices2(flags, devices, max_devices);
}

int drmGetDevices(void **devices, int max_devices)
{
   return panthor_kbase_drmGetDevices2(0, devices, max_devices);
}

void panthor_kbase_drmFreeDevices(void **devices, int count)
{
   if (!devices)
      return;
   for (int i = 0; i < count; i++)
      free_synthetic_panthor_drm_device((struct hook_drmDevice *)devices[i]);
}

void drmFreeDevices(void **devices, int count)
{
   panthor_kbase_drmFreeDevices(devices, count);
}

int drmGetDevice2(int fd, uint32_t flags, void **device)
{
   (void)fd;
   (void)flags;
   if (!device)
      return -EINVAL;
   *device = alloc_synthetic_panthor_drm_device();
   return *device ? 0 : -ENOMEM;
}

int drmGetDeviceFromDevId(dev_t find_rdev, uint32_t flags, void **device)
{
   (void)find_rdev;
   return drmGetDevice2(-1, flags, device);
}

void drmFreeDevice(void **device)
{
   if (device && *device) {
      free_synthetic_panthor_drm_device((struct hook_drmDevice *)*device);
      *device = NULL;
   }
}

int drmDevicesEqual(void *a, void *b)
{
   return (a != NULL && b != NULL) ? 1 : 0;
}

int drmGetNodeTypeFromFd(int fd)
{
   (void)fd;
   return HOOK_DRM_NODE_RENDER;
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
   (void)fd;
   return strdup("/dev/dri/renderD128");
}

char *drmGetDeviceNameFromFd2(int fd)
{
   (void)fd;
   return strdup("/dev/dri/renderD128");
}

/* Forward declarers for libdrm functions implemented in panthor_kbase_adapter.c */
extern void *drmGetVersion(int fd);
extern void drmFreeVersion(void *ptr);
extern int drmIoctl(int fd, unsigned long request, void *arg);
extern int drmGetCap(int fd, uint64_t capability, uint64_t *value);
extern int drmSetClientCap(int fd, uint64_t capability, uint64_t value);
extern int drmCloseBufferHandle(int fd, uint32_t handle);
extern int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
extern int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
extern int drmSyncobjDestroy(int fd, uint32_t handle);
extern int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                          int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                                  unsigned num_handles, int64_t timeout_nsec,
                                  unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                                    uint64_t *points, uint32_t handle_count);
extern int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
extern int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                            uint32_t handle_count, uint32_t flags);
extern int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                              uint32_t src_handle, uint64_t src_point, uint32_t flags);
extern int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
extern int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
extern int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
extern int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);

/* Hook dlopen so that dynamically loaded libvulkan_panfrost.so gets patched immediately */
void *panthor_kbase_hook_dlopen(const char *filename, int flags)
{
   ensure_orig_resolved();
   void *handle = orig_dlopen ? orig_dlopen(filename, flags) : NULL;
   if (handle && filename &&
       (strstr(filename, "panfrost") != NULL ||
        strstr(filename, "vulkan") != NULL ||
        strstr(filename, "mesa") != NULL)) {
      panthor_kbase_install_runtime_hooks();
   }
   return handle;
}

/* ============================================================================
 * ELF GOT / PLT Relocation Patching Engine (AArch64 & x86_64)
 * ============================================================================ */

struct hook_symbol_entry {
   const char *name;
   void *replacement;
};

static const struct hook_symbol_entry g_hook_table[] = {
   /* Syscall / Libc entrypoints used by unpatched pan_kmod.c & panvk */
   { "open",                      (void *)panthor_kbase_hook_open },
   { "open64",                    (void *)panthor_kbase_hook_open },
   { "__open_2",                  (void *)panthor_kbase_hook_open2 },
   { "close",                     (void *)panthor_kbase_hook_close },
   { "dup",                       (void *)panthor_kbase_hook_dup },
   { "fcntl",                     (void *)panthor_kbase_hook_fcntl },
   { "ioctl",                     (void *)panthor_kbase_hook_ioctl },
   { "mmap",                      (void *)panthor_kbase_hook_mmap },
   { "mmap64",                    (void *)panthor_kbase_hook_mmap },
   { "munmap",                    (void *)panthor_kbase_hook_munmap },
   { "stat",                      (void *)panthor_kbase_hook_stat },
   { "dlopen",                    (void *)panthor_kbase_hook_dlopen },

   /* libdrm device & version query entrypoints */
   { "drmIoctl",                  (void *)drmIoctl },
   { "drmGetVersion",             (void *)drmGetVersion },
   { "drmFreeVersion",            (void *)drmFreeVersion },
   { "drmGetCap",                 (void *)drmGetCap },
   { "drmSetClientCap",           (void *)drmSetClientCap },
   { "drmCloseBufferHandle",      (void *)drmCloseBufferHandle },
   { "drmCommandWrite",           (void *)drmCommandWrite },
   { "drmGetDevices2",            (void *)drmGetDevices2 },
   { "drmGetDevices",             (void *)drmGetDevices },
   { "drmFreeDevices",            (void *)drmFreeDevices },
   { "drmGetDevice2",             (void *)drmGetDevice2 },
   { "drmGetDeviceFromDevId",     (void *)drmGetDeviceFromDevId },
   { "drmFreeDevice",             (void *)drmFreeDevice },
   { "drmDevicesEqual",           (void *)drmDevicesEqual },
   { "drmGetNodeTypeFromFd",      (void *)drmGetNodeTypeFromFd },
   { "drmGetRenderDeviceNameFromFd", (void *)drmGetRenderDeviceNameFromFd },
   { "drmGetDeviceNameFromFd2",   (void *)drmGetDeviceNameFromFd2 },

   /* libdrm PRIME / dma-buf entrypoints */
   { "drmPrimeHandleToFD",        (void *)drmPrimeHandleToFD },
   { "drmPrimeFDToHandle",        (void *)drmPrimeFDToHandle },

   /* libdrm drm_syncobj entrypoints */
   { "drmSyncobjCreate",          (void *)drmSyncobjCreate },
   { "drmSyncobjDestroy",         (void *)drmSyncobjDestroy },
   { "drmSyncobjReset",           (void *)drmSyncobjReset },
   { "drmSyncobjWait",            (void *)drmSyncobjWait },
   { "drmSyncobjTimelineWait",    (void *)drmSyncobjTimelineWait },
   { "drmSyncobjSignal",          (void *)drmSyncobjSignal },
   { "drmSyncobjTimelineSignal",  (void *)drmSyncobjTimelineSignal },
   { "drmSyncobjQuery",           (void *)drmSyncobjQuery },
   { "drmSyncobjQuery2",          (void *)drmSyncobjQuery2 },
   { "drmSyncobjTransfer",        (void *)drmSyncobjTransfer },
   { "drmSyncobjExportSyncFile",  (void *)drmSyncobjExportSyncFile },
   { "drmSyncobjImportSyncFile",  (void *)drmSyncobjImportSyncFile },
   { "drmSyncobjHandleToFD",      (void *)drmSyncobjHandleToFD },
   { "drmSyncobjFDToHandle",      (void *)drmSyncobjFDToHandle },
};

static bool patch_got_slot(void **slot_addr, void *new_target)
{
   if (!slot_addr || !new_target)
      return false;

   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;

   uintptr_t addr = (uintptr_t)slot_addr;
   uintptr_t page_start = addr & ~((uintptr_t)page_size - 1ULL);

   if (mprotect((void *)page_start, (size_t)page_size,
                PROT_READ | PROT_WRITE) != 0) {
      /* Fallback in case executable permissions are also present on the segment */
      if (mprotect((void *)page_start, (size_t)page_size,
                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
         return false;
   }

   __atomic_store_n(slot_addr, new_target, __ATOMIC_SEQ_CST);
   return true;
}

static uint32_t patch_rela_table(uintptr_t base_addr,
                                 const ElfW(Rela) *rela,
                                 size_t rela_size_bytes,
                                 const ElfW(Sym) *symtab,
                                 const char *strtab)
{
   if (!rela || !symtab || !strtab || rela_size_bytes == 0)
      return 0;

   uint32_t patched = 0;
   size_t count = rela_size_bytes / sizeof(ElfW(Rela));
   for (size_t i = 0; i < count; i++) {
      uint32_t r_type = (uint32_t)ELF64_R_TYPE(rela[i].r_info);
      uint32_t r_sym  = (uint32_t)ELF64_R_SYM(rela[i].r_info);

      if (r_type != R_AARCH64_JUMP_SLOT &&
          r_type != R_AARCH64_GLOB_DAT  &&
          r_type != R_X86_64_JUMP_SLOT  &&
          r_type != R_X86_64_GLOB_DAT) {
         continue;
      }
      if (r_sym == 0)
         continue;

      const char *sym_name = strtab + symtab[r_sym].st_name;
      if (!sym_name || sym_name[0] == '\0')
         continue;

      for (size_t h = 0; h < sizeof(g_hook_table) / sizeof(g_hook_table[0]); h++) {
         if (strcmp(sym_name, g_hook_table[h].name) == 0) {
            void **got_slot = (void **)(base_addr + rela[i].r_offset);
            if (patch_got_slot(got_slot, g_hook_table[h].replacement))
               patched++;
            break;
         }
      }
   }
   return patched;
}

struct phdr_walk_ctx {
   const char *filter_substr;
   uint32_t total_patched;
};

static bool should_skip_module(const char *name)
{
   if (!name)
      return true;
   /* Never patch our own adapter, runtime hook, kbase emulator, or system libc/ld */
   if (strstr(name, "panthor_kbase_adapter") != NULL ||
       strstr(name, "panthor_kbase_runtime_hook") != NULL ||
       strstr(name, "mali_kbase_emu") != NULL ||
       strstr(name, "libc.so") != NULL ||
       strstr(name, "libdl.so") != NULL ||
       strstr(name, "libpthread.so") != NULL ||
       strstr(name, "ld-linux") != NULL ||
       strstr(name, "linker64") != NULL ||
       strstr(name, "linux-vdso") != NULL) {
      return true;
   }
   return false;
}

static bool should_hook_module(const char *name, const char *filter_substr)
{
   if (should_skip_module(name))
      return false;
   if (filter_substr && filter_substr[0] != '\0')
      return strstr(name, filter_substr) != NULL;

   /* Skip empty dlpi_name (main executable and kernel vDSO) */
   if (name[0] == '\0')
      return false;

   return (strstr(name, "vulkan") != NULL ||
           strstr(name, "panfrost") != NULL ||
           strstr(name, "panvk") != NULL ||
           strstr(name, "mesa") != NULL ||
           strstr(name, "unpatched") != NULL ||
           strstr(name, "test") != NULL);
}

static int phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
   (void)size;
   struct phdr_walk_ctx *ctx = (struct phdr_walk_ctx *)data;
   const char *mod_name = info->dlpi_name ? info->dlpi_name : "";

   g_stats.modules_scanned++;
   if (!should_hook_module(mod_name, ctx->filter_substr))
      return 0;

   const ElfW(Dyn) *dyn = NULL;
   for (int i = 0; i < (int)info->dlpi_phnum; i++) {
      if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
         dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
         break;
      }
   }
   if (!dyn)
      return 0;

   const ElfW(Sym) *symtab = NULL;
   const char *strtab = NULL;
   const ElfW(Rela) *jmprel = NULL;
   size_t pltrelsz = 0;
   const ElfW(Rela) *rela = NULL;
   size_t relasz = 0;

   for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
      uintptr_t ptr = (uintptr_t)d->d_un.d_ptr;
      /* On Android Bionic & non-relocated dynamic tables, d_ptr may be an offset */
      if (ptr != 0 && ptr < (uintptr_t)info->dlpi_addr)
         ptr += (uintptr_t)info->dlpi_addr;

      switch (d->d_tag) {
      case DT_SYMTAB:
         symtab = (const ElfW(Sym) *)ptr;
         break;
      case DT_STRTAB:
         strtab = (const char *)ptr;
         break;
      case DT_JMPREL:
         jmprel = (const ElfW(Rela) *)ptr;
         break;
      case DT_PLTRELSZ:
         pltrelsz = (size_t)d->d_un.d_val;
         break;
      case DT_RELA:
         rela = (const ElfW(Rela) *)ptr;
         break;
      case DT_RELASZ:
         relasz = (size_t)d->d_un.d_val;
         break;
      default:
         break;
      }
   }

   uint32_t mod_patched = 0;
   mod_patched += patch_rela_table(info->dlpi_addr, jmprel, pltrelsz, symtab, strtab);
   mod_patched += patch_rela_table(info->dlpi_addr, rela, relasz, symtab, strtab);

   if (mod_patched > 0) {
      g_stats.modules_hooked++;
      g_stats.got_slots_patched += mod_patched;
      ctx->total_patched += mod_patched;
   }
   return 0;
}

int panthor_kbase_patch_module_got(const char *module_substr)
{
   ensure_orig_resolved();
   pthread_mutex_lock(&g_hook_lock);
   struct phdr_walk_ctx ctx = {
      .filter_substr = module_substr,
      .total_patched = 0,
   };
   dl_iterate_phdr(phdr_callback, &ctx);
   pthread_mutex_unlock(&g_hook_lock);
   return (int)ctx.total_patched;
}

int panthor_kbase_install_runtime_hooks(void)
{
   return panthor_kbase_patch_module_got(NULL);
}

void panthor_kbase_get_hook_stats(struct panthor_kbase_hook_stats *out_stats)
{
   if (!out_stats)
      return;
   pthread_mutex_lock(&g_hook_lock);
   *out_stats = g_stats;
   pthread_mutex_unlock(&g_hook_lock);
}

void panthor_kbase_reset_hook_counters(void)
{
   pthread_mutex_lock(&g_hook_lock);
   g_stats.open_intercepts = 0;
   g_stats.ioctl_intercepts = 0;
   g_stats.mmap_intercepts = 0;
   g_stats.munmap_intercepts = 0;
   g_stats.drm_devices_enumerated = 0;
   pthread_mutex_unlock(&g_hook_lock);
}

__attribute__((constructor))
static void panthor_kbase_runtime_hook_auto_init(void)
{
   panthor_kbase_install_runtime_hooks();
}
