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
    ASCIILiteral todo { };
};

[[noreturn]] void failNativeTest(const char* file, int line, const char* condition);

inline constexpr bool framesAreThoseOfAddressSanitizer = ASAN_ENABLED;

#define AOT_TEST_CHECK(condition) do { \
        if (!(condition)) [[unlikely]] \
            ::JSC::AOT::failNativeTest(__FILE__, __LINE__, #condition); \
    } while (false)

#define FOR_EACH_AOT_IR_NATIVE_TEST_PROVIDER(v) \
    v(idTests) \
    v(stringTableTests) \
    v(arrayTests) \
    v(idSetTests) \
    v(sharedArrayTests) \
    v(bodyTests) \
    v(readerTests)

#define FOR_EACH_AOT_IMAGE_NATIVE_TEST_PROVIDER(v) \
    v(siteTableTests) \
    v(regExpTableTests) \
    v(determinismTests) \
    v(symbolTests) \
    v(codeLayoutTests) \
    v(programInputTests) \
    v(codeLinkerTests) \
    v(sizesOfImageTests) \
    v(faultTableTests)

#define FOR_EACH_AOT_ENGINE_NATIVE_TEST_PROVIDER(v) \
    v(runtimeOperationTests) \
    v(supportTests) \
    v(builtinTableTests) \
    v(builtinValidationTests) \
    v(stubTests) \
    v(stubAssemblerTests) \
    v(stubBlobTests) \
    v(imageTableTests) \
    v(imageBuildTests) \
    v(identifierNumberingTests) \
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
    v(dataBuilderTests) \
    v(programIdentifiersTests) \
    v(programStringsTests) \
    v(programStaticsTests) \
    v(builtinCellsTests) \
    v(compiledFrameTests) \
    v(stackTraceTests) \
    v(unwindingTests) \
    v(compactStorageTests) \
    v(compactTableTests) \
    v(propertyLookupTests) \
    v(propertyLookupTableTests) \
    v(propertyLookupTableCollectionTests) \
    v(storeAnswerTests) \
    v(lockedRealmTests) \
    v(engineHookTests) \
    v(pristineStructureTests) \
    v(nativeStoreTests) \
    v(slimClosureTests) \
    v(slotAccessTests) \
    v(structuredCloneTests) \
    v(moduleTableTests) \
    v(removableFeatureTests) \
    v(stringSearchTests) \
    v(stringBuilderStorageTests) \
    v(driverTests) \
    v(childProcessTests) \
    v(begunImporterTests) \
    v(boxedCallProgramTests) \
    v(builtinResultObjectTests) \
    v(builtinSubclassTests) \
    v(cacheSweepTests) \
    v(classMemberOrderTests) \
    v(classWithoutInstanceTestTests) \
    v(closedWorldTests) \
    v(collectorTests) \
    v(constructorFunctionTests) \
    v(coroutineJobTests) \
    v(deferredClosureProgramTests) \
    v(dictionaryAccessByNameProgramTests) \
    v(dictionaryExtraPropertiesTests) \
    v(exportsThatStayInCompiledCodeTests) \
    v(extensionTests) \
    v(falseTypeProgramTests) \
    v(forInExpansionProgramTests) \
    v(frameCellProgramTests) \
    v(globalBindingProgramTests) \
    v(globalProxyReadProgramTests) \
    v(hashedSlotTableTests) \
    v(imageFunctionTableTests) \
    v(imageTextTests) \
    v(instanceKeyOrderTests) \
    v(interpretedStoreUnderCompiledPrototypeTests) \
    v(storesOfStubsProgramTests) \
    v(interpreterSiteCacheTests) \
    v(locationsReadAtRunTimeTests) \
    v(lockedRealmMemberTests) \
    v(membersLeftOutTests) \
    v(lookupBehindTablesProgramTests) \
    v(membersNamedLikeBuiltinsTests) \
    v(moduleProgramTests) \
    v(moduleRequestListTests) \
    v(nakedConstructorTests) \
    v(objectCreateProgramTests) \
    v(originalBesideMockTests) \
    v(privateAccessorProgramTests) \
    v(privateBrandProgramTests) \
    v(privateFieldDefinitionTests) \
    v(privateMemberKindProgramTests) \
    v(privateMethodProgramTests) \
    v(propertyAttributeTests) \
    v(propertyRunProgramTests) \
    v(prototypeSetByKeyTests) \
    v(publishedEnumeratorProgramTests) \
    v(publishedEnumeratorTests) \
    v(regExpCompilerTests) \
    v(replacedBuiltinTests) \
    v(requiredNameTests) \
    v(savedFrameTests) \
    v(sharingModeAndNamespaceTests) \
    v(sparseElementProgramTests) \
    v(startupTests) \
    v(staticMembersNamedLikeBuiltinsTests) \
    v(storageOwnerProgramTests) \
    v(stubBoxedCallsOfLanguageTests) \
    v(stubCallTests) \
    v(stubCollectionTests) \
    v(stubPropertyReadTests) \
    v(stubPropertyStoreTests) \
    v(stubRegExpTests) \
    v(stubRuntimeTests) \
    v(stubStringTests) \
    v(stubValueTests) \
    v(stubVectorGrowthTests) \
    v(tailCallProgramTests) \
    v(testRunOriginalsTests) \
    v(thisAllocationProgramTests) \
    v(thisAllocationTests) \
    v(typedResultProgramTests) \
    v(unitQueueTests) \
    v(growthOfWorkTests) \
    v(stackTestPlanTests) \
    v(guessesGrowthTests) \
    v(linkGrowthTests) \
    v(siteAnswersOfGettersTests) \
    v(unionConsecutiveSetsTests) \
    v(stringStraysTests) \
    v(knownJoinsTests) \
    v(knownThreadingTests) \
    v(branchesGrowthTests) \
    v(knownAnswersTests) \
    v(walkStateTests) \
    v(listOfUnitTests) \
    v(waiterOrderProgramTests) \
    v(machineFoldingTests) \
    v(machineNumberingTests) \
    v(machineOrderTests) \
    v(machineTestsOfPhisTests) \
    v(machineAllocationTests) \
    v(machineCallsOnWaysTests) \
    v(machinePairsOfLoadsTests) \
    v(machinePatchTests) \
    v(machineFrameTests) \
    v(machineFrameFormTests) \
    v(machineOperationTests) \
    v(machineRandomTests) \
    v(machineExecutionFuzzerTests) \
    v(machineMemoryTests) \
    v(machineCheckerTests) \
    v(outputTests)

#define AOT_DECLARE_NATIVE_TEST_PROVIDER(name) std::span<const NativeTest> name();
namespace Typed {
FOR_EACH_AOT_IR_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
} // namespace Typed
namespace ImageTests {
FOR_EACH_AOT_IMAGE_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
} // namespace ImageTests
FOR_EACH_AOT_ENGINE_NATIVE_TEST_PROVIDER(AOT_DECLARE_NATIVE_TEST_PROVIDER)
#undef AOT_DECLARE_NATIVE_TEST_PROVIDER

using NativeTestProvider = std::span<const NativeTest> (*)();

std::span<const NativeTestProvider> nativeTestProvidersOfHost();

class ChildProcessLauncher;

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
    std::span<const ASCIILiteral> testsTakenAsTodo { };
};

struct NativeTestSummary {
    unsigned passed { 0 };
    unsigned failed { 0 };
    unsigned todo { 0 };
};

NativeTestSummary runNativeTests(const NativeTestRequest&, PrintStream&);

inline constexpr ASCIILiteral nativeSubcommandName = "native"_s;
inline constexpr ASCIILiteral previousNativeTestFlag = "--after"_s;
inline constexpr ASCIILiteral singleProcessFlag = "--single-process"_s;
inline constexpr ASCIILiteral nativeTestPartFlag = "--part"_s;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
