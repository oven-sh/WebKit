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

# What aot/support/generator/vector.ts writes.
set(JavaScriptCore_AOT_VECTOR_GENERATED_HEADERS "${JavaScriptCore_AOT_GENERATED_DIR}/AOTVector.h")

# WEBKIT_COMPUTE_SOURCES bundles aot/ as it bundles the rest of JavaScriptCore. There are three lists because a bundle is compiled with one set of options.
set(JavaScriptCoreAOT_UNIFIED_SOURCE_LIST_FILES "aot/Sources.txt")
# People keep these two lists. Build-only sources: nothing of them is left in an executable without the compiler.
# What every build runs for a moment: the assembler and the catalog of stubs, what writes the image, what reads flow's results.
set(JavaScriptCoreAOTCompiledForSize_UNIFIED_SOURCE_LIST_FILES "aot/SourcesCompiledForSize.txt")
set(JavaScriptCoreAOTCompiledForSize_UNIFIED_SOURCE_BUNDLE_PREFIX "CompiledForSize")
# What a build never runs (verifiers, printers) or hardly ever (passes and emitters for rare forms). Nothing is inlined here unless that makes the caller smaller.
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
    "${JAVASCRIPTCORE_DIR}/aot/analysis"
    "${JAVASCRIPTCORE_DIR}/aot/builtins"
    "${JAVASCRIPTCORE_DIR}/aot/codegen"
    "${JAVASCRIPTCORE_DIR}/aot/image"
    "${JAVASCRIPTCORE_DIR}/aot/ir"
    "${JAVASCRIPTCORE_DIR}/aot/object"
    "${JAVASCRIPTCORE_DIR}/aot/passes"
    "${JAVASCRIPTCORE_DIR}/aot/pipeline"
    "${JAVASCRIPTCORE_DIR}/aot/place"
    "${JAVASCRIPTCORE_DIR}/aot/represent"
    "${JAVASCRIPTCORE_DIR}/aot/runtime"
    "${JAVASCRIPTCORE_DIR}/aot/select"
    "${JAVASCRIPTCORE_DIR}/aot/stubs"
    "${JAVASCRIPTCORE_DIR}/aot/support"
    "${JAVASCRIPTCORE_DIR}/aot/tests"
    "${JAVASCRIPTCORE_DIR}/aot/tests/unit"
    "${JAVASCRIPTCORE_DIR}/aot/valueflow"
)

# Written by aot-source-lists.ts: what Bun, a JavaScriptCore header outside aot/ or an exported header of the built-ins table includes, and what those include.
set(JavaScriptCore_AOT_EXPORTED_HEADERS
    aot/builtins/AOTBuiltinWords.h
    aot/codegen/AOTArrivalSiteNumbers.h
    aot/codegen/AOTCodegenScratchWords.h
    aot/image/AOTCallSiteTable.h
    aot/image/AOTCodeSites.h
    aot/image/AOTCompile.h
    aot/image/AOTFaultHandler.h
    aot/image/AOTFaultTable.h
    aot/image/AOTImage.h
    aot/image/AOTImageFormat.h
    aot/image/AOTImageForward.h
    aot/image/AOTImageLoading.h
    aot/image/AOTImageText.h
    aot/image/AOTObfuscatedSites.h
    aot/image/AOTRuntimeDescription.h
    aot/image/AOTVarint.h
    aot/image/AOTWasmInterface.h
    aot/ir/AOTID.h
    aot/ir/AOTIRForm.h
    aot/ir/AOTNameClass.h
    aot/ir/AOTOpcode.h
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
    aot/pipeline/AOTPassID.h
    aot/place/AOTBodyFactsForTables.h
    aot/runtime/AOTBuiltinCells.h
    aot/runtime/AOTBuiltinModules.h
    aot/runtime/AOTCallSiteCache.h
    aot/runtime/AOTCompactStorage.h
    aot/runtime/AOTCompactTable.h
    aot/runtime/AOTCompiledCaller.h
    aot/runtime/AOTCompiledFrame.h
    aot/runtime/AOTCompiledFrameExit.h
    aot/runtime/AOTCoroutineWords.h
    aot/runtime/AOTFunctionDescription.h
    aot/runtime/AOTGenerator.h
    aot/runtime/AOTHeapCellClasses.h
    aot/runtime/AOTHeapCellVisits.h
    aot/runtime/AOTHost.h
    aot/runtime/AOTInstance.h
    aot/runtime/AOTInstanceLayout.h
    aot/runtime/AOTJSONHooks.h
    aot/runtime/AOTJSONWords.h
    aot/runtime/AOTMembersLeftOut.h
    aot/runtime/AOTModuleData.h
    aot/runtime/AOTModuleGraph.h
    aot/runtime/AOTModuleHost.h
    aot/runtime/AOTModuleRecord.h
    aot/runtime/AOTModuleWords.h
    aot/runtime/AOTOperationData.h
    aot/runtime/AOTPropertyCachesStringKeys.h
    aot/runtime/AOTPropertyCachesWords.h
    aot/runtime/AOTRealmStartup.h
    aot/runtime/AOTRemovableFeature.h
    aot/runtime/AOTRuntimeCounters.h
    aot/runtime/AOTRuntimeRecords.h
    aot/runtime/AOTRuntimeReporter.h
    aot/runtime/AOTRuntimeTable.h
    aot/runtime/AOTRuntimeValidation.h
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
    aot/stubs/AOTConvention.h
    aot/stubs/AOTStubCoreFamilies.def
    aot/stubs/AOTStubFamilies.def
    aot/stubs/AOTStubFrames.h
    aot/stubs/AOTStubKey.h
    aot/stubs/AOTStubLeafConvention.h
    aot/stubs/AOTStubRegisters.h
    aot/stubs/AOTStubWords.h
    aot/stubs/AOTStubsArrayFamilies.def
    aot/stubs/AOTStubsArrayWords.h
    aot/stubs/AOTStubsCallFamilies.def
    aot/stubs/AOTStubsCoroutineFamilies.def
    aot/stubs/AOTStubsPropertyFamilies.def
    aot/stubs/AOTStubsStringFamilies.def
    aot/support/AOTCompileOptionWords.h
    aot/support/AOTCompileOptions.h
    aot/support/AOTDiagnostics.h
    aot/support/AOTLimits.h
    aot/support/AOTLosses.h
    aot/support/AOTStatistics.h
    aot/support/AOTThreshold.h
    aot/valueflow/AOTFlowReport.h
)

# Written by aot-source-lists.ts: the same of what the schema's generator writes.
set(JavaScriptCore_AOT_SCHEMA_EXPORTED_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaDeclaredEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaEntryEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaEntryMacros.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaFlagMasks.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaOperandIndexes.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaOperandPositions.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaOperationColumns.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaOperations.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaPackedRecords.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaPayloadPositions.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaRuntimeColumns.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaRuntimeOperations.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaVariadicOperandPositions.h"
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
    ${JavaScriptCore_AOT_VECTOR_GENERATED_HEADERS}
)

# Written by aot-source-lists.ts: every .cpp under aot/tests/ but AOTShell.cpp. Linked into testaot only, with JavaScriptCore_AOT_SCHEMA_GENERATED_TEST_SOURCES.
set(JavaScriptCore_AOT_TEST_SOURCES
    aot/tests/AOTAnalysisDump.cpp
    aot/tests/AOTAnalysisTestCommands.cpp
    aot/tests/AOTArrayTests.cpp
    aot/tests/AOTArrivalCountTests.cpp
    aot/tests/AOTAssignmentPlanTests.cpp
    aot/tests/AOTB3Dump.cpp
    aot/tests/AOTB3HeapTests.cpp
    aot/tests/AOTB3PolicyTests.cpp
    aot/tests/AOTBackEndPhaseTests.cpp
    aot/tests/AOTBackEndReportTests.cpp
    aot/tests/AOTBoxedCallProgramTests.cpp
    aot/tests/AOTBuiltinCellsTests.cpp
    aot/tests/AOTBuiltinLocationWatchTests.cpp
    aot/tests/AOTBuiltinResultObjectTests.cpp
    aot/tests/AOTBuiltinSubclassTests.cpp
    aot/tests/AOTBuiltinTableTests.cpp
    aot/tests/AOTCacheRegistryRootTests.cpp
    aot/tests/AOTCacheRegistryTests.cpp
    aot/tests/AOTCacheSweepTests.cpp
    aot/tests/AOTCallingConventionTests.cpp
    aot/tests/AOTCheckLines.cpp
    aot/tests/AOTChildProcess.cpp
    aot/tests/AOTChildProcessTests.cpp
    aot/tests/AOTClassMemberOrderTests.cpp
    aot/tests/AOTClassWithoutInstanceTestTests.cpp
    aot/tests/AOTClosedWorldTests.cpp
    aot/tests/AOTCodeGeneratorTestCommands.cpp
    aot/tests/AOTCodeUnitTests.cpp
    aot/tests/AOTCollectorTestCommands.cpp
    aot/tests/AOTCollectorTests.cpp
    aot/tests/AOTCompactStorageTests.cpp
    aot/tests/AOTCompactTableTests.cpp
    aot/tests/AOTCompiledFrameTests.cpp
    aot/tests/AOTCompiledStructureTests.cpp
    aot/tests/AOTConcurrentComponentsTests.cpp
    aot/tests/AOTConstructorFunctionTests.cpp
    aot/tests/AOTCoroutineJobTests.cpp
    aot/tests/AOTDataBuilderTests.cpp
    aot/tests/AOTDeferredClosureProgramTests.cpp
    aot/tests/AOTDiagnosticTestCommands.cpp
    aot/tests/AOTDictionaryAccessByNameProgramTests.cpp
    aot/tests/AOTDictionaryExtraPropertiesTests.cpp
    aot/tests/AOTDriverTests.cpp
    aot/tests/AOTEffectSummariesTests.cpp
    aot/tests/AOTEffectsTestCommands.cpp
    aot/tests/AOTEngineHookTests.cpp
    aot/tests/AOTExtensionTests.cpp
    aot/tests/AOTFFIStubTests.cpp
    aot/tests/AOTFalseTypeProgramTests.cpp
    aot/tests/AOTFieldMoveTests.cpp
    aot/tests/AOTFlowReportTestCommands.cpp
    aot/tests/AOTFlowResultsDump.cpp
    aot/tests/AOTFlowResultsTests.cpp
    aot/tests/AOTFlowWorkLimitProgramTests.cpp
    aot/tests/AOTForInExpansionProgramTests.cpp
    aot/tests/AOTFrameAllocationTestCommands.cpp
    aot/tests/AOTFrameCellProgramTests.cpp
    aot/tests/AOTFunctionAnalysesTestCommands.cpp
    aot/tests/AOTGeneratedCodeTestCommands.cpp
    aot/tests/AOTGlobalBindingProgramTests.cpp
    aot/tests/AOTGlobalProxyReadProgramTests.cpp
    aot/tests/AOTGuardTestCommands.cpp
    aot/tests/AOTHashedSlotTableTests.cpp
    aot/tests/AOTHeapCageTests.cpp
    aot/tests/AOTHeapRememberedSlotTests.cpp
    aot/tests/AOTHeatGraphTests.cpp
    aot/tests/AOTIDTests.cpp
    aot/tests/AOTIRFunctionTests.cpp
    aot/tests/AOTIRLexer.cpp
    aot/tests/AOTIRParser.cpp
    aot/tests/AOTIRParserFunctions.cpp
    aot/tests/AOTIRParserTests.cpp
    aot/tests/AOTIRParserTypes.cpp
    aot/tests/AOTIRPrinter.cpp
    aot/tests/AOTIRPrinterTests.cpp
    aot/tests/AOTIRPrinterTypes.cpp
    aot/tests/AOTIRProgramTests.cpp
    aot/tests/AOTIRTestCommands.cpp
    aot/tests/AOTIRTests.cpp
    aot/tests/AOTIdentifierNumberingTests.cpp
    aot/tests/AOTImageBuildTests.cpp
    aot/tests/AOTImageCodeLayoutTests.cpp
    aot/tests/AOTImageCodeLinkerTests.cpp
    aot/tests/AOTImageDeterminismTests.cpp
    aot/tests/AOTImageEmissionTests.cpp
    aot/tests/AOTImageFaultTableTests.cpp
    aot/tests/AOTImageFunctionMergingTests.cpp
    aot/tests/AOTImageFunctionTableTests.cpp
    aot/tests/AOTImageOutliningRewriteTests.cpp
    aot/tests/AOTImageOutliningTests.cpp
    aot/tests/AOTImageProgramTests.cpp
    aot/tests/AOTImageRegExpTableTests.cpp
    aot/tests/AOTImageSiteTableTests.cpp
    aot/tests/AOTImageStackUsageTests.cpp
    aot/tests/AOTImageSymbolTests.cpp
    aot/tests/AOTImageTableTests.cpp
    aot/tests/AOTImageTestSupport.cpp
    aot/tests/AOTImageTextTests.cpp
    aot/tests/AOTImageUnitPlacementTests.cpp
    aot/tests/AOTInlinedSerializerTests.cpp
    aot/tests/AOTInnerPointerValidationTests.cpp
    aot/tests/AOTInstanceKeyOrderTests.cpp
    aot/tests/AOTInterpretedStoreUnderCompiledPrototypeTests.cpp
    aot/tests/AOTLedgerTests.cpp
    aot/tests/AOTListedFieldsTests.cpp
    aot/tests/AOTLoadedProgramTestSupport.cpp
    aot/tests/AOTLocationsReadAtRunTimeTests.cpp
    aot/tests/AOTLockedRealmMemberTests.cpp
    aot/tests/AOTLockedRealmTests.cpp
    aot/tests/AOTLookupBehindTablesProgramTests.cpp
    aot/tests/AOTMemberGuardTests.cpp
    aot/tests/AOTMembersNamedLikeBuiltinsTests.cpp
    aot/tests/AOTModuleGraphTests.cpp
    aot/tests/AOTModuleProgramTests.cpp
    aot/tests/AOTModuleRequestListTests.cpp
    aot/tests/AOTModuleTableTests.cpp
    aot/tests/AOTNakedConstructorTests.cpp
    aot/tests/AOTNarrowReferenceValidationTests.cpp
    aot/tests/AOTNativeCallValidationTests.cpp
    aot/tests/AOTNativeModuleExportTests.cpp
    aot/tests/AOTNativeStoreTests.cpp
    aot/tests/AOTNativeTestRunner.cpp
    aot/tests/AOTObjectCreateProgramTests.cpp
    aot/tests/AOTObjectPlacementDumps.cpp
    aot/tests/AOTObjectSlotExitTests.cpp
    aot/tests/AOTOperationScopeTests.cpp
    aot/tests/AOTOptionWordTests.cpp
    aot/tests/AOTParsedOptionalMemberTests.cpp
    aot/tests/AOTPassTestCommands.cpp
    aot/tests/AOTPipelineTestCommands.cpp
    aot/tests/AOTPipelineTesting.cpp
    aot/tests/AOTPlaceTypeTestsDump.cpp
    aot/tests/AOTPlaceTypeTestsTestCommands.cpp
    aot/tests/AOTPlacementDump.cpp
    aot/tests/AOTPlacementTestCommands.cpp
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
    aot/tests/AOTProgramTableTestCommands.cpp
    aot/tests/AOTProofValidationTests.cpp
    aot/tests/AOTPropertyAccessTestCommands.cpp
    aot/tests/AOTPropertyAttributeTests.cpp
    aot/tests/AOTPropertyLookupTableCollectionTests.cpp
    aot/tests/AOTPropertyLookupTableTests.cpp
    aot/tests/AOTPropertyLookupTests.cpp
    aot/tests/AOTPropertyRunProgramTests.cpp
    aot/tests/AOTPrototypeSetByKeyTests.cpp
    aot/tests/AOTPublishedEnumeratorProgramTests.cpp
    aot/tests/AOTPublishedEnumeratorTests.cpp
    aot/tests/AOTReaderFuzzer.cpp
    aot/tests/AOTReaderRecordTests.cpp
    aot/tests/AOTReaderStreamTests.cpp
    aot/tests/AOTReaderTests.cpp
    aot/tests/AOTRealmTestSupport.cpp
    aot/tests/AOTRegExpCompilerTests.cpp
    aot/tests/AOTRegExpImageTests.cpp
    aot/tests/AOTRemovableFeatureTests.cpp
    aot/tests/AOTReplacedBuiltinTests.cpp
    aot/tests/AOTRepresentationTestCommands.cpp
    aot/tests/AOTRequiredNameTests.cpp
    aot/tests/AOTRuntimeCounterTests.cpp
    aot/tests/AOTRuntimeOperationTests.cpp
    aot/tests/AOTRuntimeRecordTests.cpp
    aot/tests/AOTRuntimeReporterTests.cpp
    aot/tests/AOTRuntimeTypeTests.cpp
    aot/tests/AOTSavedFrameTests.cpp
    aot/tests/AOTScratchRegisterChecker.cpp
    aot/tests/AOTSelectionFormTestCommands.cpp
    aot/tests/AOTSelectionGroupTestCommands.cpp
    aot/tests/AOTSelectionTestCommands.cpp
    aot/tests/AOTSharingModeAndNamespaceTests.cpp
    aot/tests/AOTSiteCacheFillTests.cpp
    aot/tests/AOTSlimClosureTests.cpp
    aot/tests/AOTSlotAccessTests.cpp
    aot/tests/AOTSlotPlanDump.cpp
    aot/tests/AOTSlotPlanTestCommands.cpp
    aot/tests/AOTSparseElementProgramTests.cpp
    aot/tests/AOTStackCheckTests.cpp
    aot/tests/AOTStackTraceTests.cpp
    aot/tests/AOTStartupTests.cpp
    aot/tests/AOTStaticFieldTests.cpp
    aot/tests/AOTStaticMembersNamedLikeBuiltinsTests.cpp
    aot/tests/AOTStorageOwnerProgramTests.cpp
    aot/tests/AOTStoreAnswerTests.cpp
    aot/tests/AOTStringArenaTests.cpp
    aot/tests/AOTStringBuilderStorageTests.cpp
    aot/tests/AOTStringSearchTests.cpp
    aot/tests/AOTStringTableTests.cpp
    aot/tests/AOTStructuredCloneTests.cpp
    aot/tests/AOTStubAssemblerTests.cpp
    aot/tests/AOTStubBlobTests.cpp
    aot/tests/AOTStubCallTests.cpp
    aot/tests/AOTStubCatalogTests.cpp
    aot/tests/AOTStubCollectionTests.cpp
    aot/tests/AOTStubPropertyReadTests.cpp
    aot/tests/AOTStubPropertyStoreTests.cpp
    aot/tests/AOTStubRegExpTests.cpp
    aot/tests/AOTStubRuntimeTests.cpp
    aot/tests/AOTStubScratchRegisterTests.cpp
    aot/tests/AOTStubStringTests.cpp
    aot/tests/AOTStubTests.cpp
    aot/tests/AOTStubValueTests.cpp
    aot/tests/AOTStubVectorGrowthTests.cpp
    aot/tests/AOTSupportTestCommands.cpp
    aot/tests/AOTSupportTests.cpp
    aot/tests/AOTTableInImageTests.cpp
    aot/tests/AOTTableWriterTests.cpp
    aot/tests/AOTTailCallProgramTests.cpp
    aot/tests/AOTTestCommandRegistry.cpp
    aot/tests/AOTTestFile.cpp
    aot/tests/AOTTestFileTests.cpp
    aot/tests/AOTTestRunner.cpp
    aot/tests/AOTThisAllocationProgramTests.cpp
    aot/tests/AOTThisAllocationTests.cpp
    aot/tests/AOTThreadTestCommands.cpp
    aot/tests/AOTTypeArenaTestCommands.cpp
    aot/tests/AOTTypeArenaTests.cpp
    aot/tests/AOTTypeErrorMessageTests.cpp
    aot/tests/AOTTypeTestCommands.cpp
    aot/tests/AOTTypedResultProgramTests.cpp
    aot/tests/AOTUnwindingTests.cpp
    aot/tests/AOTValidationTestCommands.cpp
    aot/tests/AOTValueFlowBodiesTestCommands.cpp
    aot/tests/AOTValueFlowFamiliesTestCommands.cpp
    aot/tests/AOTValueFlowResultsTestCommands.cpp
    aot/tests/AOTValueFlowSolveTestCommands.cpp
    aot/tests/AOTValueFlowTestCommands.cpp
    aot/tests/AOTValueFlowWhyTestCommands.cpp
    aot/tests/AOTValueFlowWidenedReport.cpp
    aot/tests/AOTVerifierTests.cpp
    aot/tests/AOTWaiterOrderProgramTests.cpp
    aot/tests/AOTWebAssemblyImageTests.cpp
    aot/tests/AOTWebAssemblyInstanceTests.cpp
    aot/tests/AOTWireProgramBuilder.cpp
    aot/tests/cliffs/stack-capture-large-image.cpp
    aot/tests/testaot.cpp
    aot/tests/unit/BuiltinValidationTests.cpp
    aot/tests/unit/JSONKeyOrderTests.cpp
    aot/tests/unit/JSONParseOperationTests.cpp
    aot/tests/unit/JSONScannerTests.cpp
    aot/tests/unit/JSONShapeParserTests.cpp
    aot/tests/unit/JSONSourceTests.cpp
    aot/tests/unit/JSONWriterTests.cpp
    aot/tests/unit/PristineStructureTests.cpp
    aot/tests/unit/ResultNodeDeclineTests.cpp
    aot/tests/unit/ResultNodeTests.cpp
    aot/tests/valueflow/AOTValueFlowCoreTests.cpp
)

# Linked into every program but testaot, where AOTRemovableFeatureTests.cpp defines the same symbols.
set(JavaScriptCore_AOT_PROGRAM_SOURCES
    aot/runtime/AOTNoFeatureRemoved.cpp
)

# Linked into jsc.
set(JavaScriptCore_AOT_SHELL_SOURCES
    aot/tests/AOTShell.cpp
)

# Compiled for size like aot/SourcesCompiledForSize.txt.
set(JavaScriptCore_AOT_GENERATED_SOURCES_COMPILED_FOR_SIZE
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaRecords.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTSchemaVerifier.cpp"
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
    endif ()
    if (COMPILER_IS_GCC_OR_CLANG AND NOT MSVC)
        # A switch that has no default names every enumerator: whoever adds one is shown each question it has to answer.
        # The prefix is read as text. A precompiled header is loaded in front of every -include, and AOTPrefix.h begins with what has to stand in front of the standard library's headers.
        set_property(SOURCE ${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Werror=switch -include "${JAVASCRIPTCORE_DIR}/aot/support/AOTPrefix.h")
        set_source_files_properties(${JavaScriptCoreAOT_SOURCES} ${JavaScriptCoreAOTCompiledForSize_SOURCES} ${JavaScriptCoreAOTCompiledForMinimumSize_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
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

function(AOT_GENERATE_VECTOR)
    set(generator "${JAVASCRIPTCORE_DIR}/aot/support/generator/vector.ts")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${generator}" "${WTF_DIR}/wtf/Vector.h")
    execute_process(COMMAND "${NODE_EXECUTABLE}" "${generator}" --wtf "${WTF_DIR}" --out "${JavaScriptCore_AOT_GENERATED_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    AOT_REQUIRE_GENERATED_FILES(aot/support/generator/vector.ts ${JavaScriptCore_AOT_VECTOR_GENERATED_HEADERS})
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
        ${JavaScriptCore_AOT_VECTOR_GENERATED_HEADERS}
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

AOT_GENERATE_FROM_SCHEMA()
AOT_GENERATE_BUILTIN_TABLE()
AOT_GENERATE_VECTOR()
AOT_GENERATE_RUNTIME_FACTS()
AOT_COMPUTE_SOURCES()
AOT_CHECK_SOURCE_LISTS()
AOT_CHECK_EXPORTED_HEADERS()
