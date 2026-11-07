cmake_minimum_required(VERSION 3.16)
project(r54p3_vulkan_icd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mali0_emu/include")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../mali0_emu/include)
endif()

add_library(r54p3_vulkan_icd_shared SHARED src/r54p3_vulkan_icd.cpp)
set_target_properties(r54p3_vulkan_icd_shared PROPERTIES OUTPUT_NAME "r54p3_vulkan_icd")
target_link_libraries(r54p3_vulkan_icd_shared PRIVATE dl pthread)

add_library(r54p3_vulkan_icd_static STATIC src/r54p3_vulkan_icd.cpp)
set_target_properties(r54p3_vulkan_icd_static PROPERTIES OUTPUT_NAME "r54p3_vulkan_icd")
target_link_libraries(r54p3_vulkan_icd_static PRIVATE dl pthread)

# Generate ICD JSON manifest
set(ICD_JSON_PATH "${CMAKE_CURRENT_BINARY_DIR}/r54p3_decompiled_icd.x86_64.json")
file(WRITE ${ICD_JSON_PATH} "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"$<TARGET_FILE:r54p3_vulkan_icd_shared>\",\n        \"api_version\": \"1.3.250\"\n    }\n}\n")

install(TARGETS r54p3_vulkan_icd_shared r54p3_vulkan_icd_static
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
install(FILES ${ICD_JSON_PATH} DESTINATION share/vulkan/icd.d)
install(DIRECTORY include/ DESTINATION include)
