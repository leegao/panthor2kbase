cmake_minimum_required(VERSION 3.18)
project(panthor_kbase_uapi_adapter VERSION 2.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PANTHOR_KBASE_BUILD_TESTS "Build unit, compliance, and equivalence test suites" ON)
option(PANTHOR_KBASE_BUILD_AOT "Build AOT lifter, libr54p3_vulkan_icd.so, and DXVK layers" ON)
option(PANTHOR_KBASE_ENABLE_COVERAGE "Enable gcov/lcov coverage instrumentation" OFF)

if(CMAKE_C_COMPILER MATCHES "android" OR CMAKE_CXX_COMPILER MATCHES "android")
   set(ANDROID ON)
   set(PANTHOR_KBASE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
   set(PANTHOR_KBASE_BUILD_AOT OFF CACHE BOOL "" FORCE)
endif()
if(ANDROID)
   message(STATUS "Configuring Panthor-to-Kbase C++ Adapter Suite for Android NDK (${ANDROID_ABI}, API ${ANDROID_PLATFORM})")
   set(PLATFORM_LIBS dl log m)
   add_compile_definitions(HAVE_MEMFD_CREATE=1)
   find_library(VULKAN_LIB vulkan)
   if(VULKAN_LIB)
      list(APPEND PLATFORM_LIBS ${VULKAN_LIB})
   endif()
else()
   message(STATUS "Configuring Panthor-to-Kbase C++ Adapter Suite for Host (${CMAKE_SYSTEM_PROCESSOR})")
   find_package(Threads REQUIRED)
   set(PLATFORM_LIBS dl Threads::Threads m)
   find_library(VULKAN_LIB NAMES vulkan vulkan.1 libvulkan.so.1 HINTS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64)
   if(NOT VULKAN_LIB)
      if(EXISTS "/usr/lib/x86_64-linux-gnu/libvulkan.so.1")
         set(VULKAN_LIB "/usr/lib/x86_64-linux-gnu/libvulkan.so.1")
      elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libvulkan.so")
         set(VULKAN_LIB "/usr/lib/x86_64-linux-gnu/libvulkan.so")
      elseif(EXISTS "/usr/lib64/libvulkan.so.1")
         set(VULKAN_LIB "/usr/lib64/libvulkan.so.1")
      elseif(EXISTS "/usr/lib/libvulkan.so.1")
         set(VULKAN_LIB "/usr/lib/libvulkan.so.1")
      else()
         set(VULKAN_LIB "")
      endif()
   endif()
endif()

if(PANTHOR_KBASE_ENABLE_COVERAGE AND NOT ANDROID)
   add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0 -g -UNDEBUG)
   add_link_options(--coverage)
else()
   add_compile_options(-O2 -g -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=. -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
endif()

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,nodelete")

# Per-architecture subdirectories:
# build-$ARCH/
#   + adapter
#   + emulator
#   + aot
#   + tests
#   + scratch
set(ADAPTER_DIR "${CMAKE_BINARY_DIR}/adapter")
set(EMULATOR_DIR "${CMAKE_BINARY_DIR}/emulator")
set(AOT_DIR "${CMAKE_BINARY_DIR}/aot")
set(TESTS_DIR "${CMAKE_BINARY_DIR}/tests")
set(SCRATCH_DIR "${CMAKE_BINARY_DIR}/scratch")

file(MAKE_DIRECTORY "${ADAPTER_DIR}")
file(MAKE_DIRECTORY "${EMULATOR_DIR}")
file(MAKE_DIRECTORY "${AOT_DIR}")
file(MAKE_DIRECTORY "${TESTS_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

# RPATH configuration: ensures binaries resolve libraries across build subdirectories
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(ROOT_RPATH "$ORIGIN;$ORIGIN/..;$ORIGIN/../..;$ORIGIN/../../..;$ORIGIN/../adapter;$ORIGIN/../emulator;$ORIGIN/../aot;$ORIGIN/../../build-x86-64;$ORIGIN/../../build-x86-64/adapter;$ORIGIN/../../build-x86-64/emulator;$ORIGIN/../../build-x86-64/aot;$ORIGIN/../../src/aot/layers;$ORIGIN/../../../build-x86-64;$ORIGIN/../../../build-x86-64/adapter;$ORIGIN/../../../build-x86-64/emulator;$ORIGIN/../../../build-x86-64/aot")
set(CMAKE_BUILD_RPATH "${ROOT_RPATH}")
set(CMAKE_INSTALL_RPATH "${ROOT_RPATH}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

include_directories(
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter
   ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator
)

# Helper macro: copy target artifact to expected relative path in source tree if different
function(target_copy_to_source_tree target_name dest_path)
   add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:${target_name}>
              "${dest_path}"
      COMMENT "Copying ${target_name} -> ${dest_path}"
   )
endfunction()

# -----------------------------------------------------------------------------
# 1. Core Panthor-to-Kbase C++ UAPI Adapter Library (Shared & Static)
# -----------------------------------------------------------------------------
add_library(panthor_kbase_adapter SHARED
   src/adapter/panthor_kbase_adapter.cpp
   src/adapter/mali_kbase_caps.cpp
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
target_link_libraries(panthor_kbase_adapter PRIVATE ${PLATFORM_LIBS})
set_target_properties(panthor_kbase_adapter PROPERTIES
   LIBRARY_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
   ARCHIVE_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
add_custom_command(TARGET panthor_kbase_adapter POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:panthor_kbase_adapter> "${CMAKE_BINARY_DIR}/libpanthor_kbase_adapter.so"
)

add_library(panthor_kbase_adapter_static STATIC
   src/adapter/panthor_kbase_adapter.cpp
   src/adapter/mali_kbase_caps.cpp
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
set_target_properties(panthor_kbase_adapter_static PROPERTIES
   OUTPUT_NAME panthor_kbase_adapter
   ARCHIVE_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
add_custom_command(TARGET panthor_kbase_adapter_static POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:panthor_kbase_adapter_static> "${CMAKE_BINARY_DIR}/libpanthor_kbase_adapter.a"
)

# -----------------------------------------------------------------------------
# 2. Complete libdrm.a Shim Library (Bundles C++ Adapter + Logger + KMS Stubs)
# -----------------------------------------------------------------------------
add_library(drm_shim STATIC
   src/adapter/panthor_kbase_adapter.cpp
   src/adapter/mali_kbase_caps.cpp
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
set_target_properties(drm_shim PROPERTIES
   OUTPUT_NAME drm
   ARCHIVE_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
if(ANDROID)
   add_custom_command(TARGET drm_shim POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_BINARY_DIR}/libdrm.a"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/shims/lib"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_CURRENT_SOURCE_DIR}/shims/lib/libdrm.a"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_CURRENT_SOURCE_DIR}/shims/libdrm.a"
   )
endif()

# -----------------------------------------------------------------------------
# 3. User-Space /dev/mali0 Kbase CSF & Valhall ISA Emulator (Shared & Static)
# -----------------------------------------------------------------------------
if(NOT ANDROID)
   add_library(horizon2llvm_passes OBJECT
      src/aot/horizon2llvm/mali_llvm_passes.cpp
      src/aot/horizon2llvm/llvm_backend.cpp
      src/aot/horizon2llvm/aarch64/aarch64_lifter.cpp
      src/aot/horizon2llvm/passes.cpp
      src/aot/horizon2llvm/lowering/pass_aapcs64_abi_contract.cpp
      src/aot/horizon2llvm/lowering/pass_aarch64_idiom_deconstruct.cpp
      src/aot/horizon2llvm/lowering/pass_cpu_state_promotion.cpp
      src/aot/horizon2llvm/lowering/pass_fcmp_predicate_fusion.cpp
      src/aot/horizon2llvm/lowering/pass_guest_stack_alignment.cpp
      src/aot/horizon2llvm/lowering/pass_jump_table_lowering.cpp
      src/aot/horizon2llvm/lowering/pass_link_register_elision.cpp
      src/aot/horizon2llvm/lowering/pass_nzcv_dead_flag_elim.cpp
      src/aot/horizon2llvm/lowering/pass_nzcv_predicate_fusion.cpp
      src/aot/horizon2llvm/lowering/pass_tail_call_devirt.cpp
   )
   # Preferred: the pinned static LLVM-21 prebuilt under third_party/prebuilts/llvm-21.
   # That directory is .gitignored (only the headers are tracked), so it is absent on a
   # fresh checkout such as GitHub CI. Fall back to whatever llvm-config is installed.
   set(LLVM21_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/prebuilts/llvm-21/include")
   set(LLVM21_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/prebuilts/llvm-21/lib")
   file(GLOB LLVM21_STATIC_LIBS "${LLVM21_LIB_DIR}/*.a")
   list(SORT LLVM21_STATIC_LIBS)

   set(LLVM21_LINK_LIBS "")
   if(LLVM21_STATIC_LIBS)
      message(STATUS "horizon2llvm: using pinned static LLVM-21 prebuilt (${LLVM21_LIB_DIR})")
      list(APPEND LLVM21_LINK_LIBS -Wl,--start-group ${LLVM21_STATIC_LIBS} -Wl,--end-group)
   else()
      # No prebuilt static libs: locate llvm-config (LLVM_CONFIG cache var wins) and use
      # the shared libLLVM it points at. The headers still come from the tracked
      # third_party/prebuilts/llvm-21/include tree, which is the same LLVM-21 ABI.
      if(NOT LLVM_CONFIG)
         find_program(LLVM_CONFIG NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18)
      endif()
      if(LLVM_CONFIG)
         execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE LLVM21_SYS_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
         execute_process(COMMAND ${LLVM_CONFIG} --libdir OUTPUT_VARIABLE LLVM21_SYS_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
         execute_process(COMMAND ${LLVM_CONFIG} --version OUTPUT_VARIABLE LLVM21_SYS_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
         # The sources are pinned to the LLVM 21 API, so a different major version is not usable.
         if(LLVM21_SYS_VERSION AND NOT LLVM21_SYS_VERSION MATCHES "^21\.")
            message(FATAL_ERROR
               "horizon2llvm: llvm-config reports LLVM ${LLVM21_SYS_VERSION}, but the sources require LLVM 21. "
               "Point -DLLVM_CONFIG= at an llvm-config-21, or install llvm-21-dev.")
         endif()
         if(LLVM21_SYS_INCLUDE_DIR AND EXISTS "${LLVM21_SYS_INCLUDE_DIR}/llvm/IR/IRBuilder.h")
            set(LLVM21_INC_DIR "${LLVM21_SYS_INCLUDE_DIR}")
         endif()
         find_library(LLVM21_SHARED_LIB NAMES LLVM LLVM-${LLVM21_SYS_VERSION} LLVM-${LLVM21_SYS_VERSION}.1 HINTS "${LLVM21_SYS_LIB_DIR}" NO_DEFAULT_PATH)
         if(NOT LLVM21_SHARED_LIB)
            find_library(LLVM21_SHARED_LIB NAMES LLVM LLVM-${LLVM21_SYS_VERSION} HINTS "${LLVM21_SYS_LIB_DIR}")
         endif()
      endif()

      if(LLVM21_SHARED_LIB)
         message(STATUS "horizon2llvm: using system llvm-config LLVM ${LLVM21_SYS_VERSION} (${LLVM21_SHARED_LIB})")
         list(APPEND LLVM21_LINK_LIBS "${LLVM21_SHARED_LIB}")
      else()
         message(FATAL_ERROR
            "horizon2llvm: no LLVM-21 available. Expected static libs in "
            "third_party/prebuilts/llvm-21/lib (gitignored, so absent on fresh checkouts) "
            "or an llvm-config providing libLLVM. Install llvm-dev (or set -DLLVM_CONFIG=/path/to/llvm-config).")
      endif()
   endif()

   target_include_directories(horizon2llvm_passes PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/horizon2llvm"
      ${LLVM21_INC_DIR}
   )
   set_target_properties(horizon2llvm_passes PROPERTIES CXX_STANDARD 20)
   target_compile_definitions(horizon2llvm_passes PRIVATE HORIZON2LLVM_USE_SYSTEM_LLVM_HEADERS=1)

   add_executable(horizon2llvm
      src/aot/horizon2llvm/main.cpp
      src/aot/horizon2llvm/ko_lifter.cpp
      $<TARGET_OBJECTS:horizon2llvm_passes>
   )
   target_include_directories(horizon2llvm PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/horizon2llvm"
      ${LLVM21_INC_DIR}
   )
   target_compile_definitions(horizon2llvm PRIVATE HORIZON2LLVM_USE_SYSTEM_LLVM_HEADERS=1)
   set_target_properties(horizon2llvm PROPERTIES
      CXX_STANDARD 20
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   find_library(ZLIB_LIB NAMES z libz.so.1 HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64)
   find_library(ZSTD_LIB NAMES zstd libzstd.so.1 HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64)
   set(LLVM21_EXT_LIBS "")
   if(ZLIB_LIB)
      list(APPEND LLVM21_EXT_LIBS "${ZLIB_LIB}")
   endif()
   if(ZSTD_LIB)
      list(APPEND LLVM21_EXT_LIBS "${ZSTD_LIB}")
   endif()
   target_link_libraries(horizon2llvm PRIVATE
      ${LLVM21_LINK_LIBS}
      ${LLVM21_EXT_LIBS}
      dl ${PLATFORM_LIBS}
   )

   add_library(mali_jit SHARED
      $<TARGET_OBJECTS:horizon2llvm_passes>
   )
   set_target_properties(mali_jit PROPERTIES
      CXX_STANDARD 20
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   target_link_options(mali_jit PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions)
   target_link_libraries(mali_jit PRIVATE
      ${LLVM21_LINK_LIBS}
      ${LLVM21_EXT_LIBS}
      dl ${PLATFORM_LIBS}
   )
   add_custom_command(TARGET mali_jit POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mali_jit> "${CMAKE_BINARY_DIR}/libmali_jit.so"
   )

   add_custom_command(
      OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.h"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.c"
      COMMAND $<TARGET_FILE:horizon2llvm> --lift-ko-dual
              "${CMAKE_CURRENT_SOURCE_DIR}/kernel/mali_kbase_mt6878_r44p1.ko"
              "${CMAKE_CURRENT_SOURCE_DIR}/kernel/mali_kbase_husky_r54p1.ko"
              "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.h"
              "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.c"
              --out-ir "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
              --out-obj-x86 "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko_x86_64.o"
              --out-obj-arm64 ""
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS horizon2llvm
              "${CMAKE_CURRENT_SOURCE_DIR}/kernel/mali_kbase_mt6878_r44p1.ko"
              "${CMAKE_CURRENT_SOURCE_DIR}/kernel/mali_kbase_husky_r54p1.ko"
      COMMENT "Lifting kernel modules -> lifted_kbase_ko.ll via horizon2llvm"
   )
endif()

if(ANDROID)
   set(LIFTED_KBASE_KO_LL_OBJ "${EMULATOR_DIR}/lifted_kbase_ko_arm64.o")
   set(LIFTED_KBASE_KO_SRC_OBJ "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko_arm64.o")
   add_custom_command(
      OUTPUT "${LIFTED_KBASE_KO_LL_OBJ}"
      COMMAND "${CMAKE_C_COMPILER}" --target=aarch64-linux-android30 -O2 -fPIC -Wno-override-module -c
              "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
              -o "${LIFTED_KBASE_KO_LL_OBJ}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${LIFTED_KBASE_KO_LL_OBJ}"
              "${LIFTED_KBASE_KO_SRC_OBJ}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
      COMMENT "Compiling lifted_kbase_ko.ll -> ${LIFTED_KBASE_KO_LL_OBJ} (Android arm64 -O2)"
   )
else()
   set(LIFTED_KBASE_KO_LL_OBJ "${EMULATOR_DIR}/lifted_kbase_ko_x86_64.o")
   set(LIFTED_KBASE_KO_SRC_OBJ "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko_x86_64.o")
   add_custom_command(
      OUTPUT "${LIFTED_KBASE_KO_LL_OBJ}"
      COMMAND $<TARGET_FILE:horizon2llvm> --compile-ir
              "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
              "${LIFTED_KBASE_KO_LL_OBJ}" -O2 --target-triple x86_64-pc-linux-gnu
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${LIFTED_KBASE_KO_LL_OBJ}"
              "${LIFTED_KBASE_KO_SRC_OBJ}"
      DEPENDS horizon2llvm "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/lifted_kbase_ko.ll"
      COMMENT "Compiling lifted_kbase_ko.ll -> ${LIFTED_KBASE_KO_LL_OBJ} (Host x86-64 -O2 via horizon2llvm)"
   )
endif()
set_source_files_properties(${LIFTED_KBASE_KO_LL_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# Regenerate the CSF genxml decode header from the pinned Panfrost v11 XML.
set(CSFFW_GENXML_XML "${CMAKE_CURRENT_SOURCE_DIR}/specs/panfrost/v11.xml")
set(CSFFW_GENXML_HDR "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/generated/csffw_genxml_decode.h")
if(EXISTS "${CSFFW_GENXML_XML}")
   add_custom_command(
      OUTPUT "${CSFFW_GENXML_HDR}"
      COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/tools/gen_csffw_genxml.py"
              --xml "${CSFFW_GENXML_XML}" --out "${CSFFW_GENXML_HDR}"
      DEPENDS "${CSFFW_GENXML_XML}" "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/tools/gen_csffw_genxml.py"
      COMMENT "Regenerating csffw_genxml_decode.h from Panfrost v11 genxml"
      VERBATIM
   )
   set_source_files_properties("${CSFFW_GENXML_HDR}" PROPERTIES GENERATED TRUE)
endif()

add_library(mali_kbase_emu SHARED
   src/emulator/mali_kbase_emu.c
   src/emulator/lifted_kbase_ko.c
   src/emulator/lifted_csffw.c
   ${CSFFW_GENXML_HDR}
   ${LIFTED_KBASE_KO_LL_OBJ}
   src/emulator/panfrost_valhall_disasm_gen.c
    src/emulator/valhall_vulkan_renderer.cpp
    src/r54p3_vulkan_icd/valhall_spirv_lifter.cpp
)
target_link_options(mali_kbase_emu PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions)
target_link_libraries(mali_kbase_emu PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})
set_target_properties(mali_kbase_emu PROPERTIES
   LIBRARY_OUTPUT_DIRECTORY "${EMULATOR_DIR}"
   ARCHIVE_OUTPUT_DIRECTORY "${EMULATOR_DIR}"
)
add_custom_command(TARGET mali_kbase_emu POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mali_kbase_emu> "${CMAKE_BINARY_DIR}/libmali_kbase_emu.so"
)

add_library(mali_kbase_emu_static STATIC
   src/emulator/mali_kbase_emu.c
   src/emulator/lifted_kbase_ko.c
   src/emulator/lifted_csffw.c
   ${CSFFW_GENXML_HDR}
   ${LIFTED_KBASE_KO_LL_OBJ}
   src/emulator/panfrost_valhall_disasm_gen.c
    src/emulator/valhall_vulkan_renderer.cpp
    src/r54p3_vulkan_icd/valhall_spirv_lifter.cpp
)
set_target_properties(mali_kbase_emu_static PROPERTIES
   OUTPUT_NAME mali_kbase_emu
   ARCHIVE_OUTPUT_DIRECTORY "${EMULATOR_DIR}"
)
add_custom_command(TARGET mali_kbase_emu_static POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mali_kbase_emu_static> "${CMAKE_BINARY_DIR}/libmali_kbase_emu.a"
)

# -----------------------------------------------------------------------------
# 4. Source-Patched PanVK ICD Target (libvulkan_panfrost_patched.so) & UAPI Tracer
# -----------------------------------------------------------------------------
add_library(vulkan_panfrost_patched SHARED
   src/vulkan_android_icd/panvk_android_icd.c
)
target_compile_definitions(vulkan_panfrost_patched PRIVATE PANTHOR_KBASE_PATCHED_KMOD=1)
target_link_libraries(vulkan_panfrost_patched PRIVATE panthor_kbase_adapter mali_kbase_emu ${PLATFORM_LIBS})
target_link_options(vulkan_panfrost_patched PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,--no-as-needed)
set_target_properties(vulkan_panfrost_patched PROPERTIES
   LIBRARY_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
file(WRITE "${CMAKE_BINARY_DIR}/panfrost_host_icd.x86_64.json" "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"./libvulkan_panfrost.so\",\n        \"api_version\": \"1.3.296\"\n    }\n}\n")
add_custom_command(TARGET vulkan_panfrost_patched POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vulkan_panfrost_patched> "${CMAKE_BINARY_DIR}/libvulkan_panfrost_patched.so"
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vulkan_panfrost_patched> "${CMAKE_BINARY_DIR}/libvulkan_panfrost.so"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/mali_uapi_tracer_preload.c")
   add_library(mali_uapi_tracer SHARED
      src/tools/mali_uapi_tracer_preload.c
   )
   target_link_libraries(mali_uapi_tracer PRIVATE ${PLATFORM_LIBS})
   set_target_properties(mali_uapi_tracer PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
   )
   add_custom_command(TARGET mali_uapi_tracer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mali_uapi_tracer> "${CMAKE_BINARY_DIR}/libmali_uapi_tracer.so"
   )
endif()

# -----------------------------------------------------------------------------
# 5. Host AOT Translator, Decompiled r54p3 ICD, DXVK Layers & Replayers
# -----------------------------------------------------------------------------
if(PANTHOR_KBASE_BUILD_AOT AND NOT ANDROID)
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd/lib")
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers")

   # Layers
   add_library(VkLayer_PANTHOR_capture SHARED
      src/aot/layers/panthor_vk_capture_layer.cpp
   )
   target_include_directories(VkLayer_PANTHOR_capture PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/mesa/include"
   )
   target_link_options(VkLayer_PANTHOR_capture PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,-soname,libVkLayer_PANTHOR_capture.so)
   target_link_libraries(VkLayer_PANTHOR_capture PRIVATE ${PLATFORM_LIBS})
   set_target_properties(VkLayer_PANTHOR_capture PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   target_copy_to_source_tree(VkLayer_PANTHOR_capture "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers/libVkLayer_PANTHOR_capture.so")

   # Replayers
   add_executable(panthor_drm_replayer
      tools/panthor_drm_replayer.cpp
   )
   target_link_libraries(panthor_drm_replayer PRIVATE panthor_kbase_adapter_static mali_kbase_emu_static ${PLATFORM_LIBS})
   set_target_properties(panthor_drm_replayer PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   add_executable(panthor_vk_replayer
      tools/panthor_vk_replayer.cpp
   )
   target_include_directories(panthor_vk_replayer PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/mesa/include"
   )
   target_link_libraries(panthor_vk_replayer PRIVATE VkLayer_PANTHOR_capture ${PLATFORM_LIBS})
   set_target_properties(panthor_vk_replayer PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   # AOT binaries
   add_executable(test_vulkan_samples_compliance
      src/aot/tests/test_vulkan_samples_compliance.cpp
      src/r54p3_vulkan_icd/valhall_spirv_lifter.cpp
   )
   target_include_directories(test_vulkan_samples_compliance PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd"
   )
   set_target_properties(test_vulkan_samples_compliance PROPERTIES
      CXX_STANDARD 20
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   target_link_libraries(test_vulkan_samples_compliance PRIVATE ${PLATFORM_LIBS})

   add_executable(test_vulkan_icd_client
      src/aot/tests/test_vulkan_icd_client.cpp
   )
   target_link_libraries(test_vulkan_icd_client PRIVATE ${PLATFORM_LIBS})
   set_target_properties(test_vulkan_icd_client PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   set(AOT_MBS2_OBJ "${CMAKE_BINARY_DIR}/r54p3_lifted.o")
   set(AOT_LIBCPP_OBJ "${CMAKE_BINARY_DIR}/r54p3_libcpp_lifted.o")
   set(AOT_LIBCPP_SO "${CMAKE_CURRENT_SOURCE_DIR}/ddk/libc++_shared.so")
   set_source_files_properties("${AOT_MBS2_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
   set_source_files_properties("${AOT_LIBCPP_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

   set(AOT_SEEDS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd/aot_seeds.txt")
   set(AOT_LIFT_EXTRA_DEPENDS)
   if(EXISTS "${AOT_SEEDS_FILE}")
      list(APPEND AOT_LIFT_EXTRA_DEPENDS "${AOT_SEEDS_FILE}")
   endif()

   add_custom_command(
      OUTPUT "${AOT_MBS2_OBJ}"
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/translate.sh" ddk/libGLES_mali.so "${CMAKE_BINARY_DIR}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS horizon2llvm
              "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/translate.sh"
              ${AOT_LIFT_EXTRA_DEPENDS}
      COMMENT "Lifting ddk/libGLES_mali.so -> r54p3_lifted.o (translate.sh)"
   )
   add_custom_command(
      OUTPUT "${AOT_LIBCPP_OBJ}"
      COMMAND $<TARGET_FILE:horizon2llvm> --lift-libcpp "${AOT_LIBCPP_SO}" "${CMAKE_BINARY_DIR}" -j2
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS horizon2llvm
      COMMENT "Lifting NDK libc++_shared.so -> r54p3_libcpp_lifted.o"
   )
   add_custom_target(r54p3_lifted_obj DEPENDS "${AOT_MBS2_OBJ}" "${AOT_LIBCPP_OBJ}")

   add_library(r54p3_vulkan_icd SHARED
      src/r54p3_vulkan_icd/generated/mali_aot_forwarders_autogen.cpp
      src/r54p3_vulkan_icd/generated/mali_plt_thunks_autogen.cpp
      src/r54p3_vulkan_icd/mali_icd_internal.cpp
      src/r54p3_vulkan_icd/mali_icd_physdev.cpp
      src/r54p3_vulkan_icd/mali_icd_wsi.cpp
      src/r54p3_vulkan_icd/mali_plt_dispatch.cpp
      src/r54p3_vulkan_icd/mali_vulkan_icd.cpp
      src/r54p3_vulkan_icd/r54p3_vulkan_icd.cpp
      src/r54p3_vulkan_icd/valhall_spirv_lifter.cpp
      "${AOT_MBS2_OBJ}"
      "${AOT_LIBCPP_OBJ}"
   )
   add_dependencies(r54p3_vulkan_icd r54p3_lifted_obj mali_jit)
   target_include_directories(r54p3_vulkan_icd PRIVATE
      "${CMAKE_BINARY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator"
   )
   target_link_options(r54p3_vulkan_icd PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,--no-as-needed)
   target_link_libraries(r54p3_vulkan_icd PRIVATE mali_kbase_emu z atomic ${PLATFORM_LIBS})
   set_target_properties(r54p3_vulkan_icd PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   file(WRITE "${CMAKE_BINARY_DIR}/r54p3_decompiled_icd.x86_64.json" "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"./libr54p3_vulkan_icd.so\",\n        \"api_version\": \"1.3.250\"\n    }\n}\n")
   file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json" "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"./libr54p3_vulkan_icd.so\",\n        \"api_version\": \"1.3.250\"\n    }\n}\n")
   add_custom_command(TARGET r54p3_vulkan_icd POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:r54p3_vulkan_icd> "${CMAKE_BINARY_DIR}/libr54p3_vulkan_icd.so"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:r54p3_vulkan_icd> "${CMAKE_CURRENT_SOURCE_DIR}/src/r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.so"
   )

   add_custom_target(aot_targets ALL
      DEPENDS r54p3_vulkan_icd
   )
   set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
      "${CMAKE_BINARY_DIR}/mali_mbs2_unopt.ll"
      "${CMAKE_BINARY_DIR}/mali_mbs2_opt.ll"
      "${CMAKE_BINARY_DIR}/r54p3_lifted.o.stamp"
      "${CMAKE_BINARY_DIR}/r54p3_lifted.o"
      "${CMAKE_BINARY_DIR}/libr54p3_vulkan_icd.so"
      "${AOT_DIR}/r54p3_lifted.o"
   )
endif()

# -----------------------------------------------------------------------------
# 6. Test Suites & Formal Verification (CTest Integration)
# -----------------------------------------------------------------------------
if(PANTHOR_KBASE_BUILD_TESTS AND NOT ANDROID)
   enable_testing()
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/artifacts")

   add_executable(unit_tests_edge_cases
      tests/unit_tests_edge_cases.c
   )
   target_link_libraries(unit_tests_edge_cases PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      VkLayer_PANTHOR_capture
      ${PLATFORM_LIBS}
   )
   set_target_properties(unit_tests_edge_cases PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   if(TARGET aot_targets)
      add_dependencies(unit_tests_edge_cases aot_targets)
   endif()
   add_test(NAME edge_case_unit_tests COMMAND unit_tests_edge_cases WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(run_vulkan_samples_and_equivalence_suite
      tests/run_vulkan_samples_and_equivalence_suite.c
   )
   target_link_libraries(run_vulkan_samples_and_equivalence_suite PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   add_dependencies(run_vulkan_samples_and_equivalence_suite vulkan_panfrost_patched)
   set_target_properties(run_vulkan_samples_and_equivalence_suite PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   add_test(NAME vulkan_samples_equivalence_suite COMMAND run_vulkan_samples_and_equivalence_suite WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(panprobe_runner_suite
      tests/panprobe/panprobe_runner_suite.cpp
   )
   target_link_libraries(panprobe_runner_suite PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   set_target_properties(panprobe_runner_suite PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   add_test(NAME panprobe_runner_suite COMMAND panprobe_runner_suite WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(test_ludashi_panvk_aot_and_host
      tests/test_ludashi_panvk_aot_and_host.cpp
   )
   target_link_libraries(test_ludashi_panvk_aot_and_host PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   set_target_properties(test_ludashi_panvk_aot_and_host PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   add_test(NAME ludashi_panvk_aot_and_host COMMAND test_ludashi_panvk_aot_and_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(test_dxvk_aot_layers_e2e
      tests/test_dxvk_aot_layers_e2e.cpp
   )
   target_include_directories(test_dxvk_aot_layers_e2e PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include/uapi"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
   )
   target_link_libraries(test_dxvk_aot_layers_e2e PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   set_target_properties(test_dxvk_aot_layers_e2e PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   if(TARGET aot_targets)
      add_dependencies(test_dxvk_aot_layers_e2e aot_targets)
   endif()
   add_test(NAME test_dxvk_aot_layers_e2e COMMAND test_dxvk_aot_layers_e2e WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   find_package(Python3 COMPONENTS Interpreter)
   if(Python3_Interpreter_FOUND)
      add_test(
         NAME ortools_uapi_formal_verifier
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/verifier/ortools_uapi_verifier.py
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      add_test(
         NAME kbase_kernel_symbolic_verifier
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/verifier/kbase_kernel_symbolic_verifier.py
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      add_test(
         NAME aarch64_mc_lifter_verifier
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/verifier/aarch64_mc_lifter.py ${CMAKE_CURRENT_SOURCE_DIR}/kernel/mali_kbase_gs201_r47p0_csf_5_10_189.ko
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
   add_test(NAME csffw_formal_semantics_verifier
      COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/src/verifier/csffw_formal_semantics_verifier.py"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
   )
   endif()
endif()
