cmake_minimum_required(VERSION 3.16)
project(horizon2llvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_program(LLVM_CONFIG_EXE NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15)
if(LLVM_CONFIG_EXE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --includedir OUTPUT_VARIABLE LLVM_INC_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libdir OUTPUT_VARIABLE LLVM_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --version OUTPUT_VARIABLE LLVM_VER_STR OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Found llvm-config (${LLVM_VER_STR}): inc=${LLVM_INC_DIR} lib=${LLVM_LIB_DIR}")
    include_directories(${LLVM_INC_DIR})
    link_directories(${LLVM_LIB_DIR})
    find_library(LLVM_SHARED_LIB NAMES LLVM LLVM-21 LLVM-20 LLVM-19 LLVM-18 LLVM-17 LLVM-16 LLVM-15 PATHS ${LLVM_LIB_DIR} NO_DEFAULT_PATH)
endif()

if(NOT LLVM_SHARED_LIB)
    find_library(LLVM_SHARED_LIB NAMES LLVM LLVM-21 LLVM-20 LLVM-19 LLVM-18 LLVM-17 LLVM-16 LLVM-15 PATHS /usr/lib/llvm-21/lib /usr/lib/llvm-20/lib /usr/lib/llvm-19/lib /usr/lib/llvm-18/lib /usr/lib/x86_64-linux-gnu /usr/lib)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB_RECURSE CORE_SRCS
    "ir/*.cpp"
    "frontend/*.cpp"
    "lowering/*.cpp"
    "llvm_backend.cpp"
)

add_library(horizon2llvm_core STATIC ${CORE_SRCS})
target_compile_options(horizon2llvm_core PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

add_executable(horizon2llvm main.cpp)
target_compile_options(horizon2llvm PRIVATE -O2 -Wall -Wextra)
target_link_libraries(horizon2llvm PRIVATE horizon2llvm_core ${LLVM_SHARED_LIB} dl pthread)
if(LLVM_LIB_DIR)
    set_target_properties(horizon2llvm PROPERTIES BUILD_RPATH "${LLVM_LIB_DIR}")
endif()
