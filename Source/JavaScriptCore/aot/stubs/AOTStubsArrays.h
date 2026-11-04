/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTConvention.h"
#include "AOTStubDescriptor.h"
#include "AOTStubHelpers.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSArray.h"
#include "MarkedSpace.h"

namespace JSC { namespace AOT {

static_assert(maxArrayElementsInRegisters <= 4);
static_assert(JSObject::butterflyOffset() == sizeof(JSCell) && sizeof(JSCell) == sizeof(uint64_t));
static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(sizeof(IndexingHeader)) && Butterfly::offsetOfVectorLength() == Butterfly::offsetOfPublicLength() + static_cast<ptrdiff_t>(sizeof(uint32_t)));

enum class ArrayElementKind : uint8_t { Int32, Double, Contiguous };
enum class EmptyArray : uint8_t { ContiguousCapacity1, ContiguousCapacity3, DoubleCapacity1, DoubleCapacity3, DoubleDefaultCapacity, Int32DefaultCapacity };
// WithPropertyRoom: propertyRoomOfArray bytes lie in front of the indexing header. AsDoubles: an array of int32 is copied into an array of doubles.
enum class ArraySliceRange : uint8_t { Bounded, Whole, BoundedWithPropertyRoom, WholeWithPropertyRoom, BoundedAsDoubles, WholeAsDoubles };
// Number: an array of int32 that takes a number which may be none becomes an array of doubles.
enum class ArrayStoredValue : uint8_t { Value, Number };
// OfListAsMade: nothing stands under an index above the list, so what it lacks reads as undefined. OfAnyList: the cell says whether that is so, and where it is not the run time looks.
enum class ArrayRead : uint8_t { OfListAsMade, OfAnyList };
// Where the elements lie that every array of a site shares: in the image, or in storage that the first array of the site makes.
enum class SharedElements : uint8_t { InImage, InStorageOfSite };
// Strings: every element below the length is a JSString.
enum class SearchedElements : uint8_t { Values, Strings, ValuesFromStart, StringsFromStart };

constexpr IndexingType indexingTypeOf(ArrayElementKind kind)
{
    switch (kind) {
    case ArrayElementKind::Int32:
        return ArrayWithInt32;
    case ArrayElementKind::Double:
        return ArrayWithDouble;
    case ArrayElementKind::Contiguous:
        return ArrayWithContiguous;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr ArrayElementKind arrayElementKindOf(IndexingType indexingType)
{
    switch (indexingType & IndexingShapeMask) {
    case Int32Shape:
        return ArrayElementKind::Int32;
    case DoubleShape:
        return ArrayElementKind::Double;
    case ContiguousShape:
        return ArrayElementKind::Contiguous;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

#if CPU(X86_64)
inline constexpr StubRegister arrayPushKindRegister = StubRegister::A0;
inline constexpr StubRegister arrayAllocationScratchRegister = StubRegister::A0;
inline constexpr StubRegister arrayStorageRegister = StubRegister::A5;
#elif CPU(ARM64)
inline constexpr StubRegister arrayPushKindRegister = StubRegister::T11;
inline constexpr StubRegister arrayAllocationScratchRegister = StubRegister::T11;
inline constexpr StubRegister arrayStorageRegister = StubRegister::T12;
#endif

inline constexpr int64_t arraySliceToEnd = 1ll << 32;

constexpr StubKey arrayLiteral(unsigned count) { return StubKey(StubFamily::ArrayLiteral).withCount(count); }
// Behind the elements: the address of sizeOfFrameArray(count) bytes in the frame of the caller.
constexpr StubKey frameArrayLiteral(unsigned count) { return StubKey(StubFamily::FrameArrayLiteral).withCount(count); }
constexpr size_t sizeOfFrameArray(unsigned capacity) { return sizeof(JSArray) + sizeof(IndexingHeader) + capacity * sizeof(EncodedJSValue); }
constexpr StubKey arrayLiteralWithDouble(unsigned count) { return StubKey(StubFamily::ArrayLiteralWithDouble).withCount(count); }
constexpr StubKey arrayLiteralWithDoubleSlow() { return StubKey(StubFamily::ArrayLiteralWithDoubleSlow); }
// The operands: the number of the DataRequest, and NumberSpace::DataElements of the data or what the word of the storage holds, which may be nothing yet.
constexpr StubKey newCopyOnWriteArray(ArrayElementKind kind, SharedElements elements) { return StubKey(StubFamily::NewCopyOnWriteArray).withForm(static_cast<uint8_t>(kind) + 3 * static_cast<uint8_t>(elements)); }
constexpr StubKey newEmptyArray(EmptyArray array) { return StubKey(StubFamily::NewEmptyArray).withForm(static_cast<uint8_t>(array)); }
constexpr StubKey newEmptyArrayWithCapacity() { return StubKey(StubFamily::NewEmptyArrayWithCapacity); }
constexpr StubKey newArrayWithLength(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayWithLength).withForm(static_cast<uint8_t>(kind)); }
// The operands: an array whose elements the new array holds as the iterator of an array reads them, a hole as undefined, and below 2^32 the number of elements to make room for, which may be too low.
// An element that the kind does not take makes an array of another kind.
constexpr StubKey newArrayOfElements(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayOfElements).withForm(static_cast<uint8_t>(kind)); }
// The operand: a Set that is walked as the built-in iterator walks it. A member that the kind does not take makes an array of another kind.
constexpr StubKey newArrayOfMembers(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayOfMembers).withForm(static_cast<uint8_t>(kind)); }
// The operands are those of the operation: the length and the capacity in one word, and the indexing type.
constexpr StubKey newArrayOfHolesSlow() { return StubKey(StubFamily::NewArrayOfHolesSlow); }
constexpr StubKey newEmptyArrayAtSite(ArrayElementKind seed) { return StubKey(StubFamily::NewEmptyArrayAtSite).withForm(static_cast<uint8_t>(seed)); }
constexpr StubKey arraySlice(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::BoundedWithPropertyRoom : ArraySliceRange::Bounded)); }
constexpr StubKey arrayCopy(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::WholeWithPropertyRoom : ArraySliceRange::Whole)); }
constexpr StubKey arraySliceAsDoubles() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::BoundedAsDoubles)); }
constexpr StubKey arrayCopyAsDoubles() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::WholeAsDoubles)); }
// The `arguments` object of strict code. The operands are those of the operation: the number of arguments, the list of those behind the parameters, and where the values of the parameters are.
constexpr StubKey createArguments() { return StubKey(StubFamily::CreateArguments); }
// The `arguments` object of sloppy code. The operands are those of the operation, the function first, and behind them the number of parameters.
constexpr StubKey createMappedArguments() { return StubKey(StubFamily::CreateMappedArguments); }
constexpr StubKey arrayPush() { return StubKey(StubFamily::ArrayPush); }
constexpr StubKey arrayPushOfKnownKind() { return StubKey(StubFamily::ArrayPushOfKnownKind); }
// The operands: an array, and the array whose elements go behind its own as the iterator of an array reads them: a hole as undefined.
constexpr StubKey appendElements() { return StubKey(StubFamily::AppendElements); }
// The operands: an array, and a Set that is walked as the built-in iterator walks it, whose members go behind the elements of the array.
constexpr StubKey appendMembers() { return StubKey(StubFamily::AppendMembers); }
// The operands: an array, and any value but undefined and null, which is spread behind the elements of the array. Only where nobody can see a list that is as it was made being iterated.
constexpr StubKey appendIterable() { return StubKey(StubFamily::AppendIterable); }
// The operands: any value but undefined and null, a count, and room for as many values: what the steps of a pattern hand out. Under the same condition.
constexpr StubKey destructureIterable() { return StubKey(StubFamily::DestructureIterable); }
// What stands behind the code in line of an access to an element of an array whose holes read as undefined.
constexpr StubKey arrayPutAtIndex(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtIndex).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey arrayPutAtNumber(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtNumber).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey arrayGetAtIndex(ArrayRead read = ArrayRead::OfListAsMade) { return StubKey(StubFamily::ArrayGetAtIndex).withForm(static_cast<uint8_t>(read)); }
constexpr StubKey arrayGetAtNumber() { return StubKey(StubFamily::ArrayGetAtNumber); }
// Operands: a JSArray with pristine elements, a JSString and, in the FromStart forms, the first index to look at, below 2^32. Result: the index of the first equal string, or -1.
constexpr StubKey arrayIndexOfString(SearchedElements elements) { return StubKey(StubFamily::ArrayIndexOfString).withForm(static_cast<uint8_t>(elements)); }

Jump branchIfHavingABadTime(StubAssembler&);
// In the space of plain cells.
ptrdiff_t offsetOfArrayWithStorageAllocator(unsigned vectorLength, unsigned propertyRoom = 0);
void emitEmptyArrayOfHoles(StubAssembler&, EmptyArray);
void emitElementBarrierAndReturn(StubAssembler&, GPRReg array, GPRReg value);

struct SparseElementSearch {
    // Has an ArrayStorage. Preserved.
    GPRReg owner;
    // Below 2^32. Preserved.
    GPRReg index;
    // On x86-64 rax and rdx.
    GPRReg entry;
    GPRReg probe;
    GPRReg table;
};
// Falls through with the address of the SparseArrayEntry where the map of the storage has the index.
void emitFindSparseElement(StubAssembler&, const SparseElementSearch&, JumpList& isAbsent);

static_assert(exists(arrayLiteral(0)) && exists(arrayLiteral(maxArrayElementsInRegisters)) && !exists(arrayLiteral(maxArrayElementsInRegisters).withCount(maxArrayElementsInRegisters + 1)));
static_assert(exists(frameArrayLiteral(0)) && exists(frameArrayLiteral(maxArrayElementsInRegisters)) && !exists(frameArrayLiteral(maxArrayElementsInRegisters).withCount(maxArrayElementsInRegisters + 1)));
static_assert(exists(arrayLiteralWithDouble(1)) && !exists(arrayLiteralWithDouble(1).withCount(0)) && exists(arrayLiteralWithDoubleSlow()));
static_assert(exists(newCopyOnWriteArray(ArrayElementKind::Contiguous, SharedElements::InStorageOfSite)) && !exists(newCopyOnWriteArray(ArrayElementKind::Contiguous, SharedElements::InStorageOfSite).withForm(6)));
static_assert(exists(newEmptyArray(EmptyArray::Int32DefaultCapacity)) && !exists(newEmptyArray(EmptyArray::Int32DefaultCapacity).withForm(6)));
static_assert(exists(newArrayWithLength(ArrayElementKind::Contiguous)) && exists(newEmptyArrayAtSite(ArrayElementKind::Contiguous)) && exists(newEmptyArrayWithCapacity()) && exists(newArrayOfHolesSlow()));
static_assert(exists(arraySlice(true)) && exists(arrayCopy(true)) && exists(arraySliceAsDoubles()) && exists(arrayCopyAsDoubles()) && !exists(arrayCopyAsDoubles().withForm(6)));
static_assert(exists(arrayIndexOfString(SearchedElements::Strings)) && exists(arrayIndexOfString(SearchedElements::StringsFromStart)) && !exists(arrayIndexOfString(SearchedElements::Strings).withForm(4)));
static_assert(exists(arraySlice()) && exists(arrayCopy()) && exists(createArguments()) && exists(arrayPush()) && exists(arrayPushOfKnownKind()) && exists(appendElements()));
static_assert(exists(appendMembers()) && exists(newArrayOfElements(ArrayElementKind::Contiguous)) && exists(newArrayOfMembers(ArrayElementKind::Contiguous)) && !exists(newArrayOfMembers(ArrayElementKind::Contiguous).withForm(3)));
static_assert(bodyOf(arrayPushOfKnownKind()) == arrayPush());
static_assert(exists(arrayPutAtIndex(ArrayStoredValue::Number)) && exists(arrayPutAtNumber(ArrayStoredValue::Number)) && exists(arrayGetAtIndex()) && exists(arrayGetAtIndex(ArrayRead::OfAnyList)) && !exists(arrayGetAtIndex().withForm(2)) && exists(arrayGetAtNumber()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
