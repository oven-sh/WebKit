/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "CCallHelpers.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

#define FOR_EACH_AOT_OPERATION_WITH_FRONT_END(v) \
    v(GetById) \
    v(GetByVal) \
    v(PutById) \
    v(PutByVal) \
    v(NewObjectLiteral) \
    v(CompareStrictEq) \
    v(CompareEq) \

static constexpr ptrdiff_t offsetOfInstanceRegisterInAdapter = -24;
static constexpr ptrdiff_t offsetOfNumberTagRegisterInAdapter = -16;
static constexpr ptrdiff_t offsetOfNotCellMaskRegisterInAdapter = -8;
#if CPU(X86_64)
static constexpr ptrdiff_t offsetOfR12InAdapter = -32;
static constexpr ptrdiff_t offsetOfRBXInAdapter = -40;
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -48;
static constexpr unsigned adapterSaveAreaSize = 48;
#else
static constexpr ptrdiff_t offsetOfCellCageBaseRegisterInAdapter = -32;
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -40;
static constexpr unsigned adapterSaveAreaSize = 48;
#endif

static constexpr unsigned minCodeGranuleShift = 6;
static constexpr unsigned maxCodeGranuleShift = 12;

#define FOR_EACH_AOT_OPERATION_WITH_HELPER(v) \
    v(NewArray, operationAOTNewArray) \
    v(ValueAdd, operationAOTValueAdd) \

#define FOR_EACH_AOT_HELPER(v) \
    v(HelperNewArray) \
    v(HelperNewInt32Array) \
    v(HelperAddStrings) \

#define FOR_EACH_AOT_STUB(v) \
    v(ReturnFromCallWithList) \
    v(Call) \
    v(CallHostFunction) \
    v(CallInternalFunction) \
    v(ConstructInternalFunction) \
    v(CallList) \
    v(TailCallList) \
    v(CheckCallable) \
    v(CheckCallableNamed) \
    v(FindMethodByName) \
    v(FindMethodCellByName) \
    v(FindMethodByNameAndFillSiteCache) \
    v(Enter) \
    v(EnterModule) \
    v(EnterClosure) \
    v(EnterSlimClosure) \
    v(EnterResume) \
    v(EnterSavedFrame) \
    v(RestoreSavedFrameToStack) \
    v(EnterAsyncGenerator) \
    v(CallValue) \
    v(CallValueWithoutThis) \
    v(CallValueNamed) \
    v(CallMethod) \
    v(CallMethodReadThroughGetter) \
    v(CallMethodReadPastEntries) \
    v(CallMethodRead) \
    v(CallMethodByName) \
    v(ApplyArray) \
    v(SpreadArray) \
    v(ConstructClosure) \
    v(EnterFunctionForCall) \
    v(EnterFunctionForConstruct) \
    v(CallBoundFunction) \
    v(OperationValue) \
    v(OperationVoid) \
    v(OperationDouble) \
    v(OperationValueWithGlobalObject) \
    v(OperationValueWithInstance) \
    v(OperationVoidWithInstance) \
    v(OperationDoubleWithInstance) \
    v(UncheckedOperationWithInstance) \
    v(UncheckedOperationDoubleWithInstance) \
    v(ThrowAtSite) \
    v(LeafThrowAtSite) \
    v(ThrowAtSiteByWord) \
    v(LeafThrowAtSiteByWord) \
    v(ThrowAtFault) \
    v(LeafThrowAtFault) \
    v(ThrowAtTrap) \
    v(LeafThrowAtTrap) \
    v(ColdOperationVoid) \
    v(LeafColdOperationVoid) \
    v(ColdOperationVoidWithoutFPRs) \
    v(LeafColdOperationVoidWithoutFPRs) \
    v(ColdOperationValue) \
    v(LeafColdOperationValue) \
    v(ColdOperationValueWithoutFPRs) \
    v(LeafColdOperationValueWithoutFPRs) \
    v(ColdPlainOperation) \
    v(LeafColdPlainOperation) \
    v(ColdPlainOperationWithoutFPRs) \
    v(LeafColdPlainOperationWithoutFPRs) \
    v(ColdPlainOperationWithoutInstance) \
    v(LeafColdPlainOperationWithoutInstance) \
    v(ColdPlainOperationDouble) \
    v(LeafColdPlainOperationDouble) \
    v(ColdPlainOperationDoubleWithoutInstance) \
    v(LeafColdPlainOperationDoubleWithoutInstance) \
    v(ColdOperationDouble) \
    v(LeafColdOperationDouble) \
    v(ColdCallFunction) \
    v(LeafColdCallFunction) \
    v(ColdCallFunctionForValue) \
    v(LeafColdCallFunctionForValue) \
    v(PlainOperation) \
    v(PlainOperationWithInstance) \
    v(DidCreateTypedField) \
    v(LeafDidCreateTypedField) \
    v(CreateTypedFieldOutOfLine) \
    v(LeafCreateTypedFieldOutOfLine) \
    v(DidCreateTypedFieldRun) \
    v(LeafDidCreateTypedFieldRun) \
    v(PutPropertiesWithStorage) \
    v(RememberElement) \
    v(LeafRememberElement) \
    v(AllocateTypedCell) \
    v(LeafAllocateTypedCell) \
    v(AllocateTypedObject) \
    v(LeafAllocateTypedObject) \
    v(AllocateTypedObjectFromTemplate) \
    v(LeafAllocateTypedObjectFromTemplate) \
    v(CopyTypedObject16) \
    v(LeafCopyTypedObject16) \
    v(CopyTypedObject32) \
    v(LeafCopyTypedObject32) \
    v(CopyTypedObject48) \
    v(LeafCopyTypedObject48) \
    v(CopyTypedObject64) \
    v(LeafCopyTypedObject64) \
    v(CopyTypedObjectAnySize) \
    v(LeafCopyTypedObjectAnySize) \
    v(LookupCopyKeyOrder) \
    v(LeafLookupCopyKeyOrder) \
    v(AllocateTypedExtension16) \
    v(AllocateTypedExtension32) \
    v(AllocateTypedExtension48) \
    v(AllocateTypedExtension64) \
    v(AllocateTypedExtension80) \
    v(AllocateTypedExtension96) \
    v(AllocateTypedExtension112) \
    v(AllocateTypedExtension128) \
    v(AllocateTypedExtension144) \
    v(AllocateTypedExtension160) \
    v(AllocateTypedExtension176) \
    v(AllocateTypedExtension192) \
    v(AllocateTypedExtension208) \
    v(AllocateTypedExtension224) \
    v(AllocateTypedExtension240) \
    v(AllocateTypedExtension256) \
    v(NewTypedClosure0) \
    v(LeafNewTypedClosure0) \
    v(NewTypedClosure1) \
    v(LeafNewTypedClosure1) \
    v(NewTypedClosure2) \
    v(LeafNewTypedClosure2) \
    v(NewTypedClosure3) \
    v(LeafNewTypedClosure3) \
    v(NewTypedClosure4) \
    v(LeafNewTypedClosure4) \
    v(NewTypedClosureOnce) \
    v(LeafNewTypedClosureOnce) \
    v(NewTypedClosureOnce32) \
    v(LeafNewTypedClosureOnce32) \
    v(NewTypedClosureWithoutCaptureOnce32) \
    v(LeafNewTypedClosureWithoutCaptureOnce32) \
    v(InitializeTypedClosure0) \
    v(LeafInitializeTypedClosure0) \
    v(InitializeTypedClosure1) \
    v(LeafInitializeTypedClosure1) \
    v(InitializeTypedClosure2) \
    v(LeafInitializeTypedClosure2) \
    v(InitializeTypedClosure3) \
    v(LeafInitializeTypedClosure3) \
    v(InitializeTypedClosure4) \
    v(LeafInitializeTypedClosure4) \
    v(CopySlimClosure0) \
    v(CopySlimClosure1) \
    v(CopySlimClosure2) \
    v(CopySlimClosure3) \
    v(CopySlimClosure4) \
    v(MaterializeEnvironment) \
    v(LeafMaterializeEnvironment) \
    v(TypedString) \
    v(LeafTypedString) \
    v(CallModuleFunction) \
    v(TypedStringFillingWord) \
    v(TypedStringInRCX) \
    v(TypedStringInRDX) \
    v(TypedStringInRBX) \
    v(TypedStringInRSI) \
    v(TypedStringInRDI) \
    v(TypedStringInR8) \
    v(TypedStringInR9) \
    v(TypedStringInR12) \
    v(TypedStringInR15) \
    v(TypedClosureInRCX) \
    v(TypedClosureInRDX) \
    v(TypedClosureInRBX) \
    v(TypedClosureInRSI) \
    v(TypedClosureInRDI) \
    v(TypedClosureInR8) \
    v(TypedClosureInR9) \
    v(TypedClosureInR12) \
    v(TypedClosureInR15) \
    v(TypedStringCached) \
    v(LeafTypedStringCached) \
    v(TypedStringInBlock1) \
    v(LeafTypedStringInBlock1) \
    v(TypedStringCachedInBlock1) \
    v(LeafTypedStringCachedInBlock1) \
    v(TypedStringInBlock2) \
    v(LeafTypedStringInBlock2) \
    v(TypedStringCachedInBlock2) \
    v(LeafTypedStringCachedInBlock2) \
    v(TypedStringInBlock3) \
    v(LeafTypedStringInBlock3) \
    v(TypedStringCachedInBlock3) \
    v(LeafTypedStringCachedInBlock3) \
    v(TypedStringInBlock4) \
    v(LeafTypedStringInBlock4) \
    v(TypedStringCachedInBlock4) \
    v(LeafTypedStringCachedInBlock4) \
    v(TypedStringInBlock5) \
    v(LeafTypedStringInBlock5) \
    v(TypedStringCachedInBlock5) \
    v(LeafTypedStringCachedInBlock5) \
    v(TypedStringInBlock6) \
    v(LeafTypedStringInBlock6) \
    v(TypedStringCachedInBlock6) \
    v(LeafTypedStringCachedInBlock6) \
    v(TypedStringInBlock7) \
    v(LeafTypedStringInBlock7) \
    v(TypedStringCachedInBlock7) \
    v(LeafTypedStringCachedInBlock7) \
    v(ValueEqualsLiteral) \
    v(LeafValueEqualsLiteral) \
    v(StringEqualsLiteral) \
    v(LeafStringEqualsLiteral) \
    v(LiteralStringToString) \
    v(LeafLiteralStringToString) \
    v(ToLiteralString) \
    v(LeafToLiteralString) \
    v(ShortStringOfValue) \
    v(LeafShortStringOfValue) \
    v(ValueEqualsLiteralOfLength1) \
    v(LeafValueEqualsLiteralOfLength1) \
    v(StringEqualsLiteralOfLength1) \
    v(LeafStringEqualsLiteralOfLength1) \
    v(ValueEqualsLiteralOfLength2) \
    v(LeafValueEqualsLiteralOfLength2) \
    v(StringEqualsLiteralOfLength2) \
    v(LeafStringEqualsLiteralOfLength2) \
    v(ValueEqualsLiteralOfLength3) \
    v(LeafValueEqualsLiteralOfLength3) \
    v(StringEqualsLiteralOfLength3) \
    v(LeafStringEqualsLiteralOfLength3) \
    v(ValueEqualsLiteralOfLength4) \
    v(LeafValueEqualsLiteralOfLength4) \
    v(StringEqualsLiteralOfLength4) \
    v(LeafStringEqualsLiteralOfLength4) \
    v(ValueEqualsLiteralOfLength5) \
    v(LeafValueEqualsLiteralOfLength5) \
    v(StringEqualsLiteralOfLength5) \
    v(LeafStringEqualsLiteralOfLength5) \
    v(ValueEqualsLiteralOfLength6) \
    v(LeafValueEqualsLiteralOfLength6) \
    v(StringEqualsLiteralOfLength6) \
    v(LeafStringEqualsLiteralOfLength6) \
    v(ValueEqualsLiteralOfLength7) \
    v(LeafValueEqualsLiteralOfLength7) \
    v(StringEqualsLiteralOfLength7) \
    v(LeafStringEqualsLiteralOfLength7) \
    v(ValueEqualsLiteralOfLength8) \
    v(LeafValueEqualsLiteralOfLength8) \
    v(StringEqualsLiteralOfLength8) \
    v(LeafStringEqualsLiteralOfLength8) \
    v(StringReplaceRegExpStringMayNotMatch) \
    v(NewRegExpLiteral) \
    v(UncheckedNewRegExpLiteral) \
    v(RegExpExecWithFastPath) \
    v(RegExpTestWithFastPath) \
    v(RegExpFindWithFastPath) \
    v(RegExpTestBoxedWithFastPath) \
    v(StringMatchRegExpWithFastPath) \
    v(StringToLowerCase) \
    v(StringToUpperCase) \
    v(UncheckedStringToLowerCase) \
    v(UncheckedStringToUpperCase) \
    v(StringToNumber8) \
    v(ParseInt8) \
    v(NewTypedArray) \
    v(UncheckedNewTypedArray) \
    v(CoerceToType) \
    v(LeafCoerceToType) \
    v(CastInRegister) \
    v(LeafCastInRegister) \
    v(StringStartsWithLiteral) \
    v(LeafStringStartsWithLiteral) \
    v(StringEndsWithLiteral) \
    v(LeafStringEndsWithLiteral) \
    v(FlatStringsAreEqual) \
    v(LeafFlatStringsAreEqual) \
    v(StringsAreEqual) \
    v(LeafStringsAreEqual) \
    v(ConcatenateTwo) \
    v(LeafConcatenateTwo) \
    v(ConcatenateThree) \
    v(LeafConcatenateThree) \
    v(Int32ToString) \
    v(LeafInt32ToString) \
    v(StringTrim) \
    v(LeafStringTrim) \
    v(StringTrimStart) \
    v(LeafStringTrimStart) \
    v(StringTrimEnd) \
    v(LeafStringTrimEnd) \
    v(StringTrimFlat8) \
    v(LeafStringTrimFlat8) \
    v(StringTrimStartFlat8) \
    v(LeafStringTrimStartFlat8) \
    v(StringTrimEndFlat8) \
    v(LeafStringTrimEndFlat8) \
    v(TypedClosure) \
    v(LeafTypedClosure) \
    v(TypedBuiltinCell) \
    v(LeafTypedBuiltinCell) \
    v(TypedClosureEntry) \
    v(LeafTypedClosureEntry) \
    v(TypedNewPromise) \
    v(LeafTypedNewPromise) \
    v(AwaitMayContinue) \
    v(LeafAwaitMayContinue) \
    v(AwaitBySavingFrame) \
    v(AwaitCallBySavingFrame) \
    v(ResolvePromiseAfterAwait) \
    v(ResolvePromiseOrReturnResult) \
    v(AsyncIteratorNext) \
    v(AwaitInAsyncGenerator) \
    v(AsyncGeneratorYield) \
    v(StepGenerator) \
    v(StepGeneratorUnmodifiedUnlessNext) \
    v(StepGeneratorIfUnmodified) \
    v(StepIfGenerator) \
    v(StepIfUnmodifiedGenerator) \
    v(IterHasInFrame) \
    v(CallValueWithoutArguments) \
    v(ResolveCallValue) \
    v(CallResolvedValue) \
    v(IterOpenInFrame) \
    v(IterOpenPairsInFrame) \
    v(IterOpenTableInFrame) \
    v(IterCloseInFrame) \
    v(ArrayLiteral0) \
    v(LeafArrayLiteral0) \
    v(ArrayLiteral1) \
    v(LeafArrayLiteral1) \
    v(ArrayLiteral2) \
    v(LeafArrayLiteral2) \
    v(ArrayLiteral3) \
    v(LeafArrayLiteral3) \
    v(ArrayLiteral4) \
    v(LeafArrayLiteral4) \
    v(ArrayLiteralWithDouble1) \
    v(LeafArrayLiteralWithDouble1) \
    v(ArrayLiteralWithDouble2) \
    v(LeafArrayLiteralWithDouble2) \
    v(ArrayLiteralWithDouble3) \
    v(LeafArrayLiteralWithDouble3) \
    v(ArrayLiteralWithDouble4) \
    v(LeafArrayLiteralWithDouble4) \
    v(ArrayLiteralWithDoubleSlow) \
    v(LeafArrayLiteralWithDoubleSlow) \
    v(ElementOrEmpty) \
    v(LeafElementOrEmpty) \
    v(NewArrayWithDouble) \
    v(LeafNewArrayWithDouble) \
    v(NewArrayWithInt32) \
    v(LeafNewArrayWithInt32) \
    v(NewEmptyArrayWithDouble) \
    v(LeafNewEmptyArrayWithDouble) \
    v(NewEmptyArrayWithInt32) \
    v(LeafNewEmptyArrayWithInt32) \
    v(NewEmptyArrayWithDoubleCapacity1) \
    v(LeafNewEmptyArrayWithDoubleCapacity1) \
    v(NewEmptyArrayWithDoubleCapacity3) \
    v(LeafNewEmptyArrayWithDoubleCapacity3) \
    v(NewEmptyArrayCapacity1) \
    v(LeafNewEmptyArrayCapacity1) \
    v(NewEmptyArrayCapacity3) \
    v(LeafNewEmptyArrayCapacity3) \
    v(NewEmptyArrayWithCapacity) \
    v(LeafNewEmptyArrayWithCapacity) \
    v(NewArrayWithContiguous) \
    v(LeafNewArrayWithContiguous) \
    v(NewArrayUnderConstructionWithContiguous) \
    v(LeafNewArrayUnderConstructionWithContiguous) \
    v(NewArrayUnderConstructionWithDouble) \
    v(LeafNewArrayUnderConstructionWithDouble) \
    v(NewArrayUnderConstructionWithInt32) \
    v(LeafNewArrayUnderConstructionWithInt32) \
    v(NewEmptyArrayAtSite) \
    v(LeafNewEmptyArrayAtSite) \
    v(PushToFullVector) \
    v(LeafPushToFullVector) \
    v(PushDoubleToFullVector) \
    v(LeafPushDoubleToFullVector) \
    v(Substring) \
    v(LeafSubstring) \
    v(StoreElement) \
    v(LeafStoreElement) \
    v(StoreDoubleElement) \
    v(LeafStoreDoubleElement) \
    v(LoadElement) \
    v(LeafLoadElement) \
    v(LoadElementAsDoubleOrNullish) \
    v(LeafLoadElementAsDoubleOrNullish) \
    v(ToNumberAsDouble) \
    v(LeafToNumberAsDouble) \
    v(ArrayElementAs) \
    v(LeafArrayElementAs) \
    v(ArrayElement) \
    v(LeafArrayElement) \
    v(AbsentFieldOfFlaggedObject) \
    v(LeafAbsentFieldOfFlaggedObject) \
    v(AbsentFieldOfFlaggedObjectChecked) \
    v(LeafAbsentFieldOfFlaggedObjectChecked) \
    v(AbsentFieldOfFlaggedObjectAsNumber) \
    v(LeafAbsentFieldOfFlaggedObjectAsNumber) \
    v(ArraySlice) \
    v(LeafArraySlice) \
    v(ArrayCopy) \
    v(LeafArrayCopy) \
    v(CreateArguments) \
    v(LeafCreateArguments) \
    v(WriteBarrier) \
    v(IterHas) \
    v(ToBoolean) \
    v(ValueIsTruthy) \
    v(SubstringCharacters) \
    v(RopeCharacters) \
    v(LeafRopeCharacters) \
    v(RopeCodeUnit) \
    v(LeafRopeCodeUnit) \
    v(StrictEqual) \
    v(LooseEqual) \
    v(LooseEqualsLiteral) \
    v(Add) \
    v(Mod) \
    v(Sub) \
    v(Mul) \
    v(Div) \
    v(BitAnd) \
    v(BitOr) \
    v(BitXor) \
    v(LShift) \
    v(RShift) \
    v(URShift) \
    v(Less) \
    v(LessEq) \
    v(Greater) \
    v(GreaterEq) \
    v(AddAfterInt32Check) \
    v(SubAfterInt32Check) \
    v(MulAfterInt32Check) \
    v(BitAndAfterInt32Check) \
    v(BitOrAfterInt32Check) \
    v(BitXorAfterInt32Check) \
    v(LShiftAfterInt32Check) \
    v(RShiftAfterInt32Check) \
    v(URShiftAfterInt32Check) \
    v(LessAfterInt32Check) \
    v(LessEqAfterInt32Check) \
    v(GreaterAfterInt32Check) \
    v(GreaterEqAfterInt32Check) \
    v(AddDoubleAsDouble) \
    v(MulDoubleAsDouble) \
    v(SubDoubleAsDouble) \
    v(SubDoubleOnRightAsDouble) \
    v(DivDoubleAsDouble) \
    v(DivDoubleOnRightAsDouble) \
    v(GetByValAtIndex) \
    v(GetByVal) \
    v(PutByValAtIndex) \
    v(PutByVal) \
    v(PutByValAtIndexSloppy) \
    v(PutByValSloppy) \
    v(GetLength) \
    v(GetById) \
    v(PutById) \
    v(PutByIdDouble) \
    v(GetByIdAsDouble) \
    v(GetByIdAsDoubleOrUndefined) \
    v(GetByIdAsDoubleOrNull) \
    v(GetByIdAsDoubleOrNullish) \
    v(GetByIdAsBoolean) \
    v(GetByIdAsBooleanOrUndefined) \
    v(GetByIdAsBooleanOrNull) \
    v(GetByIdAsBooleanOrNullish) \
    v(GetByIdAsString) \
    v(GetByIdAsStringOrUndefined) \
    v(GetByIdAsStringOrNull) \
    v(GetByIdAsStringOrNullish) \
    v(GetByIdAsArray) \
    v(GetByIdAsArrayOrUndefined) \
    v(GetByIdAsArrayOrNull) \
    v(GetByIdAsArrayOrNullish) \
    v(GetByNameAsDouble) \
    v(GetByNameAsDoublePreservingTwo) \
    v(GetByNameAsDoubleOrUndefined) \
    v(GetByNameAsDoubleOrUndefinedPreservingTwo) \
    v(GetByNameAsDoubleOrNull) \
    v(GetByNameAsDoubleOrNullPreservingTwo) \
    v(GetByNameAsDoubleOrNullish) \
    v(GetByNameAsDoubleOrNullishPreservingTwo) \
    v(GetByNameAsBoolean) \
    v(GetByNameAsBooleanPreservingTwo) \
    v(GetByNameAsBooleanOrUndefined) \
    v(GetByNameAsBooleanOrUndefinedPreservingTwo) \
    v(GetByNameAsBooleanOrNull) \
    v(GetByNameAsBooleanOrNullPreservingTwo) \
    v(GetByNameAsBooleanOrNullish) \
    v(GetByNameAsBooleanOrNullishPreservingTwo) \
    v(GetByNameAsString) \
    v(GetByNameAsStringPreservingTwo) \
    v(GetByNameAsStringOrUndefined) \
    v(GetByNameAsStringOrUndefinedPreservingTwo) \
    v(GetByNameAsStringOrNull) \
    v(GetByNameAsStringOrNullPreservingTwo) \
    v(GetByNameAsStringOrNullish) \
    v(GetByNameAsStringOrNullishPreservingTwo) \
    v(GetByNameAsArray) \
    v(GetByNameAsArrayPreservingTwo) \
    v(GetByNameAsArrayOrUndefined) \
    v(GetByNameAsArrayOrUndefinedPreservingTwo) \
    v(GetByNameAsArrayOrNull) \
    v(GetByNameAsArrayOrNullPreservingTwo) \
    v(GetByNameAsArrayOrNullish) \
    v(GetByNameAsArrayOrNullishPreservingTwo) \
    v(GetByNameGeneric) \
    v(GetThroughSlotByName) \
    v(GetThroughAccessorByName) \
    v(GetThroughOwnAccessorByName) \
    v(GetThroughExtraAccessorByName) \
    v(GetThroughKnownCustomGetterByName) \
    v(GetNamespaceExportByName) \
    v(ReadAbsentByName) \
    v(ReadOutOfLineByName) \
    v(ReadInheritedInlineByName) \
    v(ReadInheritedOutOfLineByName) \
    v(ReadFieldCell32ByName) \
    v(ReadFieldInt32ByName) \
    v(ReadFieldBoxedByName) \
    v(ReadFieldDoubleByName) \
    v(ReadFieldBooleanByName) \
    v(ReadFieldOptionalBooleanByName) \
    v(ReadOwnAccessorByName) \
    v(ReadAccessorByName) \
    v(ReadExtraPropertyByName) \
    v(ReadKnownCustomGetterByName) \
    v(ReadLazyClosureByName) \
    v(ReadNamespaceExportByName) \
    v(ReadThroughSlotsByName) \
    v(ReadFieldCell32ByNamePreservingTwo) \
    v(ReadFieldInt32ByNamePreservingTwo) \
    v(ReadFieldBoxedByNamePreservingTwo) \
    v(ReadFieldDoubleByNamePreservingTwo) \
    v(ReadFieldBooleanByNamePreservingTwo) \
    v(ReadFieldOptionalBooleanByNamePreservingTwo) \
    v(ReadOwnAccessorByNamePreservingTwo) \
    v(ReadAccessorByNamePreservingTwo) \
    v(ReadExtraPropertyByNamePreservingTwo) \
    v(ReadKnownCustomGetterByNamePreservingTwo) \
    v(ReadLazyClosureByNamePreservingTwo) \
    v(ReadNamespaceExportByNamePreservingTwo) \
    v(ReadThroughSlotsByNamePreservingTwo) \
    v(ReadFieldCell32ByNameWithinFrame) \
    v(ReadFieldInt32ByNameWithinFrame) \
    v(ReadFieldBoxedByNameWithinFrame) \
    v(ReadFieldDoubleByNameWithinFrame) \
    v(ReadFieldBooleanByNameWithinFrame) \
    v(ReadFieldOptionalBooleanByNameWithinFrame) \
    v(ReadOwnAccessorByNameWithinFrame) \
    v(ReadAccessorByNameWithinFrame) \
    v(ReadExtraPropertyByNameWithinFrame) \
    v(ReadKnownCustomGetterByNameWithinFrame) \
    v(ReadLazyClosureByNameWithinFrame) \
    v(ReadNamespaceExportByNameWithinFrame) \
    v(ReadThroughSlotsByNameWithinFrame) \
    v(GetByNameWithinFrameAfterFirstProbe) \
    v(GetByName) \
    v(GetByNamePreservingTwo) \
    v(GetByNameWithinFrame) \
    v(GetByNameWithDirectColumn) \
    v(GetByNameWithDirectColumnPreservingTwo) \
    v(GetByNameWithReceiverInRCX) \
    v(GetByNameWithReceiverInRDX) \
    v(GetByNameWithReceiverInRBX) \
    v(GetByNameWithReceiverInRAX) \
    v(GetByNameWithReceiverInRDI) \
    v(GetByNameWithReceiverInR8) \
    v(GetByNameWithReceiverInR15) \
    v(GetByNamePreservingTwoWithReceiverInRCX) \
    v(GetByNamePreservingTwoWithReceiverInRDX) \
    v(GetByNamePreservingTwoWithReceiverInRBX) \
    v(GetByNamePreservingTwoWithReceiverInRAX) \
    v(GetByNamePreservingTwoWithReceiverInRDI) \
    v(GetByNamePreservingTwoWithReceiverInR8) \
    v(GetByNamePreservingTwoWithReceiverInR15) \
    v(GetByNameWithDirectColumnWithReceiverInRCX) \
    v(GetByNameWithDirectColumnWithReceiverInRDX) \
    v(GetByNameWithDirectColumnWithReceiverInRBX) \
    v(GetByNameWithDirectColumnWithReceiverInRAX) \
    v(GetByNameWithDirectColumnWithReceiverInRDI) \
    v(GetByNameWithDirectColumnWithReceiverInR8) \
    v(GetByNameWithDirectColumnWithReceiverInR15) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInRCX) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInRDX) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInRBX) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInRAX) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInRDI) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInR8) \
    v(GetByNameWithDirectColumnPreservingTwoWithReceiverInR15) \
    v(GetByNameAndFillSiteCache) \
    v(GetByNameAndFillSiteCachePreservingTwo) \
    v(GetByNameChain) \
    v(GetByNameChainPreservingTwo) \
    v(GetByNameChainReturningBoth) \
    v(GetTwoByName) \
    v(GetByNameFromTwo) \
    v(GetByNameIsTruthy) \
    v(GetByNameIsTruthyPreservingTwo) \
    v(GetLengthByName) \
    v(GetLengthByNamePreservingTwo) \
    v(GetParentDataByName) \
    v(AddInlineByName) \
    v(AddOutOfLineByName) \
    v(ReplaceOutOfLineByName) \
    v(PutByNameGeneric) \
    v(PutThroughSlotByName) \
    v(PutThroughAccessorByName) \
    v(PutByNameWithNewStorage) \
    v(PutByNameAddsStorage) \
    v(PutByNameAddsStoragePreservingTwo) \
    v(PutByNameThroughSlots) \
    v(PutByNameThroughSlotsPreservingTwo) \
    v(PutByNameThroughAccessor) \
    v(PutByNameThroughAccessorPreservingTwo) \
    v(PutByNameSetsArrayLength) \
    v(PutByNameSetsArrayLengthPreservingTwo) \
    v(PutByNameStoresExtraProperty) \
    v(PutByNameStoresExtraPropertyPreservingTwo) \
    v(PutByNameStoresFieldCell32) \
    v(PutByNameStoresFieldCell32PreservingTwo) \
    v(PutByNameStoresFieldInt32) \
    v(PutByNameStoresFieldInt32PreservingTwo) \
    v(PutByNameStoresFieldBoxed) \
    v(PutByNameStoresFieldBoxedPreservingTwo) \
    v(PutByNameStoresFieldDouble) \
    v(PutByNameStoresFieldDoublePreservingTwo) \
    v(PutByNameStoresFieldBoolean) \
    v(PutByNameStoresFieldBooleanPreservingTwo) \
    v(PutByName) \
    v(PutByNamePreservingTwo) \
    v(PutByNameWithDirectColumn) \
    v(PutByNameWithDirectColumnPreservingTwo) \
    v(PutByNameSloppyGeneric) \
    v(PutThroughSlotByNameSloppy) \
    v(PutThroughAccessorByNameSloppy) \
    v(PutByNameSloppyWithNewStorage) \
    v(PutByNameSloppyAddsStorage) \
    v(PutByNameSloppyAddsStoragePreservingTwo) \
    v(PutByNameSloppyThroughSlots) \
    v(PutByNameSloppyThroughSlotsPreservingTwo) \
    v(PutByNameSloppyThroughAccessor) \
    v(PutByNameSloppyThroughAccessorPreservingTwo) \
    v(PutByNameSloppySetsArrayLength) \
    v(PutByNameSloppySetsArrayLengthPreservingTwo) \
    v(PutByNameSloppyStoresExtraProperty) \
    v(PutByNameSloppyStoresExtraPropertyPreservingTwo) \
    v(PutByNameSloppyStoresFieldCell32) \
    v(PutByNameSloppyStoresFieldCell32PreservingTwo) \
    v(PutByNameSloppyStoresFieldInt32) \
    v(PutByNameSloppyStoresFieldInt32PreservingTwo) \
    v(PutByNameSloppyStoresFieldBoxed) \
    v(PutByNameSloppyStoresFieldBoxedPreservingTwo) \
    v(PutByNameSloppyStoresFieldDouble) \
    v(PutByNameSloppyStoresFieldDoublePreservingTwo) \
    v(PutByNameSloppyStoresFieldBoolean) \
    v(PutByNameSloppyStoresFieldBooleanPreservingTwo) \
    v(PutByNameSloppy) \
    v(PutByNameSloppyPreservingTwo) \
    v(PutByNameSloppyWithDirectColumn) \
    v(PutByNameSloppyWithDirectColumnPreservingTwo) \
    v(PutByNameAndFillSiteCache) \
    v(PutByNameAndFillSiteCachePreservingTwo) \
    v(PutByNameSloppyAndFillSiteCache) \
    v(PutByNameSloppyAndFillSiteCachePreservingTwo) \
    v(DefineByNameGeneric) \
    v(DefineThroughSlotByName) \
    v(DefineByNameWithNewStorage) \
    v(DefineByNameAddsStorage) \
    v(DefineByNameAddsStoragePreservingTwo) \
    v(DefineByNameThroughSlots) \
    v(DefineByNameThroughSlotsPreservingTwo) \
    v(DefineByNameStoresExtraProperty) \
    v(DefineByNameStoresExtraPropertyPreservingTwo) \
    v(DefineByName) \
    v(DefineByNamePreservingTwo) \
    v(DefineByNameWithDirectColumn) \
    v(DefineByNameWithDirectColumnPreservingTwo) \
    v(TryGetByNameAndStructure) \
    v(GetByNameUncached) \
    v(TryPutByNameAndStructure) \
    v(PutByNameUncached) \
    v(PutByNameUncachedSloppy) \
    v(DefineOwnProperty) \
    v(GetByValCached) \
    v(GetByValCachedOfCells) \
    v(GetOwnByValIfCached) \
    v(PutByValCached) \
    v(PutByValCachedSloppy) \
    v(PrivateGet) \
    v(PrivateSet) \
    v(PrivateGetAtSite) \
    v(PrivateSetAtSite) \
    v(PrivateHas) \
    v(PrivateFieldAdd) \
    v(DefineByKeyCached) \
    v(PutByValDirectCached) \
    v(DefineGetterCached) \
    v(DefineSetterCached) \
    v(ToPropertyKey) \
    v(PrivateMethodsAdd) \
    v(InByValCached) \
    v(DeleteByValCached) \
    v(DeleteByValCachedSloppy) \
    v(InstanceOfEngineClass) \
    v(InstanceOfCached) \
    v(CallHasInstance) \
    v(InstanceOfClass) \
    v(InstanceOfLeafClass) \
    v(InstanceOfClassCheckingPrototype) \
    v(InstanceOfLeafClassCheckingPrototype) \
    v(InstanceOfClassByChain) \
    v(MapGetCheckingReceiver) \
    v(MapGet) \
    v(MapHasCheckingReceiver) \
    v(MapHas) \
    v(MapSetCheckingReceiver) \
    v(MapSet) \
    v(SetHasCheckingReceiver) \
    v(SetHas) \
    v(SetAddCheckingReceiver) \
    v(SetAdd) \
    v(MapSetWithStringKeyCheckingReceiver) \
    v(MapSetWithStringKey) \
    v(SetAddWithStringKeyCheckingReceiver) \
    v(SetAddWithStringKey) \
    v(MapGetWithStringKeyCheckingReceiver) \
    v(MapGetWithStringKey) \
    v(MapHasWithStringKeyCheckingReceiver) \
    v(MapHasWithStringKey) \
    v(SetHasWithStringKeyCheckingReceiver) \
    v(SetHasWithStringKey) \
    v(MapDelete) \
    v(SetDelete) \
    v(MapGetByBits) \
    v(MapHasByBits) \
    v(MapSetByBits) \
    v(SetHasByBits) \
    v(SetAddByBits) \
    v(WeakMapGet) \
    v(WeakMapHas) \
    v(WeakSetHas) \
    v(WeakMapSet) \
    v(StringStartsWith) \
    v(StringIndexOf) \
    v(StringFindCharacter) \
    v(StringFindLiteral) \
    v(StringFindString) \
    v(ArrayIncludes) \
    v(ArrayIndexOf) \
    v(ArrayIndexOfFrom) \
    v(ArrayIncludesFrom) \
    v(ArrayIncludesOrCall) \
    v(ArrayIndexOfOrCall) \
    v(ArrayIncludesOrCallMethodRead) \
    v(ArrayIndexOfOrCallMethodRead) \
    v(ArrayPushOrCallMethod) \
    v(ArrayPushOrCallValue) \
    v(MapGetOrCallMethod) \
    v(MapGetOrCallValue) \
    v(MapHasOrCallMethod) \
    v(MapHasOrCallValue) \
    v(MapSetOrCallMethod) \
    v(MapSetOrCallValue) \
    v(SetHasOrCallMethod) \
    v(SetHasOrCallValue) \
    v(SetAddOrCallMethod) \
    v(SetAddOrCallValue) \
    v(RegExpExecOrCallMethod) \
    v(RegExpExecOrCallValue) \
    v(RegExpTestOrCallMethod) \
    v(RegExpTestOrCallValue) \
    v(StringMatchOrCallMethod) \
    v(StringMatchOrCallValue) \
    v(ReadBatchedElements) \
    v(SuspendAtFirstAwait) \
    v(AppendSpread) \
    v(HandleException) \
    v(HandleExceptionPreservingRegisters) \
    v(PollAtPrologue) \
    v(PollAtLoop) \
    v(LeafPollAtLoop) \
    v(PollBeforeEntering) \
    v(HandleExceptionAtPrologue) \
    v(ThrowStackOverflowAtPrologue) \
    v(ThrowStackOverflow) \
    v(Catch) \
    v(RejectInFrame) \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \
    v(FrontEndGetById) \
    v(FrontEndGetByVal) \
    v(FrontEndPutById) \
    v(FrontEndPutByVal) \
    v(FrontEndNewObjectLiteral) \
    v(FrontEndCompareStrictEq) \
    v(FrontEndCompareEq) \
    v(NewMap) \
    v(FrontEndTypedPutExtra) \
    v(FrontEndTypedGetExtra) \
    v(FrontEndTypedGetByNameChecked) \
    v(FrontEndTypedGetByNameCheckedAsNumber) \
    v(FrontEndTypedHasOwnPropertyByName) \
    v(FrontEndTypedToString) \
    v(FrontEndTypedBitNot) \
    v(FrontEndTypedConstruct) \
    v(FrontEndTypedCloneObject) \
    v(FrontEndTypedCloneObjectAndExtend) \
    v(FrontEndTypedCloneObjectAndExtendWithNumber) \
    v(FrontEndTypedCloneObjectWithSubtypeCheck) \
    v(FrontEndTypedCloneObjectAs) \
    v(FrontEndTypedCoerce) \
    v(ConvertByPlan) \
    v(FrontEndTypedConstructSuper) \
    FOR_EACH_AOT_HELPER(v) \
    v(NewArrayWithFastPath) \
    v(ValueAddWithFastPath) \
    v(NewObjectLiteral0) \
    v(NewObjectLiteral1) \
    v(NewObjectLiteral2) \
    v(NewObjectLiteral3) \
    v(NewObjectLiteral4) \
    v(NewObjectLiteral5) \
    v(NewObjectLiteral6) \
    v(NewObjectFromDescriptor0) \
    v(NewObjectFromDescriptor1) \
    v(NewObjectFromDescriptor2) \
    v(NewObjectFromDescriptor3) \
    v(NewObjectFromDescriptor4) \
    v(NewObjectFromDescriptor5) \
    v(NewObjectFromDescriptor6) \
    v(CreateThisByNewTarget) \
    v(ConstructClosureList) \
    v(BoxNumber) \
    v(BoxOptionalNumber) \
    v(UnboxNumberArgument0ToFPR0) \
    v(LeafUnboxNumberArgument0ToFPR0) \
    v(UnboxNumberArgument1ToFPR0) \
    v(LeafUnboxNumberArgument1ToFPR0) \
    v(UnboxNumberArgument1ToFPR1) \
    v(LeafUnboxNumberArgument1ToFPR1) \
    v(UnboxNumberArgument2ToFPR0) \
    v(LeafUnboxNumberArgument2ToFPR0) \
    v(UnboxNumberArgument2ToFPR1) \
    v(LeafUnboxNumberArgument2ToFPR1) \
    v(UnboxNumberArgument2ToFPR2) \
    v(LeafUnboxNumberArgument2ToFPR2) \
    v(UnboxNumberArgument3ToFPR0) \
    v(LeafUnboxNumberArgument3ToFPR0) \
    v(UnboxNumberArgument3ToFPR1) \
    v(LeafUnboxNumberArgument3ToFPR1) \
    v(UnboxNumberArgument3ToFPR2) \
    v(LeafUnboxNumberArgument3ToFPR2) \
    v(UnboxNumberArgument3ToFPR3) \
    v(LeafUnboxNumberArgument3ToFPR3) \

enum class Stub : uint16_t {
#define AOT_DEFINE_STUB(name) name,
    FOR_EACH_AOT_STUB(AOT_DEFINE_STUB)
#undef AOT_DEFINE_STUB
    NumberOfStubs
};
static constexpr unsigned numberOfStubs = static_cast<unsigned>(Stub::NumberOfStubs);
static constexpr unsigned numberOfTypedStringBlocks = 8;
static constexpr unsigned typedStringStubsPerBlock = 4;
static_assert(static_cast<unsigned>(Stub::TypedStringInBlock7) == static_cast<unsigned>(Stub::TypedStringInBlock1) + typedStringStubsPerBlock * (numberOfTypedStringBlocks - 2));
static_assert(static_cast<unsigned>(Stub::TypedStringCachedInBlock7) == static_cast<unsigned>(Stub::TypedStringCachedInBlock1) + typedStringStubsPerBlock * (numberOfTypedStringBlocks - 2));
ASCIILiteral nameOf(Stub);
static constexpr Stub checkingReceiver(Stub stub) { return static_cast<Stub>(static_cast<unsigned>(stub) - 1); }
static_assert(checkingReceiver(Stub::MapGet) == Stub::MapGetCheckingReceiver && checkingReceiver(Stub::MapHas) == Stub::MapHasCheckingReceiver && checkingReceiver(Stub::MapSet) == Stub::MapSetCheckingReceiver && checkingReceiver(Stub::SetHas) == Stub::SetHasCheckingReceiver && checkingReceiver(Stub::SetAdd) == Stub::SetAddCheckingReceiver && checkingReceiver(Stub::MapSetWithStringKey) == Stub::MapSetWithStringKeyCheckingReceiver && checkingReceiver(Stub::SetAddWithStringKey) == Stub::SetAddWithStringKeyCheckingReceiver && checkingReceiver(Stub::MapGetWithStringKey) == Stub::MapGetWithStringKeyCheckingReceiver && checkingReceiver(Stub::MapHasWithStringKey) == Stub::MapHasWithStringKeyCheckingReceiver && checkingReceiver(Stub::SetHasWithStringKey) == Stub::SetHasWithStringKeyCheckingReceiver);
static constexpr bool isHelper(Stub stub) { return stub >= Stub::HelperNewArray && stub <= Stub::HelperAddStrings; }
static constexpr unsigned maxArgumentsUnboxedByStub = 4;
static constexpr Stub unboxNumberArgumentStub(unsigned argument, unsigned fpr, bool isLeaf) { return static_cast<Stub>(static_cast<unsigned>(Stub::UnboxNumberArgument0ToFPR0) + 2 * (argument * (argument + 1) / 2 + fpr) + isLeaf); }
static_assert(unboxNumberArgumentStub(maxArgumentsUnboxedByStub - 1, maxArgumentsUnboxedByStub - 1, true) == Stub::LeafUnboxNumberArgument3ToFPR3 && unboxNumberArgumentStub(2, 1, false) == Stub::UnboxNumberArgument2ToFPR1);
static_assert(maxLiteralPropertiesInRegisters <= 6 && maxArrayElementsInRegisters <= 4);
static constexpr unsigned numberArrayCapacityShift = 8;
static constexpr uint32_t numberArrayIsUnderConstruction = 0x100;
static constexpr unsigned arraySiteAllocationCountShift = 8;
static constexpr uint32_t arraySiteInterval = 64;
static constexpr unsigned maxNumberArrayCapacity = 127;
static constexpr Stub arrayLiteralStub(unsigned count) { return static_cast<Stub>(static_cast<unsigned>(Stub::ArrayLiteral0) + 2 * count); }
static constexpr Stub doubleArrayLiteralStub(unsigned count) { return static_cast<Stub>(static_cast<unsigned>(Stub::ArrayLiteralWithDouble1) + 2 * (count - 1)); }
static constexpr unsigned arrayLiteralWithDoubleIsNumberMaskShift = 8;


struct Site {
    static constexpr unsigned identifierBits = 20;
    static constexpr unsigned extraBits = 32 - identifierBits;
    static bool fits(unsigned identifier, unsigned extra) { return identifier < (1u << identifierBits) && extra < (1u << extraBits); }

    static constexpr unsigned storesLazyClosure = 4;
    static constexpr unsigned hasThisStructure = 8;

    uint32_t identifierAndExtra { 0 };
};

struct ImageKey {
    uint32_t module { 0 };
    uint32_t start { 0 };
    uint32_t kind { 0 };
    uint32_t record { 0 };

    static constexpr uint32_t typedBody = 1u << 16;
    static constexpr uint32_t typedFunction = 1u << 17;
    static constexpr uint32_t isEntryInsideFunction = 1u << 31;
    bool sameFunction(const ImageKey& other) const { return module == other.module && start == other.start && kind == other.kind; }
    unsigned hash() const
    {
        uint64_t h = (static_cast<uint64_t>(module) << 32 | start) * 0x9e3779b97f4a7c15ULL;
        h ^= (h >> 29) + kind * 0x85ebca6bU;
        h *= 0xbf58476d1ce4e5b9ULL;
        return static_cast<unsigned>(h >> 32);
    }
};
static_assert(sizeof(ImageKey) == 16);

struct StubCall {
    static constexpr uint32_t noFunction = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noCallSite = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noArgumentMoves = std::numeric_limits<uint32_t>::max();
    static constexpr uint64_t noImmediate = std::numeric_limits<uint64_t>::max();

    uint32_t offset;
    Stub stub;
    bool isTailCall;
    uint16_t thunk { 0 };
    uint32_t function { noFunction };
    uint32_t callSite { noCallSite };
    uint32_t argumentMoves { noArgumentMoves };
    uint32_t body { 0 };
    uint8_t firstOmittedGPR { 0 };
    uint8_t endOfOmittedGPRs { 0 };
    uint32_t firstReadSite { noCallSite };

    static constexpr uint8_t firstOmittedGPRForPreambleWithThis = 0xfd;
    static constexpr uint8_t firstOmittedGPRForPreamble = 0xfe;
    static constexpr uint8_t endOfOmittedGPRsForPreamble = 0xff;
    bool entersPreambleWithThis() const { return firstOmittedGPR == firstOmittedGPRForPreambleWithThis && endOfOmittedGPRs == endOfOmittedGPRsForPreamble; }
    bool entersPreamble() const { return (firstOmittedGPR == firstOmittedGPRForPreamble && endOfOmittedGPRs == endOfOmittedGPRsForPreamble) || entersPreambleWithThis(); }
};

std::optional<unsigned> thunkFor(Stub, uint32_t t9Value);
void setTypedStringsWithWord(Vector<uint32_t>&& keysInOrder);
std::optional<unsigned> indexOfTypedStringWithWord(uint32_t key);

enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant, KindCheck, ShapeCheck };
namespace KindCheck {
static constexpr uint64_t Undefined = 1 << 0;
static constexpr uint64_t Null = 1 << 1;
static constexpr uint64_t Boolean = 1 << 2;
static constexpr uint64_t Number = 1 << 3;
static constexpr uint64_t Object = 1 << 4;
static constexpr uint64_t Function = 1 << 5;
static constexpr uint64_t ChangedArray = 1 << 6;
static constexpr unsigned typesShift = 8;
static constexpr unsigned maxTypes = 4;
}
using ArgumentMoves = Vector<uint64_t, 8>;
inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}
struct ArgumentThunks {
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<StubCall> jumps;
};
ArgumentThunks generateArgumentThunks(const Vector<const ArgumentMoves*>&);
Vector<uint8_t> generateSignatureAdapterPreamble(uint32_t signatureIndex, size_t& sizeBeforeEntryWithThis);
Vector<uint8_t> generateResultAdapterPreamble(uint32_t result, uint32_t sizeOfResults, unsigned numberOfArguments, size_t& sizeBeforeEntryWithThis);
void jumpUnlessCellPassesChecks(CCallHelpers&, GPRReg value, GPRReg scratch, const ArgumentMoves& tests, CCallHelpers::JumpList& doesNotFit, CCallHelpers::JumpList* passJumps = nullptr);
bool cellChecksNeedScratch(const ArgumentMoves& tests);
GPRReg castInRegisterWordGPR();
namespace ShapeCheck {
static constexpr unsigned countShift = 32;
static constexpr uint64_t maxCount = (1ull << 23) - 1;
static constexpr unsigned passesEarlierBlocksShift = 55;
static constexpr unsigned blockByteShift = 56;
}
namespace CastInRegister {
static constexpr unsigned operationShift = 32;
static constexpr unsigned registerShift = 56;
static constexpr unsigned returnsValueShift = 48;
static constexpr uint64_t returnsValue = 1ull << returnsValueShift;
inline uint64_t word(uint32_t argument, unsigned offsetOfOperation, bool operationReturnsValue, GPRReg held)
{
    return argument | static_cast<uint64_t>(offsetOfOperation) << operationShift | (operationReturnsValue ? returnsValue : 0) | static_cast<uint64_t>(held) << registerShift;
}
}
struct OutlinedSite {
    uint32_t offset;
    uint32_t length;
    uint32_t body;
};
struct OutlinedBody {
    uint32_t function;
    uint32_t offset;
    uint32_t length;
    uint32_t numberOfPlaces;
    uint32_t singleCaller;
    bool endsInCall;
};
struct Outlining {
    static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    Vector<Vector<OutlinedSite>> places;
    Vector<OutlinedBody> bodies;
};
struct OutlinedBodies {
    static constexpr uint32_t noJump = std::numeric_limits<uint32_t>::max();
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<StubCall> jumps;
    size_t sizeOf(uint32_t number) const { return (number + 1 < offsets.size() ? offsets[number + 1] : bytes.size()) - offsets[number]; }
};

void emitSubstringCharacters(CCallHelpers&);
GPRReg defaultOperandRegister(Stub);
bool preservesOperandRegister(Stub);
std::optional<RegisterSet> registersClobberedBy(Stub);
Stub stubVariantPreserving(Stub, const RegisterSet& live);
bool hasResultGPRVariants(Stub);
Stub stubVariantForResultGPR(Stub, GPRReg result);
Stub stubVariantForName(Stub, uint32_t name);
bool hasReceiverGPRVariants(Stub);
Stub stubVariantForReceiverGPR(Stub, GPRReg receiver);
enum class PerNameStubKind : uint8_t { Reads, Stores, Definitions, Calls };
void clearNamesWithDedicatedStubs();
enum class CollectionMethod : uint8_t { None, Get, Has, Set, Add };
CollectionMethod collectionMethodNamed(std::span<const uint8_t> name);
void setCollectionMethodNamed(uint32_t name, CollectionMethod);
bool isCollectionMethodName(uint32_t name);
void setNameOfLength(uint32_t name);
void setHasDedicatedStub(PerNameStubKind, uint32_t numberOfIdentifier);
bool methodCallHasDedicatedStub(unsigned count, uint32_t name);
void dumpRegistersClobberedByStubs(PrintStream&);

struct StubBlob {
    Vector<uint8_t> bytes;
    unsigned offsets[numberOfStubs];
    Vector<unsigned> thunkOffsets;
    Vector<unsigned> returnsIntoAdapters;
    Vector<unsigned> returnsFromFindingTargets;
    Vector<std::pair<String, unsigned>> names;
};
std::unique_ptr<StubBlob> createStubBlob(unsigned codeGranuleShift, bool functionLengthsAreBytes, bool mayRunInMultipleInstances, const BitVector& calledThunks, const BitVector& calledStubs, bool imageHasWebAssembly);
constexpr bool isGeneratedOnlyIfCalled(Stub stub) { return (stub >= Stub::GetByIdAsDouble && stub <= Stub::GetByNameAsArrayOrNullishPreservingTwo) || (stub >= Stub::UnboxNumberArgument0ToFPR0 && stub <= Stub::LeafUnboxNumberArgument3ToFPR3); }
bool thunkIsGeneratedOnlyIfCalled(unsigned thunk);

struct IndexReference {
    uint32_t offset;
    uint32_t addend;
    uint16_t scale;
    bool isSiteCacheWord { false };
};
class IndexReferences {
public:
    void load(CCallHelpers&, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale);
    void load16(CCallHelpers&, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale);
    void loadSiteCacheWord(CCallHelpers&, GPRReg dest, unsigned wordInFunction);
    void getAddressOfSiteCacheWord(CCallHelpers&, GPRReg dest, unsigned wordInFunction);
    static void loadFromCachedLocation(CCallHelpers&, GPRReg receiver, GPRReg cachedWord, GPRReg cachedHolder, GPRReg dest);
    Vector<IndexReference> link(LinkBuffer&);
    static void fill(uint8_t* code, const IndexReference&, uint32_t index);

private:
    struct Reference {
        CCallHelpers::Label instructions;
        uint32_t addend;
        uint32_t scale;
        bool isSiteCacheWord { false };
    };
    Vector<Reference, 2> m_references;
};

struct PackedSite {
    static constexpr unsigned shift = 24;
    static constexpr unsigned maxInlineFrames = 254;
    static constexpr uint32_t isTailCall = 1u << (shift - 1);
    static bool fits(uint32_t bits) { return !(bits >> (shift - 1)); }
    static uint32_t pack(unsigned inlineFrame, uint32_t bits, bool tailCall = false)
    {
        RELEASE_ASSERT(inlineFrame <= maxInlineFrames && fits(bits));
        return inlineFrame << shift | (tailCall ? isTailCall : 0) | bits;
    }
    static unsigned inlineFrame(uint32_t site) { return site >> shift; }
    static uint32_t bits(uint32_t site) { return site & (isTailCall - 1); }
    static bool isTailCallSite(uint32_t site) { return site & isTailCall; }
};

struct SpreadSite {
    uint32_t callSite;
    uint32_t item;
    uint32_t site;
};

struct CallSite {
    uint32_t bits { StubCall::noCallSite };
};

#if CPU(ARM64)
static constexpr GPRReg functionIndexGPR = ARM64Registers::x15;
static constexpr GPRReg throwWordGPR = ARM64Registers::x16;
#elif CPU(X86_64)
static constexpr GPRReg functionIndexGPR = X86Registers::r10;
static constexpr GPRReg throwWordGPR = X86Registers::r11;
#endif
#if CPU(ARM64) || CPU(X86_64)
void loadFunctionIndexAt(CCallHelpers&, GPRReg pc);
#endif

class StubCalls {
public:
    void call(CCallHelpers&, Stub, CallSite);
    void callForTwoReads(CCallHelpers&, Stub, CallSite firstRead, CallSite secondRead);
    void call(CCallHelpers&, Stub, uint32_t t9Value, CallSite);
    void callMethodWithDedicatedStub(CCallHelpers&, unsigned count, uint32_t name, CallSite);
    void call(CCallHelpers&, Stub, uint32_t t9Value, CallSite, const ArgumentMoves&);
    void call(CCallHelpers&, Stub, CallSite, const ArgumentMoves&);
    void tailCall(CCallHelpers&, Stub);
    void tailCall(CCallHelpers&, Stub, uint32_t t9Value);
    void callFunction(CCallHelpers&, uint32_t knownCallee, CallSite, uint8_t firstOmittedGPR = 0, uint8_t endOfOmittedGPRs = 0);
    void jumpToFunction(CCallHelpers&, uint32_t knownCallee);
    Vector<StubCall> link(LinkBuffer&);
    Vector<ArgumentMoves> takeArgumentMoves() { return std::exchange(m_argumentMoves, { }); }

private:
    struct Pending {
        CCallHelpers::Call call;
        Stub stub;
        bool isTailCall;
        uint32_t callSite;
        uint32_t function { StubCall::noFunction };
        uint16_t thunk { 0 };
        uint32_t argumentMoves { StubCall::noArgumentMoves };
        uint8_t firstOmittedGPR { 0 };
        uint8_t endOfOmittedGPRs { 0 };
        uint32_t firstReadSite { StubCall::noCallSite };
    };
    Vector<Pending, 8> m_pending;
    Vector<ArgumentMoves> m_argumentMoves;
};

void retargetStubCall(uint8_t* base, size_t instruction, size_t target, bool isTailCall);
void writeCallOfResolvedValue(uint8_t* base, size_t instruction);
#if CPU(X86_64)
static constexpr size_t sizeOfJump = 5;
static constexpr size_t sizeOfNearCall = 5;
static constexpr size_t codeOffsetUnit = 1;
static constexpr size_t stubCallReach = 1536 * MB;
#else
static constexpr size_t sizeOfJump = sizeof(uint32_t);
static constexpr size_t sizeOfNearCall = sizeof(uint32_t);
static constexpr size_t codeOffsetUnit = sizeof(uint32_t);
static constexpr size_t stubCallReach = 96 * MB;
#endif
static constexpr size_t offsetOfReturnAddressInTrap = sizeOfNearCall - 1;
#if CPU(X86_64) && OS(LINUX)
static constexpr size_t sizeOfLoopPoll = 7;
static constexpr ptrdiff_t offsetOfLeafPollInLoopPollPage = 4;
#else
static constexpr size_t sizeOfLoopPoll = 0;
#endif
#if CPU(X86_64)
static constexpr uint8_t trapInstruction = 0xf4;
static constexpr uint8_t trapInstructionWithoutFrame = 0xfa;
#if OS(LINUX)
static constexpr size_t sizeOfCallAtPrologue = 1;
#else
static constexpr size_t sizeOfCallAtPrologue = sizeOfNearCall;
#endif
inline bool trapAtPrologueMakesFrameRecord(const uint8_t* trap)
{
    return trap[-4] == 0x55 && trap[-3] == 0x48 && trap[-2] == 0x89 && trap[-1] == 0xe5 && trap[1] == 0x5d;
}

inline uint8_t* afterStackCheckBranchingTo(uint8_t* startOfFunction, uint8_t* arm)
{
#if OS(LINUX)
    if (static_cast<size_t>(arm - startOfFunction) > 4 && trapAtPrologueMakesFrameRecord(arm))
        arm -= 4;
#endif
    constexpr unsigned sizeOfConditionalJump = 6;
    constexpr unsigned sizeOfShortJump = 2;
    auto relative = [](uint8_t* at) { int32_t value; memcpy(&value, at, sizeof(value)); return value; };
    uint8_t* instruction = startOfFunction;
    if (*instruction == 0xe9)
        instruction += sizeOfNearCall + relative(instruction + 1);
    else if (*instruction == 0xeb)
        instruction += sizeOfShortJump + static_cast<int8_t>(instruction[1]);
    constexpr unsigned maxBytesInStackCheck = 40;
    for (unsigned i = 0; i < maxBytesInStackCheck; ++i, ++instruction) {
        if (instruction[0] == 0x0f && instruction[1] == 0x82 && instruction + sizeOfConditionalJump + relative(instruction + 2) == arm)
            return instruction + sizeOfConditionalJump;
        if (instruction[0] == 0x72 && instruction + sizeOfShortJump + static_cast<int8_t>(instruction[1]) == arm)
            return instruction + sizeOfShortJump;
    }
    return nullptr;
}
#endif
static constexpr size_t sizeOfVeneer = 3 * sizeof(uint32_t);
static constexpr size_t stubCallReachSlack = 24 * MB;
void writeVeneer(uint8_t* base, size_t veneer, size_t target);
size_t sizeOfOmittedArguments(unsigned firstGPR, unsigned endOfGPRs);
void writeOmittedArguments(uint8_t* base, size_t at, unsigned firstGPR, unsigned endOfGPRs);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
