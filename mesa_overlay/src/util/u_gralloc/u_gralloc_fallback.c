/*
 * Mesa 3-D graphics library
 *
 * Copyright © 2021, Google Inc.
 * SPDX-License-Identifier: MIT
 */

#include "u_gralloc_internal.h"

#include <hardware/gralloc.h>

#include "drm-uapi/drm_fourcc.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/u_memory.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Vendor-neutral Android Gralloc4 / Gralloc5 metadata query via
 * AIMapper v5 C ABI or HIDL IMapper v4.0 passthrough C++ vtable ABI.
 *
 * Inside isolated linker namespaces (such as libadrenotools's
 * adrenotools-driver / sphal namespace inside untrusted_app processes like
 * Winlator), GraphicBufferMapper::get() in /system/lib64/libui.so fails
 * because AServiceManager_openDeclaredPassthroughHal is SELinux-denied on
 * hal_graphics_mapper_service and libui.so does not fall back to loading
 * the passthrough mapper via android_load_sphal_library or direct dlopen.
 *
 * This module implements:
 *  1. Multi-FD dma-buf discovery (panvk_v19_find_dmabuf_fd_index) for vendor
 *     handles where data[0] is metadata and data[k] is the seekable dma-buf.
 *  2. AIMapper v5 C ABI direct query (AServiceManager_openDeclaredPassthroughHal,
 *     android_load_sphal_library, and direct dlopen across /vendor/lib64/hw).
 *  3. HIDL IMapper@4.0 raw vtable query on __aarch64__ (HIDL_FETCH_IMapper).
 *  4. Refusal to return DRM_FORMAT_MOD_INVALID as success (returning -ENOTSUP
 *     so vk_android_get_ahb_layout can invoke its linear probe/fallback).
 */

int
panvk_v19_find_dmabuf_fd_index(const native_handle_t *handle)
{
   if (!handle || handle->numFds <= 0)
      return -1;

   for (int i = 0; i < handle->numFds; i++) {
      if (handle->data[i] >= 0 && lseek(handle->data[i], 0, SEEK_END) > 0)
         return i;
   }
   return 0;
}

typedef int32_t (*pfn_aimapper_import)(const native_handle_t *handle,
                                       buffer_handle_t *out_handle);
typedef int32_t (*pfn_aimapper_free)(buffer_handle_t handle);
typedef int32_t (*pfn_aimapper_get_meta)(buffer_handle_t handle,
                                         int64_t metadata_type,
                                         void *out_data,
                                         size_t out_data_size);

struct aimapper_v5_callbacks {
   pfn_aimapper_import importBuffer;
   pfn_aimapper_free freeBuffer;
   void *getTransportSize;
   void *lock;
   void *unlock;
   void *flushLockedBuffer;
   void *rereadLockedBuffer;
   pfn_aimapper_get_meta getMetadata;
   void *getStandardMetadata;
   void *setMetadata;
   void *setStandardMetadata;
   void *listSupportedMetadataTypes;
   void *dumpBuffer;
   void *getReservedRegion;
};

struct aimapper {
   uint32_t version;
   struct aimapper_v5_callbacks v5;
};

typedef int32_t (*pfn_AIMapper_loadIMapper)(struct aimapper **out_impl);

enum {
   AIMAPPER_METADATA_LAYER_COUNT           = 5,
   AIMAPPER_METADATA_PIXEL_FORMAT_FOURCC   = 20,
   AIMAPPER_METADATA_PIXEL_FORMAT_MODIFIER = 21,
   AIMAPPER_METADATA_ALLOCATION_SIZE       = 23,
   AIMAPPER_METADATA_PLANE_LAYOUTS         = 24,
};

#define GRALLOC4_STANDARD_METADATA_PREFIX "android.hardware.graphics.common.StandardMetadataType"

struct hidl_string {
   const char *mBuffer;
   uint32_t mSize;
   uint32_t mPad;
};

struct hidl_vec_u8 {
   uint8_t *mBuffer;
   uint32_t mSize;
   uint8_t mOwnsBuffer;
   uint8_t mPad[3];
};

struct hidl_handle_wrap {
   const void *vptr;
   const native_handle_t *mHandle;
   uint8_t mOwnsHandle;
   uint8_t mPad[7];
};

struct imapper_metadata_type {
   struct hidl_string name;
   int64_t value;
};

struct hidl_return_status {
   uint8_t storage[64];
};

#if defined(__aarch64__)
struct get_cb_state {
   int32_t error;
   uint8_t *buf;
   size_t size;
   size_t cap;
};

static void
imapper4_cb_destroy(void *self)
{
   (void)self;
}

static void
imapper4_cb_invoke(void *self, const int32_t *err_ptr,
                   const struct hidl_vec_u8 *vec)
{
   struct get_cb_state *st = *(struct get_cb_state **)((char *)self + 8);
   if (!st)
      return;
   st->error = err_ptr ? *err_ptr : -1;
   if (st->error == 0 && vec && vec->mBuffer && vec->mSize > 0) {
      size_t n = vec->mSize < st->cap ? vec->mSize : st->cap;
      memcpy(st->buf, vec->mBuffer, n);
      st->size = n;
   }
}

static const void *imapper4_cb_vtable[7] = {
   NULL,
   NULL,
   (const void *)imapper4_cb_destroy,
   (const void *)imapper4_cb_destroy,
   (const void *)imapper4_cb_invoke,
   NULL,
   NULL,
};

/* Raw vtable indices for android::hardware::graphics::mapper::V4_0::IMapper:
 *   15: createDescriptor
 *   16: importBuffer(const hidl_handle&, importBuffer_cb) -> Return<void>
 *   17: freeBuffer(void* buffer) -> Return<Error>
 *   18: validateBufferSize
 *   19: getTransportSize
 *   20: lock
 *   21: get(void* buffer, const MetadataType&, get_cb) -> Return<void>
 */
enum {
   IMAPPER4_VTABLE_IMPORT_BUFFER = 16,
   IMAPPER4_VTABLE_FREE_BUFFER   = 17,
   IMAPPER4_VTABLE_GET           = 21,
};

struct import_cb_state {
   int32_t error;
   void *buffer;
};

static void
imapper4_import_cb_invoke(void *self, const int32_t *err_ptr,
                          void * const *buf_ptr)
{
   struct import_cb_state *st = *(struct import_cb_state **)((char *)self + 8);
   if (!st)
      return;
   st->error = err_ptr ? *err_ptr : -1;
   st->buffer = (st->error == 0 && buf_ptr) ? *buf_ptr : NULL;
}

static const void *imapper4_import_cb_vtable[7] = {
   NULL,
   NULL,
   (const void *)imapper4_cb_destroy,
   (const void *)imapper4_cb_destroy,
   (const void *)imapper4_import_cb_invoke,
   NULL,
   NULL,
};

typedef void (*pfn_imapper4_import)(void *this_ptr,
                                    const struct hidl_handle_wrap *raw_handle,
                                    void *std_function_cb);
typedef void (*pfn_imapper4_free)(void *this_ptr, void *buffer);
typedef void (*pfn_imapper4_get)(void *this_ptr, void *buffer,
                                 const struct imapper_metadata_type *type,
                                 void *std_function_cb);

static inline void
call_imapper4_import(pfn_imapper4_import fn, void *this_ptr,
                     const struct hidl_handle_wrap *raw_handle,
                     void *std_function_cb,
                     struct hidl_return_status *out_ret)
{
   register void *x0 __asm__("x0") = this_ptr;
   register const struct hidl_handle_wrap *x1 __asm__("x1") = raw_handle;
   register void *x2 __asm__("x2") = std_function_cb;
   register struct hidl_return_status *x8 __asm__("x8") = out_ret;
   __asm__ volatile("blr %4"
                    : "+r"(x0)
                    : "r"(x1), "r"(x2), "r"(x8), "r"(fn)
                    : "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11",
                      "x12", "x13", "x14", "x15", "x16", "x17", "x30",
                      "memory", "cc");
}

static inline void
call_imapper4_free(pfn_imapper4_free fn, void *this_ptr, void *buffer,
                   struct hidl_return_status *out_ret)
{
   register void *x0 __asm__("x0") = this_ptr;
   register void *x1 __asm__("x1") = buffer;
   register struct hidl_return_status *x8 __asm__("x8") = out_ret;
   __asm__ volatile("blr %3"
                    : "+r"(x0)
                    : "r"(x1), "r"(x8), "r"(fn)
                    : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10",
                      "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                      "x30", "memory", "cc");
}

static inline void
call_imapper4_get(pfn_imapper4_get fn, void *this_ptr, void *buffer,
                  const struct imapper_metadata_type *type,
                  void *std_function_cb,
                  struct hidl_return_status *out_ret)
{
   register void *x0 __asm__("x0") = this_ptr;
   register void *x1 __asm__("x1") = buffer;
   register const struct imapper_metadata_type *x2 __asm__("x2") = type;
   register void *x3 __asm__("x3") = std_function_cb;
   register struct hidl_return_status *x8 __asm__("x8") = out_ret;
   __asm__ volatile("blr %5"
                    : "+r"(x0)
                    : "r"(x1), "r"(x2), "r"(x3), "r"(x8), "r"(fn)
                    : "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12",
                      "x13", "x14", "x15", "x16", "x17", "x30", "memory",
                      "cc");
}

static int
imapper4_import_buffer(void *imapper_obj, const native_handle_t *raw,
                       void **out_buf)
{
   void **vptr = *(void ***)imapper_obj;
   pfn_imapper4_import fn = (pfn_imapper4_import)vptr[IMAPPER4_VTABLE_IMPORT_BUFFER];
   struct hidl_handle_wrap wrap;
   memset(&wrap, 0, sizeof(wrap));
   wrap.mHandle = raw;
   wrap.mOwnsHandle = 0;

   struct import_cb_state st = { .error = -1, .buffer = NULL };
   void *cb_obj[4];
   cb_obj[0] = (void *)&imapper4_import_cb_vtable[2];
   cb_obj[1] = &st;
   cb_obj[2] = NULL;
   cb_obj[3] = &cb_obj[0];

   struct hidl_return_status ret_status;
   memset(&ret_status, 0, sizeof(ret_status));
   call_imapper4_import(fn, imapper_obj, &wrap, cb_obj, &ret_status);
   if (st.error != 0 || !st.buffer)
      return -1;
   *out_buf = st.buffer;
   return 0;
}

static void
imapper4_free_buffer(void *imapper_obj, void *buffer)
{
   void **vptr = *(void ***)imapper_obj;
   pfn_imapper4_free fn = (pfn_imapper4_free)vptr[IMAPPER4_VTABLE_FREE_BUFFER];
   struct hidl_return_status ret_status;
   memset(&ret_status, 0, sizeof(ret_status));
   call_imapper4_free(fn, imapper_obj, buffer, &ret_status);
}

static ssize_t
imapper4_get_metadata(void *imapper_obj, void *buffer, int64_t type_val,
                      uint8_t *out_buf, size_t out_cap)
{
   void **vptr = *(void ***)imapper_obj;
   pfn_imapper4_get fn = (pfn_imapper4_get)vptr[IMAPPER4_VTABLE_GET];
   struct imapper_metadata_type mtype;
   mtype.name.mBuffer = GRALLOC4_STANDARD_METADATA_PREFIX;
   mtype.name.mSize = sizeof(GRALLOC4_STANDARD_METADATA_PREFIX) - 1;
   mtype.name.mPad = 0;
   mtype.value = type_val;

   struct get_cb_state st = {
      .error = -1,
      .buf = out_buf,
      .size = 0,
      .cap = out_cap,
   };
   void *cb_obj[4];
   cb_obj[0] = (void *)&imapper4_cb_vtable[2];
   cb_obj[1] = &st;
   cb_obj[2] = NULL;
   cb_obj[3] = &cb_obj[0];

   struct hidl_return_status ret_status;
   memset(&ret_status, 0, sizeof(ret_status));
   call_imapper4_get(fn, imapper_obj, buffer, &mtype, cb_obj, &ret_status);
   if (st.error != 0)
      return -1;
   return (ssize_t)st.size;
}
#endif /* __aarch64__ */

/* Byte-stream decoder for AIMapper v5 and Gralloc4 ByteStream PlaneLayouts. */
static int
parse_plane_layouts(const uint8_t *buf, size_t len, bool has_gralloc4_header,
                    int *out_num_planes, int out_offsets[4], int out_strides[4])
{
   size_t pos = 0;
   if (has_gralloc4_header) {
      if (len < 12)
         return -1;
      int64_t status = 0;
      memcpy(&status, buf, 8);
      if (status != 0)
         return -1;
      pos = 8;
   }

   if (pos + 8 > len)
      return -1;
   int64_t num_planes = 0;
   memcpy(&num_planes, buf + pos, 8);
   pos += 8;

   if (num_planes <= 0 || num_planes > 4)
      return -1;

   for (int p = 0; p < (int)num_planes; p++) {
      if (pos + 8 > len)
         return -1;
      int64_t num_components = 0;
      memcpy(&num_components, buf + pos, 8);
      pos += 8;
      if (num_components < 0 || num_components > 16)
         return -1;
      for (int c = 0; c < (int)num_components; c++) {
         if (pos + 4 > len)
            return -1;
         uint32_t str_len = 0;
         memcpy(&str_len, buf + pos, 4);
         pos += 4;
         if (str_len > len - pos)
            return -1;
         pos += str_len;
         /* 8 + 8 + 8 = 24 bytes: type value (int64), offsetInBits (int64), sizeInBits (int64) */
         if (pos + 24 > len)
            return -1;
         pos += 24;
      }
      /* 8 integers of 8 bytes = 64 bytes:
       *   [0] offsetInBytes
       *   [1] sampleIncrementInBits
       *   [2] strideInBytes
       *   [3] widthInSamples
       *   [4] heightInSamples
       *   [5] totalSizeInBytes
       *   [6] horizontalSubsampling
       *   [7] verticalSubsampling
       */
      if (pos + 64 > len)
         return -1;
      int64_t offset_bytes = 0;
      int64_t stride_bytes = 0;
      memcpy(&offset_bytes, buf + pos + 0, 8);
      memcpy(&stride_bytes, buf + pos + 16, 8);
      pos += 64;
      out_offsets[p] = (int)offset_bytes;
      out_strides[p] = (int)stride_bytes;
   }

   *out_num_planes = (int)num_planes;
   return 0;
}

static pthread_once_t g_mapper_once = PTHREAD_ONCE_INIT;
static struct aimapper *g_aimapper = NULL;
#if defined(__aarch64__)
static void *g_imapper4 = NULL;
#endif

static void *
load_vendor_hal_lib(const char *rel_or_abs_name)
{
   void *lib = NULL;
   typedef void *(*pfn_load_sphal)(const char *, int);
   pfn_load_sphal load_sphal =
      (pfn_load_sphal)dlsym(RTLD_DEFAULT, "android_load_sphal_library");
   if (load_sphal)
      lib = load_sphal(rel_or_abs_name, RTLD_NOW | RTLD_LOCAL);
   if (!lib)
      lib = dlopen(rel_or_abs_name, RTLD_NOW | RTLD_LOCAL);
   return lib;
}

static bool
try_load_aimapper_from_lib(const char *path)
{
   void *lib = load_vendor_hal_lib(path);
   if (!lib)
      return false;
   pfn_AIMapper_loadIMapper load_fn =
      (pfn_AIMapper_loadIMapper)dlsym(lib, "AIMapper_loadIMapper");
   if (!load_fn) {
      dlclose(lib);
      return false;
   }
   struct aimapper *m = NULL;
   if (load_fn(&m) == 0 && m && m->version >= 5 &&
       m->v5.importBuffer && m->v5.freeBuffer && m->v5.getMetadata) {
      g_aimapper = m;
      mesa_logi("u_gralloc: loaded AIMapper v%u from %s", m->version, path);
      return true;
   }
   dlclose(lib);
   return false;
}

#if defined(__aarch64__)
static bool
try_load_imapper4_from_lib(const char *path)
{
   void *lib = load_vendor_hal_lib(path);
   if (!lib)
      return false;
   typedef void *(*pfn_fetch)(const char *);
   pfn_fetch fetch_fn = (pfn_fetch)dlsym(lib, "HIDL_FETCH_IMapper");
   if (!fetch_fn) {
      dlclose(lib);
      return false;
   }
   void *obj = fetch_fn("default");
   if (obj) {
      g_imapper4 = obj;
      mesa_logi("u_gralloc: loaded HIDL IMapper@4.0 from %s", path);
      return true;
   }
   dlclose(lib);
   return false;
}
#endif

static void
init_vendor_mapper_once(void)
{
   typedef int (*pfn_prop_get)(const char *, char *);
   pfn_prop_get prop_get =
      (pfn_prop_get)dlsym(RTLD_DEFAULT, "__system_property_get");

   char prop_gralloc[96] = {0};
   char prop_hardware[96] = {0};
   char prop_platform[96] = {0};
   char prop_soc[96] = {0};
   if (prop_get) {
      prop_get("ro.hardware.gralloc", prop_gralloc);
      prop_get("ro.hardware", prop_hardware);
      prop_get("ro.board.platform", prop_platform);
      prop_get("ro.soc.model", prop_soc);
   }

   const char *candidates[] = {
      prop_gralloc[0] ? prop_gralloc : NULL,
      prop_hardware[0] ? prop_hardware : NULL,
      prop_platform[0] ? prop_platform : NULL,
      prop_soc[0] ? prop_soc : NULL,
      "mali",
      "mtk",
      "mt6989",
      "mt6897",
      "mt6991",
      "mt6895",
      "mt6985",
      "mt6983",
      "pixel",
      "gs101",
      "gs201",
      "zuma",
      "zumapro",
      "exynos",
      "default",
      "arm",
      "ranchu",
      "minigbm",
   };

   /* 1. Try AServiceManager_openDeclaredPassthroughHal if available and allowed */
   typedef void *(*pfn_open_hal)(const char *, const char *, int);
   pfn_open_hal open_hal = (pfn_open_hal)dlsym(
      RTLD_DEFAULT, "AServiceManager_openDeclaredPassthroughHal");
   if (open_hal) {
      for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
         if (!candidates[i])
            continue;
         void *lib = open_hal("mapper", candidates[i], RTLD_LOCAL | RTLD_NOW);
         if (!lib)
            continue;
         pfn_AIMapper_loadIMapper load_fn =
            (pfn_AIMapper_loadIMapper)dlsym(lib, "AIMapper_loadIMapper");
         if (load_fn) {
            struct aimapper *m = NULL;
            if (load_fn(&m) == 0 && m && m->version >= 5 &&
                m->v5.importBuffer && m->v5.freeBuffer && m->v5.getMetadata) {
               g_aimapper = m;
               mesa_logi("u_gralloc: loaded AIMapper v%u via passthrough HAL '%s'",
                         m->version, candidates[i]);
               return;
            }
         }
      }
   }

   /* 2. Try loading AIMapper v5 directly via sphal / dlopen */
   const char *hw_dirs[] = {
      "/vendor/lib64/hw",
      "/odm/lib64/hw",
      "/system/lib64/hw",
      NULL,
   };

   for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
      if (!candidates[i])
         continue;
      char rel_name[160];
      snprintf(rel_name, sizeof(rel_name), "hw/mapper.%s.so", candidates[i]);
      if (try_load_aimapper_from_lib(rel_name))
         return;
      for (size_t d = 0; hw_dirs[d]; d++) {
         char abs_path[256];
         snprintf(abs_path, sizeof(abs_path), "%s/mapper.%s.so",
                  hw_dirs[d], candidates[i]);
         if (try_load_aimapper_from_lib(abs_path))
            return;
      }
   }

   for (size_t d = 0; hw_dirs[d]; d++) {
      DIR *dir = opendir(hw_dirs[d]);
      if (!dir)
         continue;
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
         if (strncmp(de->d_name, "mapper.", 7) == 0 &&
             strstr(de->d_name, ".so") != NULL) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", hw_dirs[d], de->d_name);
            if (try_load_aimapper_from_lib(full)) {
               closedir(dir);
               return;
            }
         }
      }
      closedir(dir);
   }

#if defined(__aarch64__)
   /* 3. Try HIDL IMapper@4.0 raw vtable on aarch64 */
   const char *hidl_names[] = {
      "hw/android.hardware.graphics.mapper@4.0-impl.so",
      "hw/android.hardware.graphics.mapper@4.0-impl-mali.so",
      "hw/android.hardware.graphics.mapper@4.0-impl-mtk.so",
      "hw/android.hardware.graphics.mapper@4.0-impl-arm.so",
      "hw/android.hardware.graphics.mapper@4.0-impl-pixel.so",
      "/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.so",
      "/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mali.so",
      "/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mtk.so",
      "/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-arm.so",
      "/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-pixel.so",
   };
   for (size_t i = 0; i < ARRAY_SIZE(hidl_names); i++) {
      if (try_load_imapper4_from_lib(hidl_names[i]))
         return;
   }

   for (size_t d = 0; hw_dirs[d]; d++) {
      DIR *dir = opendir(hw_dirs[d]);
      if (!dir)
         continue;
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
         if (strncmp(de->d_name, "android.hardware.graphics.mapper@4.0-impl", 41) == 0 &&
             strstr(de->d_name, ".so") != NULL) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", hw_dirs[d], de->d_name);
            if (try_load_imapper4_from_lib(full)) {
               closedir(dir);
               return;
            }
         }
      }
      closedir(dir);
   }
#endif
}

int
panvk_v19_query_mapper(const native_handle_t *raw,
                       struct u_gralloc_buffer_basic_info *out)
{
   if (!raw || raw->numFds <= 0 || !out)
      return -1;

   pthread_once(&g_mapper_once, init_vendor_mapper_once);

   int base_fd_idx = panvk_v19_find_dmabuf_fd_index(raw);
   if (base_fd_idx < 0)
      base_fd_idx = 0;

   if (g_aimapper) {
      buffer_handle_t imported = NULL;
      if (g_aimapper->v5.importBuffer(raw, &imported) == 0 && imported) {
         uint32_t fourcc = 0;
         uint64_t modifier = DRM_FORMAT_MOD_INVALID;
         uint64_t alloc_size = 0;
         uint64_t layer_count = 1;
         uint8_t layouts_buf[2048];

         int32_t sz_fourcc = g_aimapper->v5.getMetadata(
            imported, AIMAPPER_METADATA_PIXEL_FORMAT_FOURCC, &fourcc, sizeof(fourcc));
         int32_t sz_mod = g_aimapper->v5.getMetadata(
            imported, AIMAPPER_METADATA_PIXEL_FORMAT_MODIFIER, &modifier, sizeof(modifier));
         int32_t sz_alloc = g_aimapper->v5.getMetadata(
            imported, AIMAPPER_METADATA_ALLOCATION_SIZE, &alloc_size, sizeof(alloc_size));
         int32_t sz_layers = g_aimapper->v5.getMetadata(
            imported, AIMAPPER_METADATA_LAYER_COUNT, &layer_count, sizeof(layer_count));
         int32_t sz_layouts = g_aimapper->v5.getMetadata(
            imported, AIMAPPER_METADATA_PLANE_LAYOUTS, layouts_buf, sizeof(layouts_buf));

         g_aimapper->v5.freeBuffer(imported);

         int num_planes = 0;
         int offsets[4] = {0};
         int strides[4] = {0};
         if (sz_fourcc == (int32_t)sizeof(fourcc) &&
             sz_mod == (int32_t)sizeof(modifier) &&
             modifier != DRM_FORMAT_MOD_INVALID &&
             sz_layouts > 0 && sz_layouts <= (int32_t)sizeof(layouts_buf) &&
             parse_plane_layouts(layouts_buf, (size_t)sz_layouts, false,
                                 &num_planes, offsets, strides) == 0) {
            out->drm_fourcc = fourcc;
            out->modifier = modifier;
            out->alloc_size = (sz_alloc == (int32_t)sizeof(alloc_size)) ? alloc_size : 0;
            out->layer_count = (sz_layers == (int32_t)sizeof(layer_count) && layer_count > 0)
                                  ? layer_count : 1;
            out->num_planes = num_planes;
            int fd_index = base_fd_idx;
            for (int i = 0; i < num_planes; i++) {
               out->offsets[i] = offsets[i];
               out->strides[i] = strides[i];
               if (offsets[i] == 0 && i > 0 && (fd_index + 1) < raw->numFds)
                  fd_index++;
               out->fds[i] = raw->data[fd_index];
            }
            return 0;
         }
      }
   }

#if defined(__aarch64__)
   if (g_imapper4) {
      void *imported = NULL;
      if (imapper4_import_buffer(g_imapper4, raw, &imported) == 0 && imported) {
         uint8_t buf_fourcc[32];
         uint8_t buf_mod[32];
         uint8_t buf_alloc[32];
         uint8_t buf_layers[32];
         uint8_t buf_layouts[2048];

         ssize_t n_fourcc = imapper4_get_metadata(
            g_imapper4, imported, AIMAPPER_METADATA_PIXEL_FORMAT_FOURCC,
            buf_fourcc, sizeof(buf_fourcc));
         ssize_t n_mod = imapper4_get_metadata(
            g_imapper4, imported, AIMAPPER_METADATA_PIXEL_FORMAT_MODIFIER,
            buf_mod, sizeof(buf_mod));
         ssize_t n_alloc = imapper4_get_metadata(
            g_imapper4, imported, AIMAPPER_METADATA_ALLOCATION_SIZE,
            buf_alloc, sizeof(buf_alloc));
         ssize_t n_layers = imapper4_get_metadata(
            g_imapper4, imported, AIMAPPER_METADATA_LAYER_COUNT,
            buf_layers, sizeof(buf_layers));
         ssize_t n_layouts = imapper4_get_metadata(
            g_imapper4, imported, AIMAPPER_METADATA_PLANE_LAYOUTS,
            buf_layouts, sizeof(buf_layouts));

         imapper4_free_buffer(g_imapper4, imported);

         int64_t st_fourcc = -1, st_mod = -1;
         if (n_fourcc >= 12)
            memcpy(&st_fourcc, buf_fourcc, 8);
         if (n_mod >= 16)
            memcpy(&st_mod, buf_mod, 8);

         int num_planes = 0;
         int offsets[4] = {0};
         int strides[4] = {0};
         if (st_fourcc == 0 && st_mod == 0 && n_layouts > 12 &&
             parse_plane_layouts(buf_layouts, (size_t)n_layouts, true,
                                 &num_planes, offsets, strides) == 0) {
            uint32_t fourcc = 0;
            uint64_t modifier = DRM_FORMAT_MOD_INVALID;
            memcpy(&fourcc, buf_fourcc + 8, 4);
            memcpy(&modifier, buf_mod + 8, 8);
            if (modifier != DRM_FORMAT_MOD_INVALID) {
               out->drm_fourcc = fourcc;
               out->modifier = modifier;
               out->alloc_size = 0;
               if (n_alloc >= 16) {
                  int64_t st_a = -1;
                  memcpy(&st_a, buf_alloc, 8);
                  if (st_a == 0)
                     memcpy(&out->alloc_size, buf_alloc + 8, 8);
               }
               out->layer_count = 1;
               if (n_layers >= 16) {
                  int64_t st_l = -1;
                  memcpy(&st_l, buf_layers, 8);
                  if (st_l == 0)
                     memcpy(&out->layer_count, buf_layers + 8, 8);
               }
               out->num_planes = num_planes;
               int fd_index = base_fd_idx;
               for (int i = 0; i < num_planes; i++) {
                  out->offsets[i] = offsets[i];
                  out->strides[i] = strides[i];
                  if (offsets[i] == 0 && i > 0 && (fd_index + 1) < raw->numFds)
                     fd_index++;
                  out->fds[i] = raw->data[fd_index];
               }
               return 0;
            }
         }
      }
   }
#endif

   return -1;
}

struct fallback_gralloc {
   struct u_gralloc base;
   gralloc_module_t *gralloc_module;
};

/* returns # of fds, and by reference the actual fds */
static unsigned
get_native_buffer_fds(const native_handle_t *handle, int fds[3])
{
   if (!handle)
      return 0;

   /*
    * Various gralloc implementations exist, but the dma-buf fd tends
    * to be first. Access it directly to avoid a dependency on specific
    * gralloc versions.
    */
   for (int i = 0; i < handle->numFds && i < 3; i++)
      fds[i] = handle->data[i];

   return handle->numFds < 3 ? handle->numFds : 3;
}

static int
fallback_gralloc_get_yuv_info(struct u_gralloc *gralloc,
                              struct u_gralloc_buffer_handle *hnd,
                              struct u_gralloc_buffer_basic_info *out)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   gralloc_module_t *gr_mod = gr->gralloc_module;
   struct android_ycbcr ycbcr;
   int num_fds = 0;
   int fds[3];
   int ret;

   num_fds = get_native_buffer_fds(hnd->handle, fds);
   if (num_fds == 0)
      return -EINVAL;

   if (!gr_mod || !gr_mod->lock_ycbcr) {
      return -EINVAL;
   }

   memset(&ycbcr, 0, sizeof(ycbcr));
   ret = gr_mod->lock_ycbcr(gr_mod, hnd->handle, 0, 0, 0, 0, 0, &ycbcr);
   if (ret) {
      /* HACK: See native_window_buffer_get_buffer_info() and
       * https://issuetracker.google.com/32077885.*/
      if (hnd->hal_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
         return -EAGAIN;

      mesa_logw("gralloc->lock_ycbcr failed: %d", ret);
      return -EINVAL;
   }
   gr_mod->unlock(gr_mod, hnd->handle);

   ret = bufferinfo_from_ycbcr(&ycbcr, hnd, out);
   if (ret)
      return ret;

   /*
    * Since this is EGL_NATIVE_BUFFER_ANDROID don't assume that
    * the single-fd case cannot happen.  So handle eithe single
    * fd or fd-per-plane case:
    */
   if (num_fds == 1) {
      out->fds[1] = out->fds[0] = fds[0];
      if (out->num_planes == 3)
         out->fds[2] = fds[0];
   } else {
      assert(num_fds == out->num_planes);
      out->fds[0] = fds[0];
      out->fds[1] = fds[1];
      out->fds[2] = fds[2];
   }

   return 0;
}

static int
fallback_gralloc_get_buffer_info(struct u_gralloc *gralloc,
                                 struct u_gralloc_buffer_handle *hnd,
                                 struct u_gralloc_buffer_basic_info *out)
{
   int num_planes = 0;
   int drm_fourcc = 0;
   int stride = 0;

   if (!hnd || !hnd->handle || hnd->handle->numFds == 0)
      return -EINVAL;

   /* Query AIMapper v5 or HIDL IMapper v4 directly first. */
   if (panvk_v19_query_mapper(hnd->handle, out) == 0 &&
       out->modifier != DRM_FORMAT_MOD_INVALID) {
      return 0;
   }

   if (is_hal_format_yuv(hnd->hal_format)) {
      int ret = fallback_gralloc_get_yuv_info(gralloc, hnd, out);
      /*
       * HACK: https://issuetracker.google.com/32077885
       * There is no API available to properly query the
       * IMPLEMENTATION_DEFINED format. As a workaround we rely here on
       * gralloc allocating either an arbitrary YCbCr 4:2:0 or RGBX_8888, with
       * the latter being recognized by lock_ycbcr failing.
       */
      if (ret != -EAGAIN)
         return ret;
   }

   /*
    * Non-YUV formats could *also* have multiple planes, such as ancillary
    * color compression state buffer, but the rest of the code isn't ready
    * yet to deal with modifiers:
    */
   num_planes = 1;

   drm_fourcc = get_fourcc_from_hal_format(hnd->hal_format);
   if (drm_fourcc == -1) {
      mesa_loge("Failed to get drm_fourcc");
      return -EINVAL;
   }

   stride = hnd->pixel_stride * get_hal_format_bpp(hnd->hal_format);
   if (stride == 0) {
      mesa_loge("Failed to calcuulate stride");
      return -EINVAL;
   }

   int fd_idx = panvk_v19_find_dmabuf_fd_index(hnd->handle);
   if (fd_idx < 0)
      fd_idx = 0;

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_INVALID;
   out->num_planes = num_planes;
   out->fds[0] = hnd->handle->data[fd_idx];
   out->strides[0] = stride;

#ifdef HAS_FREEDRENO
   uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
   if (hnd->handle->numInts >= 2 && hnd->handle->data[hnd->handle->numFds] == gmsm) {
      /* This UBWC flag was introduced in a5xx. */
      bool ubwc = hnd->handle->data[hnd->handle->numFds + 1] & 0x08000000;
      out->modifier = ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
   }
#endif

   /* Never return 0 with DRM_FORMAT_MOD_INVALID: returning -ENOTSUP lets
    * vk_android_get_ahb_layout invoke its non-destructive linear AHB probe
    * and DMA-BUF size/stride resolution instead of crashing in
    * panvk_image_get_explicit_mod (panvk_image.c:327).
    */
   if (out->modifier == DRM_FORMAT_MOD_INVALID)
      return -ENOTSUP;

   return 0;
}

static int
destroy(struct u_gralloc *gralloc)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   if (gr->gralloc_module) {
      dlclose(gr->gralloc_module->common.dso);
   }

   FREE(gr);

   return 0;
}

struct u_gralloc *
u_gralloc_fallback_create()
{
   struct fallback_gralloc *gr = CALLOC_STRUCT(fallback_gralloc);
   int err = 0;

   err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                       (const hw_module_t **)&gr->gralloc_module);

   if (err) {
      mesa_logw(
         "No gralloc hwmodule detected (video buffers won't be supported)");
   } else if (!gr->gralloc_module->lock_ycbcr) {
      mesa_logw("Gralloc doesn't support lock_ycbcr (video buffers won't be "
                "supported)");
   }

   gr->base.ops.get_buffer_basic_info = fallback_gralloc_get_buffer_info;
   gr->base.ops.destroy = destroy;

   mesa_logi("Using fallback gralloc implementation");

   return &gr->base;
}
