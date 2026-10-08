/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
    v(operationAOTTypedSetFree) \
    v(operationAOTTypedGetByName) \
    v(operationAOTTypedSetByName) \
    v(operationAOTTypedDefineByName) \
    v(operationAOTTypedHasName) \
    v(operationAOTTypedDeleteByName) \
    v(operationAOTTypedGetByKey) \
    v(operationAOTTypedSetByKey) \
    v(operationAOTTypedHasKey) \
    v(operationAOTTypedDeleteByKey) \
    v(operationAOTTypedCall) \
    v(operationAOTTypedCallMethod) \
    v(operationAOTTypedLanguageOperation) \
    v(operationAOTTypedGetExternalModule) \
    v(operationAOTTypedConstruct) \
    v(operationAOTTypedConstructWith) \
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
    v(operationAOTTypedIsCallable) \
    v(operationAOTTypedIsClosureOf) \
    v(operationAOTTypedInheritsFromClass) \
    v(operationAOTTypedInstanceOf) \
    v(operationAOTTypedToString) \
    v(operationAOTTypedToKey) \
    v(operationAOTTypedFmod) \
    v(operationAOTTypedPow) \
    v(operationAOTTypedToInt32) \
    v(operationAOTTypedAllocSlow) \
    v(operationAOTTypedCoerce) \
    v(operationAOTTypedDidCreateField) \
    v(operationAOTTypedGetExtra) \
    v(operationAOTTypedHasExtra) \
    v(operationAOTTypedPutExtra) \
    v(operationAOTTypedStringConstant) \
    v(operationAOTTypedBigIntConstant) \
    v(operationAOTTypedBarrierSlow) \
    v(operationAOTTypedPollSlow) \
    v(operationAOTTypedThrow) \
    v(operationAOTTypedCatch) \
    v(operationAOTTypedThrowTypeMismatch) \
    v(operationAOTTypedNonNullFailed) \
    v(operationAOTTypedThrowNullish) \
    v(operationAOTTypedFactDoesNotHold) \
    v(operationAOTTypedThrowStackOverflow) \
    v(operationAOTTypedThrowTDZError) \
    v(operationAOTTypedNewObject) \
    v(operationAOTTypedNewArray) \
    v(operationAOTTypedArrayPush) \
    v(operationAOTTypedArrayGet) \
    v(operationAOTTypedArrayPut) \
    v(operationAOTTypedCallReaderOfArray) \
    v(operationAOTTypedOpenChangedArray) \
    v(operationAOTTypedBuiltinCell) \
    v(operationAOTTypedNewTypedArray) \
    v(operationAOTTypedTypedArrayPut) \
    v(operationAOTTypedFunctionEntry) \
    v(operationAOTTypedClosureConstant) \
    v(operationAOTTypedNewArrayWithLength) \
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

// With Primitive::*, what operationAOTTypedThrowTypeMismatch is told of the type: it has objects of any sort, it has functions.
static constexpr uint32_t typeAllowsObjects = 1 << 10;
static constexpr uint32_t typeAllowsFunctions = 1 << 11;

} // namespace Typed

#define AOT_DECLARE_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }
#define AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(name, writes, result, parameters) \
    JSC_DECLARE_NOEXCEPT_JIT_OPERATION(name, result, parameters); \
    template<> struct Typed::WritesOf<name> { static constexpr Typed::Writes value = Typed::Writes::writes; }

AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFree, Heap, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetFreeForTypeOf, Heap, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetFree, Heap, void, (Instance*, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByName, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
// CreateDataPropertyOrThrow.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDefineByName, Heap, void, (Instance*, EncodedJSValue, uint32_t, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByName, Heap, size_t, (Instance*, EncodedJSValue, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetByKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedSetByKey, Heap, void, (Instance*, EncodedJSValue, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedHasKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedDeleteByKey, Heap, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCall, Heap, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExternalModule, RuntimeState, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedLanguageOperation, Heap, EncodedJSValue, (Instance*, uint32_t, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCallMethod, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t, uint32_t, const EncodedJSValue*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstruct, Heap, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t, const EncodedJSValue*));
// [[Construct]] with the object it would make already made, as nobody has seen it yet. A callee that makes its own does.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedConstructWith, Heap, EncodedJSValue, (Instance*, EncodedJSValue callee, JSCell* object, uint32_t, const EncodedJSValue*));
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
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIsCallable, Nothing, size_t, (Instance*, JSCell*));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedIsClosureOf, Nothing, size_t, (Instance*, JSCell*, uint32_t));
// By LayoutData::index. A proxy on the prototype chain runs its trap.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedInheritsFromClass, Heap, size_t, (Instance*, JSCell*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToString, Heap, JSCell*, (Instance*, EncodedJSValue));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedToKey, Heap, EncodedJSValue, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFmod, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedPow, Pure, double, (double, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedToInt32, Pure, size_t, (double));
// Fills Instance::typedAllocations[allocation]. The cell has its header and both words. Every field that can be absent is, the others are clear.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedAllocSlow, RuntimeState, JSCell*, (Instance*, uint32_t allocation));
// A Cast for the reason As, Flow or Coerce, where there may be something to make of the value: State::coerce(). The type is by TypeID. It
// writes the shape word, the order word and the fields of an object it turns into one of a specialization, and the elements of arrays.
// Code of the program runs only where Coerce converts an object to a primitive. What a cast may do of all this: writesOfCast().
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCoerce, Heap, EncodedJSValue, (Instance*, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t position));
// The field by that name, of the layout of the object itself, is absent and is about to be stored to. Writes the order word only.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedDidCreateField, Fields, void, (Instance*, JSCell* object, uint32_t name));
// A property that the layout of the object itself has no field for, as a type sees it: its own, `undefined` if it has none. The
// prototype chain is not asked, as for a field that is absent. No code of the program runs: an accessor throws.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedGetExtra, Nothing, EncodedJSValue, (Instance*, JSCell* object, uint32_t name));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedHasExtra, Nothing, size_t, (Instance*, JSCell* object, uint32_t name));
// [[Set]] of a property that the layout of the object itself has no field for. Where that would run a setter, it throws.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPutExtra, Fields, void, (Instance*, JSCell* object, uint32_t name, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedStringConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedBigIntConstant, RuntimeState, EncodedJSValue, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBarrierSlow, RuntimeState, void, (Instance*, JSCell*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedPollSlow, Nothing, void, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrow, Nothing, void, (Instance*, EncodedJSValue));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedCatch, RuntimeState, EncodedJSValue, (Instance*));
// The value is not of its type. Kinds: Primitive::* and the two above. Name: the string that names the type, UINT32_MAX if none does.
// Where: the function, and Value::position.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowTypeMismatch, Nothing, void, (Instance*, EncodedJSValue, uint32_t kinds, uint32_t name, uint32_t function, uint32_t position));
// `value!`, where the value is not of the type (by TypeID), which is to say `undefined` or `null`. It throws, unless `goesOn`: then
// Options::reportAOTTypedCoercions() is set, there is room for the value in what comes of the cast, and to report it is all.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNonNullFailed, Nothing, void, (Instance*, EncodedJSValue, uint32_t type, uint32_t goesOn, uint32_t function, uint32_t position));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowNullish, Nothing, void, (Instance*, EncodedJSValue, uint32_t));
// Options::validateAOTTypedFacts(): the value of that number in that function is not of its type. Does not return.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFactDoesNotHold, Nothing, void, (Instance*, EncodedJSValue, uint32_t function, uint32_t value));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowStackOverflow, Nothing, void, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedThrowTDZError, Nothing, void, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedNewObject, RuntimeState, JSCell*, (Instance*));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewArray, RuntimeState, JSCell*, (Instance*, uint32_t, const EncodedJSValue*));
// What is typed as an array was of ArrayType when it was taken for one, and may have become of DerivedArrayType since: see
// JSObject::becomeDerivedArray(). These do what the language says for either. Those that change the heap run whatever code of the
// program that takes. Those that do not run none: where the language would (an accessor, a proxy on the prototype chain, a function of
// the program in the place of the engine's), they throw a TypeError.
//
// `array.push(value)`: yields what the call yields.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPush, Heap, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue value));
// `array[key]`, with a number for the key.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayGet, Nothing, EncodedJSValue, (Instance*, JSCell* array, EncodedJSValue key));
// `array[key] = value` in strict code, with a number for the key.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedArrayPut, Heap, void, (Instance*, JSCell* array, EncodedJSValue key, EncodedJSValue value));
// `array.name(...)` where that comes down to the entry of the table, a method of which the table says that it changes nothing the program sees.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedCallReaderOfArray, RuntimeState, EncodedJSValue, (Instance*, JSCell* array, uint32_t builtin, uint32_t count, const EncodedJSValue*));
// `for (... of array)` of an array that is no longer of ArrayType. It comes back if that is still the iteration of arrays, which goes by index.
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedOpenChangedArray, Nothing, void, (Instance*, JSCell* array));
// Fills the entry of Instance::typedBuiltins.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedBuiltinCell, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewTypedArray, RuntimeState, JSCell*, (Instance*, uint32_t, double));
// `view[index] = number`, whatever the length of the view depends on. It allocates nothing, throws nothing and comes to no safepoint.
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedTypedArrayPut, TypedArrayProperties, void, (Instance*, JSCell* view, uint64_t index, double));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedFunctionEntry, RuntimeState, void*, (Instance*, uint32_t));
AOT_DECLARE_NOEXCEPT_TYPED_OPERATION(operationAOTTypedClosureConstant, RuntimeState, JSCell*, (Instance*, uint32_t));
AOT_DECLARE_TYPED_OPERATION(operationAOTTypedNewArrayWithLength, RuntimeState, JSCell*, (Instance*, uint64_t));
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
