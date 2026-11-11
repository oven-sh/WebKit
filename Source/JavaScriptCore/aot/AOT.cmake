# CMakeLists.txt includes this file, and so does Bun's build, whose reader (scripts/build/cmake.ts in Bun) runs set(),
# list(APPEND) and include() of an absolute path and skips function bodies. So every list is written out, outside a function.
# aot/Sources.txt and the four lists marked "written by" below are lists of the tree's files, kept in the tree as JavaScriptCore keeps
# Sources.txt and its lists of headers. They may be edited by hand; `bun scripts/build/aot-source-lists.ts` in Bun brings them up to date,
# and configuring fails where they do not match the tree. It finds each of the four by its first line and the next line that is only ")".

# Where the generators of aot/ write. One folder, no folders in it.
set(JavaScriptCore_AOT_GENERATED_DIR "${JavaScriptCore_DERIVED_SOURCES_DIR}/aot")

include("${JAVASCRIPTCORE_DIR}/aot/ir/AOTSchemaFiles.cmake")
include("${JAVASCRIPTCORE_DIR}/aot/builtins/AOTBuiltinTable.cmake")

# What aot/image/generator/runtime-facts.ts writes.
set(JavaScriptCore_AOT_RUNTIME_FACTS_GENERATED_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTRuntimeFactsOfLayouts.def"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTRuntimeFactsOfLayoutsIncludes.h"
)

# WEBKIT_COMPUTE_SOURCES bundles aot/ as it bundles the rest of JavaScriptCore. There are three lists because a bundle is compiled with one set of options.
set(JavaScriptCoreAOT_UNIFIED_SOURCE_LIST_FILES "aot/Sources.txt")
# People keep these two lists. Build-only sources: nothing of them is left in an executable without the compiler.
# What every build runs for a moment: what generates the stubs, the driver's stages around Compile, what numbers the image and writes its tables.
set(JavaScriptCoreAOTCompiledForSize_UNIFIED_SOURCE_LIST_FILES "aot/SourcesCompiledForSize.txt")
set(JavaScriptCoreAOTCompiledForSize_UNIFIED_SOURCE_BUNDLE_PREFIX "CompiledForSize")
# What a shipping build never runs (verifiers, checkers, printers) or hardly ever (emitters and stubs for rare forms, what --explain writes). Nothing is inlined here unless that makes the caller smaller.
set(JavaScriptCoreAOTCompiledForMinimumSize_UNIFIED_SOURCE_LIST_FILES "aot/SourcesCompiledForMinimumSize.txt")
set(JavaScriptCoreAOTCompiledForMinimumSize_UNIFIED_SOURCE_BUNDLE_PREFIX "CompiledForMinimumSize")
# All that a build without AoT (-DENABLE_AOT=0 among the compiler's flags) compiles of the three lists: what the rest of JavaScriptCore calls either way.
set(JavaScriptCore_AOT_SOURCES_WITHOUT_AOT
    aot/runtime/AOTRemovableFeature.cpp
)

list(APPEND JavaScriptCore_SOURCES
    ${JavaScriptCore_AOT_SCHEMA_GENERATED_SOURCES}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_SOURCES}
)

# A shared library has no undefined symbols.
if (NOT ENABLE_STATIC_JSC)
    list(APPEND JavaScriptCore_SOURCES ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES})
endif ()

list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES "${JavaScriptCore_AOT_GENERATED_DIR}")

# Written by aot-source-lists.ts: every folder under aot/ that has a header.
list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES
    "${JAVASCRIPTCORE_DIR}/aot/builtins"
    "${JAVASCRIPTCORE_DIR}/aot/codegen"
    "${JAVASCRIPTCORE_DIR}/aot/driver"
    "${JAVASCRIPTCORE_DIR}/aot/facts"
    "${JAVASCRIPTCORE_DIR}/aot/image"
    "${JAVASCRIPTCORE_DIR}/aot/ir"
    "${JAVASCRIPTCORE_DIR}/aot/machine"
    "${JAVASCRIPTCORE_DIR}/aot/object"
    "${JAVASCRIPTCORE_DIR}/aot/plan"
    "${JAVASCRIPTCORE_DIR}/aot/runtime"
    "${JAVASCRIPTCORE_DIR}/aot/stubs"
    "${JAVASCRIPTCORE_DIR}/aot/support"
    "${JAVASCRIPTCORE_DIR}/aot/tests"
    "${JAVASCRIPTCORE_DIR}/aot/walk"
)

# Written by aot-source-lists.ts: what Bun, a JavaScriptCore header outside aot/ or an exported header of the built-ins table includes, and what those include.
set(JavaScriptCore_AOT_EXPORTED_HEADERS
    aot/builtins/AOTBuiltinWords.h
    aot/driver/AOTDriver.h
    aot/driver/AOTOptionWords.h
    aot/image/AOTCallSiteTable.h
    aot/image/AOTCodeSites.h
    aot/image/AOTFaultHandler.h
    aot/image/AOTFaultTable.h
    aot/image/AOTImage.h
    aot/image/AOTImageFormat.h
    aot/image/AOTImageForward.h
    aot/image/AOTImageLoading.h
    aot/image/AOTImageText.h
    aot/image/AOTPrefixCode.h
    aot/image/AOTRegExpSummary.h
    aot/image/AOTRuntimeDescription.h
    aot/image/AOTSideTableCodes.h
    aot/image/AOTStoredStrings.h
    aot/image/AOTVarint.h
    aot/image/AOTWasmInterface.h
    aot/ir/AOTID.h
    aot/ir/AOTNameClass.h
    aot/ir/AOTPrimitives.h
    aot/object/AOTCellFormat.h
    aot/object/AOTClosure.h
    aot/object/AOTClosureWord.h
    aot/object/AOTCompiledCallee.h
    aot/object/AOTCompiledStructure.h
    aot/object/AOTEnvironment.h
    aot/object/AOTFunctionData.h
    aot/object/AOTFunctionTables.h
    aot/object/AOTLayoutData.h
    aot/object/AOTObject.h
    aot/object/AOTObjectVisiting.h
    aot/object/AOTObjectWords.h
    aot/object/AOTPagedBitSet.h
    aot/object/AOTProgramTables.h
    aot/object/AOTPropertyList.h
    aot/object/AOTPropertyWalk.h
    aot/object/AOTPropertyWalkInlines.h
    aot/object/AOTRecordTypes.h
    aot/object/AOTRuntimeType.h
    aot/object/AOTSlimClosure.h
    aot/object/AOTSlotAccess.h
    aot/object/AOTSlotRepresentation.h
    aot/runtime/AOTBuiltinCells.h
    aot/runtime/AOTBuiltinModules.h
    aot/runtime/AOTCallSiteCache.h
    aot/runtime/AOTChainDependents.h
    aot/runtime/AOTCompactStorage.h
    aot/runtime/AOTCompactTable.h
    aot/runtime/AOTCompiledCaller.h
    aot/runtime/AOTCompiledFrame.h
    aot/runtime/AOTCompiledFrameExit.h
    aot/runtime/AOTCoroutineWords.h
    aot/runtime/AOTFunctionDescription.h
    aot/runtime/AOTGenerator.h
    aot/runtime/AOTGlobalBindings.h
    aot/runtime/AOTHeapCellClasses.h
    aot/runtime/AOTHeapCellVisits.h
    aot/runtime/AOTHost.h
    aot/runtime/AOTInstance.h
    aot/runtime/AOTInstanceLayout.h
    aot/runtime/AOTJSONWords.h
    aot/runtime/AOTMembersLeftOut.h
    aot/runtime/AOTModuleData.h
    aot/runtime/AOTModuleHost.h
    aot/runtime/AOTModuleRecord.h
    aot/runtime/AOTModuleTable.h
    aot/runtime/AOTModuleWords.h
    aot/runtime/AOTModules.h
    aot/runtime/AOTOperationData.h
    aot/runtime/AOTPropertyCachesStringKeys.h
    aot/runtime/AOTPropertyCachesWords.h
    aot/runtime/AOTRealmStartup.h
    aot/runtime/AOTRemovableFeature.h
    aot/runtime/AOTRuntimeRecords.h
    aot/runtime/AOTRuntimeTable.h
    aot/runtime/AOTRuntimeWords.h
    aot/runtime/AOTSavedFrame.h
    aot/runtime/AOTSavedFrameMap.h
    aot/runtime/AOTShownFrameFilter.h
    aot/runtime/AOTStackTrace.h
    aot/runtime/AOTState.h
    aot/runtime/AOTStringBuilderStorage.h
    aot/runtime/AOTStringBuilderWords.h
    aot/runtime/AOTTraceFrame.h
    aot/runtime/AOTVMState.h
    aot/runtime/AOTWatchpoints.h
    aot/stubs/AOTConvention.h
    aot/stubs/AOTStubCoreFamilies.def
    aot/stubs/AOTStubFamilies.def
    aot/stubs/AOTStubFrames.h
    aot/stubs/AOTStubKey.h
    aot/stubs/AOTStubLeafConvention.h
    aot/stubs/AOTStubRegisters.h
    aot/stubs/AOTStubWords.h
    aot/stubs/AOTStubsAccessTableWords.h
    aot/stubs/AOTStubsArrayFamilies.def
    aot/stubs/AOTStubsArrayWords.h
    aot/stubs/AOTStubsCallFamilies.def
    aot/stubs/AOTStubsCoroutineFamilies.def
    aot/stubs/AOTStubsPropertyFamilies.def
    aot/stubs/AOTStubsStringFamilies.def
    aot/support/AOTCompileOptionWords.h
    aot/support/AOTCompileOptions.h
    aot/support/AOTCurrentWork.h
    aot/support/AOTHostFunctionSources.h
    aot/support/AOTLimits.h
    aot/support/AOTSort.h
    aot/support/AOTThreads.h
    aot/support/AOTVector.h
    aot/tests/AOTBuiltProgramTestSupport.h
    aot/tests/AOTLoadedProgramTestSupport.h
    aot/tests/AOTNativeTest.h
    aot/tests/AOTRealmTestSupport.h
)

# Written by aot-source-lists.ts: the same of what the schema's generator writes.
set(JavaScriptCore_AOT_SCHEMA_EXPORTED_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaDeclaredEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaEntryEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaPackedRecords.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaRuntimeOperations.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWire.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWireDiagnosticColumns.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWireEnumColumns.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWireEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWireLimitColumns.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWireRecords.h"
)

list(APPEND JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    ${JavaScriptCore_AOT_EXPORTED_HEADERS}
    ${JavaScriptCore_AOT_SCHEMA_EXPORTED_HEADERS}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_HEADERS}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_GENERATED_HEADERS}
)

# Written by aot-source-lists.ts: every .cpp under aot/tests/ that is in no list below. Linked into testaot only, with JavaScriptCore_AOT_SCHEMA_GENERATED_TEST_SOURCES.
set(JavaScriptCore_AOT_TEST_SOURCES
    aot/tests/AOTArrayTests.cpp
    aot/tests/AOTBodyTests.cpp
    aot/tests/AOTBoxedCallProgramTests.cpp
    aot/tests/AOTBranchesGrowthTests.cpp
    aot/tests/AOTBuiltProgramTestSupport.cpp
    aot/tests/AOTBuiltinCellsTests.cpp
    aot/tests/AOTBuiltinResultObjectTests.cpp
    aot/tests/AOTBuiltinSubclassTests.cpp
    aot/tests/AOTBuiltinTableTests.cpp
    aot/tests/AOTCacheRegistryRootTests.cpp
    aot/tests/AOTCacheRegistryTests.cpp
    aot/tests/AOTCacheSweepTests.cpp
    aot/tests/AOTCheckLines.cpp
    aot/tests/AOTChildProcess.cpp
    aot/tests/AOTChildProcessTests.cpp
    aot/tests/AOTClassMemberOrderTests.cpp
    aot/tests/AOTClassWithoutInstanceTestTests.cpp
    aot/tests/AOTClosedWorldTests.cpp
    aot/tests/AOTCodeParser.cpp
    aot/tests/AOTCodeText.cpp
    aot/tests/AOTCollectorTests.cpp
    aot/tests/AOTCompactStorageTests.cpp
    aot/tests/AOTCompactTableTests.cpp
    aot/tests/AOTCompiledFrameTests.cpp
    aot/tests/AOTCompiledStructureTests.cpp
    aot/tests/AOTConstructorFunctionTests.cpp
    aot/tests/AOTCoroutineJobTests.cpp
    aot/tests/AOTDataBuilderTests.cpp
    aot/tests/AOTDeferredClosureProgramTests.cpp
    aot/tests/AOTDiagnosticTestCommands.cpp
    aot/tests/AOTDictionaryAccessByNameProgramTests.cpp
    aot/tests/AOTDictionaryExtraPropertiesTests.cpp
    aot/tests/AOTDriverTests.cpp
    aot/tests/AOTEnforcedFieldsText.cpp
    aot/tests/AOTEngineHookTests.cpp
    aot/tests/AOTExportsThatStayInCompiledCodeTests.cpp
    aot/tests/AOTExtensionTests.cpp
    aot/tests/AOTFFIStubTests.cpp
    aot/tests/AOTFactsTestCommands.cpp
    aot/tests/AOTFalseTypeProgramTests.cpp
    aot/tests/AOTForInExpansionProgramTests.cpp
    aot/tests/AOTFrameCellProgramTests.cpp
    aot/tests/AOTGlobalBindingProgramTests.cpp
    aot/tests/AOTGlobalProxyReadProgramTests.cpp
    aot/tests/AOTGrowthOfWorkTests.cpp
    aot/tests/AOTGuessesGrowthTests.cpp
    aot/tests/AOTHashedSlotTableTests.cpp
    aot/tests/AOTHeapCageTests.cpp
    aot/tests/AOTHeapRememberedSlotTests.cpp
    aot/tests/AOTICUDataOfTestProgram.cpp
    aot/tests/AOTIDSetTests.cpp
    aot/tests/AOTIDTests.cpp
    aot/tests/AOTIRLexer.cpp
    aot/tests/AOTIRParser.cpp
    aot/tests/AOTIRParserFunctions.cpp
    aot/tests/AOTIRParserTypes.cpp
    aot/tests/AOTIRPrinter.cpp
    aot/tests/AOTIRPrinterTypes.cpp
    aot/tests/AOTIRTestCommands.cpp
    aot/tests/AOTIdentifierNumberingTests.cpp
    aot/tests/AOTImageBuildTests.cpp
    aot/tests/AOTImageCodeLayoutTests.cpp
    aot/tests/AOTImageCodeLinkerTests.cpp
    aot/tests/AOTImageDeterminismTests.cpp
    aot/tests/AOTImageFaultTableTests.cpp
    aot/tests/AOTImageFunctionTableTests.cpp
    aot/tests/AOTImageProgramTests.cpp
    aot/tests/AOTImageRegExpTableTests.cpp
    aot/tests/AOTImageSiteTableTests.cpp
    aot/tests/AOTImageSymbolTests.cpp
    aot/tests/AOTImageTableTests.cpp
    aot/tests/AOTImageTestSupport.cpp
    aot/tests/AOTImageTextTests.cpp
    aot/tests/AOTInstanceKeyOrderTests.cpp
    aot/tests/AOTInterpretedStoreUnderCompiledPrototypeTests.cpp
    aot/tests/AOTInterpreterSiteCacheTests.cpp
    aot/tests/AOTKnownAnswersTests.cpp
    aot/tests/AOTKnownJoinsTests.cpp
    aot/tests/AOTKnownThreadingTests.cpp
    aot/tests/AOTLinkGrowthTests.cpp
    aot/tests/AOTListOfUnitTests.cpp
    aot/tests/AOTLoadedProgramTestSupport.cpp
    aot/tests/AOTLocationsReadAtRunTimeTests.cpp
    aot/tests/AOTLockedRealmMemberTests.cpp
    aot/tests/AOTLockedRealmTests.cpp
    aot/tests/AOTLookupBehindTablesProgramTests.cpp
    aot/tests/AOTMachineAllocationTests.cpp
    aot/tests/AOTMachineCallsOnWaysTests.cpp
    aot/tests/AOTMachineCheckerTests.cpp
    aot/tests/AOTMachineFoldingTests.cpp
    aot/tests/AOTMachineFrameFormTests.cpp
    aot/tests/AOTMachineFrameTests.cpp
    aot/tests/AOTMachineFuzzer.cpp
    aot/tests/AOTMachineMemoryTests.cpp
    aot/tests/AOTMachineNumberingTests.cpp
    aot/tests/AOTMachineOperationTests.cpp
    aot/tests/AOTMachineOrderTests.cpp
    aot/tests/AOTMachinePairsOfLoadsTests.cpp
    aot/tests/AOTMachinePatchTests.cpp
    aot/tests/AOTMachineRandomProcedure.cpp
    aot/tests/AOTMachineRandomTests.cpp
    aot/tests/AOTMachineTestCommands.cpp
    aot/tests/AOTMachineTestPatches.cpp
    aot/tests/AOTMachineTesting.cpp
    aot/tests/AOTMachineTestsOfPhisTests.cpp
    aot/tests/AOTMembersLeftOutTests.cpp
    aot/tests/AOTMembersNamedLikeBuiltinsTests.cpp
    aot/tests/AOTModuleProgramTests.cpp
    aot/tests/AOTModuleRequestListTests.cpp
    aot/tests/AOTModuleTableTests.cpp
    aot/tests/AOTNakedConstructorTests.cpp
    aot/tests/AOTNativeStoreTests.cpp
    aot/tests/AOTNativeTestRunner.cpp
    aot/tests/AOTObjectCreateProgramTests.cpp
    aot/tests/AOTObjectSlotExitTests.cpp
    aot/tests/AOTOriginalBesideMockTests.cpp
    aot/tests/AOTOutputTests.cpp
    aot/tests/AOTPlanTestCommands.cpp
    aot/tests/AOTPrivateAccessorProgramTests.cpp
    aot/tests/AOTPrivateBrandProgramTests.cpp
    aot/tests/AOTPrivateFieldDefinitionTests.cpp
    aot/tests/AOTPrivateMemberKindProgramTests.cpp
    aot/tests/AOTPrivateMethodProgramTests.cpp
    aot/tests/AOTProgramFile.cpp
    aot/tests/AOTProgramIdentifiersTests.cpp
    aot/tests/AOTProgramStaticsTests.cpp
    aot/tests/AOTProgramStringsTests.cpp
    aot/tests/AOTProgramSubcommands.cpp
    aot/tests/AOTProgramTestCommands.cpp
    aot/tests/AOTPropertyAttributeTests.cpp
    aot/tests/AOTPropertyLookupTableCollectionTests.cpp
    aot/tests/AOTPropertyLookupTableTests.cpp
    aot/tests/AOTPropertyLookupTests.cpp
    aot/tests/AOTPropertyRunProgramTests.cpp
    aot/tests/AOTPrototypeSetByKeyTests.cpp
    aot/tests/AOTPublishedEnumeratorProgramTests.cpp
    aot/tests/AOTPublishedEnumeratorTests.cpp
    aot/tests/AOTReaderTests.cpp
    aot/tests/AOTRealmTestSupport.cpp
    aot/tests/AOTRegExpCompilerTests.cpp
    aot/tests/AOTRegExpImageTests.cpp
    aot/tests/AOTRemovableFeatureTests.cpp
    aot/tests/AOTReplacedBuiltinTests.cpp
    aot/tests/AOTRequiredNameTests.cpp
    aot/tests/AOTRuntimeOperationTests.cpp
    aot/tests/AOTRuntimeRecordTests.cpp
    aot/tests/AOTRuntimeTypeTests.cpp
    aot/tests/AOTSavedFrameTests.cpp
    aot/tests/AOTSharedArrayTests.cpp
    aot/tests/AOTSharingModeAndNamespaceTests.cpp
    aot/tests/AOTSiteAnswersOfGettersTests.cpp
    aot/tests/AOTSlimClosureTests.cpp
    aot/tests/AOTSlotAccessTests.cpp
    aot/tests/AOTSparseElementProgramTests.cpp
    aot/tests/AOTStackTestPlanTests.cpp
    aot/tests/AOTStackTraceTests.cpp
    aot/tests/AOTStagedBuild.cpp
    aot/tests/AOTStartupTests.cpp
    aot/tests/AOTStaticMembersNamedLikeBuiltinsTests.cpp
    aot/tests/AOTStorageOwnerProgramTests.cpp
    aot/tests/AOTStoreAnswerTests.cpp
    aot/tests/AOTStoresOfStubsProgramTests.cpp
    aot/tests/AOTStringBuilderStorageTests.cpp
    aot/tests/AOTStringSearchTests.cpp
    aot/tests/AOTStringStraysTests.cpp
    aot/tests/AOTStringTableTests.cpp
    aot/tests/AOTStructuredCloneTests.cpp
    aot/tests/AOTStubAssemblerTests.cpp
    aot/tests/AOTStubBlobTests.cpp
    aot/tests/AOTStubBoxedCallsOfLanguageTests.cpp
    aot/tests/AOTStubCallTests.cpp
    aot/tests/AOTStubCollectionTests.cpp
    aot/tests/AOTStubPropertyReadTests.cpp
    aot/tests/AOTStubPropertyStoreTests.cpp
    aot/tests/AOTStubRegExpTests.cpp
    aot/tests/AOTStubRuntimeTests.cpp
    aot/tests/AOTStubStringTests.cpp
    aot/tests/AOTStubTests.cpp
    aot/tests/AOTStubValueTests.cpp
    aot/tests/AOTStubVectorGrowthTests.cpp
    aot/tests/AOTSupportTestCommands.cpp
    aot/tests/AOTSupportTests.cpp
    aot/tests/AOTTableWriterTests.cpp
    aot/tests/AOTTailCallProgramTests.cpp
    aot/tests/AOTTestCommandRegistry.cpp
    aot/tests/AOTTestFile.cpp
    aot/tests/AOTTestRunOriginalsTests.cpp
    aot/tests/AOTTestRunReportSizesTests.cpp
    aot/tests/AOTTestRunner.cpp
    aot/tests/AOTTestRunsTestCommands.cpp
    aot/tests/AOTThisAllocationProgramTests.cpp
    aot/tests/AOTThisAllocationTests.cpp
    aot/tests/AOTTypeErrorMessageTests.cpp
    aot/tests/AOTTypeTestCommands.cpp
    aot/tests/AOTTypedResultProgramTests.cpp
    aot/tests/AOTUnionConsecutiveSetsTests.cpp
    aot/tests/AOTUnitQueueTests.cpp
    aot/tests/AOTUnwindingTests.cpp
    aot/tests/AOTWaiterOrderProgramTests.cpp
    aot/tests/AOTWalkStateTests.cpp
    aot/tests/AOTWalkTestCommands.cpp
    aot/tests/AOTWebAssemblyImageTests.cpp
    aot/tests/AOTWebAssemblyInstanceTests.cpp
    aot/tests/AOTWireProgramBuilder.cpp
    aot/tests/testaot.cpp
    aot/tests/unit/BuiltinValidationTests.cpp
    aot/tests/unit/PristineStructureTests.cpp
)

# Linked into every program but testaot, where AOTRemovableFeatureTests.cpp defines the same symbols.
set(JavaScriptCore_AOT_PROGRAM_SOURCES
    aot/runtime/AOTNoFeatureRemoved.cpp
)

# Linked into jsc.
set(JavaScriptCore_AOT_SHELL_SOURCES
    aot/tests/AOTShell.cpp
)

# Linked into the testaot of this build, beside the stand-in host's table of built-ins. Bun links its own in their place.
set(JavaScriptCore_AOT_STAND_IN_HOST_TEST_SOURCES
    aot/tests/host/AOTNativeTestsOfStandInHost.cpp
)

# Compiled for size like aot/SourcesCompiledForSize.txt.
set(JavaScriptCore_AOT_GENERATED_SOURCES_COMPILED_FOR_SIZE
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaRecords.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTWire.cpp"
)

# 16 in a bundle, not 8: an object repeats the debug info of the headers it includes, and that is most of a tarball.
function(AOT_COMPUTE_SOURCES)
    if ("${CMAKE_CXX_FLAGS}" MATCHES "-DENABLE_AOT=0")
        set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} ${JavaScriptCore_AOT_SOURCES_WITHOUT_AOT} PARENT_SCOPE)
        return ()
    endif ()
    set(WEBKIT_MAX_BUNDLE_SIZE 16)
    foreach (list IN ITEMS JavaScriptCoreAOT JavaScriptCoreAOTCompiledForSize JavaScriptCoreAOTCompiledForMinimumSize)
        set(${list}_DERIVED_SOURCES_DIR "${JavaScriptCore_DERIVED_SOURCES_DIR}")
        WEBKIT_COMPUTE_SOURCES(${list})
    endforeach ()
    if (COMPILER_IS_CLANG)
        set_source_files_properties(${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCore_AOT_GENERATED_SOURCES_COMPILED_FOR_SIZE} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-mllvm=-force-attribute=optsize>")
        set_source_files_properties(${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-mllvm=-force-attribute=minsize>")
        # Unrolling loops whose trip count is not known adds 2% to this code and no speed. Under ThinLTO loops are unrolled in the link, so this counts where the objects are machine code.
        set_source_files_properties(${JavaScriptCoreAOT_SOURCES} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-mllvm=-unroll-runtime=false>")
    endif ()
    if (COMPILER_IS_GCC_OR_CLANG AND NOT MSVC)
        # A switch that has no default names every enumerator: whoever adds one is shown each question it has to answer.
        # The prefix is read as text. A precompiled header is loaded in front of every -include, and AOTPrefix.h begins with what has to stand in front of the standard library's headers.
        set_property(SOURCE ${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Werror=switch -include "${JAVASCRIPTCORE_DIR}/aot/support/AOTPrefix.h")
        set_source_files_properties(${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif ()
    if (COMPILER_IS_CLANG)
        # No pointer, reference or view into a temporary is kept beyond the statement that made the temporary. Both drivers of clang take the flag.
        set_property(SOURCE ${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Werror=dangling)
    endif ()
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} ${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} PARENT_SCOPE)
endfunction()

# The generators run while CMake configures, so that every generated file is there before anything is compiled. A file is written only when its text changes.
# A generator that ended well and left a file out would show as a missing header somewhere in the build.
function(AOT_REQUIRE_GENERATED_FILES generator)
    foreach (file IN LISTS ARGN)
        if (NOT EXISTS "${file}")
            message(FATAL_ERROR "${generator} ended without an error and did not write ${file}")
        endif ()
    endforeach ()
endfunction()

# The folder of generated files stands in front of aot/'s own on the include path: a file left there under the name of a file of the tree would be found first.
function(AOT_REMOVE_GENERATED_FILES_THAT_HIDE_SOURCES)
    file(GLOB generated RELATIVE "${JavaScriptCore_AOT_GENERATED_DIR}" "${JavaScriptCore_AOT_GENERATED_DIR}/*")
    file(GLOB_RECURSE handwritten RELATIVE "${JAVASCRIPTCORE_DIR}" "${JAVASCRIPTCORE_DIR}/aot/*.cpp" "${JAVASCRIPTCORE_DIR}/aot/*.h")
    list(FILTER handwritten EXCLUDE REGEX "/generated/")
    list(TRANSFORM handwritten REPLACE "^.*/" "")
    foreach (name IN LISTS generated)
        if (name IN_LIST handwritten)
            file(REMOVE "${JavaScriptCore_AOT_GENERATED_DIR}/${name}")
        endif ()
    endforeach ()
endfunction()

function(AOT_GENERATE_FROM_SCHEMA)
    find_program(NODE_EXECUTABLE node REQUIRED)
    execute_process(COMMAND "${NODE_EXECUTABLE}" --version OUTPUT_VARIABLE nodeVersion OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX REPLACE "^v" "" nodeVersion "${nodeVersion}")
    if (nodeVersion VERSION_LESS 22.18 OR (nodeVersion VERSION_GREATER_EQUAL 23 AND nodeVersion VERSION_LESS 24.2))
        message(FATAL_ERROR "The generators of aot/ need node 24.2 or later, or 22.18 or later of 22: it has to run TypeScript as it is. ${NODE_EXECUTABLE} is ${nodeVersion}.")
    endif ()
    file(GLOB generator CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/ir/generator/*.ts")
    list(TRANSFORM JavaScriptCore_AOT_SCHEMA_INPUTS PREPEND "${JAVASCRIPTCORE_DIR}/" OUTPUT_VARIABLE inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${generator} ${inputs})
    execute_process(
        COMMAND "${NODE_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/ir/generator/index.ts" --schema "${JAVASCRIPTCORE_DIR}/aot/ir/AOTSchema.def" --engine-out "${JavaScriptCore_AOT_GENERATED_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    AOT_REQUIRE_GENERATED_FILES(aot/ir/generator/index.ts ${JavaScriptCore_AOT_SCHEMA_GENERATED_HEADERS} ${JavaScriptCore_AOT_SCHEMA_GENERATED_SOURCES} ${JavaScriptCore_AOT_SCHEMA_GENERATED_TEST_SOURCES})
endfunction()

# What the engine is compiled with, and the table of the programs that this build links: the engine's entries and the stand-in host's.
function(AOT_GENERATE_BUILTIN_TABLE)
    file(GLOB generator CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/builtins/generator/*.ts")
    set(inputs
        ${JavaScriptCore_AOT_BUILTIN_TABLE_DECLARATIONS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_STAND_IN_HOST_DECLARATIONS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_STAND_IN_HOST_OWN_PROPERTIES}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_HANDWRITTEN_FUNCTIONS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_MODEL_FUNCTIONS}
        ${JavaScriptCore_OBJECT_LUT_SOURCES}
    )
    list(TRANSFORM inputs PREPEND "${JAVASCRIPTCORE_DIR}/")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${generator} ${inputs} ${JavaScriptCore_BUILTINS_SOURCES})
    execute_process(
        COMMAND "${NODE_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/builtins/generator/engine.ts" "--javascriptcore=${JAVASCRIPTCORE_DIR}" "--out=${JavaScriptCore_AOT_GENERATED_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    AOT_REQUIRE_GENERATED_FILES(aot/builtins/generator/engine.ts
        ${JavaScriptCore_AOT_BUILTIN_TABLE_RELEASED_FILES}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_HEADERS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES}
    )
    # The Dockerfiles copy this folder into the release.
    set(released "${CMAKE_BINARY_DIR}/JavaScriptCore/ReleasedSources/aot/builtins/generated")
    file(REMOVE_RECURSE "${released}")
    file(COPY ${JavaScriptCore_AOT_BUILTIN_TABLE_RELEASED_FILES} DESTINATION "${released}")
endfunction()

# It reads the text of the compiler's sources, so it is a step of the build. Windows takes a .def among the sources for the linker's.
function(AOT_GENERATE_RUNTIME_FACTS)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp" "${JAVASCRIPTCORE_DIR}/aot/*.h")
    add_custom_command(
        OUTPUT ${JavaScriptCore_AOT_RUNTIME_FACTS_GENERATED_HEADERS}
        DEPENDS ${sources} "${JAVASCRIPTCORE_DIR}/aot/image/generator/runtime-facts.ts" "${JAVASCRIPTCORE_DIR}/aot/image/AOTRuntimeFactsLeftOut.txt"
        COMMAND "${NODE_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/image/generator/runtime-facts.ts" --javascriptcore "${JAVASCRIPTCORE_DIR}" --out "${JavaScriptCore_AOT_GENERATED_DIR}"
        VERBATIM
    )
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} "${JavaScriptCore_AOT_GENERATED_DIR}/AOTRuntimeFactsOfLayoutsIncludes.h" PARENT_SCOPE)
endfunction()

# It reads the text of the compiler's sources too. What it writes says only that they have passed.
function(AOT_CHECK_READS_THAT_ASSUME)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp" "${JAVASCRIPTCORE_DIR}/aot/*.h")
    add_custom_command(
        OUTPUT "${JavaScriptCore_AOT_GENERATED_DIR}/AOTReadsThatAssumeChecked.h"
        DEPENDS ${sources} "${JAVASCRIPTCORE_DIR}/aot/codegen/generator/reads-that-assume.ts" "${JAVASCRIPTCORE_DIR}/aot/codegen/AOTReadsThatAssume.txt" "${JAVASCRIPTCORE_DIR}/aot/codegen/generator/stub-registers.ts"
        COMMAND "${NODE_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/codegen/generator/reads-that-assume.ts" --javascriptcore "${JAVASCRIPTCORE_DIR}" --out "${JavaScriptCore_AOT_GENERATED_DIR}"
        VERBATIM
    )
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} "${JavaScriptCore_AOT_GENERATED_DIR}/AOTReadsThatAssumeChecked.h" PARENT_SCOPE)
endfunction()

# It reads the text of the runtime's sources. What it writes says only that they have passed.
function(AOT_CHECK_LATE_STORES)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp" "${JAVASCRIPTCORE_DIR}/aot/*.h")
    add_custom_command(
        OUTPUT "${JavaScriptCore_AOT_GENERATED_DIR}/AOTLateStoresChecked.h"
        DEPENDS ${sources} "${JAVASCRIPTCORE_DIR}/aot/runtime/generator/late-stores.ts" "${JAVASCRIPTCORE_DIR}/aot/runtime/AOTLateStores.txt"
        COMMAND "${NODE_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/runtime/generator/late-stores.ts" --javascriptcore "${JAVASCRIPTCORE_DIR}" --out "${JavaScriptCore_AOT_GENERATED_DIR}"
        VERBATIM
    )
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} "${JavaScriptCore_AOT_GENERATED_DIR}/AOTLateStoresChecked.h" PARENT_SCOPE)
endfunction()

# It reads the text of the compiler's sources as well, and its own fixtures first. What it writes says only that they have passed.
function(AOT_CHECK_FORCED_JUMPS)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp" "${JAVASCRIPTCORE_DIR}/aot/*.h" "${JAVASCRIPTCORE_DIR}/aot/runtime/generator/forced-jumps-fixtures/*.txt")
    add_custom_command(
        OUTPUT "${JavaScriptCore_AOT_GENERATED_DIR}/AOTForcedJumpsChecked.h"
        DEPENDS ${sources} "${JAVASCRIPTCORE_DIR}/aot/runtime/generator/forced-jumps.py" "${JAVASCRIPTCORE_DIR}/aot/runtime/AOTForcedJumps.txt"
        COMMAND "${PYTHON_EXECUTABLE}" "${JAVASCRIPTCORE_DIR}/aot/runtime/generator/forced-jumps.py" --javascriptcore "${JAVASCRIPTCORE_DIR}" --out "${JavaScriptCore_AOT_GENERATED_DIR}"
        VERBATIM
    )
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} "${JavaScriptCore_AOT_GENERATED_DIR}/AOTForcedJumpsChecked.h" PARENT_SCOPE)
endfunction()

function(AOT_READ_LIBRARY_SOURCES result problemsResult)
    set(problems)
    set(sources)
    foreach (list IN ITEMS ${JavaScriptCoreAOT_UNIFIED_SOURCE_LIST_FILES} ${JavaScriptCoreAOTCompiledForSize_UNIFIED_SOURCE_LIST_FILES} ${JavaScriptCoreAOTCompiledForMinimumSize_UNIFIED_SOURCE_LIST_FILES})
        file(STRINGS "${JAVASCRIPTCORE_DIR}/${list}" paths)
        set(previous "")
        foreach (path IN LISTS paths)
            if (NOT "${previous}" STRLESS "${path}")
                list(APPEND problems "${path}: not in ascending order in ${list}")
            endif ()
            list(APPEND sources ${path})
            set(previous ${path})
        endforeach ()
    endforeach ()
    set(${result} ${sources} PARENT_SCOPE)
    set(${problemsResult} ${problems} PARENT_SCOPE)
endfunction()

function(AOT_CHECK_SOURCE_LISTS)
    file(GLOB_RECURSE found RELATIVE "${JAVASCRIPTCORE_DIR}" CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp")
    AOT_READ_LIBRARY_SOURCES(librarySources problems)

    set(listed
        ${librarySources}
        ${JavaScriptCore_AOT_TEST_SOURCES}
        ${JavaScriptCore_AOT_PROGRAM_SOURCES}
        ${JavaScriptCore_AOT_SHELL_SOURCES}
        ${JavaScriptCore_AOT_STAND_IN_HOST_TEST_SOURCES}
    )
    foreach (path IN LISTS found)
        if (NOT path IN_LIST listed)
            list(APPEND problems "${path}: in no list")
        endif ()
    endforeach ()
    foreach (path IN LISTS listed)
        if (NOT path IN_LIST found)
            list(APPEND problems "${path}: listed, but there is no such file")
        endif ()
    endforeach ()

    list(SORT listed COMPARE STRING)
    set(previous "")
    foreach (path IN LISTS listed)
        if ("${path}" STREQUAL "${previous}")
            list(APPEND problems "${path}: listed more than once")
        endif ()
        set(previous ${path})
    endforeach ()

    AOT_REPORT_LIST_PROBLEMS(${problems})
endfunction()

function(AOT_CHECK_EXPORTED_HEADERS)
    set(exported
        ${JavaScriptCore_AOT_EXPORTED_HEADERS}
        ${JavaScriptCore_AOT_SCHEMA_EXPORTED_HEADERS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_HEADERS}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_GENERATED_HEADERS}
    )
    set(problems)
    set(exportedNames)
    foreach (header IN LISTS exported)
        get_filename_component(name ${header} NAME)
        if (name IN_LIST exportedNames)
            list(APPEND problems "${header}: a header named ${name} is exported more than once")
        endif ()
        list(APPEND exportedNames ${name})
    endforeach ()

    foreach (header IN LISTS exported)
        get_filename_component(path ${header} ABSOLUTE BASE_DIR "${JAVASCRIPTCORE_DIR}")
        if (NOT EXISTS "${path}")
            list(APPEND problems "${header}: exported, but there is no such file")
            continue ()
        endif ()
        file(STRINGS "${path}" includeLines REGEX "^[ \t]*#[ \t]*include[ \t]+[\"<](JavaScriptCore/)?AOT[^\">]*[\">]")
        foreach (line IN LISTS includeLines)
            string(REGEX REPLACE "^[^\"<]*[\"<](JavaScriptCore/)?([^\">]*)[\">].*$" "\\2" name "${line}")
            if (NOT name IN_LIST exportedNames)
                list(APPEND problems "${header}: is exported and includes ${name}, which is not")
            endif ()
        endforeach ()
    endforeach ()

    AOT_REPORT_LIST_PROBLEMS(${problems})
endfunction()

function(AOT_REPORT_LIST_PROBLEMS)
    if (NOT ARGN)
        return ()
    endif ()
    set(problems ${ARGN})
    list(REMOVE_DUPLICATES problems)
    list(JOIN problems "\n  " report)
    get_filename_component(checkout "${JAVASCRIPTCORE_DIR}/../.." ABSOLUTE)
    message(FATAL_ERROR "The lists of aot/ do not match the tree.\n  ${report}\nTo write them again, in Bun's tree: `bun scripts/build/aot-source-lists.ts --webkit=${checkout}`")
endfunction()

AOT_REMOVE_GENERATED_FILES_THAT_HIDE_SOURCES()
AOT_GENERATE_FROM_SCHEMA()
AOT_GENERATE_BUILTIN_TABLE()
AOT_GENERATE_RUNTIME_FACTS()
AOT_CHECK_READS_THAT_ASSUME()
AOT_CHECK_LATE_STORES()
AOT_CHECK_FORCED_JUMPS()
AOT_COMPUTE_SOURCES()
AOT_CHECK_SOURCE_LISTS()
AOT_CHECK_EXPORTED_HEADERS()
