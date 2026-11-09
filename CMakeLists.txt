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
else()
   message(STATUS "Configuring Panthor-to-Kbase C++ Adapter Suite for Host (${CMAKE_SYSTEM_PROCESSOR})")
   find_package(Threads REQUIRED)
   set(PLATFORM_LIBS dl Threads::Threads m)
endif()

if(PANTHOR_KBASE_ENABLE_COVERAGE AND NOT ANDROID)
   add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0 -g)
   add_link_options(--coverage)
else()
   add_compile_options(-O2 -g -Wall -Wextra -Wno-unused-parameter)
endif()

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,nodelete")

include_directories(
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter
   ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator
)

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

add_library(panthor_kbase_adapter_static STATIC
   src/adapter/panthor_kbase_adapter.cpp
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
set_target_properties(panthor_kbase_adapter_static PROPERTIES OUTPUT_NAME panthor_kbase_adapter)

# -----------------------------------------------------------------------------
# 2. Complete libdrm.a Shim Library (Bundles C++ Adapter + Logger + KMS Stubs)
#    Used by shims.zip when cross-compiling Mesa 26.3.0 for Android NDK
# -----------------------------------------------------------------------------
add_library(drm_shim STATIC
   src/adapter/panthor_kbase_adapter.cpp
   src/adapter/panthor_drm_capture_replay.cpp
   src/adapter/log.cpp
   src/adapter/drm_mode_stubs.c
)
set_target_properties(drm_shim PROPERTIES OUTPUT_NAME drm)

# -----------------------------------------------------------------------------
# 3. User-Space /dev/mali0 Kbase CSF & Valhall ISA Emulator
#    (Includes upstream Mesa ISA.xml generated Valhall disassembler)
# -----------------------------------------------------------------------------
add_library(mali_kbase_emu SHARED
   src/emulator/mali_kbase_emu.c
   src/emulator/panfrost_valhall_disasm_gen.c
)
target_link_libraries(mali_kbase_emu PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})

# -----------------------------------------------------------------------------
# 4. Source-Patched PanVK ICD Target (libvulkan_panfrost_patched.so) & UAPI Tracer
# -----------------------------------------------------------------------------
add_library(vulkan_panfrost_patched SHARED
   src/vulkan_android_icd/panvk_android_icd.c
)
target_compile_definitions(vulkan_panfrost_patched PRIVATE PANTHOR_KBASE_PATCHED_KMOD=1)
target_link_libraries(vulkan_panfrost_patched PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/mali_uapi_tracer_preload.c")
   add_library(mali_uapi_tracer SHARED
      src/tools/mali_uapi_tracer_preload.c
   )
   target_link_libraries(mali_uapi_tracer PRIVATE ${PLATFORM_LIBS})
endif()

# -----------------------------------------------------------------------------
# 5. Host AOT Translator, Decompiled r54p3 ICD, DXVK Layers & Vulkan Test Suites
# -----------------------------------------------------------------------------
if(PANTHOR_KBASE_BUILD_AOT AND NOT ANDROID)
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/bin")
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/r54p3_vulkan_icd/lib")

   add_executable(aot_vkcube
      src/aot/tests/vkcube_wsi_e2e.cpp
   )
   set_target_properties(aot_vkcube PROPERTIES
      OUTPUT_NAME vkcube
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/bin"
      BUILD_RPATH "\$ORIGIN/../../..;\$ORIGIN"
   )
   target_link_libraries(aot_vkcube PRIVATE ${PLATFORM_LIBS})

   add_executable(test_vulkan_samples_compliance
      src/aot/tests/test_vulkan_samples_compliance.cpp
   )
   target_include_directories(test_vulkan_samples_compliance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include")
   set_target_properties(test_vulkan_samples_compliance PROPERTIES
      CXX_STANDARD 20
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/bin"
      BUILD_RPATH "\$ORIGIN/../../..;\$ORIGIN"
   )
   target_link_libraries(test_vulkan_samples_compliance PRIVATE ${PLATFORM_LIBS})

   add_executable(test_vulkan_icd_client
      src/aot/tests/test_vulkan_icd_client.cpp
   )
   set_target_properties(test_vulkan_icd_client PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/bin"
      BUILD_RPATH "\$ORIGIN/../../..;\$ORIGIN"
   )
   target_link_libraries(test_vulkan_icd_client PRIVATE ${PLATFORM_LIBS})

   add_library(r54p3_vulkan_icd SHARED
      src/aot/r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp
   )
   target_include_directories(r54p3_vulkan_icd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/r54p3_vulkan_icd/include")
   target_link_options(r54p3_vulkan_icd PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions)
   set_target_properties(r54p3_vulkan_icd PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/r54p3_vulkan_icd/lib"
      BUILD_RPATH "\$ORIGIN/../../../..;\$ORIGIN"
   )
   target_link_libraries(r54p3_vulkan_icd PRIVATE mali_kbase_emu ${PLATFORM_LIBS})

   add_library(VkLayer_PANTHOR_capture SHARED
      src/aot/layers/panthor_vk_capture_layer.cpp
   )
   target_include_directories(VkLayer_PANTHOR_capture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include")
   target_link_options(VkLayer_PANTHOR_capture PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,-soname,libVkLayer_PANTHOR_capture.so)
   set_target_properties(VkLayer_PANTHOR_capture PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers"
      OUTPUT_NAME "VkLayer_PANTHOR_capture"
   )
   target_link_libraries(VkLayer_PANTHOR_capture PRIVATE ${PLATFORM_LIBS})

   add_executable(panthor_drm_replayer
      tools/panthor_drm_replayer.cpp
   )
   target_link_libraries(panthor_drm_replayer PRIVATE panthor_kbase_adapter mali_kbase_emu ${PLATFORM_LIBS})

   add_executable(panthor_vk_replayer
      tools/panthor_vk_replayer.cpp
      src/aot/layers/panthor_vk_capture_layer.cpp
   )
   target_include_directories(panthor_vk_replayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/include")
   target_link_libraries(panthor_vk_replayer PRIVATE ${PLATFORM_LIBS})
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
   set_target_properties(unit_tests_edge_cases PROPERTIES
      BUILD_RPATH "${CMAKE_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/aot/layers"
   )
   target_link_libraries(unit_tests_edge_cases PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      VkLayer_PANTHOR_capture
      ${PLATFORM_LIBS}
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
   add_test(NAME vulkan_samples_equivalence_suite COMMAND run_vulkan_samples_and_equivalence_suite WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(panprobe_runner_suite
      tests/panprobe/panprobe_runner_suite.cpp
   )
   target_link_libraries(panprobe_runner_suite PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
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
   add_test(NAME ludashi_panvk_aot_and_host COMMAND test_ludashi_panvk_aot_and_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
