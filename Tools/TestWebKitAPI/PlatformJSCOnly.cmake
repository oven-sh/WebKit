set(TESTWEBKITAPI_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TestWebKitAPI")

list(APPEND TestWTF_SOURCES generic/main.cpp)

if (LOWERCASE_EVENT_LOOP_TYPE STREQUAL "glib")
    list(APPEND TestWTF_SOURCES
        Tests/WTF/glib/FilePathWatcher.cpp
    )
    list(APPEND TestWTF_LIBRARIES
        GLib::Gio
    )
endif ()

get_directory_property(TestJavaScriptCore_AOT_PROGRAM_SOURCES
    DIRECTORY "${JAVASCRIPTCORE_DIR}" DEFINITION JavaScriptCore_AOT_PROGRAM_SOURCES)
list(TRANSFORM TestJavaScriptCore_AOT_PROGRAM_SOURCES PREPEND "${JAVASCRIPTCORE_DIR}/")
list(APPEND TestJavaScriptCore_SOURCES ${TestJavaScriptCore_AOT_PROGRAM_SOURCES})
if (ENABLE_STATIC_JSC)
    list(APPEND TestJavaScriptCore_SOURCES $<TARGET_OBJECTS:aot-builtin-table-obj>)
endif ()
