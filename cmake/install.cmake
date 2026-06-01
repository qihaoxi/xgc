install(TARGETS xgc
    EXPORT xgcTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if (TARGET xgc_shared)
    install(TARGETS xgc_shared
        EXPORT xgcTargets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endif ()

install(FILES
    ${XGC_INCLUDE_ROOT}/xgc/gc.h
    ${XGC_INCLUDE_ROOT}/xgc/gc_config.h
    DESTINATION include/xgc
)

install(EXPORT xgcTargets
    FILE xgcTargets.cmake
    NAMESPACE xgc::
    DESTINATION lib/cmake/xgc
)
