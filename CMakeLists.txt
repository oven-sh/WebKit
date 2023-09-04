# -----------------------------------------------------------------------------
# Determine CMake version and build type.
# -----------------------------------------------------------------------------
# NOTE: cmake_minimum_required() and project() *MUST* be the two first commands
# used, see https://cmake.org/cmake/help/v3.3/command/project.html -- the
# latter in particular handles loading a bunch of shared CMake definitions
# and loading the cross-compilation settings from CMAKE_TOOLCHAIN_FILE.

cmake_minimum_required(VERSION 3.16)

# https://github.com/microsoft/vcpkg#vcpkg-as-a-submodule-with-cmake
# > When using vcpkg as a submodule of your project, 
# > you can add the following to your CMakeLists.txt before the first project() call,
# > instead of passing CMAKE_TOOLCHAIN_FILE to the cmake invocation.
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
  CACHE STRING "Vcpkg toolchain file")

set(VCPKG_LIBRARY_LINKAGE static)
if (WIN32)
    set(VCPKG_CRT_LINKAGE static)
    set(VCPKG_TARGET_ARCHITECTURE x64)
endif()

project(WebKit)

# Remove this cmake_policy() after upgrading cmake_minimum_required() to 3.20.
if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.20")
    cmake_policy(SET CMP0116 OLD)
endif ()

# -----------------------------------------------------------------------------
# Common configuration
#------------------------------------------------------------------------------
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/Source/cmake")
include(WebKitCommon)

# -----------------------------------------------------------------------------
# Enable API unit tests and create a target for the test runner
# -----------------------------------------------------------------------------
if (ENABLE_API_TESTS)
    enable_testing()
endif ()

# -----------------------------------------------------------------------------
# Add module directories
# -----------------------------------------------------------------------------
add_subdirectory(Source)

# -----------------------------------------------------------------------------
# Add tools
# -----------------------------------------------------------------------------
if (ENABLE_TOOLS)
    add_subdirectory(Tools)
endif ()

if (DEVELOPER_MODE)
    add_subdirectory(PerformanceTests)
endif ()

# -----------------------------------------------------------------------------
# Print the features list last, for maximum visibility.
# -----------------------------------------------------------------------------
PRINT_WEBKIT_OPTIONS()
