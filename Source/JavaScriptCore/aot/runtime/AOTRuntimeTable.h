/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRemovableFeature.h"
#include "AOTSchemaRuntimeOperations.h"
#include "CallLinkInfo.h"
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;
class Structure;

namespace AOT {

enum class LeafAnswer : size_t { TakeSlowPath, No, Yes };
enum class LeafPresence : size_t { TakeSlowPath, Absent, Inherited, Own };

// What the code generator told the back end, and a run has found untrue. --aot-check=ClaimsToBackEnd, --aot-check=MovedLoads
enum class BrokenClaim : uint32_t {
    StackSlotIsWrittenBeforeRead,
    WordNeverChanges,
    LoadMayMove,
    ValueIsInRange,
    ComparisonIsDecided,
    CounterStepsAsTold,
};
// Beside the claim in its byte: the report prints a place once and returns.
static constexpr uint32_t reportOfBrokenClaimComesBack = 0x80;

// What a build under Sabotage::MemoryRunsOutWhenCounted counts.
enum class CountedLackOfMemory : uint32_t {
    Operation,
    Flattening,
};

#if ASAN_ENABLED && CPU(X86_64) && OS(LINUX)
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 1
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    v(operationAOTReportPoisonedAccess)
#else
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 0
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v)
#endif

// No compiled code calls the reserved ones. The entries behind them keep their numbers.
#define FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(v, reserved) \
    v(operationAOTPrepareProxyCall) \
    FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    v(operationAOTCallEngineCallee) \
    v(operationAOTReportBrokenClaim) \
    reserved(operationArrayIndexOfValueInt32OrContiguous) \
    reserved(operationArrayIncludesValueInt32OrContiguous) \
    v(operationAOTLookupExceptionHandler) \
    v(operationAOTFindLandingOfMisfit) \
    v(operationAOTBytesAllocatedAtPrologueCheck) \
    v(operationAOTCopyDataPropertiesByFieldMap)

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException)

#define FOR_EACH_AOT_POINTER(v) \
    v(CallLinkInfoForCall) \
    v(VirtualCallTrampoline) \
    v(ErrorConstructorClassInfo) \
    v(NonConstructorHostFunction) \
    v(AsyncGeneratorClassInfo) \
    v(GeneratorClassInfo) \
    v(WebAssemblyFunctionClassInfo) \
    v(CallNativeFunctionUnderValidation) \
    v(ConstructNativeFunctionUnderValidation) \
    v(CheckThatThrowingCallHasSite)

enum class Entry : uint16_t {
#define AOT_DEFINE_SCHEMA_ENTRY(name) operationAOT##name,
#define AOT_DEFINE_ENTRY(name) name,
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_DEFINE_SCHEMA_ENTRY)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_DEFINE_ENTRY, AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_DEFINE_ENTRY)
#undef AOT_DEFINE_ENTRY
#undef AOT_DEFINE_SCHEMA_ENTRY
#define AOT_STUB_FRONT_END(Operation, Family) Raw##Operation,
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
    NumberOfEntries
};

constexpr unsigned numberOfEntries = static_cast<unsigned>(Entry::NumberOfEntries);
constexpr unsigned numberOfRuntimeOperations = numberOfEnumerators<RuntimeOperation>;
static_assert(numberOfEntries <= 4096, "An ARM64 load from the table reaches 32 KB.");

constexpr uint32_t offsetInRuntimeTable(Entry entry)
{
    return static_cast<uint32_t>(entry) * sizeof(void*);
}

constexpr Entry entryAtOffset(uint32_t offset)
{
    uint32_t index = offset / sizeof(void*);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(index < numberOfEntries);
    Entry entry = static_cast<Entry>(index);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(offsetInRuntimeTable(entry) == offset);
    return entry;
}

constexpr Entry entryOf(RuntimeOperation operation)
{
    return static_cast<Entry>(operation);
}

constexpr std::optional<RuntimeOperation> runtimeOperationOf(Entry entry)
{
    if (static_cast<unsigned>(entry) >= numberOfRuntimeOperations)
        return std::nullopt;
    return static_cast<RuntimeOperation>(entry);
}

constexpr bool isOperation(Entry entry)
{
    if (runtimeOperationOf(entry))
        return true;
    switch (entry) {
#define AOT_CASE_OF_OPERATION(name) case Entry::name:
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_CASE_OF_OPERATION, AOT_CASE_OF_OPERATION)
#undef AOT_CASE_OF_OPERATION
        return true;
    default:
        return false;
    }
}

constexpr bool isRawOperation(Entry entry)
{
    switch (entry) {
#define AOT_STUB_FRONT_END(Operation, Family) case Entry::Raw##Operation:
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
        return true;
    default:
        return false;
    }
}

constexpr ArgumentBufferAccess argumentBufferAccessOf(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(entry))
        return argumentBufferAccessOf(*operation);
    return ArgumentBufferAccess::ReadsAndWrites;
}

// The table holds the address of every operation, so each would stay in an executable with all that it reaches. An image lists the entries that it uses, and whoever builds an
// executable keeps the feature of each of them: an operation with a feature leaves an executable whose image lists none of that feature. These features are for nothing else.
// What hello world lists has none: the library calls it as it starts. An operation that is not named here has none either, which costs its bytes and nothing else.
#define FOR_EACH_AOT_OPERATION_IN_FAMILY(v) \
    v(OperationsOfChecks, CompareOperandWithPhotograph) \
    v(OperationsOfChecks, ContentsHaveType) \
    v(OperationsOfChecks, CountExecution) \
    v(OperationsOfChecks, CountStopsForCollector) \
    v(OperationsOfChecks, ExpectRows) \
    v(OperationsOfChecks, LeaveFrameOfCheckedCells) \
    v(OperationsOfChecks, MakeCheckedFrameCell) \
    v(OperationsOfChecks, NoteFailedGuard) \
    v(OperationsOfChecks, PhotographOperand) \
    v(OperationsOfChecks, RecordArrival) \
    v(OperationsOfChecks, ReportEscapedFrameCell) \
    v(OperationsOfChecks, ReportMismatch) \
    v(OperationsOfChecks, RunOutOfMemoryWhenCounted) \
    v(OperationsOfChecks, ValidateGlobalBinding) \
    v(OperationsOfChecks, ValidateLookup) \
    v(OperationsOfChecks, ValueHasType) \
    v(OperationsOnProxies, FinishHasThroughProxy) \
    v(OperationsOnProxies, FinishSetThroughProxy) \
    v(OperationsOnProxies, FinishStoreByTrap) \
    v(OperationsOnProxies, GetWithProxyTrap) \
    v(OperationsOnProxies, HasOwnByAnswerOfDescriptorTrap) \
    v(OperationsOnProxies, HasWithProxyTrap) \
    v(OperationsOnProxies, SetWithProxyTrap) \
    v(OperationsOnProxies, ValidateAnswerOfGetTrap) \
    v(OperationsOnProxies, ValidateAnswerOfProxyTrap) \
    v(OperationsOnProxies, ValidateConsentOfDeleteTrap) \
    v(OperationsOnProxies, ValidateDenialOfHasTrap) \
    v(OperationsOfJSONParse, JSONDecodeBytes) \
    v(OperationsOfJSONParse, JSONFindMember) \
    v(OperationsOfJSONParse, JSONMemberRepeats) \
    v(OperationsOfJSONParse, JSONObjectFinish) \
    v(OperationsOfJSONParse, JSONObjectKey) \
    v(OperationsOfJSONParse, JSONParseExtraMembers) \
    v(OperationsOfJSONParse, JSONParseInteger) \
    v(OperationsOfJSONParse, JSONParseKey) \
    v(OperationsOfJSONParse, JSONParseNumber) \
    v(OperationsOfJSONParse, JSONParseString) \
    v(OperationsOfJSONParse, JSONParseValue) \
    v(OperationsOfJSONParse, JSONSourceAbandon) \
    v(OperationsOfJSONParse, JSONSourceClose) \
    v(OperationsOfJSONParse, JSONSourceOpen) \
    v(OperationsOfJSONStringify, JSONAppendString) \
    v(OperationsOfJSONStringify, JSONAppendValue) \
    v(OperationsOfJSONStringify, JSONEnterWatchedLevel) \
    v(OperationsOfJSONStringify, KeyNamesInOrder) \
    v(OperationsOfJSONStringify, KeyOrderEntries) \
    v(OperationsOfJSONStringify, StringBuilderAppendJSON) \
    v(OperationsOfJSONStringify, StringBuilderAppendJSONMembers) \
    v(OperationsOnStringBuilders, Concatenate) \
    v(OperationsOnStringBuilders, ConcatenatePair) \
    v(OperationsOnStringBuilders, StackStringBuilderAppend) \
    v(OperationsOnStringBuilders, StackStringBuilderAppendStringRange) \
    v(OperationsOnStringBuilders, StackStringBuilderDiscard) \
    v(OperationsOnStringBuilders, StackStringBuilderFinish) \
    v(OperationsOnStringBuilders, StackStringBuilderGrow) \
    v(OperationsOnStringBuilders, StringBuilderAppend) \
    v(OperationsOnStringBuilders, StringBuilderAppendString) \
    v(OperationsOnStringBuilders, StringBuilderAppendStringRange) \
    v(OperationsOnStringBuilders, StringBuilderBegin) \
    v(OperationsOnStringBuilders, StringBuilderCheckLength) \
    v(OperationsOnStringBuilders, StringBuilderDiscard) \
    v(OperationsOnStringBuilders, StringBuilderFinish) \
    v(OperationsOnStringBuilders, StringBuilderReserve) \
    v(OperationsOnStringBuilders, StringBuilderRollBack) \
    v(OperationsOnStringBuilders, StringBuilderToString) \
    v(OperationsOnTypedArrays, ByteLengthOfAnyValue) \
    v(OperationsOnTypedArrays, NewObjectOfBuiltinLayout) \
    v(OperationsOnTypedArrays, NewTypedArray) \
    v(OperationsOnTypedArrays, TypedArrayPut) \
    v(OperationsOnTypedArrays, TypedArrayRange) \
    v(OperationsOnTypedArrays, TypedArraySetOfAnyValue) \
    v(OperationsOnTypedArrays, TypedArraySubarrayOfAnyValue) \
    v(OperationsOnTypedArrays, ViewByteLength) \
    v(OperationsOnEncodedText, EncodeTextIntoBytes) \
    v(OperationsOnEncodedText, EncodeTextToBytes) \
    v(OperationsOnEncodedText, TextOfBytes) \
    v(OperationsOnPrivateNames, BeginMemberGuards) \
    v(OperationsOnPrivateNames, NewClassToken) \
    v(OperationsOnPrivateNames, PrivateDefine) \
    v(OperationsOnPrivateNames, PrivateGet) \
    v(OperationsOnPrivateNames, PrivateHas) \
    v(OperationsOnPrivateNames, PrivateSet) \
    v(OperationsOnSuper, GetSuperBase) \
    v(OperationsOnSuper, GetSuperPropertyById) \
    v(OperationsOnSuper, SuperConstruct) \
    v(OperationsOnSuper, SuperConstructList) \
    v(OperationsOnSuper, SuperGet) \
    v(OperationsOnSuper, SuperSet) \
    v(OperationsOnSuper, ThrowBecauseParentMadeNoObject) \
    v(OperationsOnClasses, DefineMethod) \
    v(OperationsOnClasses, GetClassPrototype) \
    v(OperationsOnClasses, GetConstructPrototype) \
    v(OperationsOnClasses, HasOnlyClassFieldNames) \
    v(OperationsOnClasses, NewClass) \
    v(OperationsOnClasses, ResolveHeritage) \
    v(OperationsOnClasses, SetConstructPrototype) \
    v(OperationsOfDynamicImport, AreLoaderFunctionsStored) \
    v(OperationsOfDynamicImport, AwaitWithSavedFrameInModuleBody) \
    v(OperationsOfDynamicImport, ImportDynamic) \
    v(OperationsOfDynamicImport, ImportModule) \
    v(OperationsOfDynamicImport, IsNamespaceOfModule) \
    v(OperationsOfDynamicImport, RequireDynamic) \
    v(OperationsOfDynamicImport, RequireIfHeldForImport) \
    v(OperationsOfDynamicImport, ResolveDynamic) \
    v(OperationsOfDynamicImport, ResolveModule) \
    v(OperationsOfDynamicImport, SaveFrameAndAwaitInModuleBody) \
    v(OperationsOfDynamicImport, WasLoadedSynchronously) \
    v(OperationsOnReExports, TryDefineLiveBinding) \
    v(OperationsOnReExports, TryReExportFields) \
    v(OperationsOnModulesThatFail, AbandonModuleEvaluation) \
    v(OperationsOnModulesThatFail, FillVariablesOfFiles) \
    v(OperationsOnModulesThatFail, RecordRequire) \
    v(OperationsOnModulesThatFail, SeesMockOf) \
    v(OperationsOnModulesThatFail, StopLoadingBehindFile) \
    v(OperationsOnModulesThatFail, ThrowModuleFailure) \
    v(OperationsOnGlobalBindings, DeleteFreeName) \
    v(OperationsOnGlobalBindings, GetGlobalBinding) \
    v(OperationsOnGlobalBindings, GetGlobalObjectBinding) \
    v(OperationsOnGlobalBindings, ScopeHasBinding) \
    v(OperationsOnGlobalBindings, SetGlobalBinding) \
    v(OperationsOnGlobalBindings, SetGlobalBindingSloppy) \
    v(OperationsOnGlobalBindings, SetGlobalObjectBinding) \
    v(OperationsOnGlobalBindings, SetGlobalObjectBindingSloppy) \
    v(OperationsOnArgumentsObjects, CreateMappedArguments) \
    v(OperationsOnArgumentsObjects, CreateUnmappedArguments) \
    v(OperationsOnArgumentsObjects, GetFromArguments) \
    v(OperationsOnArgumentsObjects, GetMappedArgument) \
    v(OperationsOnArgumentsObjects, SetMappedArgument) \
    v(OperationsOnAsyncIterators, AsyncIteratorStep) \
    v(OperationsOnAsyncIterators, CreateAsyncFromSyncIterator) \
    v(OperationsOnAsyncIterators, FinishAsyncIteratorStep) \
    v(OperationsOnGenerators, IsGeneratorSuspendedAtStart) \
    v(OperationsOnGenerators, NewGenerator) \
    v(OperationsOnGenerators, ResumeGenerator) \
    v(OperationsOnGenerators, SaveFrameAndYield) \
    v(OperationsOnGenerators, TakeGeneratorPrototype) \
    v(OperationsOfAwait, AwaitCallWithSavedFrame) \
    v(OperationsOfAwait, AwaitWithSavedFrame) \
    v(OperationsOfAwait, AwaitWithSavedFrameAfterTurn) \
    v(OperationsOfAwait, NewHeapFrame) \
    v(OperationsOfAwait, SaveFrameAndAwait) \
    v(OperationsOfAwait, SaveFrameAndAwaitAfterTurn) \
    v(OperationsOfAwait, SaveFrameAndAwaitCall) \
    v(OperationsOnPromises, CreatePromise) \
    v(OperationsOnPromises, NewPromise) \
    v(OperationsOnPromises, PromiseAll) \
    v(OperationsOnPromises, PromiseCatch) \
    v(OperationsOnPromises, PromiseThen) \
    v(OperationsOnPromises, RejectPromise) \
    v(OperationsOnPromises, ResolvePromise) \
    v(OperationsOnPromises, ResolvePromiseOrReturnResult) \
    v(OperationsOnKeysOfParts, HasByKeyParts) \
    v(OperationsOnKeysOfParts, MapFind) \
    v(OperationsOnKeysOfParts, MapGetByKeyParts) \
    v(OperationsOnMapsAndSets, MapClear) \
    v(OperationsOnMapsAndSets, MapNext) \
    v(OperationsOnMapsAndSets, MoveTableToHeap) \
    v(OperationsOnMapsAndSets, NewMap) \
    v(OperationsOnMapsAndSets, NewMapIterator) \
    v(OperationsOnMapsAndSets, NewSet) \
    v(OperationsOnMapsAndSets, NewSetIterator) \
    v(OperationsOnMapsAndSets, ReserveCollectionCapacity) \
    v(OperationsOnMapsAndSets, SetClear) \
    v(OperationsOnMapsAndSets, SetHas) \
    v(OperationsOnMapsAndSets, SetNext) \
    v(OperationsOnMapsAndSets, TryNewMapFromEntries) \
    v(OperationsOnMapsAndSets, TryNewSetFromIterable) \
    v(OperationsThatCopyObjects, AssignToNewObject) \
    v(OperationsThatCopyObjects, CopyDataProperties) \
    v(OperationsThatCopyObjects, CopyDataPropertiesByFieldMap) \
    v(OperationsThatCopyObjects, CopyObject) \
    v(OperationsThatCopyObjects, CopyObjectAndDefine) \
    v(OperationsThatCopyObjects, ObjectAssignToUnseenObject) \
    v(OperationsThatCopyObjects, ObjectAssignWithoutCode) \
    v(OperationsThatCopyObjects, TryCopyTwoObjects) \
    v(OperationsThatListProperties, EnumeratorGetByVal) \
    v(OperationsThatListProperties, EnumeratorNext) \
    v(OperationsThatListProperties, GetPropertyEnumerator) \
    v(OperationsThatListProperties, HasExtraProperty) \
    v(OperationsThatListProperties, ObjectEntries) \
    v(OperationsThatListProperties, ObjectValues) \
    v(OperationsThatListProperties, OwnPropertyKeys) \
    v(OperationsThatListProperties, PropertyIsEnumerable) \
    v(OperationsThatDescribeProperties, CallCustomGetter) \
    v(OperationsThatDescribeProperties, DefineAccessor) \
    v(OperationsThatDescribeProperties, DefineAccessors) \
    v(OperationsThatDescribeProperties, DefineOwnProperty) \
    v(OperationsThatDescribeProperties, FindAccessorOfMemberAndFillSiteCache) \
    v(OperationsThatDescribeProperties, FindGetterAndFillSiteCache) \
    v(OperationsThatDescribeProperties, GetOwnProperty) \
    v(OperationsThatDescribeProperties, GetOwnPropertyDescriptor) \
    v(OperationsThatDescribeProperties, GetOwnPropertyFields) \
    v(OperationsThatDescribeProperties, NewGetterSetter) \
    v(OperationsThatDescribeProperties, ObjectCreate) \
    v(OperationsThatDescribeProperties, SetLiteralPrototype) \
    v(OperationsThatDeleteProperties, DeleteById) \
    v(OperationsThatDeleteProperties, DeleteByIdSloppy) \
    v(OperationsThatDeleteProperties, DeleteByName) \
    v(OperationsThatDeleteProperties, DeleteByNameSloppy) \
    v(OperationsThatDeleteProperties, DeleteByVal) \
    v(OperationsThatDeleteProperties, DeleteByValSloppy) \
    v(OperationsThatChangeArrays, ArrayCopy) \
    v(OperationsThatChangeArrays, ArrayGrow) \
    v(OperationsThatChangeArrays, ArrayJoin) \
    v(OperationsThatChangeArrays, ArrayJoinOfAnyReceiver) \
    v(OperationsThatChangeArrays, ArrayOfLengthOrElement) \
    v(OperationsThatChangeArrays, ArraySpeciesCreate) \
    v(OperationsThatChangeArrays, ArraySplice) \
    v(OperationsThatChangeArrays, ArrayUnshift) \
    v(OperationsThatSearchArrays, ArrayIndexOfDouble) \
    v(OperationsThatSearchArrays, ArrayIndexOfInt32) \
    v(OperationsThatSearchArrays, ArrayIndexOfString) \
    v(OperationsThatSearchArrays, ArraySearchOfAnyValue) \
    v(OperationsThatMakeArrays, AllocateStorage) \
    v(OperationsThatMakeArrays, AppendElements) \
    v(OperationsThatMakeArrays, AppendHole) \
    v(OperationsThatMakeArrays, AppendMembers) \
    v(OperationsThatMakeArrays, ArrayPushNumber) \
    v(OperationsThatMakeArrays, ArrayPutAtIndex) \
    v(OperationsThatMakeArrays, ArrayPutAtNumber) \
    v(OperationsThatMakeArrays, ArrayPutNumberAtIndex) \
    v(OperationsThatMakeArrays, ConvertToDoubleStorage) \
    v(OperationsThatMakeArrays, DestructureIterable) \
    v(OperationsThatMakeArrays, GrowNewArray) \
    v(OperationsThatMakeArrays, NewArgumentList) \
    v(OperationsThatMakeArrays, NewArrayAtReviewedSite) \
    v(OperationsThatMakeArrays, NewArrayFromDoubles) \
    v(OperationsThatMakeArrays, NewArrayIterator) \
    v(OperationsThatMakeArrays, NewArrayOfElements) \
    v(OperationsThatMakeArrays, NewArrayOfHoles) \
    v(OperationsThatMakeArrays, NewArrayOfMembers) \
    v(OperationsThatMakeArrays, NewArrayWithHoles) \
    v(OperationsThatMakeArrays, NewArrayWithIndexingType) \
    v(OperationsThatMakeArrays, ReadsIteratorOfListUnseen) \
    v(OperationsThatMakeArrays, ReserveArrayCapacity) \
    v(OperationsThatMakeArrays, ReserveWholeArrayCapacity) \
    v(OperationsThatMakeArrays, ThrowIfTooLongForList) \
    v(OperationsThatMakeArrays, ThrowNotIterable) \
    v(OperationsThatMakeArrays, TruncateNewArray) \
    v(OperationsThatSearchStrings, CountLongSeparators16) \
    v(OperationsThatSearchStrings, CountLongSeparators8) \
    v(OperationsThatSearchStrings, FindLongSeparators16) \
    v(OperationsThatSearchStrings, FindLongSeparators8) \
    v(OperationsThatSearchStrings, StringEndsWithLiteral) \
    v(OperationsThatSearchStrings, StringFindLiteral8) \
    v(OperationsThatSearchStrings, StringIndexOfCharacter) \
    v(OperationsThatSearchStrings, StringIndexOfLiteral) \
    v(OperationsThatSearchStrings, StringLastIndexOf) \
    v(OperationsThatSearchStrings, StringLastIndexOfLiteral) \
    v(OperationsThatSearchStrings, StringReverseFindLiteral8) \
    v(OperationsThatSearchStrings, StringSplitPrefix) \
    v(OperationsThatSearchStrings, StringStartsWithLiteral) \
    v(OperationsThatMakeStrings, AppendNumberWithRadix) \
    v(OperationsThatMakeStrings, CharToString) \
    v(OperationsThatMakeStrings, CodeURIOfAnyValue) \
    v(OperationsThatMakeStrings, CompareEqToString) \
    v(OperationsThatMakeStrings, Concat) \
    v(OperationsThatMakeStrings, ConcatThree) \
    v(OperationsThatMakeStrings, DoubleToString) \
    v(OperationsThatMakeStrings, Flatten) \
    v(OperationsThatMakeStrings, Int32ToStringWithRadix) \
    v(OperationsThatMakeStrings, IntToString) \
    v(OperationsThatMakeStrings, LiteralStringToString) \
    v(OperationsThatMakeStrings, NumberToStringWithRadix) \
    v(OperationsThatMakeStrings, ObjectToStringOfAnyValue) \
    v(OperationsThatMakeStrings, ShortStringOfString) \
    v(OperationsThatMakeStrings, StringOfAnyValue) \
    v(OperationsThatMakeStrings, StringPad) \
    v(OperationsThatMakeStrings, StringRepeat) \
    v(OperationsThatMakeStrings, ToLiteralString) \
    v(OperationsThatMakeStrings, ToString) \
    v(OperationsThatCompareStrings, CompareStrings) \
    v(OperationsThatCompareStrings, StringEqualsLiteral) \
    v(OperationsThatCompareStrings, StringEqualsLiteralInRegister) \
    v(OperationsThatCompareStrings, StringsAreEqual) \
    v(OperationsOfArithmetic, AddNumberAndValueAsNumber) \
    v(OperationsOfArithmetic, BigIntConstant) \
    v(OperationsOfArithmetic, Decrement) \
    v(OperationsOfArithmetic, DivNumberAndValueAsNumber) \
    v(OperationsOfArithmetic, Increment) \
    v(OperationsOfArithmetic, MulNumberAndValueAsNumber) \
    v(OperationsOfArithmetic, NewBigInt64) \
    v(OperationsOfArithmetic, NumberOfAnyValue) \
    v(OperationsOfArithmetic, SubNumberAndValueAsNumber) \
    v(OperationsOfArithmetic, ToNumber) \
    v(OperationsOfArithmetic, ToNumeric) \
    v(OperationsOfArithmetic, ValueAdd) \
    v(OperationsOfArithmetic, ValueBitAnd) \
    v(OperationsOfArithmetic, ValueBitNot) \
    v(OperationsOfArithmetic, ValueBitOr) \
    v(OperationsOfArithmetic, ValueBitXor) \
    v(OperationsOfArithmetic, ValueDiv) \
    v(OperationsOfArithmetic, ValueLShift) \
    v(OperationsOfArithmetic, ValueMod) \
    v(OperationsOfArithmetic, ValueMul) \
    v(OperationsOfArithmetic, ValueNegate) \
    v(OperationsOfArithmetic, ValuePow) \
    v(OperationsOfArithmetic, ValueRShift) \
    v(OperationsOfArithmetic, ValueSub) \
    v(OperationsOfArithmetic, ValueURShift) \
    v(OperationsOfComparison, CompareEq) \
    v(OperationsOfComparison, CompareGreater) \
    v(OperationsOfComparison, CompareGreaterEq) \
    v(OperationsOfComparison, CompareLess) \
    v(OperationsOfComparison, CompareLessEq) \
    v(OperationsOfComparison, CompareStrictEq) \
    v(OperationsOfConversion, IsCallable) \
    v(OperationsOfConversion, IsConstructor) \
    v(OperationsOfConversion, ToBoolean) \
    v(OperationsOfConversion, ToKey) \
    v(OperationsOfConversion, ToLength) \
    v(OperationsOfConversion, ToObject) \
    v(OperationsOfConversion, ToThis) \
    v(OperationsOfConversion, TypeOf) \
    v(OperationsOfMembershipTests, DefaultHasInstance) \
    v(OperationsOfMembershipTests, HasOwnByIdAndCache) \
    v(OperationsOfMembershipTests, HasOwnByValAndCache) \
    v(OperationsOfMembershipTests, InByIdAndCache) \
    v(OperationsOfMembershipTests, InByValAndCache) \
    v(OperationsOfMembershipTests, InByValGeneric) \
    v(OperationsOfMembershipTests, InstanceOfAndCache) \
    v(OperationsOfCalls, Apply) \
    v(OperationsOfCalls, CallEngineCallee) \
    v(OperationsOfCalls, Construct) \
    v(OperationsOfCalls, ConstructWithArray) \
    v(OperationsOfCalls, FindLandingOfMisfit) \
    v(OperationsOfCalls, LinkTimeConstant) \
    v(OperationsOfCalls, NativeBinding) \
    v(OperationsOfCalls, RecordCallee) \
    v(OperationsOfCalls, SetFunctionName) \
    v(OperationsOfCalls, ThrowUnlessCallableNamed) \
    v(OperationsOfCalls, ThrowUnlessForwardedCalleeIsCallable) \
    v(OperationsOfErrors, AddressOfThrow) \
    v(OperationsOfErrors, Catch) \
    v(OperationsOfErrors, CatchException) \
    v(OperationsOfErrors, CatchValueAndException) \
    v(OperationsOfErrors, NewError) \
    v(OperationsOfErrors, NewFunctionThatThrows) \
    v(OperationsOfErrors, NewRuntimeError) \
    v(OperationsOfErrors, NewRuntimeErrorWithName) \
    v(OperationsOfErrors, NewSuppressedError) \
    v(OperationsOfErrors, Throw) \
    v(OperationsOfErrors, ThrowAfterCleanup) \
    v(OperationsOfErrors, ThrowIfTypedArrayIsDetached) \
    v(OperationsOfErrors, ThrowRuntimeError) \
    v(OperationsOfErrors, ThrowToHandlerInFunction) \
    v(OperationsOnDates, DateNow) \
    v(OperationsOnDates, DateToISOString) \
    v(OperationsOnDates, FormatWithPattern) \
    v(OperationsOnDates, NewFormatterWithPattern) \
    v(OperationsOnDates, SetUpFormatterWithOptions) \
    v(OperationsOnData, NewDictionary) \
    v(OperationsOnData, NewFromData) \
    v(OperationsOnData, NewMapFromData) \
    v(OperationsOnData, NewSetFromData) \
    v(OperationsOnData, NewTemplateObject) \
    v(OperationsOfDirectEval, DirectEval)

// The stub in front of an operation calls it by an entry of its own.
constexpr Entry operationBehindFrontEnd(Entry entry)
{
    switch (entry) {
#define AOT_STUB_FRONT_END(Operation, Family) \
    case Entry::Raw##Operation: \
        return Entry::operationAOT##Operation;
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
    default:
        return entry;
    }
}

constexpr std::optional<RemovableFeature> featureOf(Entry entry)
{
    switch (operationBehindFrontEnd(entry)) {
#define AOT_FEATURE_OF_OPERATION(family, name) \
    case Entry::operationAOT##name: \
        return RemovableFeature::family;
    FOR_EACH_AOT_OPERATION_IN_FAMILY(AOT_FEATURE_OF_OPERATION)
#undef AOT_FEATURE_OF_OPERATION
    case Entry::operationAOTFormatNumber:
        return RemovableFeature::IntlNumberFormat;
    case Entry::operationAOTFormatDateTime:
        return RemovableFeature::IntlDateTimeFormat;
    case Entry::operationAOTStringLocaleCompare:
        return RemovableFeature::IntlCollation;
    case Entry::operationAOTRegExpExec:
    case Entry::operationAOTRegExpTest:
    case Entry::operationAOTRegExpFind:
    case Entry::operationAOTRegExpTestBoxed:
    case Entry::operationAOTRegExpMatchLiteral:
    case Entry::operationAOTRegExpMatchLiteralWithoutObject:
    case Entry::operationAOTRegExpMatchFlatProgram8:
    case Entry::operationAOTRegExpMatchFlatProgram16:
    case Entry::operationAOTNewRegExpLiteral:
    case Entry::operationAOTSharedRegExpLiteral:
    case Entry::operationAOTNewRegExp:
    case Entry::operationAOTRegExpFlags:
    case Entry::operationAOTRegExpSource:
    case Entry::operationAOTStringReplaceRegExp:
    case Entry::operationAOTStringReplaceAllRegExp:
    case Entry::operationAOTStringReplaceRegExpString:
    case Entry::operationAOTStringReplaceRegExpWithFunction:
    case Entry::operationAOTStringSplitRegExp:
    case Entry::operationAOTStringMatchRegExp:
    case Entry::operationAOTStringSearchRegExp:
    case Entry::operationAOTRegExpExecPrefix:
    case Entry::operationAOTStringMatchPrefix:
        return RemovableFeature::RegExp;
    default:
        return std::nullopt;
    }
}

// Nobody asks for one of these to be removed, and nothing but the list of an image keeps one.
constexpr bool isFamilyOfOperations(RemovableFeature feature)
{
    return feature >= RemovableFeature::OperationsOfChecks && feature <= RemovableFeature::OperationsOfDirectEval;
}

JS_EXPORT_PRIVATE ASCIILiteral nameOf(Entry);

static_assert(entryAtOffset(offsetInRuntimeTable(Entry::HandleException)) == Entry::HandleException);
static_assert(entryOf(RuntimeOperation::ValueAdd) == Entry::operationAOTValueAdd);
static_assert(runtimeOperationOf(Entry::operationAOTValueAdd) == RuntimeOperation::ValueAdd);
static_assert(!runtimeOperationOf(Entry::operationAOTLookupExceptionHandler));
static_assert(!runtimeOperationOf(Entry::HandleException));
static_assert(argumentBufferAccessOf(Entry::operationAOTValueAdd) == ArgumentBufferAccess::None);
static_assert(argumentBufferAccessOf(Entry::operationAOTNewArray) == ArgumentBufferAccess::Reads);
static_assert(argumentBufferAccessOf(Entry::operationArrayIndexOfValueInt32OrContiguous) == ArgumentBufferAccess::ReadsAndWrites);
static_assert(featureOf(Entry::operationAOTFormatNumber) == RemovableFeature::IntlNumberFormat);
static_assert(featureOf(Entry::operationAOTRegExpTest) == RemovableFeature::RegExp);
static_assert(featureOf(Entry::operationAOTValueAdd) == RemovableFeature::OperationsOfArithmetic && isFamilyOfOperations(RemovableFeature::OperationsOfArithmetic));
static_assert(featureOf(Entry::RawCopyObject) == featureOf(Entry::operationAOTCopyObject) && featureOf(Entry::RawCopyObject));
static_assert(!featureOf(Entry::operationAOTGetById) && !isFamilyOfOperations(RemovableFeature::RegExp));
// Where the build forbids code from strings the operation is what throws, and the interpreter is left out.
static_assert(featureOf(Entry::operationAOTDirectEval) == RemovableFeature::OperationsOfDirectEval);
static_assert(isOperation(Entry::operationArrayIndexOfValueInt32OrContiguous) && !isOperation(Entry::VirtualCallTrampoline));

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (Instance*, void* address, uint32_t sizeAndIsStore, void* const* stackPointer));
#endif
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM*, Instance*));
// Stands in its place in the table of an image that shares realms.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandlerAmongImages, UGPRPair, (VM*, Instance*));
// How far the stack pointer at the stack check of a frame without a record is below the return address of the frame. Zero for a frame that has a record.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, UGPRPair, (Instance*, const void* returnAddress));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTFindLandingOfMisfit, void*, (Instance*, void* returnAddress, EncodedJSValue misfit));
// What is to be called in place of the Proxy that is the callee of the frame, and, where that is the trap, the list of arguments, or one where the caller makes it.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareProxyCall, UGPRPair, (Instance*, CallFrame* hostCallFrame, uint32_t makesList));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportBrokenClaim, void, (uint32_t claimAndOffset, uint32_t function, uint32_t valueIndex, const uint64_t* foundAndGiven));
JSC_DECLARE_JIT_OPERATION(operationAOTCallEngineCallee, EncodedJSValue, (Instance*, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* arguments));
JSC_DECLARE_JIT_OPERATION(operationAOTCopyDataPropertiesByFieldMap, void, (Instance*, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys));

class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

    void finishInitialization(VM&);

    bool countsCalls() const { return m_countsCalls; }
    uint64_t callCount(Entry entry) const { return m_callCounts[static_cast<unsigned>(entry)]; }
    void didCall(Entry entry)
    {
        ++m_callCounts[static_cast<unsigned>(entry)];
        m_lastCalled = entry;
    }
    // Compiled code leaves its work to an operation where an allocator has no cell at hand. Such a call refills the allocator. Only the first refill behind a call is counted.
    void willRefillAllocator()
    {
        if (m_lastCalled == Entry::NumberOfEntries)
            return;
        if (m_callsThatRefilled.isEmpty())
            m_callsThatRefilled.fill(0, numberOfEntries);
        ++m_callsThatRefilled[static_cast<unsigned>(std::exchange(m_lastCalled, Entry::NumberOfEntries))];
    }
    uint64_t numberOfCallsThatRefilled(Entry entry) const { return m_callsThatRefilled.isEmpty() ? 0 : m_callsThatRefilled[static_cast<unsigned>(entry)]; }

    Structure* compactTableStructure() const { return m_compactTableStructure; }
    uint32_t compactTableIterationEntry() const { return m_compactTableIterationEntry; }
    void setCompactTableIterationEntry(uint32_t entry) { m_compactTableIterationEntry = entry; }

    const Identifier& nameOfApplyTrap(VM&);

private:
#if defined(BUN_HARDENED)
    void** m_entries;
#else
    void* m_entries[numberOfEntries] { };
#endif
    uint64_t m_callCounts[numberOfEntries] { };
    WTF::Vector<uint64_t> m_callsThatRefilled;
    Entry m_lastCalled { Entry::NumberOfEntries };
    DataOnlyCallLinkInfo m_callLinkInfoForCall;
    Structure* m_compactTableStructure { nullptr };
    uint32_t m_compactTableIterationEntry { 0 };
    Identifier m_nameOfApplyTrap;
    bool m_countsCalls { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
