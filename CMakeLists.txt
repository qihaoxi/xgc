cmake_minimum_required(VERSION 3.12)

if (DEFINED XGC_PROJECT_ROOT)
    get_filename_component(XGC_PROJECT_ROOT "${XGC_PROJECT_ROOT}" ABSOLUTE)
else ()
    set(XGC_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
endif ()

project(xgc VERSION 0.1.0 LANGUAGES C)

set(XGC_SOURCE_ROOT "${XGC_PROJECT_ROOT}/src")
set(XGC_INCLUDE_ROOT "${XGC_PROJECT_ROOT}/include")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_SHARED "Build shared xgc library" ON)
option(ENABLE_EXAMPLES "Build xgc examples" ON)
option(ENABLE_TESTS "Build xgc tests" ON)
option(ENABLE_BENCHMARKS "Build xgc microbenchmarks" OFF)
option(GC_MULTITHREAD "Enable multi-threading support (atomic RC + background GC)" OFF)

include(${XGC_PROJECT_ROOT}/cmake/sanitizer.cmake)
include(${XGC_PROJECT_ROOT}/cmake/debug.cmake)
include(${XGC_PROJECT_ROOT}/cmake/compiler-options.cmake)

# ---- Format target ----
add_custom_target(format
    COMMAND ${CMAKE_COMMAND}
    -DXGC_PROJECT_ROOT=${XGC_PROJECT_ROOT}
    -P ${XGC_PROJECT_ROOT}/cmake/format.cmake
    WORKING_DIRECTORY ${XGC_PROJECT_ROOT}
    COMMENT "Running clang-format over xgc sources"
)

# ---- GC Algorithm select options ----
# gen_copy_ms/marksweep/bacon-rajan
set(GC_ALGORITHM "gen_copy_ms" CACHE STRING "GC algorithm to build with")
set_property(CACHE GC_ALGORITHM PROPERTY STRINGS
    gen_copy_ms
    marksweep
    bacon-rajan
)


# ---- Library sources ----
# Algorithm-specific sources (only the selected algorithm is compiled)
file(GLOB_RECURSE XGC_ALGO_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/algo/${GC_ALGORITHM}/*.c
)
# Shared runtime/core substrate sources
file(GLOB_RECURSE XGC_SHARED_SOURCES CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/core/*.c
)
file(GLOB_RECURSE XGC_LIBRARY_HEADERS CONFIGURE_DEPENDS
    ${XGC_SOURCE_ROOT}/*.h
)

set(XGC_LIBRARY_SOURCES
    ${XGC_ALGO_SOURCES}
    ${XGC_SHARED_SOURCES}
)

add_library(xgc STATIC ${XGC_LIBRARY_SOURCES} ${XGC_LIBRARY_HEADERS})
add_library(xgc::xgc ALIAS xgc)
target_include_directories(xgc
    PUBLIC
    $<BUILD_INTERFACE:${XGC_INCLUDE_ROOT}>
    $<BUILD_INTERFACE:${XGC_SOURCE_ROOT}>
    $<INSTALL_INTERFACE:include>
)
xgc_apply_target_defaults(xgc)
xgc_apply_debug_options(xgc)

if (GC_MULTITHREAD)
    target_compile_definitions(xgc PUBLIC GC_MULTITHREAD)
endif ()

if (ENABLE_SHARED)
    add_library(xgc_shared SHARED ${XGC_LIBRARY_SOURCES} ${XGC_LIBRARY_HEADERS})
    set_target_properties(xgc_shared PROPERTIES OUTPUT_NAME xgc)
    target_include_directories(xgc_shared
        PUBLIC
        $<BUILD_INTERFACE:${XGC_INCLUDE_ROOT}>
        $<BUILD_INTERFACE:${XGC_SOURCE_ROOT}>
        $<INSTALL_INTERFACE:include>
    )
    xgc_apply_target_defaults(xgc_shared)
    xgc_apply_debug_options(xgc_shared)
    if (GC_MULTITHREAD)
        target_compile_definitions(xgc_shared PUBLIC GC_MULTITHREAD)
    endif ()
endif ()

if (ENABLE_EXAMPLES)
    include(${XGC_PROJECT_ROOT}/cmake/examples.cmake)
endif ()

if (ENABLE_TESTS)
    include(CTest)
    include(${XGC_PROJECT_ROOT}/cmake/tests.cmake)
endif ()

if (ENABLE_BENCHMARKS)
    include(${XGC_PROJECT_ROOT}/cmake/benchmarks.cmake)
endif ()

include(${XGC_PROJECT_ROOT}/cmake/install.cmake)

message(STATUS "xgc version: ${PROJECT_VERSION}")
message(STATUS "xgc source root: ${XGC_SOURCE_ROOT}")
message(STATUS "GC Algorithm:    ${GC_ALGORITHM}")
message(STATUS "GC_MULTITHREAD:  ${GC_MULTITHREAD}")
