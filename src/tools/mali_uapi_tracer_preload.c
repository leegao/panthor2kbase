#ifndef __clang__
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
/*
 * SPDX-License-Identifier: MIT
 *
 * Generic /dev/mali0 & /dev/dri/renderD128 UAPI + CSF Ring Preload Tracer
 * (`libmali_uapi_tracer.so` - builds for both Android aarch64 and Linux x86-64)
 *
 * Usage (Android Winlator / Termux / adb shell or Linux host):
 *   export MALI_UAPI_TRACE_OUT=/sdcard/mali_uapi_trace.jsonl
 *   export LD_PRELOAD=/path/to/libmali_uapi_tracer.so
 *
 * Works with:
 *   1. `panvk-kbase-android` (Ludashi `libvulkan_panfrost.so` talking directly to `/dev/mali0`)
 *   2. Our patched `panvk` + `panthor_kbase_adapter`
 *   3. Proprietary ARM `libGLES_mali.so` Vulkan/GLES DDK
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mali_kbase_uapi.h"
#include "panthor_drm.h"

static int (*real_open_fn)(const char *, int, ...) = NULL;
static int (*real_close_fn)(int) = NULL;
static int (*real_ioctl_fn)(int, unsigned long, ...) = NULL;
static void *(*real_mmap_fn)(void *, size_t, int, int, int, off_t) = NULL;

static pthread_mutex_t g_trace_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_out_fp = NULL;
static uint64_t g_seq = 0;
static uint16_t g_kbase_ver_major = 1;
static uint16_t g_kbase_ver_minor = 21;

struct traced_queue {
   bool active;
   uint64_t ring_gpu_va;
   void *ring_cpu_ptr;
   uint32_t ring_size;
   uint8_t group_handle;
   uint8_t csi_index;
   uint64_t user_io_mmap_handle;
   void *user_io_ptr;
   uint64_t last_insert;
};

struct traced_region {
   bool active;
   uint64_t cookie_or_va;
   uint64_t gpu_va;
   void *cpu_ptr;
   size_t size;
   uint64_t flags;
};

static int g_mali_fds[32];
static struct traced_region g_regions[512];
static struct traced_queue g_queues[32];

static void init_tracer(void)
{
   if (!real_open_fn)
      real_open_fn = dlsym(RTLD_NEXT, "open");
   if (!real_close_fn)
      real_close_fn = dlsym(RTLD_NEXT, "close");
   if (!real_ioctl_fn)
      real_ioctl_fn = dlsym(RTLD_NEXT, "ioctl");
   if (!real_mmap_fn)
      real_mmap_fn = dlsym(RTLD_NEXT, "mmap");
   if (!g_out_fp) {
      const char *p = getenv("MALI_UAPI_TRACE_OUT");
      if (!p || !p[0])
         p = "artifacts/mali_uapi_trace.jsonl";
      g_out_fp = fopen(p, "a");
   }
}

static bool is_tracked_mali_fd(int fd)
{
   if (fd < 0)
      return false;
   for (size_t i = 0; i < 32; i++) {
      if (g_mali_fds[i] == fd)
         return true;
   }
   return false;
}

static void track_mali_fd(int fd)
{
   for (size_t i = 0; i < 32; i++) {
      if (g_mali_fds[i] <= 0 || g_mali_fds[i] == fd) {
         g_mali_fds[i] = fd;
         return;
      }
   }
}

static const char *kbase_nr_name(unsigned int nr)
{
   switch (nr) {
   case 0:  return "KBASE_IOCTL_VERSION_CHECK_RESERVED";
   case 1:  return "KBASE_IOCTL_SET_FLAGS";
   case 3:  return "KBASE_IOCTL_GET_GPUPROPS";
   case 5:  return "KBASE_IOCTL_MEM_ALLOC";
   case 6:  return "KBASE_IOCTL_MEM_QUERY";
   case 7:  return "KBASE_IOCTL_MEM_FREE";
   case 14: return "KBASE_IOCTL_MEM_JIT_INIT";
   case 15: return "KBASE_IOCTL_MEM_SYNC";
   case 20: return "KBASE_IOCTL_MEM_COMMIT";
   case 21: return "KBASE_IOCTL_MEM_ALIAS";
   case 22: return "KBASE_IOCTL_MEM_IMPORT";
   case 24: return "KBASE_IOCTL_STREAM_CREATE";
   case 36: return "KBASE_IOCTL_CS_QUEUE_REGISTER";
   case 37: return "KBASE_IOCTL_CS_QUEUE_KICK";
   case 38: return "KBASE_IOCTL_MEM_EXEC_INIT";
   case 39: return "KBASE_IOCTL_CS_QUEUE_BIND";
   case 41: return "KBASE_IOCTL_CS_QUEUE_TERMINATE";
   case 42: return "KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6";
   case 43: return "KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE";
   case 45: return "KBASE_IOCTL_KCPU_QUEUE_CREATE";
   case 46: return "KBASE_IOCTL_KCPU_QUEUE_DELETE";
   case 47: return "KBASE_IOCTL_KCPU_QUEUE_ENQUEUE";
   case 48: return "KBASE_IOCTL_CS_TILER_HEAP_INIT";
   case 49: return "KBASE_IOCTL_CS_TILER_HEAP_TERM";
   case 50: return "KBASE_IOCTL_GET_CPU_GPU_TIMEINFO";
   case 51: return "KBASE_IOCTL_CS_GET_GLB_IFACE";
   case 52: return "KBASE_IOCTL_VERSION_CHECK";
   case 54: return "KBASE_IOCTL_CONTEXT_PRIORITY_CHECK";
   case 58: return "KBASE_IOCTL_CS_QUEUE_GROUP_CREATE";
   case 59: return "KBASE_IOCTL_MEM_ALLOC_EX";
   default: return "KBASE_IOCTL_UNKNOWN";
   }
}

int open(const char *pathname, int flags, ...)
{
   init_tracer();
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap;
      va_start(ap, flags);
      mode = (mode_t)va_arg(ap, int);
      va_end(ap);
   }
   int fd = real_open_fn(pathname, flags, mode);
   if (fd >= 0 && pathname &&
       (strncmp(pathname, "/dev/mali", 9) == 0 ||
        strncmp(pathname, "/dev/dri/", 9) == 0)) {
      pthread_mutex_lock(&g_trace_mtx);
      track_mali_fd(fd);
      if (g_out_fp) {
         fprintf(g_out_fp,
                 "{\"seq\":%llu,\"op\":\"OPEN\",\"path\":\"%s\",\"flags\":\"0x%x\",\"fd\":%d}\n",
                 (unsigned long long)g_seq++, pathname, flags, fd);
         fflush(g_out_fp);
      }
      pthread_mutex_unlock(&g_trace_mtx);
   }
   return fd;
}

#ifdef __ANDROID__
int ioctl(int fd, int request_int, ...)
{
   unsigned long request = (unsigned long)(unsigned int)request_int;
   init_tracer();
   va_list ap;
   va_start(ap, request_int);
   void *arg = va_arg(ap, void *);
   va_end(ap);
#else
int ioctl(int fd, unsigned long request, ...)
{
   init_tracer();
   va_list ap;
   va_start(ap, request);
   void *arg = va_arg(ap, void *);
   va_end(ap);
#endif
   unsigned int type = _IOC_TYPE(request);
   unsigned int nr = _IOC_NR(request);

   /* Save input fields before kernel overwrites union in->out */
   uint64_t in_vp = 0, in_fl = 0;
   uint64_t in_buf_va = 0;
   uint32_t in_buf_sz = 0;
   uint8_t in_grp = 0, in_csi = 0, in_cs_min = 0;
   if (type == KBASE_IOCTL_TYPE && arg) {
      if (nr == 5) {
         union kbase_ioctl_mem_alloc *ma = (union kbase_ioctl_mem_alloc *)arg;
         in_vp = ma->in.va_pages; in_fl = ma->in.flags;
      } else if (nr == 59) {
         union kbase_ioctl_mem_alloc_ex *ma = (union kbase_ioctl_mem_alloc_ex *)arg;
         in_vp = ma->in.va_pages; in_fl = ma->in.flags;
      } else if (nr == 22) {
         union kbase_ioctl_mem_import *imp = (union kbase_ioctl_mem_import *)arg;
         in_fl = imp->in.flags;
      } else if (nr == 36) {
         struct kbase_ioctl_cs_queue_register *qr = (struct kbase_ioctl_cs_queue_register *)arg;
         in_buf_va = qr->buffer_gpu_addr; in_buf_sz = qr->buffer_size;
      } else if (nr == 39) {
         union kbase_ioctl_cs_queue_bind *qb = (union kbase_ioctl_cs_queue_bind *)arg;
         in_buf_va = qb->in.buffer_gpu_addr;
         in_grp = qb->in.group_handle; in_csi = qb->in.csi_index;
      } else if (nr == 37) {
         struct kbase_ioctl_cs_queue_kick *qk = (struct kbase_ioctl_cs_queue_kick *)arg;
         in_buf_va = qk->buffer_gpu_addr;
      } else if (nr == 42 || nr == 58) {
         union kbase_ioctl_cs_queue_group_create_1_6 *cg =
            (union kbase_ioctl_cs_queue_group_create_1_6 *)arg;
         in_cs_min = cg->in.cs_min;
      }
   }

   int ret = real_ioctl_fn(fd, request, arg);
   int saved_errno = errno;

   if (type == KBASE_IOCTL_TYPE && is_tracked_mali_fd(fd)) {
      pthread_mutex_lock(&g_trace_mtx);
      uint64_t out_va = 0, out_fl = 0;
      if (ret >= 0 && arg) {
         if (nr == 0 || nr == 52) {
            struct kbase_ioctl_version_check *vc = (struct kbase_ioctl_version_check *)arg;
            if (vc->major != 0) {
               g_kbase_ver_major = vc->major;
               g_kbase_ver_minor = vc->minor;
            }
         } else if (nr == 5) {
            union kbase_ioctl_mem_alloc *ma = (union kbase_ioctl_mem_alloc *)arg;
            out_va = ma->out.gpu_va; out_fl = ma->out.flags;
         } else if (nr == 59) {
            union kbase_ioctl_mem_alloc_ex *ma = (union kbase_ioctl_mem_alloc_ex *)arg;
            out_va = ma->out.gpu_va; out_fl = ma->out.flags;
         } else if (nr == 22) {
            union kbase_ioctl_mem_import *imp = (union kbase_ioctl_mem_import *)arg;
            out_va = imp->out.gpu_va; out_fl = imp->out.flags;
            in_vp = imp->out.va_pages;
         } else if (nr == 39) {
            union kbase_ioctl_cs_queue_bind *qb = (union kbase_ioctl_cs_queue_bind *)arg;
            out_va = qb->out.mmap_handle;
         } else if (nr == 42 || nr == 58) {
            union kbase_ioctl_cs_queue_group_create_1_6 *cg =
               (union kbase_ioctl_cs_queue_group_create_1_6 *)arg;
            out_va = cg->out.group_handle;
            in_fl = in_cs_min;
         }
         if (nr == 5 || nr == 59 || nr == 22) {
            for (size_t i = 0; i < 512; i++) {
               if (!g_regions[i].active) {
                  g_regions[i].active = true;
                  g_regions[i].cookie_or_va = out_va;
                  g_regions[i].gpu_va = out_va;
                  g_regions[i].size = (size_t)in_vp * 4096U;
                  g_regions[i].flags = out_fl ? out_fl : in_fl;
                  break;
               }
            }
         } else if (nr == 36) {
            for (size_t i = 0; i < 32; i++) {
               if (!g_queues[i].active) {
                  g_queues[i].active = true;
                  g_queues[i].ring_gpu_va = in_buf_va;
                  g_queues[i].ring_size = in_buf_sz;
                  for (size_t r = 0; r < 512; r++) {
                     if (g_regions[r].active && g_regions[r].gpu_va == in_buf_va) {
                        g_queues[i].ring_cpu_ptr = g_regions[r].cpu_ptr;
                        break;
                     }
                  }
                  break;
               }
            }
         } else if (nr == 39) {
            for (size_t i = 0; i < 32; i++) {
               if (g_queues[i].active && g_queues[i].ring_gpu_va == in_buf_va) {
                  g_queues[i].group_handle = in_grp;
                  g_queues[i].csi_index = in_csi;
                  g_queues[i].user_io_mmap_handle = out_va;
                  break;
               }
            }
         }
      }

      if (g_out_fp) {
         fprintf(g_out_fp,
                 "{\"seq\":%llu,\"op\":\"%s\",\"nr\":%u,\"va_pages\":%llu,"
                 "\"in_flags\":\"0x%llx\",\"out_flags\":\"0x%llx\","
                 "\"buf_va\":\"0x%llx\",\"out_va\":\"0x%llx\",\"ret\":%d",
                 (unsigned long long)g_seq++, kbase_nr_name(nr), nr,
                 (unsigned long long)in_vp,
                 (unsigned long long)in_fl,
                 (unsigned long long)out_fl,
                 (unsigned long long)in_buf_va,
                 (unsigned long long)out_va, ret);

         /* On CS_QUEUE_KICK, dump emitted 64-bit CSF ring instructions.
          * On kbase >= 1.35 (MALI_KBASE_CAP_CSG_USER_IO_ALLOCATION, e.g. Pixel 8 Pro husky 1.38),
          * CS_INSERT and CS_EXTRACT are indexed by csi_index * 16 inside the CSG-shared USER_IO pages! */
         if (nr == 37 && ret == 0) {
            for (size_t i = 0; i < 32; i++) {
               if (g_queues[i].active && g_queues[i].ring_gpu_va == in_buf_va &&
                   g_queues[i].user_io_ptr && g_queues[i].ring_cpu_ptr) {
                  size_t csi_off = (g_kbase_ver_major > 1 || (g_kbase_ver_major == 1 && g_kbase_ver_minor >= 35))
                                      ? ((size_t)g_queues[i].csi_index * 16U)
                                      : 0U;
                  uint64_t ins = *(volatile uint64_t *)((uint8_t *)g_queues[i].user_io_ptr + 0x1000 + csi_off);
                  uint64_t ext = *(volatile uint64_t *)((uint8_t *)g_queues[i].user_io_ptr + 0x2000 + csi_off);
                  uint64_t prev = g_queues[i].last_insert;
                  fprintf(g_out_fp, ",\"csi_index\":%u,\"cs_insert\":%llu,\"cs_extract\":%llu",
                          (unsigned)g_queues[i].csi_index,
                          (unsigned long long)ins,
                          (unsigned long long)ext);
                  if (ins > prev && g_queues[i].ring_size > 0) {
                     uint32_t off = (uint32_t)(prev % g_queues[i].ring_size);
                     uint32_t qwords = (uint32_t)((ins - prev) / 8U);
                     if (qwords > 32)
                        qwords = 32;
                     const uint64_t *ring_q =
                        (const uint64_t *)((const uint8_t *)g_queues[i].ring_cpu_ptr + off);
                     fprintf(g_out_fp, ",\"ring_qwords\":[");
                     for (uint32_t q = 0; q < qwords; q++) {
                        fprintf(g_out_fp, "\"0x%016llx\"%s",
                                (unsigned long long)ring_q[q],
                                (q + 1 < qwords) ? "," : "");
                     }
                     fprintf(g_out_fp, "]");
                     g_queues[i].last_insert = ins;
                  }
                  break;
               }
            }
         }
         fprintf(g_out_fp, "}\n");
         fflush(g_out_fp);
      }
      pthread_mutex_unlock(&g_trace_mtx);
   }

   errno = saved_errno;
   return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   init_tracer();
   void *res = real_mmap_fn(addr, length, prot, flags, fd, offset);
   if (res != MAP_FAILED && is_tracked_mali_fd(fd)) {
      uint64_t uoff = (uint64_t)offset;
      pthread_mutex_lock(&g_trace_mtx);
      for (size_t i = 0; i < 512; i++) {
         if (g_regions[i].active && g_regions[i].cookie_or_va == uoff) {
            g_regions[i].cpu_ptr = res;
            if (g_regions[i].flags & BASE_MEM_SAME_VA)
               g_regions[i].gpu_va = (uintptr_t)res;
            break;
         }
      }
      for (size_t i = 0; i < 32; i++) {
         if (g_queues[i].active && g_queues[i].user_io_mmap_handle == uoff) {
            g_queues[i].user_io_ptr = res;
            break;
         }
      }
      if (g_out_fp) {
         fprintf(g_out_fp,
                 "{\"seq\":%llu,\"op\":\"MMAP\",\"fd\":%d,\"len\":%zu,\"prot\":\"0x%x\","
                 "\"offset\":\"0x%llx\",\"va\":\"0x%llx\"}\n",
                 (unsigned long long)g_seq++, fd, length, prot,
                 (unsigned long long)uoff,
                 (unsigned long long)(uintptr_t)res);
         fflush(g_out_fp);
      }
      pthread_mutex_unlock(&g_trace_mtx);
   }
   return res;
}
