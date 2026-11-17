#include "force_aosp_abi.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include <aidl/android/hardware/graphics/common/ChromaSiting.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PlaneLayout.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Singleton.h>

/* Explicit instantiation so std::__1::basic_string<char>::~basic_string() is
 * emitted as a weak symbol in libui.a without requiring /system/lib64/libc++.so
 */
_LIBCPP_BEGIN_NAMESPACE_STD
#if _LIBCPP_VERSION >= 190000
    #define __VERBOSE_ABORT_NOEXCEPT _NOEXCEPT
#else
    #define __VERBOSE_ABORT_NOEXCEPT
#endif
__attribute__((weak)) void __libcpp_verbose_abort(const char *, ...) __VERBOSE_ABORT_NOEXCEPT { __builtin_trap(); }
_LIBCPP_END_NAMESPACE_STD
template class std::basic_string<char>;

namespace android {
class GrallocMapper { public: virtual ~GrallocMapper() = default; };

ANDROID_SINGLETON_STATIC_INSTANCE(GraphicBufferMapper)

static void *get_real_libui() {
   static void *lib = nullptr;
   static pthread_once_t once = PTHREAD_ONCE_INIT;
   pthread_once(&once, []() {
      lib = dlopen("libui.so", RTLD_NOLOAD | RTLD_LAZY);
   });
   return lib;
}

GraphicBufferMapper::GraphicBufferMapper()
   : mMapper(nullptr), mMapperVersion(static_cast<Version>(0))
{
   void *lib = get_real_libui();
   if (lib) {
      typedef void (*ctor_fn)(GraphicBufferMapper *);
      ctor_fn fn = reinterpret_cast<ctor_fn>(
         dlsym(lib, "_ZN7android19GraphicBufferMapperC1Ev"));
      if (fn && fn != reinterpret_cast<ctor_fn>(&GraphicBufferMapper::preloadHal)) {
         fn(this);
      }
   }
}

status_t GraphicBufferMapper::getPixelFormatFourCC(buffer_handle_t bufferHandle,
                                                   uint32_t *outPixelFormatFourCC)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, uint32_t *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper20getPixelFormatFourCCEPK13native_handlePj"));
      if (fn)
         return fn(this, bufferHandle, outPixelFormatFourCC);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getPixelFormatModifier(buffer_handle_t bufferHandle,
                                                     uint64_t *outPixelFormatModifier)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, uint64_t *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper22getPixelFormatModifierEPK13native_handlePm"));
      if (fn)
         return fn(this, bufferHandle, outPixelFormatModifier);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getAllocationSize(buffer_handle_t bufferHandle,
                                                uint64_t *outAllocationSize)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, uint64_t *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper17getAllocationSizeEPK13native_handlePm"));
      if (fn)
         return fn(this, bufferHandle, outAllocationSize);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getLayerCount(buffer_handle_t bufferHandle,
                                            uint64_t *outLayerCount)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, uint64_t *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper13getLayerCountEPK13native_handlePm"));
      if (fn)
         return fn(this, bufferHandle, outLayerCount);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getPlaneLayouts(buffer_handle_t bufferHandle,
                                              std::vector<ui::PlaneLayout> *outPlaneLayouts)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, std::vector<ui::PlaneLayout> *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper15getPlaneLayoutsEPK13native_handlePNSt3__16vectorIN4aidl7android8hardware8graphics6common11PlaneLayoutENS4_9allocatorISB_EEEE"));
      if (fn)
         return fn(this, bufferHandle, outPlaneLayouts);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getChromaSiting(buffer_handle_t bufferHandle,
                                              ui::ChromaSiting *outChromaSiting)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, ui::ChromaSiting *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper15getChromaSitingEPK13native_handlePN4aidl7android8hardware8graphics6common12ChromaSitingE"));
      if (fn)
         return fn(this, bufferHandle, outChromaSiting);
   }
   return NO_INIT;
}

status_t GraphicBufferMapper::getDataspace(buffer_handle_t bufferHandle,
                                           ui::Dataspace *outDataspace)
{
   void *lib = get_real_libui();
   if (lib && mMapperVersion >= GRALLOC_4) {
      typedef status_t (*fn_t)(GraphicBufferMapper *, buffer_handle_t, ui::Dataspace *);
      fn_t fn = reinterpret_cast<fn_t>(
         dlsym(lib, "_ZN7android19GraphicBufferMapper12getDataspaceEPK13native_handlePNS_8hardware8graphics6common4V1_29DataspaceE"));
      if (fn)
         return fn(this, bufferHandle, outDataspace);
   }
   return NO_INIT;
}

void GraphicBufferMapper::preloadHal() {}

} // namespace android
