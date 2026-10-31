list(TRANSFORM JavaScriptCore_AOT_PROGRAM_SOURCES PREPEND ../ OUTPUT_VARIABLE aotProgramSources)
list(TRANSFORM JavaScriptCore_AOT_SHELL_SOURCES PREPEND ../ OUTPUT_VARIABLE aotShellSources)
list(TRANSFORM JavaScriptCore_AOT_TEST_SOURCES PREPEND ../ OUTPUT_VARIABLE aotTestSources)
list(APPEND aotTestSources ${JavaScriptCore_AOT_SCHEMA_GENERATED_TEST_SOURCES})

# A static JavaScriptCore leaves the table of built-ins to the program that links it. Compiled once, for all of them.
if (ENABLE_STATIC_JSC)
    add_library(aot-builtin-table-obj OBJECT ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES})
    target_include_directories(aot-builtin-table-obj PRIVATE $<TARGET_PROPERTY:JavaScriptCore,INCLUDE_DIRECTORIES>)
    target_compile_definitions(aot-builtin-table-obj PRIVATE $<TARGET_PROPERTY:JavaScriptCore,COMPILE_DEFINITIONS>)
    target_compile_options(aot-builtin-table-obj PRIVATE $<TARGET_PROPERTY:JavaScriptCore,COMPILE_OPTIONS>)
    add_dependencies(aot-builtin-table-obj JavaScriptCore)
    set(aotBuiltinTableObjects $<TARGET_OBJECTS:aot-builtin-table-obj>)
    list(APPEND aotProgramSources ${aotBuiltinTableObjects})
endif ()

set(jsc_SOURCES ../jsc.cpp ../tools/JSDollarVMShell.cpp ${aotShellSources} ${aotProgramSources})
set(jsc_LIBRARIES ${CMAKE_DL_LIBS})
set(jsc_FRAMEWORKS
    JavaScriptCore
    WTF
    bmalloc
)

set(jsc_PRIVATE_INCLUDE_DIRECTORIES
    $<TARGET_PROPERTY:JavaScriptCore,INCLUDE_DIRECTORIES>
)

if (WTF_OS_MACOS)
    list(APPEND jsc_LIBRARIES edit)
endif ()

if (WIN32)
    list(APPEND jsc_LIBRARIES Winmm)
endif ()

if (USE_EXTERNAL_MIMALLOC)
    # The shipped static archives intentionally leave mi_* unresolved so the
    # consumer links its own mimalloc. The jsc shell still needs a working
    # allocator, so link the vendored mimalloc object library here only, plus
    # the entry points the archives expect from the consumer's mimalloc that
    # the vendored one does not have (ExternalMimallocShims.cpp).
    list(APPEND jsc_SOURCES ExternalMimallocShims.cpp)
    list(APPEND jsc_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
endif ()

# The ICU data the release lanes bundle is repacked with a zstd frame per item (icu/compress-data.ts), and the ICU they
# build calls the weak hook bun_icu_maybe_decompress on each item it loads (icu/udata-decompress-hook.patch). Bun
# defines the hook. So does the shell (jsc.cpp), so that it reads the data Bun reads, when it is given zstd's lib/
# directory to build the decoder from.
if (BUN_ICU_ZSTD_SOURCE_DIR)
    file(GLOB jsc_ZSTD_DECODER_SOURCES
        ${BUN_ICU_ZSTD_SOURCE_DIR}/common/*.c
        ${BUN_ICU_ZSTD_SOURCE_DIR}/decompress/*.c
    )
    if (NOT jsc_ZSTD_DECODER_SOURCES)
        message(FATAL_ERROR "BUN_ICU_ZSTD_SOURCE_DIR=${BUN_ICU_ZSTD_SOURCE_DIR} has no common/*.c and decompress/*.c")
    endif ()
    # ZSTD_DISABLE_ASM: no huf_decompress_amd64.S, the C one everywhere.
    set_source_files_properties(${jsc_ZSTD_DECODER_SOURCES} PROPERTIES COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM=1;ZSTD_LEGACY_SUPPORT=0;DEBUGLEVEL=0")
    set_source_files_properties(../jsc.cpp PROPERTIES COMPILE_DEFINITIONS "BUN_ICU_ZSTD=1")
    list(APPEND jsc_C_SOURCES ${jsc_ZSTD_DECODER_SOURCES})
    list(APPEND jsc_PRIVATE_INCLUDE_DIRECTORIES ${BUN_ICU_ZSTD_SOURCE_DIR})
endif ()

# mya analyzes process corpses through Mach task APIs, so it only builds on
# Apple platforms. Its sources do not compile elsewhere.
if (APPLE)
    set(mya_SOURCES ../mya/mya.cpp ${aotProgramSources})
    set(mya_FRAMEWORKS
        JavaScriptCore
        WTF
        bmalloc
    )

    set(mya_PRIVATE_INCLUDE_DIRECTORIES
        $<TARGET_PROPERTY:JavaScriptCore,INCLUDE_DIRECTORIES>
    )

    # The corpse support mya uses lives in the tools static library.
    set(mya_LIBRARIES JavaScriptCoreTools edit)
endif ()

WEBKIT_EXECUTABLE_DECLARE(jsc)

if (USE_BUN_JSC_ADDITIONS)
    # bun:ffi (JSC FFI) C++ unit / ABI test executable, modeled on the
    # testmasm_* variables below. Build and run it with:
    #     ninja testFFI && ./bin/testFFI
    # Unlike testmasm this is NOT additionally gated on DEVELOPER_MODE: the
    # standard `bun build.ts debug` configuration never enables DEVELOPER_MODE,
    # and testFFI must build from that same build directory. build.ts and the
    # release scripts build it alongside jsc and ship it in the artifact's bin/.
    # The native test fixtures (ffi/tests/FFITestFixtures.cpp) are NOT compiled
    # here: they live in libJavaScriptCore itself (Sources.txt) so that
    # $vm.ffiFixture in tools/JSDollarVM.cpp can link, and testFFI reaches them
    # through the framework it links.
    # FFI-SPEC-GAP: the target is gated on USE_BUN_JSC_ADDITIONS because
    # testFFI.cpp compiles to nothing (no main) without the feature.
    set(testFFI_SOURCES ../ffi/tests/testFFI.cpp ${aotProgramSources})
    set(testFFI_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testFFI_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testFFI_FRAMEWORKS ${jsc_FRAMEWORKS})
    set(testFFI_LIBRARIES ${CMAKE_DL_LIBS})
    if (WIN32)
        list(APPEND testFFI_LIBRARIES Winmm)
    endif ()
    if (USE_EXTERNAL_MIMALLOC)
        # Same as jsc above: the shipped static archives leave mi_* unresolved
        # under USE_EXTERNAL_MIMALLOC, so any executable that links the
        # framework must supply the vendored mimalloc objects itself.
        list(APPEND testFFI_SOURCES ExternalMimallocShims.cpp)
        list(APPEND testFFI_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
    endif ()

    WEBKIT_EXECUTABLE_DECLARE(testFFI)

    # The tests of the AoT compiler (aot/tests/). Outside DEVELOPER_MODE for testFFI's reason: every lane builds and
    # ships it. Its sources call functions of the library that are not JS_EXPORT_PRIVATE, so it links only against a
    # static JavaScriptCore (ENABLE_STATIC_JSC=ON), which every lane and the development configuration build.
    set(testaotObjects_LIBRARY_TYPE OBJECT)
    set(testaotObjects_SOURCES ${aotTestSources})
    set(testaotObjects_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testaotObjects_PRIVATE_LIBRARIES WebKit::JavaScriptCore WebKit::WTF WebKit::bmalloc)
    WEBKIT_LIBRARY_DECLARE(testaotObjects)

    add_library(testaotLibrary STATIC $<TARGET_OBJECTS:testaotObjects>)
    set_target_properties(testaotLibrary PROPERTIES OUTPUT_NAME testaot-objects LINKER_LANGUAGE CXX)
    add_library(jscLibrary STATIC $<FILTER:$<TARGET_OBJECTS:jsc>,EXCLUDE,ExternalMimallocShims>)
    set_target_properties(jscLibrary PROPERTIES OUTPUT_NAME jsc-objects LINKER_LANGUAGE CXX)

    set(testaot_SOURCES $<TARGET_OBJECTS:testaotObjects> ${aotBuiltinTableObjects})
    set(testaot_DEPENDENCIES testaotLibrary jscLibrary)
    set(testaot_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testaot_FRAMEWORKS ${jsc_FRAMEWORKS})
    set(testaot_LIBRARIES ${CMAKE_DL_LIBS})
    if (WIN32)
        list(APPEND testaot_LIBRARIES Winmm)
    endif ()
    if (USE_EXTERNAL_MIMALLOC)
        list(APPEND testaot_SOURCES ExternalMimallocShims.cpp)
        list(APPEND testaot_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
    endif ()

    WEBKIT_EXECUTABLE_DECLARE(testaot)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ENABLE_DFG_JIT AND (WTF_CPU_X86_64 OR WTF_CPU_ARM64 OR WTF_CPU_RISCV64))
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(getcontext "ucontext.h" HAVE_FOREIGN_STACK_TEST_GETCONTEXT)
    check_cxx_symbol_exists(makecontext "ucontext.h" HAVE_FOREIGN_STACK_TEST_MAKECONTEXT)
    check_cxx_symbol_exists(swapcontext "ucontext.h" HAVE_FOREIGN_STACK_TEST_SWAPCONTEXT)

    if (HAVE_FOREIGN_STACK_TEST_GETCONTEXT AND HAVE_FOREIGN_STACK_TEST_MAKECONTEXT AND HAVE_FOREIGN_STACK_TEST_SWAPCONTEXT)
        set(testForeignStackSuspension_SOURCES ../API/tests/testForeignStackSuspension.cpp)
        set(testForeignStackSuspension_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
        set(testForeignStackSuspension_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
        set(testForeignStackSuspension_FRAMEWORKS ${jsc_FRAMEWORKS})
        set(testForeignStackSuspension_LIBRARIES ${CMAKE_DL_LIBS})
        if (USE_EXTERNAL_MIMALLOC)
            list(APPEND testForeignStackSuspension_SOURCES ExternalMimallocShims.cpp)
            list(APPEND testForeignStackSuspension_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
        endif ()
        WEBKIT_EXECUTABLE_DECLARE(testForeignStackSuspension)
    endif ()
endif ()

if (APPLE)
    WEBKIT_EXECUTABLE_DECLARE(mya)
endif ()

if (DEVELOPER_MODE OR ENABLE_BUN_TEST_PROGRAMS)
    set(testapi_SOURCES
        ../API/tests/CompareAndSwapTest.cpp
        ../API/tests/ExecutionTimeLimitTest.cpp
        ../API/tests/FunctionOverridesTest.cpp
        ../API/tests/FunctionToStringTests.cpp
        ../API/tests/GlobalContextWithFinalizerTest.cpp
        ../API/tests/JSONParseTest.cpp
        ../API/tests/JSObjectGetProxyTargetTest.cpp
        ../API/tests/MultithreadedMultiVMExecutionTest.cpp
        ../API/tests/PingPongStackOverflowTest.cpp
        ../API/tests/TemporalCoreTest.cpp
        ../API/tests/TypedArrayCTest.cpp
        ../API/tests/VMManagerStopTheWorldTest.cpp
        ../API/tests/testapi.cpp
        ${aotProgramSources}
    )
    set(testapi_C_SOURCES
        ../API/tests/CustomGlobalObjectClassTest.c
        ../API/tests/testapi.c
    )
    set(testapi_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testapi_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testapi_FRAMEWORKS ${jsc_FRAMEWORKS})

    set(testmasm_SOURCES ../assembler/testmasm.cpp ${aotProgramSources})
    set(testmasm_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testmasm_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testmasm_FRAMEWORKS ${jsc_FRAMEWORKS})

    # (testFFI is declared above, outside DEVELOPER_MODE; see the comment there.)

    set(testRegExp_SOURCES ../testRegExp.cpp ${aotProgramSources})
    set(testRegExp_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testRegExp_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testRegExp_FRAMEWORKS ${jsc_FRAMEWORKS})

    set(testb3_SOURCES
        ../b3/testb3_1.cpp
        ../b3/testb3_2.cpp
        ../b3/testb3_3.cpp
        ../b3/testb3_4.cpp
        ../b3/testb3_5.cpp
        ../b3/testb3_6.cpp
        ../b3/testb3_7.cpp
        ../b3/testb3_8.cpp
        ../b3/testb3_bun.cpp
        ../b3/testb3_bun_analyses.cpp
        ../b3/testb3_bun_control_flow.cpp
        ../b3/testb3_bun_counted_loops.cpp
        ../b3/testb3_bun_fold_comparisons.cpp
        ../b3/testb3_bun_heap_sets.cpp
        ../b3/testb3_bun_hoisting.cpp
        ../b3/testb3_bun_known_properties.cpp
        ../b3/testb3_bun_loop_phases.cpp
        ../b3/testb3_bun_loop_plans.cpp
        ../b3/testb3_bun_loops.cpp
        ../b3/testb3_bun_numbering.cpp
        ../b3/testb3_bun_options.cpp
        ../b3/testb3_bun_placement.cpp
        ../b3/testb3_bun_policy.cpp
        ../b3/testb3_bun_random.cpp
        ../b3/testb3_bun_random_expressions.cpp
        ../b3/testb3_bun_random_loops.cpp
        ../b3/testb3_bun_random_patchpoints.cpp
        ../b3/testb3_bun_random_statements.cpp
        ../b3/testb3_bun_ranges.cpp
        ../b3/testb3_bun_read_only_data.cpp
        ../b3/testb3_bun_reductions.cpp
        ../b3/testb3_bun_region_cloner.cpp
        ../b3/testb3_bun_selection.cpp
        ../b3/testb3_bun_selects.cpp
        ../b3/testb3_bun_shared_steps.cpp
        ../b3/testb3_bun_store_merging.cpp
        ../b3/testb3_bun_strided_range_checks.cpp
        ../b3/testb3_bun_thread_jumps.cpp
        ../b3/testb3_bun_unroll_loops.cpp
        ../b3/testb3_bun_vectorize_loops.cpp
        ../b3/testb3_bun_version_counted_loops.cpp
        ../b3/testb3_bun_version_loops.cpp
        ${aotProgramSources}
    )
    set(testb3_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testb3_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testb3_FRAMEWORKS ${jsc_FRAMEWORKS})

    set(testair_SOURCES ../b3/air/testair.cpp ${aotProgramSources})
    set(testair_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testair_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testair_FRAMEWORKS ${jsc_FRAMEWORKS})

    if (USE_EXTERNAL_MIMALLOC)
        list(APPEND testmasm_SOURCES ExternalMimallocShims.cpp)
        list(APPEND testmasm_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
        list(APPEND testb3_SOURCES ExternalMimallocShims.cpp)
        list(APPEND testb3_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
        list(APPEND testair_SOURCES ExternalMimallocShims.cpp)
        list(APPEND testair_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
    endif ()

    set(testdfg_SOURCES ../dfg/testdfg.cpp ${aotProgramSources})
    set(testdfg_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testdfg_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
    set(testdfg_FRAMEWORKS ${jsc_FRAMEWORKS})

    # The tools library and its tests are only relevant for Apple platforms.
    if (APPLE)
        set(testLibJSCTools_SOURCES
            ../corpse/tests/CorpseAddressTest.cpp
            ../corpse/tests/CorpseByteParserTest.cpp
            ../corpse/tests/CorpseExportsTrieTest.cpp
            ../corpse/tests/CorpseMemoryTest.cpp
            ../corpse/tests/CorpseProcessTest.cpp
            ../corpse/tests/CorpseRegionTest.cpp
            ../corpse/tests/CorpseSnapshotTest.cpp
            ../corpse/tests/CorpseSymbolTest.cpp
            ../corpse/tests/CorpseThreadTest.cpp
            ../corpse/tests/LibJSCToolsTestUtilities.cpp
            ../corpse/tests/testLibJSCTools.cpp
            ${aotProgramSources}
        )
        set(testLibJSCTools_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
        set(testLibJSCTools_PRIVATE_INCLUDE_DIRECTORIES
            ${jsc_PRIVATE_INCLUDE_DIRECTORIES}
            ${JAVASCRIPTCORE_DIR}/corpse
        )
        set(testLibJSCTools_FRAMEWORKS ${jsc_FRAMEWORKS})
        set(testLibJSCTools_LIBRARIES JavaScriptCoreTools)
    endif ()

    set(testwasmdebugger_SOURCES
        ../wasm/debugger/testwasmdebugger.cpp

        ../wasm/debugger/tests/BinaryTests.cpp
        ../wasm/debugger/tests/ControlFlowTests.cpp
        ../wasm/debugger/tests/ExecutionHandlerIdleStopTest.cpp
        ../wasm/debugger/tests/ExecutionHandlerTest.cpp
        ../wasm/debugger/tests/ExecutionHandlerTestSupport.cpp
        ../wasm/debugger/tests/ExecutionHandlerVMLifecycleTest.cpp
        ../wasm/debugger/tests/ExtGCTests.cpp
        ../wasm/debugger/tests/GDBPacketParserTest.cpp
        ../wasm/debugger/tests/MemoryTests.cpp
        ../wasm/debugger/tests/SpecialTests.cpp
        ../wasm/debugger/tests/TestScripts.cpp
        ../wasm/debugger/tests/TestUtilities.cpp
        ../wasm/debugger/tests/UnaryTests.cpp

        ${aotProgramSources}
    )
    set(testwasmdebugger_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testwasmdebugger_PRIVATE_INCLUDE_DIRECTORIES
        ${jsc_PRIVATE_INCLUDE_DIRECTORIES}
        ${JAVASCRIPTCORE_DIR}/wasm/debugger/tests
    )
    set(testwasmdebugger_FRAMEWORKS ${jsc_FRAMEWORKS})

    WEBKIT_EXECUTABLE_DECLARE(testapi)
    WEBKIT_EXECUTABLE_DECLARE(testRegExp)
    WEBKIT_EXECUTABLE_DECLARE(testmasm)
    WEBKIT_EXECUTABLE_DECLARE(testb3)
    WEBKIT_EXECUTABLE_DECLARE(testair)
    WEBKIT_EXECUTABLE_DECLARE(testdfg)
    WEBKIT_EXECUTABLE_DECLARE(testwasmdebugger)

    if (APPLE)
        WEBKIT_EXECUTABLE_DECLARE(testLibJSCTools)
    endif ()

    if (COMPILER_IS_GCC_OR_CLANG)
        WEBKIT_ADD_TARGET_CXX_FLAGS(testb3 -Wno-array-bounds)
        WEBKIT_ADD_TARGET_CXX_FLAGS(testair -Wno-array-bounds)
        WEBKIT_ADD_TARGET_CXX_FLAGS(testb3 -Wno-error=stack-exhausted)
    endif ()
endif ()

WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()

WEBKIT_EXECUTABLE(jsc)
set_target_properties(jsc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (USE_BUN_JSC_ADDITIONS)
    WEBKIT_EXECUTABLE(testFFI)
    WEBKIT_LIBRARY(testaotObjects)
    WEBKIT_EXECUTABLE(testaot)
endif ()

if (TARGET testForeignStackSuspension)
    WEBKIT_EXECUTABLE(testForeignStackSuspension)
endif ()

if (SHOULD_INSTALL_JS_SHELL)
    install(TARGETS jsc DESTINATION "${LIBEXEC_INSTALL_DIR}")
endif ()

if (APPLE)
    WEBKIT_EXECUTABLE(mya)

    if (SHOULD_INSTALL_JS_SHELL)
        install(TARGETS mya DESTINATION "${LIBEXEC_INSTALL_DIR}")
    endif ()
endif ()

if (DEVELOPER_MODE OR ENABLE_BUN_TEST_PROGRAMS)
    WEBKIT_EXECUTABLE(testapi)
    WEBKIT_EXECUTABLE(testRegExp)
    WEBKIT_EXECUTABLE(testmasm)
    WEBKIT_EXECUTABLE(testb3)
    WEBKIT_EXECUTABLE(testair)
    WEBKIT_EXECUTABLE(testdfg)
    WEBKIT_EXECUTABLE(testwasmdebugger)

    if (APPLE)
        WEBKIT_EXECUTABLE(testLibJSCTools)
    endif ()

    WEBKIT_ADD_PREFIX_HEADER(testapi ../JavaScriptCorePrefix.h PREFIX_NO_CODEGEN PREFIX_LANGUAGES CXX)
    WEBKIT_REUSE_PREFIX_HEADER(testb3 testapi ../JavaScriptCorePrefix.h PREFIX_LANGUAGES CXX)
    WEBKIT_REUSE_PREFIX_HEADER(testwasmdebugger testapi ../JavaScriptCorePrefix.h PREFIX_LANGUAGES CXX)

    file(COPY
        "${JAVASCRIPTCORE_DIR}/API/tests/testapiScripts"
        DESTINATION
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    )
endif ()
