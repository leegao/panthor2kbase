cmake_minimum_required(VERSION 3.16)
project(horizon2llvm_llvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
set(H2L_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime")

find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
    set(H2L_LLVM_INCLUDE_DIRS ${LLVM_INCLUDE_DIRS})
    set(H2L_LLVM_LIB_DIR "${LLVM_LIBRARY_DIRS}")
    add_definitions(${LLVM_DEFINITIONS})
    add_library(ThirdPartyLLVM INTERFACE)
    target_link_libraries(ThirdPartyLLVM INTERFACE LLVM)
    target_include_directories(ThirdPartyLLVM INTERFACE ${H2L_LLVM_INCLUDE_DIRS})
endif()

add_library(ultratears_simd_rt STATIC
    "${H2L_RUNTIME_DIR}/simd_rt.c"
)
target_include_directories(ultratears_simd_rt PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${H2L_RUNTIME_DIR}"
)
if(NOT MSVC)
    target_compile_options(ultratears_simd_rt PRIVATE -O2 -fPIC -fno-strict-aliasing)
endif()

set(H2L_LIFTER_PASS_SOURCES
    llvm_backend.cpp
    aarch64/aarch64_lifter.cpp
    passes.cpp
    lowering/pass_guest_stack_alignment.cpp
    lowering/pass_aarch64_idiom_deconstruct.cpp
    lowering/pass_jump_table_lowering.cpp
    lowering/pass_fcmp_predicate_fusion.cpp
    lowering/pass_nzcv_predicate_fusion.cpp
    lowering/pass_nzcv_dead_flag_elim.cpp
    lowering/pass_link_register_elision.cpp
    lowering/pass_tail_call_devirt.cpp
    lowering/pass_aapcs64_abi_contract.cpp
    lowering/pass_cpu_state_promotion.cpp
)

add_library(horizon2llvm_core STATIC
    ${H2L_LIFTER_PASS_SOURCES}
)
target_include_directories(horizon2llvm_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${H2L_RUNTIME_DIR}"
    ${H2L_LLVM_INCLUDE_DIRS}
)
target_compile_definitions(horizon2llvm_core PUBLIC H2L_HAVE_LLVM_HEADERS=1)
target_link_libraries(horizon2llvm_core PUBLIC
    ThirdPartyLLVM
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

add_executable(horizon2llvm main.cpp)
target_link_libraries(horizon2llvm PRIVATE horizon2llvm_core)
