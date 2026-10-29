/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

struct NativeTest {
    ASCIILiteral name;
    void (*run)();
};

[[noreturn]] void failNativeTest(const char* file, int line, const char* condition);

// Under AddressSanitizer a frame keeps words that nothing uses any more, which a collection takes for references, and a local variable may lie outside the stack.
// A test of what a collection leaves dead, or of how far the stack reaches, says nothing there.
inline constexpr bool framesAreThoseOfAddressSanitizer = ASAN_ENABLED;

#define AOT_TEST_CHECK(condition) do { \
        if (!(condition)) [[unlikely]] \
            ::JSC::AOT::failNativeTest(__FILE__, __LINE__, #condition); \
    } while (false)

#define FOR_EACH_AOT_IR_NATIVE_TEST_PROVIDER(v) \
    v(idTests) \
    v(irTests) \
    v(irParserTests) \
    v(irFunctionTests) \
    v(stringTableTests) \
    v(flowResultsTests) \
    v(arrayTests) \
    v(concurrentComponentsTests) \
    v(effectSummariesTests) \
    v(heatGraphTests) \
    v(irProgramTests) \
    v(ledgerTests) \
    v(resultNodeDeclineTests) \
    v(resultNodeTests) \
    v(typeArenaTests) \
    v(valueFlowCoreTests)

#define FOR_EACH_AOT_IMAGE_NATIVE_TEST_PROVIDER(v) \
    v(siteTableTests) \
    v(regExpTableTests) \
    v(determinismTests) \
    v(symbolTests) \
    v(functionMergingTests) \
    v(stackUsageTests) \
    v(outliningTests) \
    v(codeLayoutTests) \
    v(emissionTests) \
    v(programInputTests) \
    v(codeLinkerTests) \
    v(faultTableTests) \
    v(outliningRewriteTests) \
    v(unitPlacementTests)

#define FOR_EACH_AOT_ENGINE_NATIVE_TEST_PROVIDER(v) \
    v(readerTests) \
    v(verifierTests) \
    v(irPrinterTests) \
    v(runtimeOperationTests) \
    v(supportTests) \
    v(builtinTableTests) \
    v(builtinValidationTests) \
    v(b3PolicyTests) \
    v(b3HeapTests) \
    v(backEndPhaseTests) \
    v(backEndReportTests) \
    v(stubTests) \
    v(stubAssemblerTests) \
    v(stubCatalogTests) \
    v(stubBlobTests) \
    v(codeUnitTests) \
    v(imageTableTests) \
    v(imageBuildTests) \
    v(stringArenaTests) \
    v(identifierNumberingTests) \
    v(stackCaptureLargeImageTests) \
    v(tableWriterTests) \
    v(regExpImageTests) \
    v(webAssemblyImageTests) \
    v(webAssemblyInstanceTests) \
    v(ffiStubTests) \
    v(cacheRegistryTests) \
    v(cacheRegistryRootTests) \
    v(heapCageTests) \
    v(heapRememberedSlotTests) \
    v(runtimeRecordTests) \
    v(runtimeTypeTests) \
    v(compiledStructureTests) \
    v(objectSlotExitTests) \
    v(fieldMoveTests) \
    v(memberGuardTests) \
    v(staticFieldTests) \
    v(dataBuilderTests) \
    v(programIdentifiersTests) \
    v(programStringsTests) \
    v(programStaticsTests) \
    v(builtinCellsTests) \
    v(runtimeCounterTests) \
    v(typeErrorMessageTests) \
    v(compiledFrameTests) \
    v(stackTraceTests) \
    v(unwindingTests) \
    v(compactStorageTests) \
    v(compactTableTests) \
    v(tableInImageTests) \
    v(propertyLookupTests) \
    v(propertyLookupTableTests) \
    v(propertyLookupTableCollectionTests) \
    v(storeAnswerTests) \
    v(lockedRealmTests) \
    v(engineHookTests) \
    v(pristineStructureTests) \
    v(slimClosureTests) \
    v(slotAccessTests) \
    v(structuredCloneTests) \
    v(moduleGraphTests) \
    v(moduleTableTests) \
    v(removableFeatureTests) \
    v(operationScopeTests) \
    v(runtimeReporterTests) \
    v(stringSearchTests) \
    v(jsonScannerTests) \
    v(jsonShapeParserTests) \
    v(jsonSourceTests) \
    v(jsonWriterTests) \
    v(driverTests) \
    v(optionWordTests) \
    v(testFileTests) \
    v(childProcessTests) \
    v(arrivalCountTests) \
    v(assignmentPlanTests) \
    v(boxedCallProgramTests) \
    v(builtinLocationWatchTests) \
    v(builtinResultObjectTests) \
    v(builtinSubclassTests) \
    v(cacheSweepTests) \
    v(callingConventionTests) \
    v(classMemberOrderTests) \
    v(classWithoutInstanceTestTests) \
    v(closedWorldTests) \
    v(collectorTests) \
    v(constructorFunctionTests) \
    v(coroutineJobTests) \
    v(deferredClosureProgramTests) \
    v(dictionaryAccessByNameProgramTests) \
    v(dictionaryExtraPropertiesTests) \
    v(extensionTests) \
    v(falseTypeProgramTests) \
    v(flowWorkLimitProgramTests) \
    v(forInExpansionProgramTests) \
    v(frameCellProgramTests) \
    v(globalBindingProgramTests) \
    v(globalProxyReadProgramTests) \
    v(hashedSlotTableTests) \
    v(imageFunctionTableTests) \
    v(imageTextTests) \
    v(inlinedSerializerTests) \
    v(innerPointerValidationTests) \
    v(instanceKeyOrderTests) \
    v(interpretedStoreUnderCompiledPrototypeTests) \
    v(jsonKeyOrderTests) \
    v(jsonParseOperationTests) \
    v(listedFieldsTests) \
    v(locationsReadAtRunTimeTests) \
    v(lockedRealmMemberTests) \
    v(lookupBehindTablesProgramTests) \
    v(membersNamedLikeBuiltinsTests) \
    v(moduleProgramTests) \
    v(moduleRequestListTests) \
    v(nakedConstructorTests) \
    v(narrowReferenceValidationTests) \
    v(nativeCallValidationTests) \
    v(nativeModuleExportTests) \
    v(objectCreateProgramTests) \
    v(parsedOptionalMemberTests) \
    v(privateAccessorProgramTests) \
    v(privateBrandProgramTests) \
    v(privateFieldDefinitionTests) \
    v(privateMemberKindProgramTests) \
    v(privateMethodProgramTests) \
    v(proofValidationTests) \
    v(propertyAttributeTests) \
    v(propertyRunProgramTests) \
    v(prototypeSetByKeyTests) \
    v(publishedEnumeratorProgramTests) \
    v(publishedEnumeratorTests) \
    v(readerStreamTests) \
    v(regExpCompilerTests) \
    v(replacedBuiltinTests) \
    v(requiredNameTests) \
    v(savedFrameTests) \
    v(sharingModeAndNamespaceTests) \
    v(siteCacheFillTests) \
    v(sparseElementProgramTests) \
    v(stackCheckTests) \
    v(startupTests) \
    v(staticMembersNamedLikeBuiltinsTests) \
    v(storageOwnerProgramTests) \
    v(stringBuilderStorageTests) \
    v(stubCallTests) \
    v(stubCollectionTests) \
    v(stubPropertyReadTests) \
    v(stubPropertyStoreTests) \
    v(stubRegExpTests) \
    v(stubRuntimeTests) \
    v(stubScratchRegisterTests) \
    v(stubStringTests) \
    v(stubValueTests) \
    v(stubVectorGrowthTests) \
    v(tailCallProgramTests) \
    v(thisAllocationProgramTests) \
    v(thisAllocationTests) \
    v(typedResultProgramTests) \
    v(waiterOrderProgramTests)

#define AOT_DECLARE_NATIVE_TEST_PROVIDER(name) std::span<const NativeTest> name();
namespace Typed {
FOR_EACH_AOT_IR_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
} // namespace Typed
namespace ImageTests {
FOR_EACH_AOT_IMAGE_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
} // namespace ImageTests
FOR_EACH_AOT_ENGINE_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
#undef AOT_DECLARE_NATIVE_TEST_PROVIDER

class ChildProcessLauncher;

// Of the tests that the filter lets through, in their order: the one with the index, and every count-th behind it.
struct NativeTestPart {
    bool contains(unsigned ordinal) const { return ordinal % count == index; }

    unsigned index { 0 };
    unsigned count { 1 };
};

struct NativeTestRequest {
    StringView filter;
    StringView previousTest;
    const ChildProcessLauncher* launcher { nullptr };
    NativeTestPart part;
};

struct NativeTestSummary {
    unsigned passed { 0 };
    unsigned failed { 0 };
    unsigned leftOut { 0 };
};

NativeTestSummary runNativeTests(const NativeTestRequest&, PrintStream&);

inline constexpr ASCIILiteral nativeSubcommandName = "native"_s;
inline constexpr ASCIILiteral previousNativeTestFlag = "--after"_s;
inline constexpr ASCIILiteral singleProcessFlag = "--single-process"_s;
inline constexpr ASCIILiteral nativeTestPartFlag = "--part"_s;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
