include_guard(GLOBAL)

function(xgc_apply_debug_options target_name)
    target_compile_definitions(${target_name} PRIVATE "$<$<CONFIG:Debug>:DEBUG>")

    if (MSVC)
        target_compile_options(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:/Zi>"
            "$<$<CONFIG:Debug>:/Od>"
            "$<$<CONFIG:Debug>:/RTC1>"
            "$<$<CONFIG:Debug>:/Oy->"
        )
    else ()
        target_compile_options(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:-g>"
            "$<$<CONFIG:Debug>:-ggdb3>"
            "$<$<CONFIG:Debug>:-O0>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        )

        if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE
                "$<$<CONFIG:Debug>:-mno-omit-leaf-frame-pointer>"
            )
        endif ()
    endif ()
endfunction()
