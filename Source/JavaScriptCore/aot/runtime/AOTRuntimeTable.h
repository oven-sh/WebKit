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
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;
class Structure;

namespace AOT {

enum class LeafAnswer : size_t { TakeSlowPath, No, Yes };
enum class LeafPresence : size_t { TakeSlowPath, Absent, Inherited, Own };

#define FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(v, reserved) \
    reserved(operationArrayIndexOfValueInt32OrContiguous) \
    reserved(operationArrayIncludesValueInt32OrContiguous) \
    v(operationAOTLookupExceptionHandler) \
    v(operationAOTBytesAllocatedAtPrologueCheck) \
    v(operationAOTCopyDataPropertiesByFieldMap)

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException)

#define FOR_EACH_AOT_POINTER(v) \
    v(CallLinkInfoForCall) \
    v(VirtualCallTrampoline) \
    v(ErrorConstructorClassInfo) \
    v(BooleanConstructorClassInfo) \
    v(NonConstructorHostFunction) \
    v(AsyncGeneratorClassInfo) \
    v(GeneratorClassInfo) \
    v(WebAssemblyFunctionClassInfo)

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

constexpr ArgumentBufferAccess argumentBufferAccessOf(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(operationBehindFrontEnd(entry)))
        return argumentBufferAccessOf(*operation);
    return ArgumentBufferAccess::ReadsAndWrites;
}

#define FOR_EACH_AOT_OPERATION_IN_FAMILY(v) \
    v(OperationsOfChecks, CountExecution) \
    v(OperationsOnProxies, FinishHasThroughProxy) \
    v(OperationsOnProxies, FinishSetThroughProxy) \
    v(OperationsOnProxies, GetWithProxyTrap) \
    v(OperationsOnProxies, HasWithProxyTrap) \
    v(OperationsOnProxies, SetWithProxyTrap) \
    v(OperationsOnProxies, ValidateAnswerOfProxyTrap) \
    v(OperationsOfJSONStringify, JSONAppendString) \
    v(OperationsOfJSONStringify, JSONAppendValue) \
    v(OperationsOfJSONStringify, JSONEnterWatchedLevel) \
    v(OperationsOnStringBuilders, Concatenate) \
    v(OperationsOnStringBuilders, ConcatenatePair) \
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
    v(OperationsOnTypedArrays, NewTypedArray) \
    v(OperationsOnTypedArrays, TypedArrayPut) \
    v(OperationsOnTypedArrays, TypedArraySetOfAnyValue) \
    v(OperationsOnTypedArrays, TypedArraySubarrayOfAnyValue) \
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
    v(OperationsOfAwait, NewHeapFrame) \
    v(OperationsOfAwait, SaveFrameAndAwait) \
    v(OperationsOfAwait, SaveFrameAndAwaitCall) \
    v(OperationsOnPromises, CreatePromise) \
    v(OperationsOnPromises, NewPromise) \
    v(OperationsOnPromises, PromiseAll) \
    v(OperationsOnPromises, PromiseCatch) \
    v(OperationsOnPromises, PromiseThen) \
    v(OperationsOnPromises, RejectPromise) \
    v(OperationsOnPromises, ResolvePromise) \
    v(OperationsOnPromises, ResolvePromiseOrReturnResult) \
    v(OperationsOnMapsAndSets, MapClear) \
    v(OperationsOnMapsAndSets, MapNext) \
    v(OperationsOnMapsAndSets, NewMap) \
    v(OperationsOnMapsAndSets, NewMapFromEntries) \
    v(OperationsOnMapsAndSets, NewMapIterator) \
    v(OperationsOnMapsAndSets, NewSet) \
    v(OperationsOnMapsAndSets, NewSetFromIterable) \
    v(OperationsOnMapsAndSets, NewSetIterator) \
    v(OperationsOnMapsAndSets, SetClear) \
    v(OperationsOnMapsAndSets, SetNext) \
    v(OperationsOnMapsAndSets, TryNewMapFromEntries) \
    v(OperationsOnMapsAndSets, TryNewSetFromIterable) \
    v(OperationsThatCopyObjects, AssignToNewObject) \
    v(OperationsThatCopyObjects, CopyDataProperties) \
    v(OperationsThatCopyObjects, CopyDataPropertiesByFieldMap) \
    v(OperationsThatCopyObjects, CopyObject) \
    v(OperationsThatCopyObjects, CopyObjectAndDefine) \
    v(OperationsThatCopyObjects, ObjectAssignWithoutCode) \
    v(OperationsThatCopyObjects, TryCopyTwoObjects) \
    v(OperationsThatListProperties, EnumeratorGetByVal) \
    v(OperationsThatListProperties, EnumeratorNext) \
    v(OperationsThatListProperties, GetPropertyEnumerator) \
    v(OperationsThatListProperties, ObjectEntries) \
    v(OperationsThatListProperties, ObjectValues) \
    v(OperationsThatListProperties, OwnPropertyKeys) \
    v(OperationsThatListProperties, PropertyIsEnumerable) \
    v(OperationsThatDescribeProperties, CallCustomGetter) \
    v(OperationsThatDescribeProperties, DefineAccessor) \
    v(OperationsThatDescribeProperties, DefineAccessors) \
    v(OperationsThatDescribeProperties, DefineOwnProperty) \
    v(OperationsThatDescribeProperties, GetOwnProperty) \
    v(OperationsThatDescribeProperties, GetOwnPropertyDescriptor) \
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
    v(OperationsThatChangeArrays, ArrayJoin) \
    v(OperationsThatChangeArrays, ArrayJoinOfAnyReceiver) \
    v(OperationsThatChangeArrays, ArrayToStringOfAnyReceiver) \
    v(OperationsThatChangeArrays, ArrayOfLengthOrElement) \
    v(OperationsThatChangeArrays, ArraySpeciesCreate) \
    v(OperationsThatChangeArrays, ArraySplice) \
    v(OperationsThatChangeArrays, ArrayUnshift) \
    v(OperationsThatSearchArrays, ArrayIndexOfDouble) \
    v(OperationsThatSearchArrays, ArrayIndexOfInt32) \
    v(OperationsThatSearchArrays, ArrayIndexOfString) \
    v(OperationsThatSearchArrays, ArraySearchOfAnyValue) \
    v(OperationsThatMakeArrays, AppendElements) \
    v(OperationsThatMakeArrays, AppendHole) \
    v(OperationsThatMakeArrays, AppendIterable) \
    v(OperationsThatMakeArrays, AppendMembers) \
    v(OperationsThatMakeArrays, ArrayPutAtIndex) \
    v(OperationsThatMakeArrays, ArrayPutAtNumber) \
    v(OperationsThatMakeArrays, ArrayPutNumberAtIndex) \
    v(OperationsThatMakeArrays, DestructureIterable) \
    v(OperationsThatMakeArrays, NewArgumentList) \
    v(OperationsThatMakeArrays, NewArrayAtReviewedSite) \
    v(OperationsThatMakeArrays, NewArrayFromDoubles) \
    v(OperationsThatMakeArrays, NewArrayIterator) \
    v(OperationsThatMakeArrays, NewArrayOfHoles) \
    v(OperationsThatMakeArrays, NewArrayWithHoles) \
    v(OperationsThatMakeArrays, NewArrayWithIndexingType) \
    v(OperationsThatMakeArrays, ReadsIteratorOfListUnseen) \
    v(OperationsThatMakeArrays, ThrowIfTooLongForList) \
    v(OperationsThatMakeArrays, ThrowNotIterable) \
    v(OperationsThatMakeArrays, TruncateNewArray) \
    v(OperationsThatSearchStrings, StringFindLiteral8) \
    v(OperationsThatSearchStrings, StringIndexOfCharacter) \
    v(OperationsThatSearchStrings, StringIndexOfLiteral) \
    v(OperationsThatSearchStrings, StringLastIndexOf) \
    v(OperationsThatSearchStrings, StringLastIndexOfLiteral) \
    v(OperationsThatSearchStrings, StringReverseFindLiteral8) \
    v(OperationsThatMakeStrings, CharToString) \
    v(OperationsThatMakeStrings, CodeURIOfAnyValue) \
    v(OperationsThatMakeStrings, Concat) \
    v(OperationsThatMakeStrings, ConcatThree) \
    v(OperationsThatMakeStrings, DoubleToString) \
    v(OperationsThatMakeStrings, Flatten) \
    v(OperationsThatMakeStrings, Int32ToStringWithRadix) \
    v(OperationsThatMakeStrings, IntToString) \
    v(OperationsThatMakeStrings, NumberToStringWithRadix) \
    v(OperationsThatMakeStrings, ObjectToStringOfAnyValue) \
    v(OperationsThatMakeStrings, StringOfAnyValue) \
    v(OperationsThatMakeStrings, StringPad) \
    v(OperationsThatMakeStrings, StringRepeat) \
    v(OperationsThatMakeStrings, ToString) \
    v(OperationsThatCompareStrings, CompareStrings) \
    v(OperationsThatCompareStrings, NarrowCharactersOfString) \
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
    v(OperationsOfCalls, Construct) \
    v(OperationsOfCalls, ConstructWithArray) \
    v(OperationsOfCalls, LinkTimeConstant) \
    v(OperationsOfCalls, NativeBinding) \
    v(OperationsOfCalls, ReadArgumentList) \
    v(OperationsOfCalls, SetFunctionName) \
    v(OperationsOfCalls, ThrowUnlessForwardedCallCanBeMade) \
    v(OperationsOfErrors, AddressOfThrow) \
    v(OperationsOfErrors, Catch) \
    v(OperationsOfErrors, CatchException) \
    v(OperationsOfErrors, CatchValueAndException) \
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
    v(OperationsOnData, NewDictionary) \
    v(OperationsOnData, NewFromData) \
    v(OperationsOnData, NewMapFromData) \
    v(OperationsOnData, NewSetFromData) \
    v(OperationsOnData, NewTemplateObject) \
    v(OperationsOfDirectEval, DirectEval)

constexpr std::optional<RemovableFeature> featureOf(Entry entry)
{
    switch (operationBehindFrontEnd(entry)) {
#define AOT_FEATURE_OF_OPERATION(family, name) \
    case Entry::operationAOT##name: \
        return RemovableFeature::family;
    FOR_EACH_AOT_OPERATION_IN_FAMILY(AOT_FEATURE_OF_OPERATION)
#undef AOT_FEATURE_OF_OPERATION
    case Entry::operationAOTStringLocaleCompare:
        return RemovableFeature::IntlCollation;
    case Entry::operationAOTRegExpExec:
    case Entry::operationAOTRegExpTest:
    case Entry::operationAOTRegExpTestBoxed:
    case Entry::operationAOTRegExpMatchLiteral:
    case Entry::operationAOTRegExpMatchLiteralWithoutObject:
    case Entry::operationAOTNewRegExpLiteral:
    case Entry::operationAOTSharedRegExpLiteral:
    case Entry::operationAOTCopyOfSharedRegExpLiteral:
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
        return RemovableFeature::RegExp;
    default:
        return std::nullopt;
    }
}

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
static_assert(featureOf(Entry::operationAOTRegExpTest) == RemovableFeature::RegExp);
static_assert(featureOf(Entry::operationAOTValueAdd) == RemovableFeature::OperationsOfArithmetic && isFamilyOfOperations(RemovableFeature::OperationsOfArithmetic));
static_assert(featureOf(Entry::operationAOTApply) == RemovableFeature::OperationsOfCalls);
static_assert(!featureOf(Entry::operationAOTGetById) && !isFamilyOfOperations(RemovableFeature::RegExp));
static_assert(featureOf(Entry::operationAOTDirectEval) == RemovableFeature::OperationsOfDirectEval);
static_assert(isOperation(Entry::operationArrayIndexOfValueInt32OrContiguous) && !isOperation(Entry::VirtualCallTrampoline));

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM*, Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, UGPRPair, (Instance*, const void* returnAddress));
JSC_DECLARE_JIT_OPERATION(operationAOTCopyDataPropertiesByFieldMap, void, (Instance*, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys));

class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

    void finishInitialization(VM&);

    Structure* compactTableStructure() const { return m_compactTableStructure; }
    Structure* textBufferStructure() const { return m_textBufferStructure; }
    uint32_t compactTableIterationEntry() const { return m_compactTableIterationEntry; }
    void setCompactTableIterationEntry(uint32_t entry) { m_compactTableIterationEntry = entry; }

private:
#if defined(BUN_HARDENED)
    void** m_entries;
#else
    void* m_entries[numberOfEntries] { };
#endif
    DataOnlyCallLinkInfo m_callLinkInfoForCall;
    Structure* m_compactTableStructure { nullptr };
    Structure* m_textBufferStructure { nullptr };
    uint32_t m_compactTableIterationEntry { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
