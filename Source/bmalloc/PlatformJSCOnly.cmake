if (APPLE AND NOT USE_BUN_JSC_ADDITIONS)
    list(APPEND bmalloc_SOURCES
        bmalloc/ProcessCheck.mm
    )
endif ()
