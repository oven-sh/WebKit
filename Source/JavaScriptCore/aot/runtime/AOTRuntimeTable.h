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
    v(ArrayConstructorClassInfo) \
    v(ObjectConstructorClassInfo) \
    v(BooleanConstructorClassInfo) \
    v(CustomGetterClassInfo) \
    v(NumberConstructorClassInfo) \
    v(StringConstructorClassInfo) \
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
    v(SelfCheckOperations, CountExecution) \
    v(ProxyOperations, FinishHasThroughProxy) \
    v(ProxyOperations, FinishSetThroughProxy) \
    v(ProxyOperations, GetWithProxyTrap) \
    v(ProxyOperations, HasWithProxyTrap) \
    v(ProxyOperations, SetWithProxyTrap) \
    v(ProxyOperations, ValidateAnswerOfProxyTrap) \
    v(StringBuilderOperations, Concatenate) \
    v(StringBuilderOperations, ConcatenatePair) \
    v(StringBuilderOperations, StringBuilderAppend) \
    v(StringBuilderOperations, StringBuilderAppendString) \
    v(StringBuilderOperations, StringBuilderAppendStringRange) \
    v(StringBuilderOperations, StringBuilderBegin) \
    v(StringBuilderOperations, StringBuilderDiscard) \
    v(StringBuilderOperations, StringBuilderFinish) \
    v(StringBuilderOperations, StringBuilderReserve) \
    v(StringBuilderOperations, StringBuilderRollBack) \
    v(StringBuilderOperations, StringBuilderToString) \
    v(TypedArrayOperations, ByteLengthOfAnyValue) \
    v(TypedArrayOperations, NewTypedArray) \
    v(TypedArrayOperations, CopyByteLiteral) \
    v(TypedArrayOperations, TypedArrayPut) \
    v(TypedArrayOperations, TypedArraySetOfAnyValue) \
    v(TypedArrayOperations, TypedArraySubarrayOfAnyValue) \
    v(TextEncodingOperations, TextOfBytes) \
    v(PrivateNameOperations, BeginMemberGuards) \
    v(PrivateNameOperations, NewClassToken) \
    v(PrivateNameOperations, PrivateDefine) \
    v(PrivateNameOperations, PrivateGet) \
    v(PrivateNameOperations, PrivateHas) \
    v(PrivateNameOperations, PrivateSet) \
    v(SuperOperations, GetSuperBase) \
    v(SuperOperations, GetSuperPropertyById) \
    v(SuperOperations, GetSuperPropertyOfClassById) \
    v(SuperOperations, SuperConstruct) \
    v(SuperOperations, SuperConstructList) \
    v(SuperOperations, SuperGet) \
    v(SuperOperations, SuperSet) \
    v(SuperOperations, ThrowBecauseParentMadeNoObject) \
    v(ClassOperations, DefineMethod) \
    v(ClassOperations, GetClassPrototype) \
    v(ClassOperations, GetConstructPrototype) \
    v(ClassOperations, NewClass) \
    v(ClassOperations, RememberStructureOfClass) \
    v(ClassOperations, ResolveHeritage) \
    v(ClassOperations, SetConstructPrototype) \
    v(DynamicImportOperations, AreLoaderFunctionsStored) \
    v(DynamicImportOperations, AwaitWithSavedFrameInModuleBody) \
    v(DynamicImportOperations, ImportDynamic) \
    v(DynamicImportOperations, ImportModule) \
    v(DynamicImportOperations, IsNamespaceOfModule) \
    v(DynamicImportOperations, RequireDynamic) \
    v(DynamicImportOperations, RequireDynamicWithRequirer) \
    v(DynamicImportOperations, RequireIfImported) \
    v(DynamicImportOperations, ResolveDynamic) \
    v(DynamicImportOperations, ResolveModule) \
    v(DynamicImportOperations, SaveFrameAndAwaitInModuleBody) \
    v(DynamicImportOperations, WasLoadedSynchronously) \
    v(ReExportOperations, TryDefineLiveBinding) \
    v(ReExportOperations, TryReExportFields) \
    v(ModuleFailureOperations, AbandonModuleEvaluation) \
    v(ModuleFailureOperations, IsLinkedBelow) \
    v(ModuleFailureOperations, RecordRequire) \
    v(ModuleFailureOperations, RecordRequireWithRequirer) \
    v(ModuleFailureOperations, SeesMockOf) \
    v(ModuleFailureOperations, StopLoadingBehindFile) \
    v(ModuleFailureOperations, ThrowModuleFailure) \
    v(GlobalBindingOperations, DeleteFreeName) \
    v(GlobalBindingOperations, GetGlobalBinding) \
    v(GlobalBindingOperations, GetGlobalObjectBinding) \
    v(GlobalBindingOperations, ScopeHasBinding) \
    v(GlobalBindingOperations, SetGlobalBinding) \
    v(GlobalBindingOperations, SetGlobalBindingSloppy) \
    v(GlobalBindingOperations, SetGlobalObjectBinding) \
    v(GlobalBindingOperations, SetGlobalObjectBindingSloppy) \
    v(ArgumentsObjectOperations, CreateMappedArguments) \
    v(ArgumentsObjectOperations, CreateUnmappedArguments) \
    v(ArgumentsObjectOperations, GetMappedArgument) \
    v(ArgumentsObjectOperations, SetMappedArgument) \
    v(AsyncIteratorOperations, AsyncIteratorStep) \
    v(AsyncIteratorOperations, CreateAsyncFromSyncIterator) \
    v(AsyncIteratorOperations, FinishAsyncIteratorStep) \
    v(GeneratorOperations, IsGeneratorSuspendedAtStart) \
    v(GeneratorOperations, NewAsyncGenerator) \
    v(GeneratorOperations, NewGenerator) \
    v(GeneratorOperations, NewGeneratorOfLibrary) \
    v(GeneratorOperations, ResumeGenerator) \
    v(GeneratorOperations, SaveFrameAndYield) \
    v(GeneratorOperations, TakeGeneratorPrototype) \
    v(AwaitOperations, AwaitCallWithSavedFrame) \
    v(AwaitOperations, AwaitWithSavedFrame) \
    v(AwaitOperations, NewHeapFrame) \
    v(AwaitOperations, SaveFrameAndAwait) \
    v(AwaitOperations, SaveFrameAndAwaitCall) \
    v(PromiseOperations, CreatePromise) \
    v(PromiseOperations, NewPromise) \
    v(PromiseOperations, PromiseAll) \
    v(PromiseOperations, PromiseCatch) \
    v(PromiseOperations, PromiseThen) \
    v(PromiseOperations, RejectPromise) \
    v(PromiseOperations, ResolvePromise) \
    v(PromiseOperations, ResolvePromiseOrReturnResult) \
    v(MapAndSetOperations, MapClear) \
    v(MapAndSetOperations, MapNext) \
    v(MapAndSetOperations, NewMap) \
    v(MapAndSetOperations, NewMapFromEntries) \
    v(MapAndSetOperations, NewMapIterator) \
    v(MapAndSetOperations, NewSet) \
    v(MapAndSetOperations, NewSetFromIterable) \
    v(MapAndSetOperations, NewSetIterator) \
    v(MapAndSetOperations, NextSetElement) \
    v(MapAndSetOperations, SetClear) \
    v(MapAndSetOperations, SetNext) \
    v(MapAndSetOperations, TryNewMapFromEntries) \
    v(MapAndSetOperations, TryNewSetFromIterable) \
    v(ObjectCopyOperations, AssignToNewObject) \
    v(ObjectCopyOperations, CopyDataProperties) \
    v(ObjectCopyOperations, CopyDataPropertiesByFieldMap) \
    v(ObjectCopyOperations, CopyObject) \
    v(ObjectCopyOperations, CopyObjectAndDefine) \
    v(ObjectCopyOperations, ObjectAssignWithoutCode) \
    v(ObjectCopyOperations, ObjectAssignWithoutGetters) \
    v(ObjectCopyOperations, TryCopyTwoObjects) \
    v(PropertyEnumerationOperations, EnumeratorGetByVal) \
    v(PropertyEnumerationOperations, EnumeratorNext) \
    v(PropertyEnumerationOperations, GetPropertyEnumerator) \
    v(PropertyEnumerationOperations, ObjectEntries) \
    v(PropertyEnumerationOperations, ObjectValues) \
    v(PropertyEnumerationOperations, OwnPropertyKeys) \
    v(PropertyEnumerationOperations, PropertyIsEnumerable) \
    v(PropertyDescriptorOperations, CallCustomGetter) \
    v(PropertyDescriptorOperations, DefineAccessor) \
    v(PropertyDescriptorOperations, DefineAccessors) \
    v(PropertyDescriptorOperations, DefineOwnProperty) \
    v(PropertyDescriptorOperations, GetOwnProperty) \
    v(PropertyDescriptorOperations, GetOwnPropertyDescriptor) \
    v(PropertyDescriptorOperations, NewGetterSetter) \
    v(PropertyDescriptorOperations, ObjectCreate) \
    v(PropertyDescriptorOperations, SetLiteralPrototype) \
    v(PropertyDeletionOperations, DeleteById) \
    v(PropertyDeletionOperations, DeleteByIdSloppy) \
    v(PropertyDeletionOperations, DeleteByName) \
    v(PropertyDeletionOperations, DeleteByNameSloppy) \
    v(PropertyDeletionOperations, DeleteByVal) \
    v(PropertyDeletionOperations, DeleteByValSloppy) \
    v(ArrayMutationOperations, ArrayCopy) \
    v(ArrayMutationOperations, ArrayJoin) \
    v(ArrayMutationOperations, ArrayJoinOfAnyReceiver) \
    v(ArrayMutationOperations, ArrayToStringOfAnyReceiver) \
    v(ArrayMutationOperations, ArrayOfLengthOrElement) \
    v(ArrayMutationOperations, ArraySpeciesCreate) \
    v(ArrayMutationOperations, ArraySplice) \
    v(ArrayMutationOperations, ArrayUnshift) \
    v(ArrayMutationOperations, SortNumbers) \
    v(ArrayMutationOperations, SetArrayLength) \
    v(ArrayMutationOperations, SetArrayLengthInSloppyCode) \
    v(ArraySearchOperations, ArrayIndexOfDouble) \
    v(ArraySearchOperations, ArrayIndexOfInt32) \
    v(ArraySearchOperations, ArrayIndexOfString) \
    v(ArraySearchOperations, ArraySearchOfAnyValue) \
    v(ArrayCreationOperations, AppendElements) \
    v(ArrayCreationOperations, AppendHole) \
    v(ArrayCreationOperations, AppendIterable) \
    v(ArrayCreationOperations, AppendMembers) \
    v(ArrayCreationOperations, AppendValuesOfMap) \
    v(ArrayCreationOperations, ArrayPutAtIndex) \
    v(ArrayCreationOperations, ArrayPutAtIndexInSloppyCode) \
    v(ArrayCreationOperations, ArrayPutAtNumber) \
    v(ArrayCreationOperations, ArrayPutNumberAtIndex) \
    v(ArrayCreationOperations, ConvertToDoubleStorage) \
    v(ArrayCreationOperations, DestructureIterable) \
    v(ArrayCreationOperations, NewArgumentList) \
    v(ArrayCreationOperations, NewArrayAtReviewedSite) \
    v(ArrayCreationOperations, NewArrayFromDoubles) \
    v(ArrayCreationOperations, NewArrayIterator) \
    v(ArrayCreationOperations, NewArrayOfGiven) \
    v(ArrayCreationOperations, NewArrayOfHoles) \
    v(ArrayCreationOperations, NewArrayOfList) \
    v(ArrayCreationOperations, NewArrayWithHoles) \
    v(ArrayCreationOperations, NewArrayWithHolesAtSite) \
    v(ArrayCreationOperations, NewArrayWithLengthAtReviewedSite) \
    v(ArrayCreationOperations, NewArrayWithIndexingType) \
    v(ArrayCreationOperations, ReadsIteratorOfListUnseen) \
    v(ArrayCreationOperations, ReserveArrayCapacity) \
    v(ArrayCreationOperations, ThrowIfTooLongForList) \
    v(ArrayCreationOperations, ThrowNotIterable) \
    v(ArrayCreationOperations, TruncateNewArray) \
    v(StringSearchOperations, StringFindLiteral16) \
    v(StringSearchOperations, StringFindLiteral8) \
    v(StringSearchOperations, StringIndexOfCharacter) \
    v(StringSearchOperations, StringIndexOfLiteral) \
    v(StringSearchOperations, StringLastIndexOf) \
    v(StringSearchOperations, StringLastIndexOfLiteral) \
    v(StringSearchOperations, StringReverseFindLiteral16) \
    v(StringSearchOperations, StringReverseFindLiteral8) \
    v(StringCreationOperations, CharToString) \
    v(StringCreationOperations, CodeURIOfAnyValue) \
    v(StringCreationOperations, Concat) \
    v(StringCreationOperations, ConcatThree) \
    v(StringCreationOperations, DoubleToString) \
    v(StringCreationOperations, Flatten) \
    v(StringCreationOperations, Int32ToStringWithRadix) \
    v(StringCreationOperations, IntToString) \
    v(StringCreationOperations, NumberToStringWithRadix) \
    v(StringCreationOperations, ObjectToStringOfAnyValue) \
    v(StringCreationOperations, StringOfAnyValue) \
    v(StringCreationOperations, StringPad) \
    v(StringCreationOperations, StringRepeat) \
    v(StringCreationOperations, ToString) \
    v(StringComparisonOperations, CompareStrings) \
    v(StringComparisonOperations, NarrowCharactersOfString) \
    v(StringComparisonOperations, StringCodeUnitAt) \
    v(StringComparisonOperations, TryResolveRope) \
    v(StringComparisonOperations, StringEqualsCharacters) \
    v(StringComparisonOperations, StringEqualsLiteral) \
    v(StringComparisonOperations, StringEqualsLiteralInRegister) \
    v(StringComparisonOperations, StringsAreEqual) \
    v(ArithmeticOperations, AddNumberAndValueAsNumber) \
    v(ArithmeticOperations, BigIntConstant) \
    v(ArithmeticOperations, Decrement) \
    v(ArithmeticOperations, DivNumberAndValueAsNumber) \
    v(ArithmeticOperations, Increment) \
    v(ArithmeticOperations, MulNumberAndValueAsNumber) \
    v(ArithmeticOperations, NewBigInt64) \
    v(ArithmeticOperations, NumberOfAnyValue) \
    v(ArithmeticOperations, SubNumberAndValueAsNumber) \
    v(ArithmeticOperations, ToNumber) \
    v(ArithmeticOperations, ToNumeric) \
    v(ArithmeticOperations, ValueAdd) \
    v(ArithmeticOperations, ValueBitAnd) \
    v(ArithmeticOperations, ValueBitNot) \
    v(ArithmeticOperations, ValueBitOr) \
    v(ArithmeticOperations, ValueBitXor) \
    v(ArithmeticOperations, ValueDiv) \
    v(ArithmeticOperations, ValueLShift) \
    v(ArithmeticOperations, ValueMod) \
    v(ArithmeticOperations, ValueMul) \
    v(ArithmeticOperations, ValueNegate) \
    v(ArithmeticOperations, ValuePow) \
    v(ArithmeticOperations, ValueRShift) \
    v(ArithmeticOperations, ValueSub) \
    v(ArithmeticOperations, ValueURShift) \
    v(ComparisonOperations, CompareEq) \
    v(ComparisonOperations, CompareGreater) \
    v(ComparisonOperations, CompareGreaterEq) \
    v(ComparisonOperations, CompareLess) \
    v(ComparisonOperations, CompareLessEq) \
    v(ComparisonOperations, CompareStrictEq) \
    v(ConversionOperations, IsCallable) \
    v(ConversionOperations, IsConstructor) \
    v(ConversionOperations, ToBoolean) \
    v(ConversionOperations, ToKey) \
    v(ConversionOperations, ToLength) \
    v(ConversionOperations, ToObject) \
    v(ConversionOperations, ToThis) \
    v(ConversionOperations, TypeOf) \
    v(MembershipTestOperations, DefaultHasInstance) \
    v(MembershipTestOperations, HasOwnByIdAndCache) \
    v(MembershipTestOperations, HasOwnByValAndCache) \
    v(MembershipTestOperations, InByIdAndCache) \
    v(MembershipTestOperations, InByValAndCache) \
    v(MembershipTestOperations, InByValGeneric) \
    v(MembershipTestOperations, InstanceOfAndCache) \
    v(CallOperations, Apply) \
    v(CallOperations, Construct) \
    v(CallOperations, ConstructWithArray) \
    v(CallOperations, LinkTimeConstant) \
    v(CallOperations, NativeBinding) \
    v(CallOperations, ReadArgumentList) \
    v(CallOperations, SetFunctionName) \
    v(CallOperations, ThrowUnlessForwardedCallCanBeMade) \
    v(ErrorOperations, AddressOfThrow) \
    v(ErrorOperations, Catch) \
    v(ErrorOperations, CatchException) \
    v(ErrorOperations, CatchValueAndException) \
    v(ErrorOperations, NewRuntimeError) \
    v(ErrorOperations, NewRuntimeErrorWithName) \
    v(ErrorOperations, NewSuppressedError) \
    v(ErrorOperations, Throw) \
    v(ErrorOperations, ThrowAfterCleanup) \
    v(ErrorOperations, ThrowIfTypedArrayIsDetached) \
    v(ErrorOperations, ThrowRuntimeError) \
    v(ErrorOperations, ThrowToHandlerInFunction) \
    v(DateOperations, DateNow) \
    v(DateOperations, DateToISOString) \
    v(ImageDataOperations, NewDictionary) \
    v(ImageDataOperations, NewFromData) \
    v(ImageDataOperations, NewMapFromData) \
    v(ImageDataOperations, NewSetFromData) \
    v(ImageDataOperations, NewTemplateObject) \
    v(DirectEvalOperations, DirectEval)

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
    case Entry::operationAOTRegExpMatchFlatProgram8:
    case Entry::operationAOTRegExpMatchFlatProgram16:
    case Entry::operationAOTRegExpTest:
    case Entry::operationAOTRegExpTestBoxed:
    case Entry::operationAOTRegExpMatchLiteral:
    case Entry::operationAOTRegExpMatchLiteralWithoutObject:
    case Entry::operationAOTRegExpMatchInParts:
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

constexpr bool isOperationFamily(RemovableFeature feature)
{
    return feature >= RemovableFeature::SelfCheckOperations && feature <= RemovableFeature::DirectEvalOperations;
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
static_assert(featureOf(Entry::operationAOTValueAdd) == RemovableFeature::ArithmeticOperations && isOperationFamily(RemovableFeature::ArithmeticOperations));
static_assert(featureOf(Entry::operationAOTApply) == RemovableFeature::CallOperations);
static_assert(!featureOf(Entry::operationAOTGetById) && !isOperationFamily(RemovableFeature::RegExp));
static_assert(featureOf(Entry::operationAOTDirectEval) == RemovableFeature::DirectEvalOperations);
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

inline constexpr int64_t cannotCompareInline = 2;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
