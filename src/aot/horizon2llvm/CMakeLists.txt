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
    -Wl,--start-group ${LLVM21_STATIC_LIBS} -Wl,--end-group
    ${ZLIB_LIB}
    ${ZSTD_LIB}
    dl pthread m
)
