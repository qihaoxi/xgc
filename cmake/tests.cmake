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

add_xgc_test(test_barrier_substrate ${XGC_PROJECT_ROOT}/tests/test_barrier_substrate.c)
add_xgc_test(test_heap_substrate ${XGC_PROJECT_ROOT}/tests/test_heap_substrate.c)

if (GC_ALGORITHM STREQUAL "marksweep")
    add_xgc_test(test_marksweep_smoke ${XGC_PROJECT_ROOT}/tests/test_marksweep_smoke.c)
endif ()

if (GC_ALGORITHM STREQUAL "gen_copy_ms")
    add_xgc_test(test_gen_copy_ms_smoke ${XGC_PROJECT_ROOT}/tests/test_gen_copy_ms_smoke.c)
endif ()

