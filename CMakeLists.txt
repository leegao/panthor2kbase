cmake_minimum_required(VERSION 3.18)
project(panthor_kbase_uapi_adapter VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PANTHOR_KBASE_BUILD_TESTS "Build unit, equivalence, and runtime-hook test suites" ON)
option(PANTHOR_KBASE_ENABLE_COVERAGE "Enable gcov/lcov coverage instrumentation" OFF)

if(ANDROID)
   message(STATUS "Configuring Panthor-to-Kbase Adapter Suite for Android NDK (${ANDROID_ABI}, API ${ANDROID_PLATFORM})")
   set(PLATFORM_LIBS dl log m)
   # On Android Bionic API 30+, memfd_create is natively in libc
   add_compile_definitions(HAVE_MEMFD_CREATE=1)
else()
   message(STATUS "Configuring Panthor-to-Kbase Adapter Suite for Host (${CMAKE_SYSTEM_PROCESSOR})")
   find_package(Threads REQUIRED)
   set(PLATFORM_LIBS dl Threads::Threads m)
endif()

if(PANTHOR_KBASE_ENABLE_COVERAGE AND NOT ANDROID)
   add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0 -g)
   add_link_options(--coverage)
else()
   add_compile_options(-O2 -g -Wall -Wextra -Wno-unused-parameter)
endif()

# Ensure shared libraries are protected against premature unloading across dlclose()
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,nodelete")

include_directories(
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter
   ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator
)

# -----------------------------------------------------------------------------
# 1. Core Panthor-to-Kbase UAPI Adapter Library (Shared & Static)
# -----------------------------------------------------------------------------
add_library(panthor_kbase_adapter SHARED
   src/adapter/panthor_kbase_adapter.c
   src/adapter/log.c
   src/adapter/drm_mode_stubs.c
)
target_link_libraries(panthor_kbase_adapter PRIVATE ${PLATFORM_LIBS})

add_library(panthor_kbase_adapter_static STATIC
   src/adapter/panthor_kbase_adapter.c
   src/adapter/log.c
   src/adapter/drm_mode_stubs.c
)
set_target_properties(panthor_kbase_adapter_static PROPERTIES OUTPUT_NAME panthor_kbase_adapter)

# -----------------------------------------------------------------------------
# 2. ELF GOT/PLT Runtime Hook Engine for Unpatched Mesa panvk (Shared & Static)
# -----------------------------------------------------------------------------
add_library(panthor_kbase_runtime_hook SHARED
   src/adapter/panthor_kbase_runtime_hook.c
)
target_link_libraries(panthor_kbase_runtime_hook PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})

add_library(panthor_kbase_runtime_hook_static STATIC
   src/adapter/panthor_kbase_runtime_hook.c
)
set_target_properties(panthor_kbase_runtime_hook_static PROPERTIES OUTPUT_NAME panthor_kbase_runtime_hook)

# -----------------------------------------------------------------------------
# 3. Complete libdrm.a Shim Library (Bundles Adapter + Runtime Hook + KMS Stubs)
#    Used by shims.zip when cross-compiling Mesa 26.3.0 for Android NDK
# -----------------------------------------------------------------------------
add_library(drm_shim STATIC
   src/adapter/panthor_kbase_adapter.c
   src/adapter/log.c
   src/adapter/panthor_kbase_runtime_hook.c
   src/adapter/drm_mode_stubs.c
)
set_target_properties(drm_shim PROPERTIES OUTPUT_NAME drm)

# -----------------------------------------------------------------------------
# 4. User-Space /dev/mali0 Kbase CSF & Valhall ISA Emulator
# -----------------------------------------------------------------------------
add_library(mali_kbase_emu SHARED
   src/emulator/mali_kbase_emu.c
)
target_link_libraries(mali_kbase_emu PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})

# -----------------------------------------------------------------------------
# 5. Dual-Variant PanVK ICD Targets:
#    - Variant A (libvulkan_panfrost_patched.so): Patched pan_kmod / panthor_kmod
#    - Variant B (libvulkan_panfrost_unpatched.so): Unpatched + Runtime Hook
# -----------------------------------------------------------------------------
add_library(vulkan_panfrost_patched SHARED
   src/vulkan_android_icd/panvk_android_icd.c
)
target_compile_definitions(vulkan_panfrost_patched PRIVATE PANTHOR_KBASE_PATCHED_KMOD=1)
target_link_libraries(vulkan_panfrost_patched PRIVATE panthor_kbase_adapter ${PLATFORM_LIBS})

add_library(vulkan_panfrost_unpatched SHARED
   src/vulkan_android_icd/panvk_android_icd.c
)
target_compile_definitions(vulkan_panfrost_unpatched PRIVATE PANTHOR_KBASE_PATCHED_KMOD=0)
# Link with -fno-plt / standard dynamic calls so GOT slots are Exercise-able by runtime hook,
# and also link panthor_kbase_runtime_hook so loading libvulkan_panfrost_unpatched.so
# automatically installs the dl_iterate_phdr() GOT hooks via its constructor.
target_link_libraries(vulkan_panfrost_unpatched PRIVATE panthor_kbase_runtime_hook panthor_kbase_adapter ${PLATFORM_LIBS})

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
      ${PLATFORM_LIBS}
   )
   add_test(NAME edge_case_unit_tests COMMAND unit_tests_edge_cases)

   add_executable(run_vulkan_samples_and_equivalence_suite
      tests/run_vulkan_samples_and_equivalence_suite.c
   )
   target_link_libraries(run_vulkan_samples_and_equivalence_suite PRIVATE
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   add_test(NAME vulkan_samples_equivalence_suite COMMAND run_vulkan_samples_and_equivalence_suite WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

   add_executable(test_runtime_hook_unpatched
      tests/test_runtime_hook_unpatched.c
   )
   target_link_libraries(test_runtime_hook_unpatched PRIVATE
      panthor_kbase_runtime_hook
      panthor_kbase_adapter
      mali_kbase_emu
      ${PLATFORM_LIBS}
   )
   add_test(
      NAME runtime_hook_dual_variant_test
      COMMAND test_runtime_hook_unpatched
         $<TARGET_FILE:vulkan_panfrost_patched>
         $<TARGET_FILE:vulkan_panfrost_unpatched>
   )

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
