# CMakeLists.txt includes this file, and so does Bun's build, whose reader (scripts/build/cmake.ts in Bun) runs set(),
# list(APPEND) and include() of an absolute path and skips function bodies. So every list is written out, outside a function.
# `bun scripts/build/aot-source-lists.ts` in Bun writes aot/Sources.txt and the three lists marked "written by" below.
# It finds each of the three by its first line and the next line that is only ")".

include("${JAVASCRIPTCORE_DIR}/aot/ir/generated/AOTSchemaFiles.cmake")
include("${JAVASCRIPTCORE_DIR}/aot/builtins/AOTBuiltinTable.cmake")

list(APPEND JavaScriptCore_UNIFIED_SOURCE_LIST_FILES
    "aot/Sources.txt"
)

list(APPEND JavaScriptCore_SOURCES
    ${JavaScriptCore_AOT_SCHEMA_GENERATED_SOURCES}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_SOURCES}
)

# A shared library has no undefined symbols.
if (NOT ENABLE_STATIC_JSC)
    list(APPEND JavaScriptCore_SOURCES ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES})
endif ()

# Written by aot-source-lists.ts: every folder under aot/ that has a header.
list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES
    "${JAVASCRIPTCORE_DIR}/aot/analysis"
    "${JAVASCRIPTCORE_DIR}/aot/builtins"
    "${JAVASCRIPTCORE_DIR}/aot/builtins/generated"
    "${JAVASCRIPTCORE_DIR}/aot/codegen"
    "${JAVASCRIPTCORE_DIR}/aot/image"
    "${JAVASCRIPTCORE_DIR}/aot/ir"
    "${JAVASCRIPTCORE_DIR}/aot/ir/generated"
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
    aot/builtins/AOTBuiltinNameSlots.h
    aot/builtins/AOTBuiltinWords.h
    aot/codegen/AOTArrivalSiteNumbers.h
    aot/codegen/AOTCodegenScratchWords.h
    aot/image/AOTCallSiteTable.h
    aot/image/AOTCodeSites.h
    aot/image/AOTCompile.h
    aot/image/AOTFaultTable.h
    aot/image/AOTHandlerTable.h
    aot/image/AOTImage.h
    aot/image/AOTImageFormat.h
    aot/image/AOTImageForward.h
    aot/image/AOTImageLoading.h
    aot/image/AOTImageText.h
    aot/image/AOTObfuscatedSites.h
    aot/image/AOTPositionTable.h
    aot/image/AOTRuntimeDescription.h
    aot/image/AOTVarint.h
    aot/image/AOTWasmInterface.h
    aot/ir/AOTID.h
    aot/ir/AOTIRForm.h
    aot/ir/AOTNameClass.h
    aot/ir/AOTOpcode.h
    aot/ir/AOTSchemaSupport.h
    aot/ir/generated/AOTSchemaDeclaredEnums.h
    aot/ir/generated/AOTSchemaEntryEnums.h
    aot/ir/generated/AOTSchemaEntryMacros.h
    aot/ir/generated/AOTSchemaEnums.h
    aot/ir/generated/AOTSchemaFlagMasks.h
    aot/ir/generated/AOTSchemaIDs.h
    aot/ir/generated/AOTSchemaOperandIndexes.h
    aot/ir/generated/AOTSchemaOperandPositions.h
    aot/ir/generated/AOTSchemaOperationColumns.h
    aot/ir/generated/AOTSchemaOperations.h
    aot/ir/generated/AOTSchemaPayloadPositions.h
    aot/ir/generated/AOTSchemaPayloadWords.h
    aot/ir/generated/AOTSchemaResultRulesInlines.h
    aot/ir/generated/AOTSchemaRuntimeColumns.h
    aot/ir/generated/AOTSchemaRuntimeOperations.h
    aot/ir/generated/AOTSchemaValueOperandAccessors.inc
    aot/ir/generated/AOTSchemaVariadicOperandPositions.h
    aot/ir/generated/AOTSchemaWalkersInlines.h
    aot/ir/generated/AOTWire.h
    aot/ir/generated/AOTWireDiagnosticColumns.h
    aot/ir/generated/AOTWireEnumColumns.h
    aot/ir/generated/AOTWireEnums.h
    aot/ir/generated/AOTWireLimitColumns.h
    aot/ir/generated/AOTWireRecords.h
    aot/object/AOTAllocationSiteRecord.h
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
    aot/object/AOTObjectCensus.h
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
    aot/runtime/AOTBuiltinCells.h
    aot/runtime/AOTBuiltinModules.h
    aot/runtime/AOTCallSiteCache.h
    aot/runtime/AOTCompactStorage.h
    aot/runtime/AOTCompactTable.h
    aot/runtime/AOTCompiledCaller.h
    aot/runtime/AOTCompiledFrame.h
    aot/runtime/AOTCompiledFrameExit.h
    aot/runtime/AOTCoroutineData.h
    aot/runtime/AOTCoroutineWords.h
    aot/runtime/AOTFunctionDescription.h
    aot/runtime/AOTGenerator.h
    aot/runtime/AOTHeapCellClasses.h
    aot/runtime/AOTHeapCellVisits.h
    aot/runtime/AOTHeapHooks.h
    aot/runtime/AOTHeapWords.h
    aot/runtime/AOTHost.h
    aot/runtime/AOTInstance.h
    aot/runtime/AOTInstanceLayout.h
    aot/runtime/AOTInterpreterSlowPaths.h
    aot/runtime/AOTIterationWords.h
    aot/runtime/AOTJSONHooks.h
    aot/runtime/AOTJSONWords.h
    aot/runtime/AOTMembersLeftOut.h
    aot/runtime/AOTModuleData.h
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
    aot/valueflow/AOTFlowReport.h
)

list(APPEND JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    ${JavaScriptCore_AOT_EXPORTED_HEADERS}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_HEADERS}
)

# Written by aot-source-lists.ts: every AOT*TestCommands.cpp and every .cpp under aot/tests/ but AOTShell.cpp. Linked into testaot only.
set(JavaScriptCore_AOT_TEST_SOURCES
    aot/analysis/AOTAnalysisTestCommands.cpp
    aot/analysis/AOTFunctionAnalysesTestCommands.cpp
    aot/analysis/AOTGuardTestCommands.cpp
    aot/analysis/AOTPropertyAccessTestCommands.cpp
    aot/codegen/AOTCodeGeneratorTestCommands.cpp
    aot/codegen/AOTCollectorTestCommands.cpp
    aot/codegen/AOTGeneratedCodeTestCommands.cpp
    aot/codegen/AOTValidationTestCommands.cpp
    aot/ir/AOTEffectsTestCommands.cpp
    aot/ir/AOTIRTestCommands.cpp
    aot/ir/AOTTypeTestCommands.cpp
    aot/passes/AOTFrameAllocationTestCommands.cpp
    aot/passes/AOTPassTestCommands.cpp
    aot/passes/AOTPlaceTypeTestsTestCommands.cpp
    aot/place/AOTPlacementTestCommands.cpp
    aot/place/AOTProgramTableTestCommands.cpp
    aot/place/AOTSlotPlanTestCommands.cpp
    aot/represent/AOTRepresentationTestCommands.cpp
    aot/select/AOTSelectionFormTestCommands.cpp
    aot/select/AOTSelectionGroupTestCommands.cpp
    aot/select/AOTSelectionTestCommands.cpp
    aot/support/AOTDiagnosticTestCommands.cpp
    aot/support/AOTSupportTestCommands.cpp
    aot/support/AOTThreadTestCommands.cpp
    aot/support/AOTTypeArenaTestCommands.cpp
    aot/tests/AOTArrayTests.cpp
    aot/tests/AOTArrivalCountTests.cpp
    aot/tests/AOTAssignmentPlanTests.cpp
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
    aot/tests/AOTCodeUnitTests.cpp
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
    aot/tests/AOTDictionaryAccessByNameProgramTests.cpp
    aot/tests/AOTDictionaryExtraPropertiesTests.cpp
    aot/tests/AOTDriverTests.cpp
    aot/tests/AOTEffectSummariesTests.cpp
    aot/tests/AOTEngineHookTests.cpp
    aot/tests/AOTExtensionTests.cpp
    aot/tests/AOTFFIStubTests.cpp
    aot/tests/AOTFalseTypeProgramTests.cpp
    aot/tests/AOTFieldMoveTests.cpp
    aot/tests/AOTFlowResultsTests.cpp
    aot/tests/AOTFlowWorkLimitProgramTests.cpp
    aot/tests/AOTForInExpansionProgramTests.cpp
    aot/tests/AOTFrameCellProgramTests.cpp
    aot/tests/AOTGlobalBindingProgramTests.cpp
    aot/tests/AOTGlobalProxyReadProgramTests.cpp
    aot/tests/AOTHashedSlotTableTests.cpp
    aot/tests/AOTHeapCageTests.cpp
    aot/tests/AOTHeapRememberedSlotTests.cpp
    aot/tests/AOTHeatGraphTests.cpp
    aot/tests/AOTIDTests.cpp
    aot/tests/AOTIRFunctionTests.cpp
    aot/tests/AOTIRParserTests.cpp
    aot/tests/AOTIRPrinterTests.cpp
    aot/tests/AOTIRProgramTests.cpp
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
    aot/tests/AOTInnerObjectTests.cpp
    aot/tests/AOTInstanceKeyOrderTests.cpp
    aot/tests/AOTInterpretedStoreUnderCompiledPrototypeTests.cpp
    aot/tests/AOTListedFieldsTests.cpp
    aot/tests/AOTLoadedProgramTestSupport.cpp
    aot/tests/AOTLocationsReadAtRunTimeTests.cpp
    aot/tests/AOTLockedRealmMemberTests.cpp
    aot/tests/AOTLockedRealmTests.cpp
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
    aot/tests/AOTObjectSlotExitTests.cpp
    aot/tests/AOTOperationScopeTests.cpp
    aot/tests/AOTOptionWordTests.cpp
    aot/tests/AOTParsedOptionalMemberTests.cpp
    aot/tests/AOTPipelineTestCommands.cpp
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
    aot/tests/AOTProofValidationTests.cpp
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
    aot/tests/AOTRequiredNameTests.cpp
    aot/tests/AOTRuntimeCounterTests.cpp
    aot/tests/AOTRuntimeOperationTests.cpp
    aot/tests/AOTRuntimeRecordTests.cpp
    aot/tests/AOTRuntimeReporterTests.cpp
    aot/tests/AOTRuntimeTypeTests.cpp
    aot/tests/AOTSavedFrameTests.cpp
    aot/tests/AOTSharingModeAndNamespaceTests.cpp
    aot/tests/AOTSiteCacheFillTests.cpp
    aot/tests/AOTSlimClosureTests.cpp
    aot/tests/AOTSlotAccessTests.cpp
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
    aot/tests/AOTStubStringTests.cpp
    aot/tests/AOTStubTests.cpp
    aot/tests/AOTStubValueTests.cpp
    aot/tests/AOTStubVectorGrowthTests.cpp
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
    aot/tests/AOTTypeArenaTests.cpp
    aot/tests/AOTTypeErrorMessageTests.cpp
    aot/tests/AOTTypedResultProgramTests.cpp
    aot/tests/AOTUnwindingTests.cpp
    aot/tests/AOTValidatedValueTests.cpp
    aot/tests/AOTVerifierTests.cpp
    aot/tests/AOTWaiterOrderProgramTests.cpp
    aot/tests/AOTWebAssemblyImageTests.cpp
    aot/tests/AOTWebAssemblyInstanceTests.cpp
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
    aot/tests/unit/ValidationPlanTextTests.cpp
    aot/tests/valueflow/AOTValueFlowCoreTests.cpp
    aot/valueflow/AOTFlowApplyTestCommands.cpp
    aot/valueflow/AOTFlowReportTestCommands.cpp
    aot/valueflow/AOTFlowTestCommands.cpp
    aot/valueflow/AOTFlowValidationTestCommands.cpp
    aot/valueflow/AOTRelationalSummariesAccessorsTestCommands.cpp
    aot/valueflow/AOTRelationalSummariesAtCallsTestCommands.cpp
    aot/valueflow/AOTRelationalSummariesOriginsTestCommands.cpp
    aot/valueflow/AOTRelationalSummariesTestCommands.cpp
    aot/valueflow/AOTValueFlowBodiesTestCommands.cpp
    aot/valueflow/AOTValueFlowFamiliesTestCommands.cpp
    aot/valueflow/AOTValueFlowResultsTestCommands.cpp
    aot/valueflow/AOTValueFlowSolveTestCommands.cpp
    aot/valueflow/AOTValueFlowTestCommands.cpp
    aot/valueflow/AOTValueFlowWhyTestCommands.cpp
)

# Linked into every program but testaot, where AOTRemovableFeatureTests.cpp defines the same symbols.
set(JavaScriptCore_AOT_PROGRAM_SOURCES
    aot/runtime/AOTNoFeatureRemoved.cpp
)

# Linked into jsc.
set(JavaScriptCore_AOT_SHELL_SOURCES
    aot/tests/AOTShell.cpp
)

# Build-only sources: nothing of them is left in an executable without the compiler. What a build runs once (the emitters of stubs), never (verifiers, printers) or hardly ever (passes for rare forms).
set(JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE
    aot/analysis/AOTAnalysisDump.cpp
    aot/analysis/AOTAnalysisVerification.cpp
    aot/analysis/AOTCheckCause.cpp
    aot/analysis/AOTClosureObservers.cpp
    aot/analysis/AOTUnspecifiedValuePaths.cpp
    aot/analysis/AOTWholeLiterals.cpp
    aot/builtins/AOTArrayBufferViewBuiltinEmitter.cpp
    aot/builtins/AOTBufferBuiltinEmitter.cpp
    aot/builtins/AOTBufferBuiltinEmitterAtomics.cpp
    aot/builtins/AOTBuiltinCallEmitterNumbers.cpp
    aot/builtins/AOTBuiltinTableDescription.cpp
    aot/builtins/AOTBuiltinTableValidation.cpp
    aot/builtins/AOTDateBuiltinEmitter.cpp
    aot/builtins/AOTEngineIntrinsicEmitter.cpp
    aot/builtins/AOTErrorBuiltinEmitter.cpp
    aot/builtins/AOTFunctionBuiltinEmitter.cpp
    aot/builtins/AOTIntlBuiltinEmitter.cpp
    aot/builtins/AOTMapSetBuiltinEmitter.cpp
    aot/builtins/AOTMathBuiltinEmitter.cpp
    aot/builtins/AOTObjectBuiltinEmitter.cpp
    aot/builtins/AOTRegExpBuiltinEmitter.cpp
    aot/builtins/AOTRuntimeIntrinsicEmitter.cpp
    aot/builtins/AOTSimplifyBuiltinCallsSplits.cpp
    aot/codegen/AOTASANShadowChecks.cpp
    aot/codegen/AOTArgumentsEmitter.cpp
    aot/codegen/AOTArrayBuiltinEmitterCopies.cpp
    aot/codegen/AOTArrivalSiteNumbers.cpp
    aot/codegen/AOTBranchCounterNumbers.cpp
    aot/codegen/AOTCallEmitterLists.cpp
    aot/codegen/AOTCallMethod.cpp
    aot/codegen/AOTClassEmitter.cpp
    aot/codegen/AOTCodeSizeMap.cpp
    aot/codegen/AOTCollectionIntrinsicEmitter.cpp
    aot/codegen/AOTElementAccessEmitterTypedArrays.cpp
    aot/codegen/AOTElementEmitterLiterals.cpp
    aot/codegen/AOTEvalEmitter.cpp
    aot/codegen/AOTExpandedIterationEmitter.cpp
    aot/codegen/AOTForInEmitter.cpp
    aot/codegen/AOTFrameAddressValidation.cpp
    aot/codegen/AOTHintEmitter.cpp
    aot/codegen/AOTJSONParseEmitter.cpp
    aot/codegen/AOTJSONParseEmitterKeyOrders.cpp
    aot/codegen/AOTJSONWriteEmitter.cpp
    aot/codegen/AOTModuleEmitter.cpp
    aot/codegen/AOTObjectCreationEmitter.cpp
    aot/codegen/AOTOperatorEmitterBitwise.cpp
    aot/codegen/AOTOperatorEmitterInstanceOf.cpp
    aot/codegen/AOTOwnAccessEmitter.cpp
    aot/codegen/AOTPrivateAccessEmitter.cpp
    aot/codegen/AOTPromiseBuiltinEmitter.cpp
    aot/codegen/AOTPromiseIntrinsicEmitter.cpp
    aot/codegen/AOTResolvedCallEmitter.cpp
    aot/codegen/AOTRuntimeValidationChecks.cpp
    aot/codegen/AOTRuntimeValidationEmitter.cpp
    aot/codegen/AOTSelectedOperatorEmitter.cpp
    aot/codegen/AOTStackCheckEmitter.cpp
    aot/codegen/AOTStringBuilderCursor.cpp
    aot/codegen/AOTStringBuilderPartEmitter.cpp
    aot/codegen/AOTStringBuiltinEmitter.cpp
    aot/codegen/AOTValidationEmitter.cpp
    aot/ir/AOTCheckInt32Promises.cpp
    aot/ir/AOTIRPrinter.cpp
    aot/ir/AOTIRPrinterTypes.cpp
    aot/ir/AOTInt32Proof.cpp
    aot/ir/AOTProgramVerifier.cpp
    aot/ir/AOTProgramVerifierFunctions.cpp
    aot/ir/AOTProgramVerifierModules.cpp
    aot/ir/AOTRelationsVerifier.cpp
    aot/ir/AOTResultRuleMonotonicity.cpp
    aot/ir/AOTTaggedLayouts.cpp
    aot/ir/AOTTypeClosure.cpp
    aot/ir/AOTVerifier.cpp
    aot/ir/AOTVerifierAccesses.cpp
    aot/ir/AOTVerifierCalls.cpp
    aot/ir/AOTVerifierDominance.cpp
    aot/ir/AOTVerifierEdges.cpp
    aot/ir/AOTVerifierGuards.cpp
    aot/ir/AOTVerifierOperations.cpp
    aot/ir/AOTVerifierTypes.cpp
    aot/ir/AOTVerifierUnspecifiedValues.cpp
    aot/ir/generated/AOTSchemaEntryPrinter.cpp
    aot/ir/generated/AOTSchemaRecordPrinter.cpp
    aot/ir/generated/AOTSchemaRecords.cpp
    aot/ir/generated/AOTSchemaVerifier.cpp
    aot/ir/generated/AOTWire.cpp
    aot/passes/AOTAllocateTaggedLayouts.cpp
    aot/passes/AOTArrayChainInterleaving.cpp
    aot/passes/AOTArrayChainStep.cpp
    aot/passes/AOTCalleeVersions.cpp
    aot/passes/AOTCheckTypesAtEntry.cpp
    aot/passes/AOTChooseCallingConventionsStubBodies.cpp
    aot/passes/AOTCoroutineSplit.cpp
    aot/passes/AOTCreateBoundFunctions.cpp
    aot/passes/AOTCreatedObjectLayouts.cpp
    aot/passes/AOTEliminateTailRecursion.cpp
    aot/passes/AOTExpandIterationCollections.cpp
    aot/passes/AOTExpandIterationForIn.cpp
    aot/passes/AOTExpandIterationGenerators.cpp
    aot/passes/AOTExpandIterationIndexed.cpp
    aot/passes/AOTExpandIterationProtocol.cpp
    aot/passes/AOTExpandIterationSupport.cpp
    aot/passes/AOTExpandOwnKeysStringify.cpp
    aot/passes/AOTFrameAllocationMaterialization.cpp
    aot/passes/AOTFrameAllocationVerifier.cpp
    aot/passes/AOTFuseGeneratorLoops.cpp
    aot/passes/AOTFuseGeneratorLoopsMerge.cpp
    aot/passes/AOTInlineHeads.cpp
    aot/passes/AOTJSONEmission.cpp
    aot/passes/AOTJSONParserArrays.cpp
    aot/passes/AOTJSONParserBuilder.cpp
    aot/passes/AOTJSONParserFunction.cpp
    aot/passes/AOTJSONParserObjects.cpp
    aot/passes/AOTJSONParserUnions.cpp
    aot/passes/AOTJSONSerializerArrays.cpp
    aot/passes/AOTJSONSerializerBuilder.cpp
    aot/passes/AOTJSONSerializerFunction.cpp
    aot/passes/AOTJSONSerializerObjects.cpp
    aot/passes/AOTJSONSupport.cpp
    aot/passes/AOTJSONVerification.cpp
    aot/passes/AOTJSONVerificationCapacity.cpp
    aot/passes/AOTReduceIntegerDivisions.cpp
    aot/passes/AOTRemoveUnreadExports.cpp
    aot/passes/AOTResolveAccessesVariables.cpp
    aot/passes/AOTResolveCallsLists.cpp
    aot/passes/AOTSelectTypedResultsDecisions.cpp
    aot/passes/AOTSelectTypedResultsForms.cpp
    aot/passes/AOTSelectTypedResultsNodes.cpp
    aot/passes/AOTSelectTypedResultsTargets.cpp
    aot/passes/AOTSelectTypedResultsTypeNodes.cpp
    aot/passes/AOTSinkAllocationsArguments.cpp
    aot/passes/AOTSinkAllocationsBuiltinResults.cpp
    aot/passes/AOTStartGeneratorsAtFirstStep.cpp
    aot/passes/AOTValidationPlanText.cpp
    aot/passes/AOTValidationRules.cpp
    aot/passes/AOTValidatorBuilder.cpp
    aot/passes/AOTValidatorFunction.cpp
    aot/passes/AOTValidatorObjects.cpp
    aot/passes/AOTValidatorUnions.cpp
    aot/pipeline/AOTInterningPhases.cpp
    aot/pipeline/AOTPipelineSteps.cpp
    aot/place/AOTCellSizeClasses.cpp
    aot/place/AOTEnvironmentPacking.cpp
    aot/place/AOTGlobalBindingPlacement.cpp
    aot/place/AOTGuardBits.cpp
    aot/place/AOTMethodTableEncoding.cpp
    aot/place/AOTPlacementCheck.cpp
    aot/place/AOTPlacementDump.cpp
    aot/place/AOTPlacementSiteCheck.cpp
    aot/place/AOTPlacementSlotCheck.cpp
    aot/place/AOTPropertyRuns.cpp
    aot/place/AOTRequestRecords.cpp
    aot/place/AOTRequestTableEncoding.cpp
    aot/place/AOTShapeVariants.cpp
    aot/place/AOTSiteWeight.cpp
    aot/place/AOTSlotPlanDump.cpp
    aot/place/AOTStaticsLayout.cpp
    aot/place/AOTStaticsTableEncoding.cpp
    aot/represent/AOTRepresentationCheck.cpp
    aot/select/AOTArrayRules.cpp
    aot/select/AOTArrayRulesInlineForms.cpp
    aot/select/AOTBuiltinFormRulesCollections.cpp
    aot/select/AOTBuiltinFormRulesNumbers.cpp
    aot/select/AOTDataRules.cpp
    aot/select/AOTNumberPathRules.cpp
    aot/select/AOTObjectCreationRules.cpp
    aot/select/AOTPromiseRulesInlineForms.cpp
    aot/select/AOTRawWordCheck.cpp
    aot/select/AOTRawWordWebs.cpp
    aot/select/AOTStringBuiltinForms.cpp
    aot/select/AOTSwitchRules.cpp
    aot/stubs/AOTStubArgumentThunks.cpp
    aot/stubs/AOTStubAssembler.cpp
    aot/stubs/AOTStubAssemblerCalls.cpp
    aot/stubs/AOTStubAssemblerOperationCalls.cpp
    aot/stubs/AOTStubBlob.cpp
    aot/stubs/AOTStubBodyConvention.cpp
    aot/stubs/AOTStubCallValidation.cpp
    aot/stubs/AOTStubCalls.cpp
    aot/stubs/AOTStubCatalog.cpp
    aot/stubs/AOTStubDescriptor.cpp
    aot/stubs/AOTStubFrontEnds.cpp
    aot/stubs/AOTStubHelpersAllocation.cpp
    aot/stubs/AOTStubHelpersFrames.cpp
    aot/stubs/AOTStubHelpersFunctions.cpp
    aot/stubs/AOTStubHelpersStorage.cpp
    aot/stubs/AOTStubHelpersValues.cpp
    aot/stubs/AOTStubPolicy.cpp
    aot/stubs/AOTStubsAllocation.cpp
    aot/stubs/AOTStubsArithmetic.cpp
    aot/stubs/AOTStubsArrayHoles.cpp
    aot/stubs/AOTStubsArrayLiterals.cpp
    aot/stubs/AOTStubsArrayPush.cpp
    aot/stubs/AOTStubsArrayReads.cpp
    aot/stubs/AOTStubsArraySlice.cpp
    aot/stubs/AOTStubsArrayStores.cpp
    aot/stubs/AOTStubsBarriers.cpp
    aot/stubs/AOTStubsBoxedCalls.cpp
    aot/stubs/AOTStubsCallAdapters.cpp
    aot/stubs/AOTStubsCallEngine.cpp
    aot/stubs/AOTStubsCallFrames.cpp
    aot/stubs/AOTStubsCallHost.cpp
    aot/stubs/AOTStubsCallLists.cpp
    aot/stubs/AOTStubsCalls.cpp
    aot/stubs/AOTStubsClosures.cpp
    aot/stubs/AOTStubsCollections.cpp
    aot/stubs/AOTStubsConstants.cpp
    aot/stubs/AOTStubsConstruct.cpp
    aot/stubs/AOTStubsCoroutineJobs.cpp
    aot/stubs/AOTStubsCoroutinePromises.cpp
    aot/stubs/AOTStubsCoroutineSteps.cpp
    aot/stubs/AOTStubsCoroutineSuspension.cpp
    aot/stubs/AOTStubsElements.cpp
    aot/stubs/AOTStubsExceptions.cpp
    aot/stubs/AOTStubsFieldCreation.cpp
    aot/stubs/AOTStubsFieldReads.cpp
    aot/stubs/AOTStubsFieldStores.cpp
    aot/stubs/AOTStubsFieldTypeTests.cpp
    aot/stubs/AOTStubsGlobalBindings.cpp
    aot/stubs/AOTStubsHashTables.cpp
    aot/stubs/AOTStubsKeyedAccess.cpp
    aot/stubs/AOTStubsKnownFieldStores.cpp
    aot/stubs/AOTStubsObjectCreation.cpp
    aot/stubs/AOTStubsOperations.cpp
    aot/stubs/AOTStubsPolls.cpp
    aot/stubs/AOTStubsPrimitiveFastPaths.cpp
    aot/stubs/AOTStubsPropertyAccessors.cpp
    aot/stubs/AOTStubsPropertyCannot.cpp
    aot/stubs/AOTStubsPropertyExtraProperties.cpp
    aot/stubs/AOTStubsPropertyFreeNames.cpp
    aot/stubs/AOTStubsPropertyInlineCode.cpp
    aot/stubs/AOTStubsPropertyInstanceOf.cpp
    aot/stubs/AOTStubsPropertyMethodCalls.cpp
    aot/stubs/AOTStubsPropertyProbes.cpp
    aot/stubs/AOTStubsPropertyRead.cpp
    aot/stubs/AOTStubsPropertyReadHandlers.cpp
    aot/stubs/AOTStubsPropertyReadPairs.cpp
    aot/stubs/AOTStubsPropertySiteCaches.cpp
    aot/stubs/AOTStubsPropertyStore.cpp
    aot/stubs/AOTStubsPropertyStoreHandlers.cpp
    aot/stubs/AOTStubsRegExp.cpp
    aot/stubs/AOTStubsRegExpLiterals.cpp
    aot/stubs/AOTStubsStringConcatenation.cpp
    aot/stubs/AOTStubsStringConversions.cpp
    aot/stubs/AOTStubsStringHelpers.cpp
    aot/stubs/AOTStubsStringLiterals.cpp
    aot/stubs/AOTStubsStringSearches.cpp
    aot/stubs/AOTStubsStringSubstrings.cpp
    aot/stubs/AOTStubsStrings.cpp
    aot/stubs/AOTStubsValues.cpp
    aot/valueflow/AOTFlowLockedWrites.cpp
    aot/valueflow/AOTFlowNamespaces.cpp
    aot/valueflow/AOTFlowResultsDump.cpp
    aot/valueflow/AOTFlowValidation.cpp
    aot/valueflow/AOTRelationalSummariesOriginsValidation.cpp
    aot/valueflow/AOTValueFlowResultsSymbols.cpp
)

# Sources whose file-local names collide with another source in their directory. Tools/Scripts/aot/check-unified-sources.py --write maintains both lists.
# Bundled with each other instead of with the rest of their directory:
set(JavaScriptCore_AOT_SEPARATELY_UNIFIED_SOURCES
    aot/analysis/AOTPropertyAccessMembers.cpp
    aot/analysis/AOTPropertyAccessPartition.cpp
    aot/builtins/AOTResolveBuiltins.cpp
    aot/builtins/AOTSimplifyBuiltinCallsByteRanges.cpp
    aot/codegen/AOTIntegerUpdatesInMemory.cpp
    aot/codegen/AOTOperatorEmitterEquality.cpp
    aot/codegen/AOTStringEmitterComparisons.cpp
    aot/codegen/AOTTypeTestEmitter.cpp
    aot/image/AOTOutliner.cpp
    aot/image/AOTSectionWriter.cpp
    aot/ir/AOTEdgeResults.cpp
    aot/ir/AOTEffectsOfOperands.cpp
    aot/ir/AOTIRParserTypes.cpp
    aot/ir/AOTIRPrinterTypes.cpp
    aot/ir/AOTProgramVerifierFunctions.cpp
    aot/ir/AOTReaderTypes.cpp
    aot/ir/AOTTypeSubset.cpp
    aot/ir/AOTVerifierOperations.cpp
    aot/object/AOTClosureMethodTable.cpp
    aot/passes/AOTChooseCallingConventionsResults.cpp
    aot/passes/AOTCreateStringBuilders.cpp
    aot/passes/AOTDecidedBranches.cpp
    aot/passes/AOTDeferInitializers.cpp
    aot/passes/AOTFreshResultsWorkers.cpp
    aot/passes/AOTFuseGeneratorLoopsMerge.cpp
    aot/passes/AOTInlineHeads.cpp
    aot/passes/AOTInsertObjectCoercibleChecks.cpp
    aot/passes/AOTOutlineColdOperations.cpp
    aot/passes/AOTOutliningCandidates.cpp
    aot/passes/AOTPlaceTypeTestsHoisting.cpp
    aot/passes/AOTPromoteEnvironmentsToVariables.cpp
    aot/passes/AOTRemoveUnusedResults.cpp
    aot/passes/AOTReuseEnvironments.cpp
    aot/passes/AOTSplitUnions.cpp
    aot/passes/AOTStringBuilderNestedWebs.cpp
    aot/passes/AOTSummarizeForInlining.cpp
    aot/passes/AOTValidatorFunction.cpp
    aot/passes/AOTValidatorObjects.cpp
    aot/place/AOTMethodTable.cpp
    aot/place/AOTObjectPacking.cpp
    aot/place/AOTPlacedSites.cpp
    aot/place/AOTStaticsTableEncoding.cpp
    aot/runtime/AOTCallOperations.cpp
    aot/runtime/AOTModuleLinking.cpp
    aot/runtime/AOTPropertyCachesFieldAnswers.cpp
    aot/runtime/AOTRuntimeReporter.cpp
    aot/runtime/AOTStackTrace.cpp
    aot/runtime/AOTState.cpp
    aot/runtime/AOTStringBuilderStorageResults.cpp
    aot/runtime/AOTStringBuiltinLiteralOperations.cpp
    aot/runtime/AOTStringBuiltinOperations.cpp
    aot/select/AOTBuiltinFormRulesNumbers.cpp
    aot/select/AOTComparisonForms.cpp
    aot/select/AOTGroupRewriterPaths.cpp
    aot/select/AOTKeyPartsRules.cpp
    aot/select/AOTPromiseRules.cpp
    aot/stubs/AOTStubCalls.cpp
    aot/stubs/AOTStubsArrayLiterals.cpp
    aot/stubs/AOTStubsCallAdapters.cpp
    aot/stubs/AOTStubsPropertyInstanceOf.cpp
    aot/stubs/AOTStubsPropertyRead.cpp
    aot/stubs/AOTStubsPropertyReadHandlers.cpp
    aot/stubs/AOTStubsPropertyStore.cpp
    aot/support/AOTCompileOptions.cpp
    aot/support/AOTDiagnostics.cpp
    aot/valueflow/AOTRelationalSummariesLink.cpp
    aot/valueflow/AOTValueFlowBodies.cpp
    aot/valueflow/AOTValueFlowBuiltinTables.cpp
    aot/valueflow/AOTValueFlowResultsTypedResults.cpp
    aot/valueflow/AOTValueFlowResultsUnseenCode.cpp
    aot/valueflow/AOTValueFlowRulesProtocols.cpp
    aot/valueflow/AOTValueFlowTypedResults.cpp
)

# Not bundled:
set(JavaScriptCore_AOT_NON_UNIFIED_SOURCES
    aot/analysis/AOTPropertyAccessStaticMembers.cpp
    aot/builtins/AOTURLBuiltinOperations.cpp
    aot/codegen/AOTCallEmitter.cpp
    aot/codegen/AOTCallEmitterTail.cpp
    aot/codegen/AOTCallReturns.cpp
    aot/codegen/AOTLinearScanAllocation.cpp
    aot/codegen/AOTLinearScanCodeGenerator.cpp
    aot/codegen/AOTLinearScanImport.cpp
    aot/codegen/AOTLinearScanLowering.cpp
    aot/image/AOTRegExpRequests.cpp
    aot/image/AOTRegExpSection.cpp
    aot/image/AOTStringUses.cpp
    aot/passes/AOTAddGenericVersion.cpp
    aot/passes/AOTChooseCallingConventionsStack.cpp
    aot/passes/AOTExpandIterationPatterns.cpp
    aot/passes/AOTFlattenInnerObjects.cpp
    aot/passes/AOTMergeDuplicateBlocks.cpp
    aot/passes/AOTNarrowFieldTypes.cpp
    aot/passes/AOTNumberLiteralStrings.cpp
    aot/passes/AOTPromoteEnvironmentsToVariablesPlan.cpp
    aot/passes/AOTReadCapturesFromEnvironments.cpp
    aot/passes/AOTRemoveUnreadFields.cpp
    aot/passes/AOTResolveCallsConstruct.cpp
    aot/passes/AOTSinkAllocationsBuilding.cpp
    aot/passes/AOTSinkAllocationsCandidates.cpp
    aot/passes/AOTSinkAllocationsDataflow.cpp
    aot/passes/AOTSinkAllocationsEdges.cpp
    aot/passes/AOTSinkAllocationsExecution.cpp
    aot/passes/AOTSinkAllocationsListUses.cpp
    aot/passes/AOTSinkAllocationsTests.cpp
    aot/passes/AOTSplitBehindEntryChecks.cpp
    aot/passes/AOTVirtualObjectParameters.cpp
    aot/place/AOTShapeTests.cpp
    aot/place/AOTSlotUses.cpp
    aot/runtime/AOTPropertyCachesFreeNameOperations.cpp
    aot/runtime/AOTPropertyCachesReadAnswers.cpp
    aot/select/AOTOperatorFormRule.cpp
    aot/select/AOTStringBuilderPartRules.cpp
    aot/select/AOTStringBuiltinForms.cpp
    aot/select/AOTStringCharacterRules.cpp
    aot/select/AOTStringLiteralRules.cpp
    aot/select/AOTStringOrderRules.cpp
    aot/select/AOTStringRangeRules.cpp
    aot/select/AOTVariableRules.cpp
    aot/stubs/AOTStubsCallEngine.cpp
    aot/stubs/AOTStubsCallFrames.cpp
    aot/stubs/AOTStubsCallLists.cpp
    aot/stubs/AOTStubsCalls.cpp
    aot/stubs/AOTStubsCoroutineSteps.cpp
    aot/stubs/AOTStubsPolls.cpp
    aot/stubs/AOTStubsPropertyStoreHandlers.cpp
    aot/valueflow/AOTRelationalSummariesOrigins.cpp
    aot/valueflow/AOTValueFlowPositions.cpp
    aot/valueflow/AOTValueFlowResultsFields.cpp
    aot/valueflow/AOTValueFlowResultsObjects.cpp
    aot/valueflow/AOTValueFlowResultsVariablesOnObjects.cpp
    aot/valueflow/AOTValueFlowRules.cpp
    aot/valueflow/AOTValueFlowRulesCalls.cpp
    aot/valueflow/AOTValueFlowRulesCoroutines.cpp
    aot/valueflow/AOTValueFlowRulesHeap.cpp
    aot/valueflow/AOTValueFlowRulesHeapLookups.cpp
    aot/valueflow/AOTValueFlowRulesHost.cpp
    aot/valueflow/AOTValueFlowRulesIteration.cpp
    aot/valueflow/AOTValueFlowRulesModules.cpp
    aot/valueflow/AOTValueFlowRulesPromises.cpp
    aot/valueflow/AOTValueFlowSolver.cpp
)

function(AOT_COMPILE_SOURCES_FOR_SIZE)
    if (COMPILER_IS_CLANG AND NOT MSVC)
        set_source_files_properties(${JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE} PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-mllvm=-force-attribute=optsize>")
    endif ()
endfunction()

# Only CMake builds aot/ as unified sources. Bun's own build reads aot/Sources.txt and compiles each source separately.
function(AOT_GENERATE_UNIFIED_SOURCES)
    if (NOT ENABLE_UNIFIED_BUILDS)
        return ()
    endif ()
    set(bundleSize 16)
    AOT_READ_LIBRARY_SOURCES(librarySources problems)
    set(sources)
    set(bundlesCompiledForSize)
    set(groups)
    foreach (path IN LISTS librarySources)
        if (path IN_LIST JavaScriptCore_AOT_NON_UNIFIED_SOURCES)
            list(APPEND sources ${path})
            continue ()
        endif ()
        string(REGEX REPLACE "^aot/([^/]+)/.*$" "\\1" group ${path})
        if (path IN_LIST JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE)
            string(APPEND group "-for-size")
        endif ()
        if (path IN_LIST JavaScriptCore_AOT_SEPARATELY_UNIFIED_SOURCES)
            string(APPEND group "-separate")
        endif ()
        list(APPEND groups ${group})
        list(APPEND sources_${group} ${path})
    endforeach ()
    list(REMOVE_DUPLICATES groups)
    foreach (group IN LISTS groups)
        list(LENGTH sources_${group} count)
        set(start 0)
        set(index 0)
        while (start LESS count)
            list(SUBLIST sources_${group} ${start} ${bundleSize} bundledSources)
            math(EXPR start "${start} + ${bundleSize}")
            math(EXPR index "${index} + 1")
            set(text "")
            foreach (path IN LISTS bundledSources)
                string(APPEND text "#include \"${path}\"\n")
            endforeach ()
            set(bundle "${JavaScriptCore_DERIVED_SOURCES_DIR}/unified-sources/UnifiedSource-aot-${group}-${index}.cpp")
            file(CONFIGURE OUTPUT "${bundle}" CONTENT "${text}" @ONLY)
            list(APPEND sources "${bundle}")
            if (group MATCHES "-for-size")
                list(APPEND bundlesCompiledForSize "${bundle}")
            endif ()
        endwhile ()
    endforeach ()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/Sources.txt")
    list(REMOVE_ITEM JavaScriptCore_UNIFIED_SOURCE_LIST_FILES "aot/Sources.txt")
    set(JavaScriptCore_UNIFIED_SOURCE_LIST_FILES ${JavaScriptCore_UNIFIED_SOURCE_LIST_FILES} PARENT_SCOPE)
    set(JavaScriptCore_SOURCES ${JavaScriptCore_SOURCES} ${sources} PARENT_SCOPE)
    set(JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE ${JavaScriptCore_AOT_SOURCES_COMPILED_FOR_SIZE} ${bundlesCompiledForSize} PARENT_SCOPE)
endfunction()

function(AOT_CHECK_SCHEMA_IS_FRESH)
    set(digests "")
    foreach (input IN LISTS JavaScriptCore_AOT_SCHEMA_INPUTS)
        file(SHA256 "${JAVASCRIPTCORE_DIR}/${input}" digest)
        list(APPEND digests "${digest} ${input}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/${input}")
    endforeach ()
    list(SORT digests)
    list(JOIN digests "\n" joinedDigests)
    string(SHA256 inputsHash "${joinedDigests}\n")
    file(STRINGS "${JAVASCRIPTCORE_DIR}/aot/ir/generated/AOTSchemaInputs.sha256" recordedHash LIMIT_COUNT 1)
    if (NOT inputsHash STREQUAL recordedHash)
        message(FATAL_ERROR "aot/ir/generated/ is stale: its inputs hash to ${inputsHash}, and it was generated from ${recordedHash}. In Bun's tree, run `bun src/codegen/aot-schema/index.ts --schema ${JAVASCRIPTCORE_DIR}/aot/ir/AOTSchema.def --engine-out ${JAVASCRIPTCORE_DIR}/aot/ir/generated --rust-out src/aot/ir/generated` and check the result in.")
    endif ()
endfunction()

function(AOT_READ_LIBRARY_SOURCES result problemsResult)
    file(STRINGS "${JAVASCRIPTCORE_DIR}/aot/Sources.txt" lines)
    set(problems)
    set(sources)
    set(previous "")
    foreach (line IN LISTS lines)
        string(REGEX REPLACE "[ \t].*$" "" path "${line}")
        if (NOT line MATCHES "[ \t]@no-unify$")
            list(APPEND problems "${path}: no @no-unify in aot/Sources.txt")
        endif ()
        if (NOT "${previous}" STRLESS "${path}")
            list(APPEND problems "${path}: not in ascending order in aot/Sources.txt")
        endif ()
        list(APPEND sources ${path})
        set(previous ${path})
    endforeach ()
    set(${result} ${sources} PARENT_SCOPE)
    set(${problemsResult} ${problems} PARENT_SCOPE)
endfunction()

function(AOT_CHECK_SOURCE_LISTS)
    file(GLOB_RECURSE found RELATIVE "${JAVASCRIPTCORE_DIR}" CONFIGURE_DEPENDS "${JAVASCRIPTCORE_DIR}/aot/*.cpp")
    AOT_READ_LIBRARY_SOURCES(librarySources problems)

    set(listed
        ${librarySources}
        ${JavaScriptCore_AOT_SCHEMA_GENERATED_SOURCES}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_SOURCES}
        ${JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES}
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
        ${JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_HEADERS}
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
        if (NOT EXISTS "${JAVASCRIPTCORE_DIR}/${header}")
            list(APPEND problems "${header}: exported, but there is no such file")
            continue ()
        endif ()
        file(STRINGS "${JAVASCRIPTCORE_DIR}/${header}" includeLines REGEX "^[ \t]*#[ \t]*include[ \t]+[\"<](JavaScriptCore/)?AOT[^\">]*[\">]")
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

AOT_GENERATE_UNIFIED_SOURCES()
AOT_COMPILE_SOURCES_FOR_SIZE()
AOT_CHECK_SCHEMA_IS_FRESH()
AOT_CHECK_SOURCE_LISTS()
AOT_CHECK_EXPORTED_HEADERS()
