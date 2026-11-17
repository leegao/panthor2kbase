#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Generates shims/ and shims.zip for cross-compiling upstream Mesa 26.3.0
# libvulkan_panfrost.so on Android NDK (aarch64-linux-android30) in both
# Patched (Variant A) and Unpatched (Variant B) modes, including AOSP
# u_gralloc Gralloc4/Gralloc5 (libui / libgralloctypes / libhidlbase / libutils)
# shims.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_DIR="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="${NDK_BIN}/aarch64-linux-android30-clang"
CXX="${NDK_BIN}/aarch64-linux-android30-clang++"
AR="${NDK_BIN}/llvm-ar"

SHIMS_DIR="${ROOT_DIR}/shims"
rm -rf "${SHIMS_DIR}"
mkdir -p "${SHIMS_DIR}/include/libdrm" \
         "${SHIMS_DIR}/include/drm-uapi" \
         "${SHIMS_DIR}/include/sync" \
         "${SHIMS_DIR}/lib/pkgconfig"

# 1. Copy DRM / Panthor / Kbase / Adapter headers into shims/include
cp -f "${ROOT_DIR}/include/drm.h"                        "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/drm_mode.h"                   "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/panthor_drm.h"                "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/mali_kbase_uapi.h"            "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/xf86drm.h"                    "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/xf86drmMode.h"                "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/include/drm.h"                        "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/drm_mode.h"                   "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/panthor_drm.h"                "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/xf86drm.h"                    "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/include/xf86drmMode.h"                "${SHIMS_DIR}/include/libdrm/"
cp -f "${ROOT_DIR}/src/adapter/panthor_kbase_adapter.h"  "${SHIMS_DIR}/include/"
cp -f "${ROOT_DIR}/src/adapter/log.h"                    "${SHIMS_DIR}/include/"

# Copy AOSP u_gralloc headers into shims/include
U_GRALLOC_OVERLAY="${ROOT_DIR}/src/mesa_shims"
if [ -d "${U_GRALLOC_OVERLAY}" ]; then
   cp -f "${U_GRALLOC_OVERLAY}/force_aosp_abi.h" "${SHIMS_DIR}/include/"
   for subdir in aidl android android-base cutils fmq gralloctypes hidl math ui utils; do
      if [ -d "${U_GRALLOC_OVERLAY}/${subdir}" ]; then
         cp -rf "${U_GRALLOC_OVERLAY}/${subdir}" "${SHIMS_DIR}/include/"
      fi
   done
fi

# Android sync.h header shim
cp -f "${ROOT_DIR}/src/stubs/include/sync/sync.h" "${SHIMS_DIR}/include/sync/sync.h"

# Copy host /usr/include/xcb and /usr/include/X11 if available
if [ -d "/usr/include/xcb" ]; then
   cp -rf /usr/include/xcb "${SHIMS_DIR}/include/"
fi
if [ -d "/usr/include/X11" ]; then
   cp -rf /usr/include/X11 "${SHIMS_DIR}/include/"
fi

# 2. Build Android aarch64 libdrm.a (includes C++ Panthor-to-Kbase Adapter + Scoped Logger + KMS stubs)
BUILD_TMP="$(mktemp -d)"
trap 'rm -rf "${BUILD_TMP}"' EXIT

"${CXX}" -std=c++17 -O2 -fPIC -fno-emulated-tls -fno-exceptions -fno-unwind-tables \
   -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/panthor_kbase_adapter.cpp" \
   -o "${BUILD_TMP}/panthor_kbase_adapter.o"

"${CXX}" -std=c++17 -O2 -fPIC -fno-emulated-tls -fno-exceptions -fno-unwind-tables \
   -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/mali_kbase_caps.cpp" \
   -o "${BUILD_TMP}/mali_kbase_caps.o"

"${CXX}" -std=c++17 -O2 -fPIC -fno-emulated-tls -fno-exceptions -fno-unwind-tables \
   -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" -I"${ROOT_DIR}/src/emulator" \
   -c "${ROOT_DIR}/src/adapter/panthor_drm_capture_replay.cpp" \
   -o "${BUILD_TMP}/panthor_drm_capture_replay.o"

"${CXX}" -std=c++17 -O2 -fPIC -fno-emulated-tls -fno-exceptions -fno-unwind-tables \
   -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/log.cpp" \
   -o "${BUILD_TMP}/log.o"

"${CC}" -O2 -fPIC -fno-emulated-tls -Wall -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src/adapter" \
   -c "${ROOT_DIR}/src/adapter/drm_mode_stubs.c" \
   -o "${BUILD_TMP}/drm_mode_stubs.o"

"${AR}" rcs "${SHIMS_DIR}/lib/libdrm.a" \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/mali_kbase_caps.o" \
   "${BUILD_TMP}/panthor_drm_capture_replay.o" \
   "${BUILD_TMP}/log.o" \
   "${BUILD_TMP}/drm_mode_stubs.o"
cp -f "${SHIMS_DIR}/lib/libdrm.a" "${SHIMS_DIR}/libdrm.a"

"${AR}" rcs "${SHIMS_DIR}/lib/libpanthor_kbase_adapter.a" \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/mali_kbase_caps.o" \
   "${BUILD_TMP}/panthor_drm_capture_replay.o" \
   "${BUILD_TMP}/log.o" \
   "${BUILD_TMP}/drm_mode_stubs.o"
cp -f "${SHIMS_DIR}/lib/libpanthor_kbase_adapter.a" "${SHIMS_DIR}/libpanthor_kbase_adapter.a"

"${CXX}" -shared -O2 -fPIC -fno-emulated-tls -fno-exceptions -static-libstdc++ -Wl,-z,nodelete \
   "${BUILD_TMP}/panthor_kbase_adapter.o" \
   "${BUILD_TMP}/mali_kbase_caps.o" \
   "${BUILD_TMP}/panthor_drm_capture_replay.o" \
   "${BUILD_TMP}/log.o" \
   "${BUILD_TMP}/drm_mode_stubs.o" \
   -ldl -llog -lm \
   -o "${SHIMS_DIR}/lib/libpanthor_kbase_adapter.so"

# 3. Build functional Android libsync.a via ioctl calls
cp -f "${ROOT_DIR}/src/stubs/libsync.c" "${BUILD_TMP}/libsync.c"

"${CC}" -O2 -fPIC -c "${BUILD_TMP}/libsync.c" -o "${BUILD_TMP}/libsync.o"
"${AR}" rcs "${SHIMS_DIR}/lib/libsync.a" "${BUILD_TMP}/libsync.o"

# 4. Build AOSP Gralloc5 / libui / libgralloctypes / libhidlbase / libutils static shims
cp -f "${ROOT_DIR}/src/stubs/aosp_ui_shim.cpp" "${BUILD_TMP}/aosp_ui_shim.cpp"

# Find system/graphics.h from Mesa's include/android_stub if available
MESA_ANDROID_STUB_INC=""
if [ -d "${ROOT_DIR}/mesa/include/android_stub" ]; then
   MESA_ANDROID_STUB_INC="-I${ROOT_DIR}/mesa/include/android_stub"
else
   echo "mesa/include/android_stub not found"
   exit 1
fi

# Create fallback system/graphics.h and system/window.h in BUILD_TMP if neither is present
mkdir -p "${BUILD_TMP}/stub_inc/system" "${BUILD_TMP}/stub_inc/hardware"
cp -f "${ROOT_DIR}/src/stubs/include/system/graphics.h" "${BUILD_TMP}/stub_inc/system/graphics.h"
cp -f "${ROOT_DIR}/src/stubs/include/hardware/gralloc.h" "${BUILD_TMP}/stub_inc/hardware/gralloc.h"

"${CXX}" -std=c++17 -O2 -fPIC -fno-emulated-tls -fno-exceptions -fno-unwind-tables \
   -include "${SHIMS_DIR}/include/force_aosp_abi.h" \
   -I"${SHIMS_DIR}/include" ${MESA_ANDROID_STUB_INC} -I"${BUILD_TMP}/stub_inc" \
   -c "${BUILD_TMP}/aosp_ui_shim.cpp" \
   -o "${BUILD_TMP}/aosp_ui_shim.o"

"${AR}" rcs "${SHIMS_DIR}/lib/libui.a" "${BUILD_TMP}/aosp_ui_shim.o"

# 5. Create empty stub archives for libatomic, librt, libpthread, and AOSP helper libs
cp -f "${ROOT_DIR}/src/stubs/empty_stub.c" "${BUILD_TMP}/empty_stub.c"
"${CC}" -O2 -fPIC -c "${BUILD_TMP}/empty_stub.c" -o "${BUILD_TMP}/empty_stub.o"
for dummy in atomic rt pthread gralloctypes hidlbase utils cutils hardware android.hardware.graphics.mapper; do
    "${AR}" rcs "${SHIMS_DIR}/lib/lib${dummy}.a" "${BUILD_TMP}/empty_stub.o"
done

# 6. Build Android aarch64 X11/XCB runtime dlopen stub library
cp -f "${ROOT_DIR}/src/stubs/x11_xcb_stubs.c" "${BUILD_TMP}/x11_xcb_stubs.c"

"${CC}" -O2 -fPIC -c "${BUILD_TMP}/x11_xcb_stubs.c" -o "${BUILD_TMP}/x11_xcb_stubs.o"

cat << 'NWSTUBEOF' > "${BUILD_TMP}/nativewindow_stubs.c"
#include <stdint.h>
#include <stddef.h>

struct AHardwareBuffer;
struct ARect;
struct ANativeWindow;
struct ANativeWindow_Buffer;

int32_t ANativeWindow_getWidth(struct ANativeWindow *w) { return 0; }
int32_t ANativeWindow_getHeight(struct ANativeWindow *w) { return 0; }
int32_t ANativeWindow_setBuffersGeometry(struct ANativeWindow *w, int32_t width, int32_t height, int32_t format) { return 0; }
int32_t ANativeWindow_lock(struct ANativeWindow *w, struct ANativeWindow_Buffer *b, struct ARect *r) { return 0; }
int32_t ANativeWindow_unlockAndPost(struct ANativeWindow *w) { return 0; }
int32_t AHardwareBuffer_lock(struct AHardwareBuffer *b, uint64_t usage, int32_t fence, const struct ARect *rect, void **outAddr) {
   if (outAddr) *outAddr = NULL;
   return -19;
}
int32_t AHardwareBuffer_unlock(struct AHardwareBuffer *b, int32_t *fence) {
   if (fence) *fence = -1;
   return 0;
}
NWSTUBEOF
"${CC}" -O2 -fPIC -c "${BUILD_TMP}/nativewindow_stubs.c" -o "${BUILD_TMP}/nativewindow_stubs.o"
"${AR}" rcs "${SHIMS_DIR}/lib/libnativewindow.a" "${BUILD_TMP}/nativewindow_stubs.o"

for libname in xcb X11-xcb x11-xcb xcb-dri3 xcb-present xcb-randr xcb-shm xcb-sync xcb-xfixes xshmfence X11 Xext Xrandr Xxf86vm; do
   "${AR}" rcs "${SHIMS_DIR}/lib/lib${libname}.a" "${BUILD_TMP}/x11_xcb_stubs.o"
done

cp -f "${SHIMS_DIR}"/lib/*.a "${SHIMS_DIR}/"

write_pc() {
   local name="$1"
   local ver="$2"
   local libs="$3"
   local cflags="$4"
   for dir in "${SHIMS_DIR}" "${SHIMS_DIR}/lib/pkgconfig"; do
      cat > "${dir}/${name}.pc" << PCEOF
Name: ${name}
Description: ${name}
Version: ${ver}
Libs: ${libs}
Cflags: ${cflags}
PCEOF
   done
}

write_pc "libdrm"      "2.5"    "-ldrm" ""
write_pc "zlib"        "1.3.1"  "-lz" ""
write_pc "xcb"         "1.15"   "-lxcb" ""
write_pc "x11-xcb"     "1.15"   "-lx11-xcb" ""
write_pc "libx11-xcb"  "2.5"    "-lX11-xcb" ""
write_pc "xcb-dri3"    "1.15"   "-lxcb-dri3" ""
write_pc "xcb-present" "1.15"   "-lxcb-present" ""
write_pc "xcb-randr"   "1.15"   "-lxcb-randr" ""
write_pc "xcb-shm"     "1.15"   "-lxcb-shm" ""
write_pc "xcb-sync"    "1.15"   "-lxcb-sync" ""
write_pc "xcb-xfixes"  "1.15"   "-lxcb-xfixes" ""
write_pc "xshmfence"   "1.15"   "-lxshmfence" ""
write_pc "x11"         "1.8.12" "-lX11" ""
write_pc "xext"        "1.3.6"  "-lXext" ""
write_pc "xrandr"      "1.15"   "-lXrandr" ""
write_pc "Xrandr"      "1.15"   "-lXrandr" ""
write_pc "xxf86vm"     "1.1.5"  "-lXxf86vm" ""
write_pc "ui"          "1.15"   "-lui" ""
write_pc "gralloctypes" "1.15"  "-lgralloctypes" ""
write_pc "hidlbase"    "1.15"   "-lhidlbase" ""
write_pc "utils"       "1.15"   "-lutils" ""
write_pc "cutils"      "1.15"   "-lcutils" ""
write_pc "hardware"    "1.15"   "-lhardware" ""
write_pc "nativewindow" "1.15"  "-lnativewindow" ""
write_pc "sync"        "1.15"   "-lsync" ""
write_pc "log"         "1.15"   "-llog" ""
write_pc "android.hardware.graphics.mapper" "4.0" "-landroid.hardware.graphics.mapper" ""

echo "Generated ${SHIMS_DIR} successfully."
