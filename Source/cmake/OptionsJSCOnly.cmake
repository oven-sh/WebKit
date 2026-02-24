find_package(Threads REQUIRED)

if (MSVC)
    include(OptionsMSVC)
else ()
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif ()

add_definitions(-DBUILDING_JSCONLY__)

set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_MICRO 0)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})

WEBKIT_OPTION_BEGIN()

# All option definitions must be between WEBKIT_OPTION_BEGIN() and WEBKIT_OPTION_END()
WEBKIT_OPTION_DEFINE(USE_BUN_JSC_ADDITIONS "Whether to enable Bun's JSC additions" PUBLIC OFF)
WEBKIT_OPTION_DEFINE(ENABLE_MALLOC_HEAP_BREAKDOWN "Whether to enable MALLOC_HEAP_BREAKDOWN" PUBLIC OFF)
WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Whether to build JavaScriptCore as a static library." PUBLIC OFF)
WEBKIT_OPTION_DEFINE(USE_LIBBACKTRACE "Whether to enable usage of libbacktrace." PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE OFF)
if (NOT WIN32)
    WEBKIT_OPTION_DEFINE(ENABLE_FUZZILLI "Whether to build JavaScriptCore with support for Fuzzilli." PUBLIC OFF)
endif ()
WEBKIT_OPTION_DEFINE(ENABLE_JSC_GLIB_API "Whether to enable the JavaScriptCore GLib API." PUBLIC OFF)
WEBKIT_OPTION_DEFINE(USE_SYSTEM_UNIFDEF "Whether to use a system-provided unifdef" PRIVATE OFF)
WEBKIT_OPTION_DEFINE(ALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS "Whether to allow line numbers & column numbers in builtins" PUBLIC OFF)
WEBKIT_OPTION_DEFINE(ENABLE_REMOTE_INSPECTOR "Whether to build JavaScriptCore with remote inspector support" PUBLIC OFF)
WEBKIT_OPTION_DEFINE(ENABLE_BUN_SKIP_FAILING_ASSERTIONS "Skip failing ASSERT when targeting Bun" PUBLIC OFF)

WEBKIT_OPTION_END()

# Process option-dependent settings AFTER WEBKIT_OPTION_END()

if(USE_BUN_JSC_ADDITIONS)
    SET_AND_EXPOSE_TO_BUILD(USE_BUN_JSC_ADDITIONS 1)

    if(WIN32)
        SET_AND_EXPOSE_TO_BUILD(JS_NO_EXPORT 1)
    endif()

    # Causing test/cli/test/bun-test.test.ts to fail.
    SET_AND_EXPOSE_TO_BUILD(BUSE_TZONE 0)
    SET_AND_EXPOSE_TO_BUILD(USE_TZONE_MALLOC 0)
endif()

# Only works on macOS for now.
if(ENABLE_MALLOC_HEAP_BREAKDOWN)
    SET_AND_EXPOSE_TO_BUILD(ENABLE_MALLOC_HEAP_BREAKDOWN 1)
    SET_AND_EXPOSE_TO_BUILD(BENABLE_MALLOC_HEAP_BREAKDOWN 1)

    # To workaround ASSERT(cell->heap() != heap()) failing.
    SET_AND_EXPOSE_TO_BUILD(USE_SYSTEM_MALLOC 1)
    SET_AND_EXPOSE_TO_BUILD(ENABLE_LIBPAS 0)
endif()

set(ALL_EVENT_LOOP_TYPES
    GLib
    Generic
    Bun
)

if (ALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS)
    set(ALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS ON)
    SET_AND_EXPOSE_TO_BUILD(USE_ALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS 1)
endif ()

set(DEFAULT_EVENT_LOOP_TYPE "Generic")

set(EVENT_LOOP_TYPE ${DEFAULT_EVENT_LOOP_TYPE} CACHE STRING "Implementation of event loop to be used in JavaScriptCore (one of ${ALL_EVENT_LOOP_TYPES})")

if (USE_BUN_EVENT_LOOP)
    set(EVENT_LOOP_TYPE "Bun")
endif ()

set(ENABLE_WEBCORE OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_WEBGL OFF)
set(ENABLE_WEBGL2 OFF)
set(ENABLE_WEBGPU OFF)

SET_AND_EXPOSE_TO_BUILD(ENABLE_RESOURCE_USAGE ON)

if (ENABLE_REMOTE_INSPECTOR)
    SET_AND_EXPOSE_TO_BUILD(ENABLE_INSPECTOR_ALTERNATE_DISPATCHERS 1)
    SET_AND_EXPOSE_TO_BUILD(USE_INSPECTOR_SOCKET_SERVER 1)
    SET_AND_EXPOSE_TO_BUILD(USE_UNIX_DOMAIN_SOCKETS 1)
else ()
    set(ENABLE_INSPECTOR_ALTERNATE_DISPATCHERS OFF)
endif ()

if (WIN32)
    set(ENABLE_API_TESTS OFF)
else ()
    set(ENABLE_API_TESTS ON)
endif ()

if (WTF_CPU_ARM OR WTF_CPU_MIPS)
    SET_AND_EXPOSE_TO_BUILD(USE_CAPSTONE TRUE)
endif ()

if (ENABLE_BUN_SKIP_FAILING_ASSERTIONS)
    set(BUN_SKIP_FAILING_ASSERTIONS ON)
    SET_AND_EXPOSE_TO_BUILD(BUN_SKIP_FAILING_ASSERTIONS 1)
endif ()

if (NOT ENABLE_STATIC_JSC)
    set(JavaScriptCore_LIBRARY_TYPE SHARED)
    set(bmalloc_LIBRARY_TYPE OBJECT)
    set(WTF_LIBRARY_TYPE OBJECT)
    set(JavaScriptCore_LIBRARY_TYPE SHARED)
    set(PAL_LIBRARY_TYPE OBJECT)
    set(WebCore_LIBRARY_TYPE SHARED)
endif ()

if (USE_BUN_JSC_ADDITIONS)
    set(CMAKE_POSITION_INDEPENDENT_CODE False)
endif ()

if (WIN32)
    add_definitions(-D_WINDOWS -DNTDDI_VERSION=0x0A000006 -D_WIN32_WINNT=0x0A00)

    add_definitions(-DNOMINMAX)
    add_definitions(-DUNICODE -D_UNICODE)
    add_definitions(-DNOCRYPT)

    # For fileno, wcsicmp, getpid and strdup.
    # https://learn.microsoft.com/en-us/previous-versions/ms235384(v=vs.100)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)

    # FIXME: warning STL4042: std::float_denorm_style, std::numeric_limits::has_denorm, and std::numeric_limits::has_denorm_loss are deprecated in C++23.
    add_definitions(-D_SILENCE_CXX23_DENORM_DEPRECATION_WARNING)

    # If <winsock2.h> is not included before <windows.h> redefinition errors occur
    # unless _WINSOCKAPI_ is defined before <windows.h> is included
    add_definitions(-D_WINSOCKAPI_=)

    if (DEFINED ENV{WEBKIT_IGNORE_PATH})
        set(CMAKE_IGNORE_PATH $ENV{WEBKIT_IGNORE_PATH})
    endif ()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

    # Disable PCH on Windows to avoid mtime race conditions with header copy operations
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

    if (ENABLE_STATIC_JSC)
        set(bmalloc_LIBRARY_TYPE STATIC)
        set(WTF_LIBRARY_TYPE STATIC)
        set(JavaScriptCore_LIBRARY_TYPE STATIC)
        set(PAL_LIBRARY_TYPE STATIC)
        set(WebCore_LIBRARY_TYPE STATIC)
    endif ()

    if (NOT WEBKIT_LIBRARIES_DIR)
        if (DEFINED ENV{WEBKIT_LIBRARIES})
            file(TO_CMAKE_PATH "$ENV{WEBKIT_LIBRARIES}" WEBKIT_LIBRARIES_DIR)
        else ()
            file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/WebKitLibraries/win" WEBKIT_LIBRARIES_DIR)
        endif ()
    endif ()
endif ()

if (ENABLE_JSC_GLIB_API)
    include(GNUInstallDirs)

    SET_AND_EXPOSE_TO_BUILD(ENABLE_2022_GLIB_API TRUE)

    set(EVENT_LOOP_TYPE "GLib")
    set(JavaScriptCore_PKGCONFIG_FILE ${CMAKE_BINARY_DIR}/Source/JavaScriptCore/javascriptcoreglib-${PROJECT_VERSION}.pc)
    set(JavaScriptCore_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/javascriptcoreglib-${PROJECT_VERSION}")

    add_definitions(-DJSC_GLIB_API_ENABLED)
    add_definitions(-DGETTEXT_PACKAGE="JSCGlib")
endif ()

string(TOLOWER ${EVENT_LOOP_TYPE} LOWERCASE_EVENT_LOOP_TYPE)
if (LOWERCASE_EVENT_LOOP_TYPE STREQUAL "glib")
    find_package(GLib 2.70.0 REQUIRED COMPONENTS GioUnix Object)
    SET_AND_EXPOSE_TO_BUILD(USE_GLIB 1)
    SET_AND_EXPOSE_TO_BUILD(USE_GLIB_EVENT_LOOP 1)
    SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
elseif (LOWERCASE_EVENT_LOOP_TYPE STREQUAL "bun")
    SET_AND_EXPOSE_TO_BUILD(USE_BUN_EVENT_LOOP 1)
    SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
else ()
    SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
    SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)
endif ()

if (DEFINED ENV{ICU_INCLUDE_DIRS})
    set(ICU_INCLUDE_DIRS "$ENV{ICU_INCLUDE_DIRS}" CACHE "" INTERNAL FORCE)
endif ()

if (DEFINED ENV{ICU_ROOT})
    set(ICU_ROOT "$ENV{ICU_ROOT}" CACHE PATH "" FORCE)
endif ()

# On Windows with ICU_ROOT set, pre-populate FindICU library cache variables
# to ensure our static ICU build is used instead of a system-installed dynamic ICU.
# Without this, FindICU may find dynamic ICU libraries from the system PATH,
# causing jsc.exe to depend on ICU DLLs that aren't shipped in the package.
if (WIN32 AND DEFINED ICU_ROOT AND EXISTS "${ICU_ROOT}/lib")
    set(_icu_lib_dir "${ICU_ROOT}/lib")
    if (NOT ICU_DATA_LIBRARY_RELEASE)
        find_library(ICU_DATA_LIBRARY_RELEASE NAMES sicudt icudt PATHS "${_icu_lib_dir}" NO_DEFAULT_PATH)
    endif ()
    if (NOT ICU_I18N_LIBRARY_RELEASE)
        find_library(ICU_I18N_LIBRARY_RELEASE NAMES icuin sicuin PATHS "${_icu_lib_dir}" NO_DEFAULT_PATH)
    endif ()
    if (NOT ICU_UC_LIBRARY_RELEASE)
        find_library(ICU_UC_LIBRARY_RELEASE NAMES icuuc sicuuc PATHS "${_icu_lib_dir}" NO_DEFAULT_PATH)
    endif ()
    if (ICU_DATA_LIBRARY_RELEASE AND ICU_I18N_LIBRARY_RELEASE AND ICU_UC_LIBRARY_RELEASE)
        message(STATUS "Pre-set static ICU libraries from ICU_ROOT=${ICU_ROOT}:")
        message(STATUS "  ICU data: ${ICU_DATA_LIBRARY_RELEASE}")
        message(STATUS "  ICU i18n: ${ICU_I18N_LIBRARY_RELEASE}")
        message(STATUS "  ICU uc:   ${ICU_UC_LIBRARY_RELEASE}")
    endif ()
    unset(_icu_lib_dir)
endif ()

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)

if (APPLE)
    add_definitions(-DU_DISABLE_RENAMING=1)
endif ()

if (USE_LIBBACKTRACE)
    find_package(LibBacktrace)
    if (NOT LIBBACKTRACE_FOUND)
        message(FATAL_ERROR "libbacktrace is required for USE_LIBBACKTRACE")
    endif ()
endif ()