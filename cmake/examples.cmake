if (NOT ENABLE_EXAMPLES)
    return()
endif ()

set(XGC_EXAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/examples")
file(MAKE_DIRECTORY "${XGC_EXAMPLE_OUTPUT_DIR}")

function(add_xgc_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_EXAMPLE_OUTPUT_DIR}"
    )
endfunction()

if (GC_ALGORITHM STREQUAL "marksweep")
    add_xgc_example(marksweep_demo ${XGC_PROJECT_ROOT}/examples/marksweep_demo.c)
endif ()

if (GC_ALGORITHM STREQUAL "gen_copy_ms")
    add_xgc_example(gen_copy_ms_demo ${XGC_PROJECT_ROOT}/examples/gen_copy_ms_demo.c)
endif ()

message(STATUS "xgc examples enabled")
