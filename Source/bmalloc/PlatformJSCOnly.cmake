if(APPLE)
endif()

if(BUN_FAST_TLS)
    add_definitions(-DBUN_FAST_TLS=1)
    add_definitions(-DUSE_BUN_FAST_TLS=1)
    add_definitions(-DHAVE_BUN_FAST_TLS=1)
    add_definitions(-DPAS_HAVE_PTHREAD_MACHDEP_H=1)
endif()