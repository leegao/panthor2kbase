#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Generates shims/ and shims.zip for cross-compiling upstream Mesa 26.3.0
# libvulkan_panfrost.so on Android NDK (aarch64-linux-android30) in both:
#   1. Patched Kmod mode (0001-panfrost-kmod-minimal-uapi-adapter-hook.patch)
#   2. Unpatched + Runtime Hook mode (libpanthor_kbase_runtime_hook.so + libdrm.a shim)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_DIR="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="${NDK_BIN}/aarch64-linux-android30-clang"
AR="${NDK_BIN}/llvm-ar"

SHIMS_DIR="${ROOT_DIR}/shims"
rm -rf "${SHIMS_DIR}"
mkdir -p "${SHIMS_DIR}/include/libdrm" \
         "${SHIMS_DIR}/include/drm-uapi" \
         "${SHIMS_DIR}/include/sync" \
         "${SHIMS_DIR}/lib/pkgconfig"

# 1. Copy DRM / Panthor / Kbase headers into shims/include
cp -f "${ROOT_DIR}/include/drm.h"            "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/drm_mode.h"       "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/panthor_drm.h"    "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/mali_kbase_uapi.h" "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/xf86drm.h"        "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/xf86drmMode.h"    "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/drm.h"            "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/drm_mode.h"       "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/panthor_drm.h"    "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/xf86drm.h"        "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/xf86drmMode.h"    "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/src/adapter/panthor_kbase_adapter.h"      "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/src/adapter/panthor_kbase_runtime_hook.h" "${SHIMS_DIR}/include/"

# Android sync.h header shim
cat > "${SHIMS_DIR}/include/sync/sync.h" << 'EOF'
#ifndef __SYS_CORE_SYNC_H
#define __SYS_CORE_SYNC_H

#include <sys/cdefs.h>
#include <stdint.h>
#include <linux/types.h>

__BEGIN_DECLS

struct sync_fence_info_data {
    uint32_t len;
    char name[32];
    int32_t status;
    uint8_t stageHeader[0];
};

struct sync_pt_info {
    uint32_t len;
    char obj_name[32];
    char driver_name[32];
    int32_t status;
    uint64_t timestamp_ns;
    uint8_t driver_data[0];
};

int sync_wait(int fd, int timeout);
int sync_merge(const char *name, int fd1, int fd2);
struct sync_fence_info_data *sync_fence_info(int fd);
struct sync_pt_info *sync_pt_info(struct sync_fence_info_data *info, struct sync_pt_info *itr);
void sync_fence_info_free(struct sync_fence_info_data *info);

__END_DECLS

#endif /* __SYS_CORE_SYNC_H */
EOF

# Copy host /usr/include/xcb and /usr/include/X11 if available in CI environment
if [ -d "/usr/include/xcb" ]; then
   cp -rf /usr/include/xcb "${SHIMS_DIR}/include/"
fi
if [ -d "/usr/include/X11" ]; then
   cp -rf /usr/include/X11 "${SHIMS_DIR}/include/"
fi

# 2. Build Android aarch64 libdrm.a (includes Panthor-to-Kbase Adapter + ELF GOT/PLT Runtime Hook + KMS stubs)
BUILD_TMP="$(mktemp -d)"
trap 'rm -rf "${BUILD_TMP}"' EXIT

"${CC}" -O2 -fPIC -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/panthor_kbase_adapter.c" \
   -o "${BUILD_TMP}/panthor_kbase_adapter.o"

"${CC}" -O2 -fPIC -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/panthor_kbase_runtime_hook.c" \
   -o "${BUILD_TMP}/panthor_kbase_runtime_hook.o"

"${CC}" -O2 -fPIC -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/drm_mode_stubs.c" \
   -o "${BUILD_TMP}/drm_mode_stubs.o"

"${AR}" rcs "${SHIMS_DIR}/lib/libdrm.a" \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/panthor_kbase_runtime_hook.o" \
   "${BUILD_TMP}/drm_mode_stubs.o"

"${AR}" rcs "${SHIMS_DIR}/lib/libpanthor_kbase_adapter.a" \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/drm_mode_stubs.o"

"${CC}" -shared -O2 -fPIC \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/drm_mode_stubs.o" \
   -ldl -llog -lm \
   -o "${SHIMS_DIR}/lib/libpanthor_kbase_adapter.so"

"${CC}" -shared -O2 -fPIC \
   "${BUILD_TMP}/panthor_kbase_runtime_hook.o" \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/drm_mode_stubs.o" \
   -ldl -llog -lm \
   -o "${SHIMS_DIR}/lib/libpanthor_kbase_runtime_hook.so"

# 3. Build functional Android libsync.a via ioctl calls
cat > "${BUILD_TMP}/libsync.c" << 'EOF'
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

struct sync_merge_data {
    char name[32];
    int32_t fd2;
    int32_t fence;
    uint32_t flags;
    uint32_t pad;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)

int sync_wait(int fd, int timeout) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeout);
}

int sync_merge(const char *name, int fd1, int fd2) {
    struct sync_merge_data data;
    memset(&data, 0, sizeof(data));
    if (name) strncpy(data.name, name, sizeof(data.name) - 1);
    data.fd2 = fd2;
    data.fence = -1;
    if (ioctl(fd1, SYNC_IOC_MERGE, &data) < 0)
        return -1;
    return data.fence;
}

void sync_fence_info_free(void *info) {
    free(info);
}
EOF

"${CC}" -O2 -fPIC -c "${BUILD_TMP}/libsync.c" -o "${BUILD_TMP}/libsync.o"
"${AR}" rcs "${SHIMS_DIR}/lib/libsync.a" "${BUILD_TMP}/libsync.o"

# 4. Create empty stub archives for libatomic, librt, libpthread
cat > "${BUILD_TMP}/empty_stub.c" << 'EOF'
static const int __unused_stub = 0;
EOF
"${CC}" -O2 -fPIC -c "${BUILD_TMP}/empty_stub.c" -o "${BUILD_TMP}/empty_stub.o"
for dummy in atomic rt pthread; do
    "${AR}" rcs "${SHIMS_DIR}/lib/lib${dummy}.a" "${BUILD_TMP}/empty_stub.o"
done

# 5. Build Android aarch64 X11/XCB runtime dlopen stub library
cat > "${BUILD_TMP}/x11_xcb_stubs.c" << 'EOF'
#include <stdint.h>
#include <stddef.h>

struct xcb_ext_t { const char *name; int global_id; };
struct xcb_ext_t xcb_present_id = { "Present", 0 };
struct xcb_ext_t xcb_dri3_id = { "DRI3", 0 };
struct xcb_ext_t xcb_randr_id = { "RANDR", 0 };
struct xcb_ext_t xcb_shm_id = { "MIT-SHM", 0 };
struct xcb_ext_t xcb_sync_id = { "SYNC", 0 };
struct xcb_ext_t xcb_xfixes_id = { "XFIXES", 0 };

#define STUB_FN(name) __attribute__((weak)) long name(void) { return 0; }
STUB_FN(XGetXCBConnection)
STUB_FN(XInitThreads)
STUB_FN(XOpenDisplay)
STUB_FN(XCloseDisplay)
STUB_FN(XDisplayString)
STUB_FN(xcb_change_property_checked)
STUB_FN(xcb_connect)
STUB_FN(xcb_connection_has_error)
STUB_FN(xcb_create_gc)
STUB_FN(xcb_delete_property_checked)
STUB_FN(xcb_depth_next)
STUB_FN(xcb_depth_visuals_iterator)
STUB_FN(xcb_discard_reply)
STUB_FN(xcb_disconnect)
STUB_FN(xcb_flush)
STUB_FN(xcb_free_pixmap)
STUB_FN(xcb_generate_id)
STUB_FN(xcb_get_extension_data)
STUB_FN(xcb_get_file_descriptor)
STUB_FN(xcb_get_geometry)
STUB_FN(xcb_get_geometry_reply)
STUB_FN(xcb_get_geometry_unchecked)
STUB_FN(xcb_get_maximum_request_length)
STUB_FN(xcb_get_setup)
STUB_FN(xcb_get_window_attributes)
STUB_FN(xcb_get_window_attributes_reply)
STUB_FN(xcb_intern_atom)
STUB_FN(xcb_intern_atom_reply)
STUB_FN(xcb_poll_for_event)
STUB_FN(xcb_poll_for_special_event)
STUB_FN(xcb_prefetch_extension_data)
STUB_FN(xcb_put_image)
STUB_FN(xcb_query_extension)
STUB_FN(xcb_query_extension_reply)
STUB_FN(xcb_query_tree)
STUB_FN(xcb_query_tree_reply)
STUB_FN(xcb_register_for_special_xge)
STUB_FN(xcb_request_check)
STUB_FN(xcb_screen_allowed_depths_iterator)
STUB_FN(xcb_screen_next)
STUB_FN(xcb_setup_roots_iterator)
STUB_FN(xcb_translate_coordinates_reply)
STUB_FN(xcb_translate_coordinates_unchecked)
STUB_FN(xcb_unregister_for_special_event)
STUB_FN(xcb_visualtype_next)
STUB_FN(xcb_wait_for_event)
STUB_FN(xcb_wait_for_special_event)
STUB_FN(xcb_dri3_buffers_from_pixmap)
STUB_FN(xcb_dri3_buffers_from_pixmap_buffers)
STUB_FN(xcb_dri3_buffers_from_pixmap_fds)
STUB_FN(xcb_dri3_buffers_from_pixmap_offsets)
STUB_FN(xcb_dri3_buffers_from_pixmap_reply)
STUB_FN(xcb_dri3_buffers_from_pixmap_strides)
STUB_FN(xcb_dri3_fd_from_fence)
STUB_FN(xcb_dri3_fd_from_fence_reply)
STUB_FN(xcb_dri3_fd_from_fence_reply_fds)
STUB_FN(xcb_dri3_fence_from_fd)
STUB_FN(xcb_dri3_free_syncobj)
STUB_FN(xcb_dri3_get_supported_modifiers)
STUB_FN(xcb_dri3_get_supported_modifiers_reply)
STUB_FN(xcb_dri3_get_supported_modifiers_screen_modifiers)
STUB_FN(xcb_dri3_get_supported_modifiers_window_modifiers)
STUB_FN(xcb_dri3_import_syncobj_checked)
STUB_FN(xcb_dri3_open)
STUB_FN(xcb_dri3_open_reply)
STUB_FN(xcb_dri3_open_reply_fds)
STUB_FN(xcb_dri3_pixmap_from_buffer_checked)
STUB_FN(xcb_dri3_pixmap_from_buffers_checked)
STUB_FN(xcb_dri3_query_version)
STUB_FN(xcb_dri3_query_version_reply)
STUB_FN(xcb_present_notify_msc)
STUB_FN(xcb_present_pixmap)
STUB_FN(xcb_present_pixmap_synced)
STUB_FN(xcb_present_query_capabilities)
STUB_FN(xcb_present_query_capabilities_reply)
STUB_FN(xcb_present_query_version)
STUB_FN(xcb_present_query_version_reply)
STUB_FN(xcb_present_select_input)
STUB_FN(xcb_present_select_input_checked)
STUB_FN(xcb_shm_attach)
STUB_FN(xcb_shm_attach_checked)
STUB_FN(xcb_shm_attach_fd)
STUB_FN(xcb_shm_attach_fd_checked)
STUB_FN(xcb_shm_create_pixmap_checked)
STUB_FN(xcb_shm_detach)
STUB_FN(xcb_shm_detach_checked)
STUB_FN(xcb_shm_query_version)
STUB_FN(xcb_shm_query_version_reply)
STUB_FN(xcb_sync_create_fence)
STUB_FN(xcb_sync_destroy_fence)
STUB_FN(xcb_sync_reset_fence)
STUB_FN(xcb_sync_trigger_fence)
STUB_FN(xcb_sync_wait)
STUB_FN(xcb_xfixes_create_region)
STUB_FN(xcb_xfixes_destroy_region)
STUB_FN(xcb_xfixes_query_version)
STUB_FN(xcb_xfixes_query_version_reply)
STUB_FN(xcb_xfixes_set_region)
STUB_FN(xcb_randr_create_lease)
STUB_FN(xcb_randr_create_lease_reply)
STUB_FN(xcb_randr_create_lease_reply_fds)
STUB_FN(xcb_randr_get_crtc_info_reply)
STUB_FN(xcb_randr_get_crtc_info_unchecked)
STUB_FN(xcb_randr_get_output_info)
STUB_FN(xcb_randr_get_output_info_name)
STUB_FN(xcb_randr_get_output_info_reply)
STUB_FN(xcb_randr_get_output_property)
STUB_FN(xcb_randr_get_output_property_data)
STUB_FN(xcb_randr_get_output_property_reply)
STUB_FN(xcb_randr_get_screen_resources_current_crtcs)
STUB_FN(xcb_randr_get_screen_resources_current_modes)
STUB_FN(xcb_randr_get_screen_resources_current_outputs)
STUB_FN(xcb_randr_get_screen_resources_current_reply)
STUB_FN(xcb_randr_get_screen_resources_current_unchecked)
STUB_FN(xcb_randr_query_version)
STUB_FN(xcb_randr_query_version_reply)
STUB_FN(xcb_randr_query_version_unchecked)
STUB_FN(xshmfence_alloc_shm)
STUB_FN(xshmfence_await)
STUB_FN(xshmfence_map_shm)
STUB_FN(xshmfence_reset)
STUB_FN(xshmfence_trigger)
STUB_FN(xshmfence_unmap_shm)
EOF

"${CC}" -O2 -fPIC -c "${BUILD_TMP}/x11_xcb_stubs.c" -o "${BUILD_TMP}/x11_xcb_stubs.o"
for libname in xcb X11-xcb xcb-dri3 xcb-present xcb-randr xcb-shm xcb-sync xcb-xfixes xshmfence X11 Xext Xrandr Xxf86vm; do
   "${AR}" rcs "${SHIMS_DIR}/lib/lib${libname}.a" "${BUILD_TMP}/x11_xcb_stubs.o"
done

# Copy static archives into shims/ root as well so -L shims and -L shims/lib both find them
cp -f "${SHIMS_DIR}"/lib/*.a "${SHIMS_DIR}/"

# 6. Write pkg-config (.pc) files into BOTH shims/ and shims/lib/pkgconfig/
write_pc() {
   local name="$1"
   local ver="$2"
   local libs="$3"
   local cflags="$4"
   for dir in "${SHIMS_DIR}" "${SHIMS_DIR}/lib/pkgconfig"; do
      cat > "${dir}/${name}.pc" << PCEOF
Name: ${name}
Description: Android NDK Shim for ${name}
Version: ${ver}
Libs: ${libs}
Cflags: ${cflags}
PCEOF
   done
}

# libdrm.pc links libdrm.a with --whole-archive so the runtime hook & adapter symbols are retained
write_pc "libdrm"      "2.5.0" "-Wl,--whole-archive -ldrm -Wl,--no-whole-archive -ldl -llog" ""
write_pc "zlib"        "1.3.1"   "-lz" ""
write_pc "xcb"         "1.17.0"  "-lxcb" ""
write_pc "x11-xcb"     "1.8.12"  "-lX11-xcb -lX11 -lxcb" ""
write_pc "xcb-dri3"    "1.17.0"  "-lxcb-dri3" ""
write_pc "xcb-present" "1.17.0"  "-lxcb-present" ""
write_pc "xcb-randr"   "1.17.0"  "-lxcb-randr" ""
write_pc "xcb-shm"     "1.17.0"  "-lxcb-shm" ""
write_pc "xcb-sync"    "1.17.0"  "-lxcb-sync" ""
write_pc "xcb-xfixes"  "1.17.0"  "-lxcb-xfixes" ""
write_pc "xshmfence"   "1.3.3"   "-lxshmfence" ""
write_pc "x11"         "1.8.12"  "-lX11" ""
write_pc "xext"        "1.3.6"   "-lXext" ""
write_pc "xrandr"      "1.5.4"   "-lXrandr" ""
write_pc "xxf86vm"     "1.1.5"   "-lXxf86vm" ""

# 7. Package shims.zip at repository root
(cd "${ROOT_DIR}" && rm -f shims.zip && python3 -m zipfile -c shims.zip shims/)
echo "Generated ${ROOT_DIR}/shims.zip successfully."
