set(VCPKG_TARGET_ARCHITECTURE arm64)
# For Bun, we want static linking to produce a single .exe
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# The following libraries should always be static
if (PORT STREQUAL "highway")
    set(VCPKG_LIBRARY_LINKAGE static)
elseif (PORT STREQUAL "pixman")
    set(VCPKG_LIBRARY_LINKAGE static)
endif ()

# Turn on zlib compatibility
if (PORT STREQUAL "zlib-ng")
    set(ZLIB_COMPAT ON)
endif ()
