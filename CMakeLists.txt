cmake_minimum_required(VERSION 3.18)
project(panthor_kbase_uapi_adapter VERSION 2.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PANTHOR_KBASE_BUILD_TESTS "Build unit, compliance, and equivalence test suites" ON)
option(PANTHOR_KBASE_BUILD_AOT "Build AOT lifter, libvulkan_mali_aot_icd.so, and DXVK layers" ON)
option(PANTHOR_KBASE_ENABLE_COVERAGE "Enable gcov/lcov coverage instrumentation" OFF)

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
   add_compile_options(-O2 -g -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
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
set(ROOT_RPATH "$ORIGIN;$ORIGIN/..;$ORIGIN/../adapter;$ORIGIN/../emulator;$ORIGIN/../aot;$ORIGIN/../../build-x86-64;$ORIGIN/../../build-x86-64/adapter;$ORIGIN/../../build-x86-64/emulator;$ORIGIN/../../build-x86-64/aot;${CMAKE_BINARY_DIR};${ADAPTER_DIR};${EMULATOR_DIR};${AOT_DIR};${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers")
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
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
set_target_properties(drm_shim PROPERTIES
   OUTPUT_NAME drm
   ARCHIVE_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
add_custom_command(TARGET drm_shim POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_BINARY_DIR}/libdrm.a"
   COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/shims/lib"
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_CURRENT_SOURCE_DIR}/shims/lib/libdrm.a"
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:drm_shim> "${CMAKE_CURRENT_SOURCE_DIR}/shims/libdrm.a"
)

# -----------------------------------------------------------------------------
# 3. User-Space /dev/mali0 Kbase CSF & Valhall ISA Emulator (Shared & Static)
# -----------------------------------------------------------------------------
add_library(mali_kbase_emu SHARED
   src/emulator/mali_kbase_emu.c
   src/emulator/panfrost_valhall_disasm_gen.c
)
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
   src/emulator/panfrost_valhall_disasm_gen.c
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
target_link_libraries(vulkan_panfrost_patched PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})
set_target_properties(vulkan_panfrost_patched PROPERTIES
   LIBRARY_OUTPUT_DIRECTORY "${ADAPTER_DIR}"
)
add_custom_command(TARGET vulkan_panfrost_patched POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vulkan_panfrost_patched> "${CMAKE_BINARY_DIR}/libvulkan_panfrost_patched.so"
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
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/bin")
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/r54p3_vulkan_icd/lib")
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers")

   # Layers
   add_library(VkLayer_BCN_bcn_layer SHARED
      src/aot/layers/bcn_layer_host.cpp
   )
   target_link_options(VkLayer_BCN_bcn_layer PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,-soname,libVkLayer_BCN_bcn_layer.so)
   target_link_libraries(VkLayer_BCN_bcn_layer PRIVATE ${PLATFORM_LIBS})
   set_target_properties(VkLayer_BCN_bcn_layer PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   target_copy_to_source_tree(VkLayer_BCN_bcn_layer "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers/libVkLayer_BCN_bcn_layer.so")

   add_library(VkLayer_COMPAT_dxvk_mali_compat SHARED
      src/aot/layers/compat_layer_host.cpp
   )
   target_link_options(VkLayer_COMPAT_dxvk_mali_compat PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,-soname,libVkLayer_COMPAT_dxvk_mali_compat.so)
   target_link_libraries(VkLayer_COMPAT_dxvk_mali_compat PRIVATE ${PLATFORM_LIBS})
   set_target_properties(VkLayer_COMPAT_dxvk_mali_compat PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )
   target_copy_to_source_tree(VkLayer_COMPAT_dxvk_mali_compat "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers/libVkLayer_COMPAT_dxvk_mali_compat.so")

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

   # Ensure layer JSON descriptor manifests exist
   add_custom_target(generate_layer_manifests ALL
      COMMAND ${CMAKE_COMMAND} -E echo "{\"file_format_version\":\"1.2.0\",\"layer\":{\"name\":\"VK_LAYER_BCN_BCnLayer\",\"type\":\"GLOBAL\",\"library_path\":\"./libVkLayer_BCN_bcn_layer.so\",\"api_version\":\"1.3.0\",\"implementation_version\":\"1\",\"description\":\"BCn Emulation Layer for Mobile GPUs (Host AOT Build)\"}}" > "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers/VkLayer_BCN_bcn_layer.json"
      COMMAND ${CMAKE_COMMAND} -E echo "{\"file_format_version\":\"1.2.0\",\"layer\":{\"name\":\"VK_LAYER_COMPAT_DxvkMaliCompatLayer\",\"type\":\"GLOBAL\",\"library_path\":\"./libVkLayer_COMPAT_dxvk_mali_compat.so\",\"api_version\":\"1.3.0\",\"implementation_version\":\"1\",\"description\":\"DXVK 1.10.3 Feature & Limit Compatibility Layer for Mali r54p3 AOT ICD\"}}" > "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers/VkLayer_COMPAT_dxvk_mali_compat.json"
      COMMENT "Generating DXVK layer JSON descriptors"
   )
   add_dependencies(VkLayer_BCN_bcn_layer generate_layer_manifests)

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
   add_executable(aot_vkcube
      src/aot/tests/vkcube_wsi_e2e.cpp
   )
   target_link_libraries(aot_vkcube PRIVATE ${PLATFORM_LIBS})
   set_target_properties(aot_vkcube PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   add_executable(test_vulkan_samples_compliance
      src/aot/tests/test_vulkan_samples_compliance.cpp
   )
   target_include_directories(test_vulkan_samples_compliance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include")
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

   add_library(r54p3_vulkan_icd SHARED
      src/aot/r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp
   )
   target_include_directories(r54p3_vulkan_icd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/r54p3_vulkan_icd/include")
   target_link_options(r54p3_vulkan_icd PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions)
   target_link_libraries(r54p3_vulkan_icd PRIVATE mali_kbase_emu_static panthor_kbase_adapter_static ${PLATFORM_LIBS})
   set_target_properties(r54p3_vulkan_icd PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   add_executable(mali_llvm_pass_driver
      src/aot/src/mali_llvm_passes.cpp
   )
   target_link_libraries(mali_llvm_pass_driver PRIVATE dl ${PLATFORM_LIBS})
   set_target_properties(mali_llvm_pass_driver PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${AOT_DIR}"
   )

   # AOT Lifter target
   add_custom_target(aot_targets ALL
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/translate.sh" libGLES_mali.so "${CMAKE_BINARY_DIR}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS mali_kbase_emu_static panthor_kbase_adapter_static mali_llvm_pass_driver
      COMMENT "Running 64-bit Mali AOT translation pipeline (translate.sh)"
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

   add_executable(test_wayland_and_vkgears_e2e
      tests/test_wayland_and_vkgears_e2e.cpp
   )
   target_include_directories(test_wayland_and_vkgears_e2e PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/mesa/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include"
   )
   set(TEST_WAYLAND_LIBS panthor_kbase_adapter mali_kbase_emu ${PLATFORM_LIBS})
   if(VULKAN_LIB)
      list(APPEND TEST_WAYLAND_LIBS ${VULKAN_LIB})
   endif()
   target_link_libraries(test_wayland_and_vkgears_e2e PRIVATE ${TEST_WAYLAND_LIBS})
   set_target_properties(test_wayland_and_vkgears_e2e PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TESTS_DIR}"
   )
   if(TARGET aot_targets)
      add_dependencies(test_wayland_and_vkgears_e2e aot_targets)
   endif()
   add_test(NAME test_wayland_and_vkgears_e2e COMMAND test_wayland_and_vkgears_e2e WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/verifier/aarch64_mc_lifter.py ${CMAKE_CURRENT_SOURCE_DIR}/mali_kbase.ko
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
   endif()
endif()
