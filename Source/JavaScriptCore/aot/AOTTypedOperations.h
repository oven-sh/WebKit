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

class JSCell;

namespace AOT {

struct Instance;

#define FOR_EACH_AOT_TYPED_OPERATION(v) \
    v(operationAOTTypedGetFree) \
    v(operationAOTTypedGetFreeForTypeOf) \
    v(operationAOTTypedGetPropertyOfGlobalThis) \
    v(operationAOTTypedSetFree) \
    v(operationAOTTypedGetByName) \
    v(operationAOTTypedSetByName) \
    v(operationAOTTypedDefineByName) \
    v(operationAOTTypedDefineOwnProperty) \
    v(operationAOTTypedDefineOwnPropertyAtSite) \
    v(operationAOTTypedHasName) \
    v(operationAOTTypedDeleteByName) \
    v(operationAOTTypedGetByKey) \
    v(operationAOTTypedSetByKey) \
    v(operationAOTTypedHasKey) \
    v(operationAOTTypedDeleteByKey) \
    v(operationAOTTypedCall) \
    v(operationAOTTypedThrowNotAFunction) \
    v(operationAOTTypedThrowMethodIsNotAFunction) \
    v(operationAOTTypedThrowAtSite) \
    v(operationAOTTypedLanguageOperation) \
    v(operationAOTTypedNewPromise) \
    v(operationAOTTypedResolvePromise) \
    v(operationAOTTypedAwait) \
    v(operationAOTTypedGetExternalModule) \
    v(operationAOTTypedConstruct) \
    v(operationAOTTypedConstructWith) \
    v(operationAOTTypedDefineAccessors) \
    v(operationAOTTypedAdd) \
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
    v(operationAOTTypedToString) \
    v(operationAOTTypedToKey) \
    v(operationAOTTypedFmod) \
    v(operationAOTTypedPow) \
    v(operationAOTTypedToInt32) \
    v(operationAOTTypedAllocSlow) \
    v(operationAOTTypedNewClosureSlow) \
    v(operationAOTTypedCoerce) \
    v(operationAOTTypedCoerceInPlace) \
    v(operationAOTTypedCoerceElement) \
    v(operationAOTTypedAuditCoerce) \
    v(operationAOTTypedDidCreateField) \
    v(operationAOTTypedDidCreateFieldAt) \
    v(operationAOTTypedDidCreateFieldOutOfLine) \
    v(operationAOTTypedCountClosure) \
    v(operationAOTTypedCountPlainAllocation) \
    v(operationAOTTypedGetExtra) \
    v(operationAOTTypedHasExtra) \
    v(operationAOTTypedFieldHasText) \
    v(operationAOTTypedPutExtra) \
    v(operationAOTTypedGetElsewhere) \
    v(operationAOTTypedGetElsewhereAsDouble) \
    v(operationAOTTypedPutElsewhere) \
    v(operationAOTTypedPutNumberElsewhere) \
    v(operationAOTTypedStringConstant) \
    v(operationAOTTypedBigIntConstant) \
    v(operationAOTTypedBarrierSlow) \
    v(operationAOTTypedRememberElement) \
    v(operationAOTTypedPollSlow) \
    v(operationAOTTypedThrow) \
    v(operationAOTTypedCatch) \
    v(operationAOTTypedRejectWithException) \
    v(operationAOTTypedAssertionFailed) \
    v(operationAOTTypedStoreAssertionFailed) \
    v(operationAOTTypedThrowReachedUnreachable) \
    v(operationAOTTypedFactDoesNotHold) \
    v(operationAOTTypedThrowStackOverflow) \
    v(operationAOTTypedNewObject) \
    v(operationAOTTypedNewObjectThatGrowsAlone) \
    v(operationAOTTypedNewObjectLiteral) \
    v(operationAOTTypedCloneObject) \
    v(operationAOTTypedTryCopyRecordAs) \
    v(operationAOTTypedPrivateGet) \
    v(operationAOTTypedPrivateSet) \
    v(operationAOTTypedPrivateHas) \
    v(operationAOTTypedPrivateFieldAdd) \
    v(operationAOTTypedNewArray) \
    v(operationAOTTypedNewArrayOfNumbers) \
    v(operationAOTTypedArrayPush) \
    v(operationAOTTypedArrayGet) \
    v(operationAOTTypedArrayPut) \
    v(operationAOTTypedArrayGetAtNumber) \
    v(operationAOTTypedArrayPutAtNumber) \
    v(operationAOTTypedTryAddNamedProperties) \
    v(operationAOTTypedOpenChangedArray) \
    v(operationAOTTypedBuiltinCell) \
    v(operationAOTTypedNewTypedArray) \
    v(operationAOTTypedTypedArrayPut) \
    v(operationAOTTypedClosureConstant) \
    v(operationAOTTypedCreateThis) \
    v(operationAOTTypedCreateThisWithFields) \
    v(operationAOTTypedCreateThisOfCapacity) \
    v(operationAOTTypedTryCreateThisWithStores) \
    v(operationAOTTypedCreateThisAt) \
    v(operationAOTTypedDidStoreToThis) \
    v(operationAOTTypedTryPutProperties) \
    v(operationAOTTypedConcat) \
    v(operationAOTTypedSubstring) \
    v(operationAOTTypedFlatten) \
    v(operationAOTTypedStringEq) \
    v(operationAOTTypedStringHasAt) \
    v(operationAOTTypedStringIndexOf) \
    v(operationAOTTypedIntToString) \
    v(operationAOTTypedDoubleToString) \
    v(operationAOTTypedCharToString) \
    v(operationAOTTypedNewMap) \
    v(operationAOTTypedIterOpen) \
    v(operationAOTTypedIterHas) \
    v(operationAOTTypedIterGet) \
    v(operationAOTTypedIterClose) \
    v(operationAOTTypedIterCloseForThrow)

namespace Typed {

template<auto operation> struct WritesOf;

// With Primitive::*, what a ThrowKind::KindMismatch site says of the type: it has objects of any sort, it has functions.
static constexpr uint32_t typeAllowsObjects = 1 << 10;
static constexpr uint32_t typeAllowsFunctions = 1 << 11;
// What operationAOTTypedAssertionFailed is told stands at the place: a Format::CastReason, or with this bit the operation (Opcode) whose
// result is held to its type.
static constexpr uint32_t assertionIsOnResult = 1 << 8;
// What was attempted on undefined or null.
enum class NullishAccess : uint32_t { Read, Write, Iterate };

} // namespace Typed

#define AOT_DECLARE_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }
#define AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_NOEXCEPT_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }

AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFree, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFreeForTypeOf, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetPropertyOfGlobalThis, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetFree, Heap, void, (Instance*, uint32_t, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByName, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
// CreateDataPropertyOrThrow.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
// Object.defineProperty(object, key, descriptor) where the descriptor is a literal that nobody else sees: it is not made. What it has: see Lowering::lowerPropertyDefinition().
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
};
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineOwnProperty, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue key, EncodedJSValue valueOrGetter, EncodedJSValue setter, uint32_t descriptorLiteral));
// For Stub::DefineOwnProperty.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineOwnPropertyAtSite, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue value, uint32_t identifier, Slot* cache, uint32_t descriptorLiteral));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByKey, Heap, void, (Instance*, EncodedJSValue, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCall, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExternalModule, Heap, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLanguageOperation, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t, const EncodedJSValue*));
// The language operations of those names.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewPromise, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedResolvePromise, Heap, void, (Instance*, JSCell*, EncodedJSValue));
// The resume closure, what is awaited, whether the coroutine is known to be an async function and no async generator.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAwait, Heap, void, (Instance*, JSCell*, EncodedJSValue, uint32_t));
// What was read under the name to be called is no function. It throws.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowNotAFunction, Nothing, void, (Instance*, EncodedJSValue callee, uint32_t name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowMethodIsNotAFunction, Nothing, void, (Instance*, EncodedJSValue callee, uint32_t identifierOfSite));
// What a site that only throws throws. The words behind the call say it: see Typed::Lowering::throwAt().
// See Lowering::newObjectWithProperties().
static constexpr unsigned maxPropertiesNamedBySite = 32;
JSObject* newObjectLiteralAtSite(Instance*, JSGlobalObject*, VM&, FunctionRef, Slot* cache, const EncodedJSValue* values, uint32_t numberOfValues);

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
    static constexpr unsigned repShift = 3;
    static constexpr unsigned registerShift = 6;
    static constexpr uint32_t hasWideA = 1u << 11;
    static constexpr uint32_t hasB = 1u << 12;
    static constexpr unsigned aShift = 13;
    static constexpr uint32_t maxNarrowA = (1u << 18) - 1;
    // In a word of its own: UINT32_MAX.
    static constexpr uint32_t none = 0x7fffffff;
    static constexpr unsigned numberOfRegistersPerBank = 32;
};
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowAtSite, Nothing, void, (Instance*, const uint64_t* registers, const uint32_t* site));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstruct, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t, const EncodedJSValue*));
// [[Construct]] with the object it would make already made, as nobody has seen it yet. A callee that makes its own does.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstructWith, Heap, EncodedJSValue, (Instance*, EncodedJSValue callee, JSCell* object, uint32_t, const EncodedJSValue*));
// The accessor functions of an object literal, in the order of the text, four values each: an Int32, the number of the string that is the key; the
// function; an Int32: 1 it is the getter and not the setter, 2 the property is enumerable; an Int32: how many keys of the finished literal come
// before this one.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineAccessors, Heap, void, (Instance*, JSCell* object, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAdd, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
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
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTruthy, RuntimeState, size_t, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypeOf, Nothing, JSCell*, (Instance*, EncodedJSValue));
// Of an object: a TypeofType.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypeOfObject, Nothing, size_t, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIsCallable, Nothing, size_t, (Instance*, JSCell*));
// By LayoutData::index. A proxy on the prototype chain runs its trap.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInheritsFromClass, Heap, size_t, (Instance*, JSCell*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToString, Heap, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFmod, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPow, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedToInt32, Pure, size_t, (double));
// Gives the allocation its row of Instance::typedAllocationRows. The cell has its header and both words. Every field that can be absent is, the others are clear.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedAllocSlow, RuntimeState, JSCell*, (Instance*, uint32_t allocation));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewClosureSlow, RuntimeState, JSCell*, (Instance*, uint64_t request, uint64_t, uint64_t, uint64_t, uint64_t));
// A Cast for the reason As, Flow or Coerce, where there may be something to make of the value: State::coerce(). The type is by TypeID. It
// writes the shape word, the order word and the fields of an object it turns into one of a specialization, and the elements of arrays.
// Code of the program runs only where Coerce converts an object to a primitive. What a cast may do of all this: writesOfCast().
// `typeAndReason`: TypeID << 4 | CastReason.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerce, Heap, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t typeAndReason));
// The same of a value that is an object of the program or no object: it stays the value it is, or is turned away.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceInPlace, Heap, void, (Instance*, EncodedJSValue value, uint32_t typeAndReason));
// Converts array[index], which is plain data, as an As cast to the type would, stores the result back and returns it.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerceElement, RuntimeState, EncodedJSValue, (Instance*, JSCell* array, uint32_t index, uint32_t type));
// The same with Options::auditAOTTypedAssertions(): a value it would turn away is reported and goes on, see State::assertionFailed(). The last
// two are an AuditSite.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedAuditCoerce, Heap, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t lineAndColumn));
// The field by that name, of the layout of the object itself, is absent and is about to be stored to. Writes the order word only.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateField, Fields, void, (Instance*, JSCell* object, uint32_t name));
// The same, of the field whose slot is at the offset in the cell. What Stub::DidCreateTypedField has no answer to.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateFieldAt, Fields, void, (Instance*, JSCell* object, uint32_t offset, Slot* cache));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateFieldOutOfLine, Fields, void, (Instance*, JSCell* object, uint32_t name, Slot* cache));
// Only in a program compiled with Options::countAOTTypedAllocations(), or for the audit: compiled code has made a closure.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCountClosure, RuntimeState, void, (Instance*, uint32_t function, uint32_t numberOfCaptures));
// Likewise: it has made a plain object or an array. `kindAndValue`: PlainAllocationKind from bit 28, and the number of the value in the function.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCountPlainAllocation, RuntimeState, void, (Instance*, JSCell*, uint32_t function, uint32_t kindAndValue));
// A property that the layout of the object itself has no field for, as a type sees it: its own, `undefined` if it has none. The
// prototype chain is not asked, as for a field that is absent. No code of the program runs: an accessor throws.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExtra, Nothing, EncodedJSValue, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedHasExtra, Nothing, size_t, (Instance*, JSCell* object, uint32_t name));
// `object.name === "text"`, of an object whose layout has a field of the name. It may make a rope flat, as any comparison of strings. See Typed::fieldIsPinned.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFieldHasText, RuntimeState, size_t, (Instance*, JSCell* object, uint32_t name, uint32_t text));
// [[Set]] of a property that the layout of the object itself has no field for. Where that would run a setter, it throws.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPutExtra, Fields, void, (Instance*, JSCell* object, uint32_t name, EncodedJSValue));
// Store::Int32OrElsewhere. The name is that of the field. The first: the word says elsewhereInt32; yields the number. The second: a number that
// the word cannot hold; writes the entry, then the word. The object may be one whose first stores are not over: it has its header, both
// words, and something the collector can look at in every field that holds cells.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedGetElsewhere, Nothing, EncodedJSValue, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedGetElsewhereAsDouble, Nothing, double, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPutElsewhere, Fields, void, (Instance*, JSCell* object, uint32_t name, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPutNumberElsewhere, Fields, void, (Instance*, JSCell* object, uint32_t name, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStringConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBigIntConstant, RuntimeState, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBarrierSlow, RuntimeState, void, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedRememberElement, RuntimeState, void, (Instance*, JSCell* array, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPollSlow, Nothing, void, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrow, Nothing, void, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCatch, RuntimeState, EncodedJSValue, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedRejectWithException, RuntimeState, JSCell*, (Instance*));
// Options::auditAOTTypedAssertions(): the value is not of the type (by TypeID) that it is held to at the place. Yields what goes on in its
// stead: State::assertionFailed(). No code of the program runs, and nothing is made. The last two are an AuditSite.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedAssertionFailed, Nothing, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t type, uint32_t what, uint32_t function, uint32_t lineAndColumn));
// The same for CastReason::Store where the store is the next operation: a store to a frozen object throws before it looks at the value.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStoreAssertionFailed, Nothing, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue storedTo, uint32_t type, uint32_t function, uint32_t lineAndColumn));
// Options::auditAOTTypedAssertions(): control has got to where the types say it never gets, and there is no code to go on with.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowReachedUnreachable, Nothing, void, (Instance*));
// Options::validateAOTTypedFacts(): the value of that number in that function is not of its type. Does not return.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFactDoesNotHold, Nothing, void, (Instance*, EncodedJSValue, uint32_t function, uint32_t value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowStackOverflow, Nothing, void, (Instance*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObject, RuntimeState, JSCell*, (Instance*));
// `{}`, `Object.create(null)` in code that runs once. See Structure::growsAlone().
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObjectThatGrowsAlone, RuntimeState, JSCell*, (Instance*, uint32_t hasNullPrototype));
// `{ a: x, b: y }`: `count` values, then `count` names, each the index of a string in a word. The cache: two slots, see fillAllocationCache(). Above the lowest bit: how many names the object is seen to be given in all, or zero.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewObjectLiteral, RuntimeState, JSCell*, (Instance*, Slot* cache, uint32_t count, uint32_t hasNullPrototypeAndInlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCloneObject, Heap, JSCell*, (Instance*, EncodedJSValue source, uint32_t keepsLayout));
// Typed::Object::tryCopyAs(). The first of the values is the source, the others are the excluded keys. Null: not done.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryCopyRecordAs, RuntimeState, JSCell*, (Instance*, uint32_t shape, uint32_t count, const EncodedJSValue* values));
// For the stubs of the same names.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateGet, Heap, EncodedJSValue, (Instance*, EncodedJSValue object, EncodedJSValue name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateSet, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateHas, Heap, EncodedJSValue, (Instance*, EncodedJSValue object, EncodedJSValue name));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPrivateFieldAdd, Heap, void, (Instance*, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisWithFields, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisOfCapacity, RuntimeState, JSCell*, (Instance*, JSCell* constructor, JSCell* newTarget, uint32_t inlineCapacity));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryCreateThisWithStores, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* valuesThenNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCreateThisAt, RuntimeState, JSCell*, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget));
// `names`: the number of each, and in bit 32 whether it was stored and not defined.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDidStoreToThis, Heap, void, (Instance*, Slot* cache, JSCell* constructor, JSCell* newTarget, JSCell* object, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* names));
// The stores of a property run, all or none. `count` values, then for each the number of its name, or its key where it has a bit in `keyed`. It runs no code of the program.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedTryPutProperties, Heap, size_t, (Instance*, Slot* cache, EncodedJSValue object, uint32_t count, uint64_t keyed, const EncodedJSValue* valuesThenNames));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewArray, RuntimeState, JSCell*, (Instance*, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewArrayOfNumbers, RuntimeState, JSCell*, (Instance*, uint32_t, uint32_t));
// What is typed as an array was of ArrayType when it was taken for one, and may have become of DerivedArrayType since: see
// JSObject::becomeDerivedArray(). These do what the language says for either. Those that change the heap run whatever code of the
// program that takes. Those that do not run none: where the language would (an accessor, a proxy on the prototype chain, a function of
// the program in the place of the engine's), they throw a TypeError.
//
// `array.push(value)`: yields what the call yields. `expected`: how many more are expected to be pushed, this one among them, or zero.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPush, Heap, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue value, uint32_t expected));
// In AOTTypedBuiltinsArray.cpp.
unsigned pushToFullVector(VM&, JSArray*, JSValue, unsigned expected);
// `array[key]`, with a number for the key.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGet, Nothing, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue key));
// `array[key] = value` in strict code, with a number for the key.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPut, Heap, void, (Instance*, JSCell* array, EncodedJSValue key, EncodedJSValue value));
// The same with the key as compiled code keeps it.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGetAtNumber, Nothing, EncodedJSValue, (Instance*, JSCell* array, double key));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPutAtNumber, Heap, void, (Instance*, JSCell* array, EncodedJSValue value, double key));
// `array.a = x; array.b = y; ..` in strict code, with nothing in between: `count` values, then `count` names, each the index of a string in
// a word. 1: all are added. 0: nothing is done, and the stores are to be made one by one.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTryAddNamedProperties, Heap, size_t, (Instance*, JSCell* array, uint32_t count, const EncodedJSValue* valuesThenNames));
// `for (... of array)` of an array that is no longer of ArrayType. It comes back if that is still the iteration of arrays, which goes by index.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedOpenChangedArray, Nothing, void, (Instance*, JSCell* array));
// Fills the entry of Instance::typedBuiltins.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBuiltinCell, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewTypedArray, RuntimeState, JSCell*, (Instance*, uint32_t, double));
// `view[index] = number`, whatever the length of the view depends on. It allocates nothing, throws nothing and comes to no safepoint.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypedArrayPut, TypedArrayProperties, void, (Instance*, JSCell* view, uint64_t index, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedClosureConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
// See Opcode::CreateThis.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCreateThis, RuntimeState, JSCell*, (Instance*, JSCell* constructor, JSCell* newTarget));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConcat, RuntimeState, JSCell*, (Instance*, JSCell*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSubstring, RuntimeState, JSCell*, (Instance*, JSCell*, int32_t, int32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedFlatten, RuntimeState, void*, (Instance*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringEq, RuntimeState, size_t, (Instance*, JSCell*, JSCell*));
// Whether the characters of the string from that place on are those of the other. The place may be outside.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringHasAt, RuntimeState, size_t, (Instance*, JSCell* string, JSCell* other, int32_t place));
// Where the other is found first, from that place on, which is not outside. -1: nowhere.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedStringIndexOf, RuntimeState, size_t, (Instance*, JSCell* string, JSCell* other, int32_t place));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIntToString, Nothing, JSCell*, (Instance*, uint64_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDoubleToString, Nothing, JSCell*, (Instance*, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCharToString, Nothing, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewMap, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterOpen, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterHas, Heap, size_t, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIterGet, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterClose, Heap, void, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedIterCloseForThrow, Heap, void, (Instance*, EncodedJSValue));

#include "AOTTypedBuiltinOperations.inc"

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
