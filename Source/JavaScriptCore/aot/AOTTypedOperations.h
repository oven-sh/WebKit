/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedBuiltinLists.h"
#include "AOTTypedData.h"
#include "JITOperationValidation.h"
#include "OperationResult.h"

namespace JSC {

class JSArray;
class JSCell;

namespace AOT {

struct Instance;
struct Slot;

#define FOR_EACH_AOT_TYPED_OPERATION(v) \
    v(operationAOTTypedGetFreeVariable) \
    v(operationAOTTypedGetFreeVariableForTypeOf) \
    v(operationAOTTypedValidateGlobalPermanently) \
    v(operationAOTTypedGetGlobalThisProperty) \
    v(operationAOTTypedLookUpGlobalMember) \
    v(operationAOTTypedSetFreeVariable) \
    v(operationAOTTypedSetFreeVariableSloppy) \
    v(operationAOTTypedGetByName) \
    v(operationAOTTypedGetSuperPropertyByName) \
    v(operationAOTTypedSetByName) \
    v(operationAOTTypedSetByNameSloppy) \
    v(operationAOTTypedThrowIfReadOnly) \
    v(operationAOTTypedGetStaticField) \
    v(operationAOTTypedSetStaticField) \
    v(operationAOTTypedSetByNameInThisStructure) \
    v(operationAOTTypedPutLazyClosure) \
    v(operationAOTTypedDefineByName) \
    v(operationAOTTypedDefineOwnProperty) \
    v(operationAOTTypedDefineOwnPropertyAtSite) \
    v(operationAOTTypedHasName) \
    v(operationAOTTypedDeleteByName) \
    v(operationAOTTypedGetByKey) \
    v(operationAOTTypedSetByKey) \
    v(operationAOTTypedHasKey) \
    v(operationAOTTypedElementOrEmpty) \
    v(operationAOTTypedDeleteByKey) \
    v(operationAOTTypedCall) \
    v(operationAOTTypedThrowUnlessCallable) \
    v(operationAOTTypedThrowMethodIsNotAFunction) \
    v(operationAOTTypedThrowUnlessMethodIsCallable) \
    v(operationAOTTypedCollectionMethod) \
    v(operationAOTTypedHasByName) \
    v(operationAOTTypedThrowAtSite) \
    v(operationAOTTypedLanguageOperation) \
    v(operationAOTTypedMakeModuleNamespace) \
    v(operationAOTTypedCreateArguments) \
    v(operationAOTTypedCopyDataProperties) \
    v(operationAOTTypedObjectEntriesFlat) \
    v(operationAOTTypedOwnPropertyDescriptorsUnlessCopied) \
    v(operationAOTTypedCopyOwnPropertyDescriptors) \
    v(operationAOTTypedNewGenerator) \
    v(operationAOTTypedGetAsyncIterator) \
    v(operationAOTTypedNewObjectWithSpreads) \
    v(operationAOTTypedNewPromise) \
    v(operationAOTTypedFinishStepForDriver) \
    v(operationAOTTypedResolvePromise) \
    v(operationAOTTypedResolvePromiseOrReturnResult) \
    v(operationAOTTypedSaveFrameAndAwaitCall) \
    v(operationAOTTypedAwaitCallWithSavedFrame) \
    v(operationAOTTypedPromiseAll) \
    v(operationAOTTypedParkAsyncFunction) \
    v(operationAOTTypedSuspendAtFirstAwait) \
    v(operationAOTTypedAwait) \
    v(operationAOTTypedGetExternalModule) \
    v(operationAOTTypedConstruct) \
    v(operationAOTTypedConstructSuper) \
    v(operationAOTTypedConstructWith) \
    v(operationAOTTypedDefineAccessors) \
    v(operationAOTTypedAdd) \
    v(operationAOTTypedAddNumberAndValueAsNumber) \
    v(operationAOTTypedSubNumberAndValueAsNumber) \
    v(operationAOTTypedMulNumberAndValueAsNumber) \
    v(operationAOTTypedDivNumberAndValueAsNumber) \
    v(operationAOTTypedSub) \
    v(operationAOTTypedMul) \
    v(operationAOTTypedDiv) \
    v(operationAOTTypedMod) \
    v(operationAOTTypedExp) \
    v(operationAOTTypedBitAnd) \
    v(operationAOTTypedBitOr) \
    v(operationAOTTypedBitXor) \
    v(operationAOTTypedShl) \
    v(operationAOTTypedShr) \
    v(operationAOTTypedUshr) \
    v(operationAOTTypedNeg) \
    v(operationAOTTypedBitNot) \
    v(operationAOTTypedToNumber) \
    v(operationAOTTypedToNumeric) \
    v(operationAOTTypedLess) \
    v(operationAOTTypedLessEq) \
    v(operationAOTTypedGreater) \
    v(operationAOTTypedGreaterEq) \
    v(operationAOTTypedStrictEq) \
    v(operationAOTTypedLooseEq) \
    v(operationAOTTypedTruthy) \
    v(operationAOTTypedTypeOf) \
    v(operationAOTTypedTypeOfObject) \
    v(operationAOTTypedIsCallable) \
    v(operationAOTTypedInheritsFromClass) \
    v(operationAOTTypedInstanceOf) \
    v(operationAOTTypedInstanceOfClass) \
    v(operationAOTTypedInstanceOfEngineClass) \
    v(operationAOTTypedToString) \
    v(operationAOTTypedToKey) \
    v(operationAOTTypedFmod) \
    v(operationAOTTypedPow) \
    v(operationAOTTypedToInt32) \
    v(operationAOTTypedIndexInString) \
    v(operationAOTTypedToLiteralString) \
    v(operationAOTTypedShortStringOfString) \
    v(operationAOTTypedLiteralStringToString) \
    v(operationAOTTypedIntegerEqualTo) \
    v(operationAOTTypedAllocSlow) \
    v(operationAOTTypedMaterializeEnvironment) \
    v(operationAOTTypedNewClosureSlow) \
    v(operationAOTTypedCopyClosureOutOfFrame) \
    v(operationAOTTypedCoerce) \
    v(operationAOTTypedCoerceInPlace) \
    v(operationAOTTypedCoerceElement) \
    v(operationAOTTypedCoerceProperty) \
    v(operationAOTTypedAuditCoerce) \
    v(operationAOTTypedDidCreateField) \
    v(operationAOTTypedDidCreateFieldAt) \
    v(operationAOTTypedDidCreateFieldOutOfLineAt) \
    v(operationAOTTypedDidCreateFieldsAt) \
    v(operationAOTTypedEnsureStorage) \
    v(operationAOTTypedEnsureStorageOfSize) \
    v(operationAOTTypedCountClosure) \
    v(operationAOTTypedCountPlainAllocation) \
    v(operationAOTTypedGetExtra) \
    v(operationAOTTypedEnforceType) \
    v(operationAOTTypedEnforceTypeOrConvert) \
    v(operationAOTTypedGetByNameChecked) \
    v(operationAOTTypedGetByNameCheckedAsNumber) \
    v(operationAOTTypedRecordTypeMismatch) \
    v(operationAOTTypedHasExtra) \
    v(operationAOTTypedStoreCreatesField) \
    v(operationAOTTypedGetAbsentField) \
    v(operationAOTTypedFieldEqualsString) \
    v(operationAOTTypedPutExtra) \
    v(operationAOTTypedPutInNumberBox) \
    v(operationAOTTypedPutNumberInNumberBox) \
    v(operationAOTTypedStringConstant) \
    v(operationAOTTypedStringCached) \
    v(operationAOTTypedBigIntConstant) \
    v(operationAOTTypedConstantSymbol) \
    v(operationAOTTypedBarrierSlow) \
    v(operationAOTTypedBarrierSlowPreservingRegisters) \
    v(operationAOTTypedRememberElement) \
    v(operationAOTTypedPollSlow) \
    v(operationAOTTypedPollAtPrologue) \
    v(operationAOTTypedThrow) \
    v(operationAOTTypedThrowToHandlerInFunction) \
    v(operationAOTTypedCatch) \
    v(operationAOTTypedCatchException) \
    v(operationAOTTypedExceptionValue) \
    v(operationAOTTypedRejectWithException) \
    v(operationAOTTypedPrepareProxyCall) \
    v(operationAOTTypedAssertionFailed) \
    v(operationAOTTypedStoreAssertionFailed) \
    v(operationAOTTypedThrowReachedUnreachable) \
    v(operationAOTTypedFactViolated) \
    v(operationAOTTypedCountBranch) \
    v(operationAOTTypedCheckKnownSlot) \
    v(operationAOTTypedThrowStackOverflow) \
    v(operationAOTTypedNewObject) \
    v(operationAOTTypedNewObjectForKeys) \
    v(operationAOTTypedNewObjectWithCapacity) \
    v(operationAOTTypedNewObjectLiteral) \
    v(operationAOTTypedCloneObject) \
    v(operationAOTTypedCloneObjectAndExtend) \
    v(operationAOTTypedCloneObjectAndExtendWithNumber) \
    v(operationAOTTypedCloneObjectAs) \
    v(operationAOTTypedCloneObjectWithSubtypeCheck) \
    v(operationAOTTypedFinishObjectLiteral) \
    v(operationAOTTypedTryCopyRecordAs) \
    v(operationAOTTypedTryCopyRecordAsAt) \
    v(operationAOTTypedTryMergeRecordsAs) \
    v(operationAOTTypedPrivateGet) \
    v(operationAOTTypedPrivateSet) \
    v(operationAOTTypedPrivateGetAtSite) \
    v(operationAOTTypedPrivateSetAtSite) \
    v(operationAOTTypedPrivateHas) \
    v(operationAOTTypedPrivateFieldAdd) \
    v(operationAOTTypedDefineByKey) \
    v(operationAOTTypedDefineGetterByKey) \
    v(operationAOTTypedDefineSetterByKey) \
    v(operationAOTTypedPrivateMethodsAdd) \
    v(operationAOTTypedNewArray) \
    v(operationAOTTypedTryGetByValWithKeyParts) \
    v(operationAOTTypedTryMapOrSetLookupWithKeyParts) \
    v(operationAOTTypedNewNumberArray) \
    v(operationAOTTypedArrayPush) \
    v(operationAOTTypedArrayPushNumber) \
    v(operationAOTTypedArraySearchByCall) \
    v(operationAOTTypedArraySlice) \
    v(operationAOTTypedArrayCopy) \
    v(operationAOTTypedArrayGet) \
    v(operationAOTTypedArrayPut) \
    v(operationAOTTypedStoreElement) \
    v(operationAOTTypedStoreNumberElement) \
    v(operationAOTTypedArrayGetAtNumber) \
    v(operationAOTTypedArrayGetAtIndex) \
    v(operationAOTTypedArrayGetAtIndexAsNumber) \
    v(operationAOTTypedArrayElementAs) \
    v(operationAOTTypedArrayElementAtNumberAs) \
    v(operationAOTTypedArrayPutAtIndex) \
    v(operationAOTTypedArrayPutAtIndexExpecting) \
    v(operationAOTTypedArrayPutNumberAtIndex) \
    v(operationAOTTypedArrayPutAtNumber) \
    v(operationAOTTypedArrayTryMakeWritableDouble) \
    v(operationAOTTypedTryAddNamedProperties) \
    v(operationAOTTypedOpenModifiedArray) \
    v(operationAOTTypedTypedArrayIteratorOpen) \
    v(operationAOTTypedTypedArrayIteratorHasNext) \
    v(operationAOTTypedBuiltinCell) \
    v(operationAOTTypedCoerceToType) \
    v(operationAOTTypedNewTypedArray) \
    v(operationAOTTypedTypedArrayPut) \
    v(operationAOTTypedClosureConstant) \
    v(operationAOTTypedCreateThis) \
    v(operationAOTTypedCreateThisWithFields) \
    v(operationAOTTypedCreateThisWithCapacity) \
    v(operationAOTTypedTryCreateThisWithStores) \
    v(operationAOTTypedCreateThisWithStores) \
    v(operationAOTTypedAssignThisStructure) \
    v(operationAOTTypedCreateThisAt) \
    v(operationAOTTypedDidStoreToThis) \
    v(operationAOTTypedTryPutProperties) \
    v(operationAOTTypedCachePropertyRun) \
    v(operationAOTTypedCachePropertyLocation) \
    v(operationAOTTypedConcat) \
    v(operationAOTTypedConcatThree) \
    v(operationAOTTypedSubstring) \
    v(operationAOTTypedRopeCodeUnit) \
    v(operationAOTTypedFlatten) \
    v(operationAOTTypedStringEq) \
    v(operationAOTTypedStringEqualsLiteralInRegister) \
    v(operationAOTTypedStringHasAt) \
    v(operationAOTTypedStringIndexOf) \
    v(operationAOTTypedIntToString) \
    v(operationAOTTypedDoubleToString) \
    v(operationAOTTypedCharToString) \
    v(operationAOTTypedNewMap) \
    v(operationAOTTypedTryNewStringSet) \
    v(operationAOTTypedTryNewMapWithStringKeys) \
    v(operationAOTTypedNewSetInImage) \
    v(operationAOTTypedNewMapInImage) \
    v(operationAOTTypedMoveTableToHeap) \
    v(operationAOTTypedIterOpen) \
    v(operationAOTTypedIterHas) \
    v(operationAOTTypedIterGet) \
    v(operationAOTTypedIterClose) \
    v(operationAOTTypedIterCloseForThrow) \
    v(operationAOTTypedIterOpenInFrame) \
    v(operationAOTTypedIterHasInFrame) \
    v(operationAOTTypedFinishIterStepInFrame) \
    v(operationAOTTypedFinishIterStep) \
    v(operationAOTTypedFinishIterOpenInFrame) \
    v(operationAOTTypedIterOpenInFrameWithMethod) \
    v(operationAOTTypedIterCloseInFrame) \
    v(operationAOTTypedIterCloseForThrowInFrame) \
    v(operationAOTTypedReadBatchedElements) \
    v(operationAOTTypedFailModuleCycle)

namespace Typed {

template<auto operation> struct WritesOf;

static constexpr uint32_t typeAllowsObjects = 1 << 10;
static constexpr uint32_t typeAllowsFunctions = 1 << 11;
static constexpr uint32_t assertionIsOnResult = 1 << 8;
static constexpr int64_t arraySearchDeclined = 1ll << 48;
enum class NullishAccess : uint32_t { Read, Write, Iterate };

} // namespace Typed

#define AOT_DECLARE_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }
#define AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_NOEXCEPT_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }

AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFreeVariable, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFreeVariableForTypeOf, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedValidateGlobalPermanently, Heap, void, (Instance*, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetGlobalThisProperty, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedLookUpGlobalMember, Heap, void, (Instance*, EncodedJSValue holder, uint32_t global, uint32_t name, uint32_t cacheNumber));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetFreeVariable, Heap, void, (Instance*, uint32_t, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetFreeVariableSloppy, Heap, void, (Instance*, uint32_t, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByName, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetAbsentField, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetSuperPropertyByName, Heap, EncodedJSValue, (Instance*, EncodedJSValue home, EncodedJSValue receiver, uint32_t nameInTable));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByNameSloppy, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowIfReadOnly, Nothing, void, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetStaticField, Heap, EncodedJSValue, (Instance*, uint32_t ofClass, uint32_t field));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetStaticField, Heap, void, (Instance*, uint32_t ofClass, uint32_t field, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByNameInThisStructure, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPutLazyClosure, Heap, void, (Instance*, EncodedJSValue, uint32_t function, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
struct DescriptorLiteral {
    static constexpr uint32_t hasValue = 1u << 0;
    static constexpr uint32_t hasWritable = 1u << 1;
    static constexpr uint32_t isWritable = 1u << 2;
    static constexpr uint32_t hasEnumerable = 1u << 3;
    static constexpr uint32_t isEnumerable = 1u << 4;
    static constexpr uint32_t hasConfigurable = 1u << 5;
    static constexpr uint32_t isConfigurable = 1u << 6;
    static constexpr uint32_t hasGet = 1u << 7;
    static constexpr uint32_t hasSet = 1u << 8;
    static constexpr uint32_t storedFieldShift = 9;
};
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineOwnProperty, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue key, EncodedJSValue valueOrGetter, EncodedJSValue setter, uint32_t descriptorLiteral));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineOwnPropertyAtSite, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue value, uint32_t identifier, Slot* cache, uint32_t descriptorLiteral));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByKey, Heap, void, (Instance*, EncodedJSValue, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedElementOrEmpty, Heap, EncodedJSValue, (Instance*, EncodedJSValue, int64_t index, EncodedJSValue ifAbsent));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCall, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExternalModule, Heap, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFailModuleCycle, Fields, void, (Instance*, uint64_t row, uint32_t numberOfRows, EncodedJSValue error));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLanguageOperation, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedMakeModuleNamespace, Heap, EncodedJSValue, (Instance*, uint32_t numberOfNames, uint32_t rowOfNames, uint32_t count, const EncodedJSValue* readerAndTrailingValues));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateArguments, RuntimeState, JSCell*, (Instance*, JSCell* list));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCopyDataProperties, Heap, void, (Instance*, JSCell* target, EncodedJSValue source));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedObjectEntriesFlat, Heap, JSCell*, (Instance*, EncodedJSValue object));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedOwnPropertyDescriptorsUnlessCopied, Heap, EncodedJSValue, (Instance*, EncodedJSValue source));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCopyOwnPropertyDescriptors, Heap, void, (Instance*, EncodedJSValue target, EncodedJSValue source, EncodedJSValue descriptors));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewGenerator, RuntimeState, JSCell*, (Instance*, JSCell* resumeClosure, EncodedJSValue closure, uint32_t isAsync, uint32_t generatorTemplate, uint32_t numberOfTemplates));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetAsyncIterator, Heap, EncodedJSValue, (Instance*, EncodedJSValue iterable));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewObjectWithSpreads, Heap, JSCell*, (Instance*, uint32_t membersPlanOffset, uint32_t numberOfSources, const EncodedJSValue* values));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewPromise, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFinishStepForDriver, Heap, EncodedJSValue, (Instance*, JSCell* generator, EncodedJSValue result));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedResolvePromise, Heap, void, (Instance*, JSCell*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedResolvePromiseOrReturnResult, Heap, EncodedJSValue, (Instance*, JSCell* promise, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSaveFrameAndAwaitCall, Heap, JSCell*, (Instance*, EncodedJSValue awaited, uint64_t frame, uint64_t bottom, uint64_t resumeAddress, uint64_t extra));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAwaitCallWithSavedFrame, Heap, JSCell*, (Instance*, EncodedJSValue awaited, JSCell* saved));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPromiseAll, Heap, JSCell*, (Instance*, uint32_t count, const EncodedJSValue* elements, uint32_t resultsOfCallsWithoutPromise));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedParkAsyncFunction, Heap, void, (Instance*, JSCell*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSuspendAtFirstAwait, Heap, void, (Instance*, JSCell* frame, EncodedJSValue awaited, uint64_t request));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAwait, Heap, void, (Instance*, JSCell*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowUnlessCallable, Nothing, void, (Instance*, EncodedJSValue callee, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowMethodIsNotAFunction, Nothing, void, (Instance*, EncodedJSValue callee, uint32_t siteIdentifier));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowUnlessMethodIsCallable, Nothing, void, (Instance*, EncodedJSValue callee, uint32_t siteIdentifier));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCollectionMethod, Heap, EncodedJSValue, (Instance*, JSCell* table, uint32_t method));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasByName, Heap, EncodedJSValue, (Instance*, JSCell* table, EncodedJSValue key, EncodedJSValue method));
static constexpr unsigned maxNamedPropertiesPerSite = 32;
JSObject* newObjectLiteralAtSite(Instance*, JSGlobalObject*, VM&, FunctionRef, Slot* cache, const EncodedJSValue* values, uint32_t numberOfValues);
JSObject* newObjectFromDescriptor(Instance*, JSGlobalObject*, VM&, uint32_t number, const EncodedJSValue* values, uint32_t numberOfValues);

enum class ThrowKind : uint8_t {
    TypeMismatch,
    KindMismatch,
    NullishRead,
    NullishWrite,
    NullishIterate,
    NonNullFailed,
    TDZ,
    NotAFunction,
};
struct ThrowSite {
    static constexpr uint32_t wordIsInRegister = 2;
    static constexpr unsigned repShift = 3;
    static constexpr unsigned registerShift = 6;
    static constexpr uint32_t hasWideA = 1u << 11;
    static constexpr uint32_t hasB = 1u << 12;
    static constexpr unsigned aShift = 13;
    static constexpr uint32_t maxNarrowA = (1u << 18) - 1;
    static constexpr uint32_t none = 0x7fffffff;
    static constexpr unsigned numberOfRegistersPerBank = 32;
};
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowAtSite, Nothing, void, (Instance*, const uint64_t* registers, const uint32_t* site, uint32_t isTrap));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstruct, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstructSuper, Heap, EncodedJSValue, (Instance*, EncodedJSValue classObject, EncodedJSValue newTarget, EncodedJSValue list, EncodedJSValue parentIfKnown));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstructWith, Heap, EncodedJSValue, (Instance*, EncodedJSValue callee, JSCell* object, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineAccessors, Heap, void, (Instance*, JSCell* object, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAdd, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAddNumberAndValueAsNumber, Heap, double, (Instance*, EncodedJSValue value, uint32_t typeAndWhetherNumberIsOnRight, double number));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSubNumberAndValueAsNumber, Heap, double, (Instance*, EncodedJSValue value, uint32_t typeAndWhetherNumberIsOnRight, double number));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedMulNumberAndValueAsNumber, Heap, double, (Instance*, EncodedJSValue value, uint32_t typeAndWhetherNumberIsOnRight, double number));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDivNumberAndValueAsNumber, Heap, double, (Instance*, EncodedJSValue value, uint32_t typeAndWhetherNumberIsOnRight, double number));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSub, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedMul, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDiv, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedMod, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedExp, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBitAnd, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBitOr, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBitXor, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedShl, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedShr, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedUshr, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNeg, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBitNot, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToNumber, Heap, double, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToNumeric, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLess, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLessEq, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGreater, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGreaterEq, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStrictEq, RuntimeState, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLooseEq, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInstanceOf, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInstanceOfClass, Heap, size_t, (Instance*, EncodedJSValue, uint32_t ofClass));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInstanceOfEngineClass, Heap, size_t, (Instance*, EncodedJSValue, uint32_t numberOfClass));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTruthy, RuntimeState, size_t, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypeOf, Nothing, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypeOfObject, Nothing, size_t, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIsCallable, Nothing, size_t, (Instance*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInheritsFromClass, Heap, size_t, (Instance*, JSCell*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToString, Heap, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFmod, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPow, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedToInt32, Pure, size_t, (double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIndexInString, Pure, int64_t, (double, uint64_t length, uint32_t isRelativeToEnd));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedToLiteralString, Pure, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedShortStringOfString, Pure, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedLiteralStringToString, Pure, JSCell*, (Instance*, uint64_t word));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIntegerEqualTo, Pure, int64_t, (EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedAllocSlow, RuntimeState, JSCell*, (Instance*, uint32_t allocation));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedMaterializeEnvironment, RuntimeState, JSCell*, (Instance*, uint32_t allocation, const EncodedJSValue* source));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewClosureSlow, RuntimeState, JSCell*, (Instance*, uint64_t request, uint64_t, uint64_t, uint64_t, uint64_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCopyClosureOutOfFrame, RuntimeState, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerce, Heap, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t typeAndReason));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceInPlace, Heap, void, (Instance*, EncodedJSValue value, uint32_t typeAndReason));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceElement, RuntimeState, EncodedJSValue, (Instance*, JSCell* array, uint32_t index, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceProperty, RuntimeState, EncodedJSValue, (Instance*, JSCell* holder, uint32_t name, EncodedJSValue, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAuditCoerce, Heap, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t lineAndColumn));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateField, Fields, void, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateFieldAt, Fields, void, (Instance*, JSCell* object, uint32_t offset, Slot* cache));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateFieldOutOfLineAt, Fields, void, (Instance*, JSCell* object, uint32_t offset, Slot* cache));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateFieldsAt, Fields, void, (Instance*, JSCell* object, uint64_t offsets, Slot* cache));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedEnsureStorage, Fields, void, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedEnsureStorageOfSize, Fields, void, (Instance*, JSCell* object, uint32_t size));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCountClosure, RuntimeState, void, (Instance*, uint32_t function, uint32_t numberOfCaptures));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCountPlainAllocation, RuntimeState, void, (Instance*, JSCell*, uint32_t function, uint32_t kindAndValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExtra, Nothing, EncodedJSValue, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedEnforceType, Nothing, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedEnforceTypeOrConvert, RuntimeState, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByNameChecked, Heap, EncodedJSValue, (Instance*, JSCell* object, uint32_t name, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByNameCheckedAsNumber, Heap, double, (Instance*, JSCell* object, uint32_t name, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedRecordTypeMismatch, Nothing, void, (Instance*, EncodedJSValue value, uint32_t type, uint32_t parameter, uint32_t isAtCaller));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedHasExtra, Nothing, size_t, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStoreCreatesField, Nothing, size_t, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFieldEqualsString, RuntimeState, size_t, (Instance*, JSCell* object, uint32_t name, uint32_t text));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPutExtra, Fields, void, (Instance*, JSCell* object, uint32_t name, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPutInNumberBox, Fields, void, (Instance*, JSCell* object, uint32_t name, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPutNumberInNumberBox, Fields, void, (Instance*, JSCell* object, uint32_t name, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStringConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStringCached, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBigIntConstant, RuntimeState, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedConstantSymbol, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBarrierSlow, RuntimeState, void, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBarrierSlowPreservingRegisters, RuntimeState, void, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCachePropertyRun, RuntimeState, size_t, (Instance*, Slot* header, EncodedJSValue, uint32_t slotsAndStored, uint64_t firstNames, uint64_t nextNames));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCachePropertyLocation, RuntimeState, size_t, (Instance*, Slot* header, EncodedJSValue, uint32_t indexAndIsStore, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedRememberElement, RuntimeState, void, (Instance*, JSCell* array, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPollSlow, Nothing, void, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPollAtPrologue, Nothing, void*, (Instance*, uint8_t* returnAddress));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrow, Nothing, void, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowToHandlerInFunction, Nothing, void, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCatch, RuntimeState, EncodedJSValue, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCatchException, RuntimeState, EncodedJSValue, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedExceptionValue, Nothing, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedRejectWithException, RuntimeState, JSCell*, (Instance*, uint64_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPrepareProxyCall, RuntimeState, uintptr_t, (Instance*, EncodedJSValue* hostFrame, uint32_t count));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedAssertionFailed, Nothing, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t type, uint32_t what, uint32_t function, uint32_t lineAndColumn));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStoreAssertionFailed, Nothing, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue storedTo, uint32_t type, uint32_t function, uint32_t lineAndColumn));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowReachedUnreachable, Nothing, void, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFactViolated, Nothing, void, (Instance*, EncodedJSValue, uint32_t function, uint32_t value));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCountBranch, Nothing, void, (Instance*, uint32_t module, uint32_t positionAndSide));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCheckKnownSlot, Nothing, size_t, (Instance*, EncodedJSValue, uint32_t name, uint32_t placeAndFlags, uint32_t function, uint32_t value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowStackOverflow, Nothing, void, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObject, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObjectForKeys, RuntimeState, JSCell*, (Instance*, uint32_t hasNullPrototype));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObjectWithCapacity, RuntimeState, JSCell*, (Instance*, uint32_t inlineCapacity));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewObjectLiteral, RuntimeState, JSCell*, (Instance*, Slot* cache, uint32_t count, uint32_t hasNullPrototypeAndInlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObject, Heap, JSCell*, (Instance*, EncodedJSValue source, uint32_t keepsLayout));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObjectAndExtend, Heap, JSCell*, (Instance*, EncodedJSValue source, uint64_t more, EncodedJSValue first, EncodedJSValue second));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObjectAndExtendWithNumber, Heap, JSCell*, (Instance*, EncodedJSValue source, uint64_t more, double first));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObjectWithSubtypeCheck, Heap, JSCell*, (Instance*, EncodedJSValue source, uint32_t supertypeShape, uint32_t lastSubtypeShape, uint32_t subtypeShapeCount));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObjectAs, Heap, JSCell*, (Instance*, EncodedJSValue source, uint64_t target));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFinishObjectLiteral, RuntimeState, JSCell*, (Instance*, JSCell* object));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryCopyRecordAs, RuntimeState, JSCell*, (Instance*, uint32_t shape, uint32_t namesKind, uint32_t count, const EncodedJSValue* values));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryCopyRecordAsAt, RuntimeState, JSCell*, (Instance*, Slot* cache, uint32_t firstShape, uint32_t secondShape, uint32_t shape, uint32_t namesKind, uint32_t count, const EncodedJSValue* values));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryMergeRecordsAs, RuntimeState, JSCell*, (Instance*, uint32_t shape, EncodedJSValue first, EncodedJSValue second));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateGet, Heap, EncodedJSValue, (Instance*, EncodedJSValue object, EncodedJSValue name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateSet, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateGetAtSite, Heap, EncodedJSValue, (Instance*, Slot*, EncodedJSValue object, EncodedJSValue name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateSetAtSite, Heap, void, (Instance*, Slot*, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateHas, Heap, EncodedJSValue, (Instance*, EncodedJSValue object, EncodedJSValue name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateFieldAdd, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineByKey, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue key, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineGetterByKey, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineSetterByKey, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateMethodsAdd, Heap, EncodedJSValue, (Instance*, EncodedJSValue object, EncodedJSValue holder));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisWithFields, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisWithCapacity, RuntimeState, JSCell*, (Instance*, JSCell* constructor, JSCell* newTarget, uint32_t inlineCapacity));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryCreateThisWithStores, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* valuesThenNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisWithStores, Heap, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisAt, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAssignThisStructure, Heap, void, (Instance*, Slot* cache, EncodedJSValue self, uint32_t count, uint32_t rowOfNames, uint32_t structureDescriptorPlusOne));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDidStoreToThis, Heap, void, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, JSCell* object, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* names));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryPutProperties, Heap, size_t, (Instance*, Slot* cache, EncodedJSValue object, uint64_t countAndNamesPlanOffset, uint64_t keyed, const EncodedJSValue* valuesThenKeys));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewArray, RuntimeState, JSCell*, (Instance*, uint32_t, const EncodedJSValue*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryGetByValWithKeyParts, RuntimeState, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t, const EncodedJSValue*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryMapOrSetLookupWithKeyParts, Nothing, EncodedJSValue, (Instance*, JSCell*, uint32_t, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewNumberArray, RuntimeState, JSCell*, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPush, Heap, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue value, uint64_t expectedAndSite));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPushNumber, Heap, EncodedJSValue, (Instance*, JSCell* array, uint64_t numberBits, uint64_t expectedAndSite));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArraySearchByCall, Heap, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue searched, EncodedJSValue methodGetById, uint32_t isIndexOf));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArraySlice, Heap, EncodedJSValue, (Instance*, JSCell* array, int64_t from, int64_t to));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayCopy, Heap, EncodedJSValue, (Instance*, JSCell* array));
unsigned pushToFullVector(VM&, JSArray*, JSValue, unsigned expected);
void putArrayIndex(JSGlobalObject*, JSArray*, uint32_t index, JSValue, bool isStrict);
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGet, Nothing, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPut, Heap, void, (Instance*, JSCell* array, EncodedJSValue key, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStoreElement, Heap, void, (Instance*, JSCell* array, int64_t index, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStoreNumberElement, Heap, void, (Instance*, JSCell* array, int64_t index, double value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGetAtNumber, Nothing, EncodedJSValue, (Instance*, JSCell* array, double key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGetAtIndex, Nothing, EncodedJSValue, (Instance*, JSCell* array, uint64_t key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGetAtIndexAsNumber, Nothing, double, (Instance*, JSCell* array, uint64_t key, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayElementAs, RuntimeState, EncodedJSValue, (Instance*, JSCell* array, uint64_t key, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayElementAtNumberAs, RuntimeState, EncodedJSValue, (Instance*, JSCell* array, double key, uint32_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPutAtIndex, Heap, void, (Instance*, JSCell* array, EncodedJSValue value, uint64_t key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPutAtIndexExpecting, Heap, void, (Instance*, JSCell* array, EncodedJSValue value, uint64_t key, uint64_t expectedAndSite));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPutNumberAtIndex, Heap, void, (Instance*, JSCell* array, EncodedJSValue value, uint64_t key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPutAtNumber, Heap, void, (Instance*, JSCell* array, EncodedJSValue value, double key));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedArrayTryMakeWritableDouble, Nothing, size_t, (Instance*, JSCell* array));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryAddNamedProperties, Heap, size_t, (Instance*, JSCell* array, uint32_t count, const EncodedJSValue* valuesThenNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedOpenModifiedArray, Nothing, void, (Instance*, JSCell* array));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTypedArrayIteratorOpen, Nothing, void, (Instance*, JSCell* view));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTypedArrayIteratorHasNext, Nothing, size_t, (Instance*, JSCell* view, uint64_t index));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBuiltinCell, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceToType, Nothing, EncodedJSValue, (Instance*, EncodedJSValue value, uint64_t type));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewTypedArray, RuntimeState, JSCell*, (Instance*, uint32_t, double));
JSCell* typedBuiltinCell(Instance*, unsigned);
JSValue callEngineBuiltin(Instance*, unsigned function, JSValue thisValue, const ArgList&);
Structure* structureAfterPropertyRun(Instance*, VM&, Structure*, std::span<UniquedStringImpl* const> names);
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypedArrayPut, TypedArrayProperties, void, (Instance*, JSCell* view, uint64_t index, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedClosureConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCreateThis, RuntimeState, JSCell*, (Instance*, JSCell* constructor, JSCell* newTarget));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConcat, RuntimeState, JSCell*, (Instance*, JSCell*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConcatThree, RuntimeState, JSCell*, (Instance*, JSCell*, JSCell*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSubstring, RuntimeState, JSCell*, (Instance*, JSCell*, int32_t, int32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedRopeCodeUnit, RuntimeState, size_t, (Instance*, JSCell*, int64_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFlatten, RuntimeState, void*, (Instance*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringEq, RuntimeState, size_t, (Instance*, JSCell*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStringEqualsLiteralInRegister, Nothing, size_t, (Instance*, JSCell*, uint64_t text));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringHasAt, RuntimeState, size_t, (Instance*, JSCell* string, JSCell* other, int32_t place));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringIndexOf, RuntimeState, size_t, (Instance*, JSCell* string, JSCell* other, int32_t place));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIntToString, Nothing, JSCell*, (Instance*, uint64_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDoubleToString, Nothing, JSCell*, (Instance*, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCharToString, Nothing, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewMap, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryNewStringSet, RuntimeState, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryNewMapWithStringKeys, RuntimeState, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewSetInImage, RuntimeState, JSCell*, (Instance*, uint32_t table));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewMapInImage, RuntimeState, JSCell*, (Instance*, uint32_t table));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedMoveTableToHeap, RuntimeState, JSCell*, (Instance*, JSCell* collection));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterOpen, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterHas, Heap, size_t, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIterGet, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterClose, Heap, void, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterCloseForThrow, Heap, void, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterOpenInFrame, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterHasInFrame, Heap, size_t, (Instance*, EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFinishIterStepInFrame, Heap, size_t, (Instance*, EncodedJSValue*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFinishIterStep, Heap, size_t, (Instance*, EncodedJSValue state, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFinishIterOpenInFrame, Heap, EncodedJSValue, (Instance*, EncodedJSValue iterator, EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterOpenInFrameWithMethod, Heap, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue*, EncodedJSValue method));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterCloseInFrame, Heap, void, (Instance*, EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterCloseForThrowInFrame, Heap, void, (Instance*, EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedReadBatchedElements, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue*));

#include "AOTTypedBuiltinOperations.inc"

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
