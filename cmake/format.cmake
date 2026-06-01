if (NOT DEFINED XGC_PROJECT_ROOT)
    message(FATAL_ERROR "XGC_PROJECT_ROOT is required")
endif ()

if (NOT DEFINED CLANG_FORMAT_EXECUTABLE)
    find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-18 clang-format-17 clang-format-16)
endif ()

if (NOT CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR "clang-format not found. Please install clang-format to use the format target.")
endif ()

set(_xgc_format_roots
    "${XGC_PROJECT_ROOT}/include"
    "${XGC_PROJECT_ROOT}/src"
    "${XGC_PROJECT_ROOT}/tests"
    "${XGC_PROJECT_ROOT}/examples"
)

set(_xgc_format_files)
foreach(_root IN LISTS _xgc_format_roots)
    file(GLOB_RECURSE _files
        "${_root}/*.c"
        "${_root}/*.h"
    )
    list(APPEND _xgc_format_files ${_files})
endforeach()

list(REMOVE_DUPLICATES _xgc_format_files)
list(SORT _xgc_format_files)

if (_xgc_format_files)
    execute_process(
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${_xgc_format_files}
        RESULT_VARIABLE _format_result
    )
    if (NOT _format_result EQUAL 0)
        message(FATAL_ERROR "clang-format failed with exit code ${_format_result}")
    endif ()
    foreach(_file IN LISTS _xgc_format_files)
        message(STATUS "Formatted ${_file}")
    endforeach()
else ()
    message(STATUS "No C/C header files found to format")
endif ()
