cmake_minimum_required(VERSION 3.16)
project(horizon2llvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LLVM21_INC_DIR "${REPO_ROOT}/third_party/prebuilts/llvm-21/include" "${REPO_ROOT}/third_party/prebuilts/llvm-21/include")
set(LLVM21_LIB_DIR "${REPO_ROOT}/third_party/prebuilts/llvm-21/lib")
file(GLOB LLVM21_STATIC_LIBS "${LLVM21_LIB_DIR}/*.a")
list(SORT LLVM21_STATIC_LIBS)

# The pinned prebuilt static libs are .gitignored, so fall back to llvm-config's libLLVM.
set(LLVM21_LINK_LIBS "")
if(LLVM21_STATIC_LIBS)
    list(APPEND LLVM21_LINK_LIBS -Wl,--start-group ${LLVM21_STATIC_LIBS} -Wl,--end-group)
else()
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
        message(FATAL_ERROR "horizon2llvm: no LLVM-21 found (third_party/prebuilts/llvm-21/lib is empty and no llvm-config).")
    endif()
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${LLVM21_INC_DIR})

file(GLOB_RECURSE CORE_SRCS
    "ir/*.cpp"
    "frontend/*.cpp"
    "lowering/*.cpp"
    "llvm_backend.cpp"
)

add_library(horizon2llvm_core STATIC ${CORE_SRCS})
target_compile_options(horizon2llvm_core PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

find_library(ZLIB_LIB NAMES z libz.so.1 HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64)
find_library(ZSTD_LIB NAMES zstd libzstd.so.1 HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64)

add_executable(horizon2llvm main.cpp)
target_compile_options(horizon2llvm PRIVATE -O2 -Wall -Wextra)
target_link_libraries(horizon2llvm PRIVATE
    horizon2llvm_core
    ${LLVM21_LINK_LIBS}
    ${ZLIB_LIB}
    ${ZSTD_LIB}
    dl pthread m
)
