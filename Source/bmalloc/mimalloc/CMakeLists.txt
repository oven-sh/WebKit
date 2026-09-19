# Bun: the mimalloc that is built is Bun's fork, oven-sh/mimalloc (branch bun-dev3-v2), at the commit Bun itself links,
# and no other. MIMALLOC_SOURCE_DIR is a checkout of it. The copy upstream WebKit keeps next to this file (mimalloc/,
# upstream mimalloc 3.2.8) is left as upstream has it and is not built: no add_subdirectory(mimalloc) here.
#
#   - CI: mimalloc/source.json names the commit, every Dockerfile's `base` stage downloads it to /mimalloc, and a lane
#     passes -DMIMALLOC_SOURCE_DIR=/mimalloc.
#   - Bun building WebKit from source (scripts/build/deps/webkit.ts in oven-sh/bun) passes the mimalloc it fetched for
#     its own build.
#   - By hand: git clone -b bun-dev3-v2 https://github.com/oven-sh/mimalloc, check out the commit in
#     mimalloc/source.json, and pass -DMIMALLOC_SOURCE_DIR=<that directory>.
#
# The libraries compile against ${MIMALLOC_SOURCE_DIR}/include (Source/bmalloc/CMakeLists.txt) and ship that mimalloc.h as
# bmalloc/mimalloc.h. With USE_EXTERNAL_MIMALLOC, which is how Bun's lanes are built, they leave mi_* unresolved for Bun
# to link, and mimalloc-obj below is linked by the executables only (jsc, testFFI). It is compiled the way Bun compiles
# its own: one translation unit, as C++, with the defines and flags of scripts/build/deps/mimalloc.ts in oven-sh/bun.
# Keep the two lists the same, so that what the tests run on is what Bun ships.

if (NOT MIMALLOC_SOURCE_DIR)
    message(FATAL_ERROR "USE_MIMALLOC needs -DMIMALLOC_SOURCE_DIR=<a checkout of https://github.com/oven-sh/mimalloc at the commit in mimalloc/source.json>. The copy of upstream's mimalloc in Source/bmalloc/mimalloc/mimalloc is deliberately not a fallback: see Source/bmalloc/mimalloc/CMakeLists.txt.")
endif ()
if (NOT EXISTS "${MIMALLOC_SOURCE_DIR}/src/static.c" OR NOT EXISTS "${MIMALLOC_SOURCE_DIR}/include/mimalloc.h")
    message(FATAL_ERROR "MIMALLOC_SOURCE_DIR (${MIMALLOC_SOURCE_DIR}) is not a mimalloc checkout: no src/static.c or include/mimalloc.h in it.")
endif ()
# Only Bun's fork has MI_NO_PROCESS_DETACH (without it mi_process_done runs at exit under threads that still allocate).
file(READ "${MIMALLOC_SOURCE_DIR}/src/prim/prim.c" _mimalloc_prim_c)
if (NOT _mimalloc_prim_c MATCHES "MI_NO_PROCESS_DETACH")
    message(FATAL_ERROR "MIMALLOC_SOURCE_DIR (${MIMALLOC_SOURCE_DIR}) is not oven-sh/mimalloc: src/prim/prim.c has no MI_NO_PROCESS_DETACH.")
endif ()
unset(_mimalloc_prim_c)

WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()

# src/static.c includes every other source file.
add_library(mimalloc-obj OBJECT "${MIMALLOC_SOURCE_DIR}/src/static.c")
# As C++: the C and C++ ABIs differ around structs with trailing flexible arrays, and C++ code uses these types.
set_source_files_properties("${MIMALLOC_SOURCE_DIR}/src/static.c" PROPERTIES LANGUAGE CXX)
set_target_properties(mimalloc-obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(mimalloc-obj PRIVATE "${MIMALLOC_SOURCE_DIR}/include")

string(TOLOWER "${CMAKE_BUILD_TYPE}" _mimalloc_build_type)
target_compile_definitions(mimalloc-obj PRIVATE
    MI_STATIC_LIB
    # No walk of every heap at exit, and no mi_process_done at all: it tears down locks and thread-local state under
    # threads that are still running.
    MI_SKIP_COLLECT_ON_EXIT=1
    MI_NO_PROCESS_DETACH=1
    # mi_free finds a block's page through the page map. Without it an arena must start on a 256 MiB boundary and
    # mi_manage_os_memory_ex refuses JSC's structure heap when its reservation is 256 MiB or less.
    MI_FREE_USE_PAGEMAP=1
    MI_CMAKE_BUILD_TYPE=${_mimalloc_build_type}
)
if (NOT _mimalloc_build_type STREQUAL "debug")
    target_compile_definitions(mimalloc-obj PRIVATE MI_BUILD_RELEASE)
else ()
    target_compile_definitions(mimalloc-obj PRIVATE MI_DEBUG=3)
endif ()
if (ENABLE_SANITIZERS MATCHES "address")
    target_compile_definitions(mimalloc-obj PRIVATE MI_TRACK_ASAN=1 MI_UBSAN=1)
endif ()

set(_mimalloc_musl OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # No transparent huge pages for mimalloc's arenas.
    target_compile_definitions(mimalloc-obj PRIVATE MI_DEFAULT_ALLOW_THP=0)
    if (NOT ANDROID)
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("#include <features.h>\n#ifndef __GLIBC__\n#error not glibc\n#endif\nint main() { return 0; }" MIMALLOC_LIBC_IS_GLIBC)
        if (NOT MIMALLOC_LIBC_IS_GLIBC)
            set(_mimalloc_musl ON)
            target_compile_definitions(mimalloc-obj PRIVATE MI_LIBC_MUSL=1)
        endif ()
    endif ()
    # malloc, free and the rest of the process go to mimalloc too, as in Bun on Linux. Not under ASan, whose
    # interceptors must see the real malloc.
    if (NOT ENABLE_SANITIZERS MATCHES "address")
        target_compile_definitions(mimalloc-obj PRIVATE MI_MALLOC_OVERRIDE)
        target_compile_options(mimalloc-obj PRIVATE -fno-builtin-malloc)
    endif ()
endif ()

if (WIN32)
    # C compiled as C++ by clang-cl: every C idiom is a warning.
    target_compile_options(mimalloc-obj PRIVATE -w)
else ()
    target_compile_options(mimalloc-obj PRIVATE
        -fvisibility=hidden
        -Wno-deprecated
        -Wno-static-in-inline
        -Wno-undef
        -Wno-cast-align
    )
    # initial-exec: one slot in the executable's static TLS block, no __tls_get_addr. musl's static TLS block has a
    # fixed size, so local-dynamic there.
    if (_mimalloc_musl)
        target_compile_options(mimalloc-obj PRIVATE -ftls-model=local-dynamic)
    else ()
        target_compile_options(mimalloc-obj PRIVATE -ftls-model=initial-exec)
    endif ()
endif ()
unset(_mimalloc_build_type)
unset(_mimalloc_musl)
