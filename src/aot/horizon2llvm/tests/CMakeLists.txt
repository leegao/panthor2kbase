add_executable(lifter_unit_tests
    test_main_snippets.cpp
)
target_include_directories(lifter_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(lifter_unit_tests PRIVATE
    horizon2llvm_core
    ultratears_simd_rt
)
add_test(NAME lifter_unit_tests COMMAND lifter_unit_tests)
