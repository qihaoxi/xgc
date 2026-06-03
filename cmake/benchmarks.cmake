if (NOT ENABLE_BENCHMARKS)
    return()
endif ()

set(XGC_BENCH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bench")
file(MAKE_DIRECTORY "${XGC_BENCH_OUTPUT_DIR}")

function(add_xgc_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE xgc)
    xgc_apply_target_defaults(${target_name})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XGC_BENCH_OUTPUT_DIR}"
    )
endfunction()

add_xgc_benchmark(bench_old_to_young_writes ${XGC_PROJECT_ROOT}/bench/old_to_young_writes.c)

message(STATUS "xgc benchmarks enabled")

