set(XGC_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests")
file(MAKE_DIRECTORY "${XGC_TEST_OUTPUT_DIR}")

enable_testing()

function(add_xgc_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_TEST_OUTPUT_DIR}"
    )
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# Tests will be registered here as source files are created
