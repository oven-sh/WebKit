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

namespace JSC { namespace AOT {

static_assert(maxArrayElementsInRegisters <= 4);

enum class ArrayElementKind : uint8_t { Int32, Double, Contiguous };
// OwnStorage: the capacity is how many elements the function that makes the array appends itself.
enum class EmptyArray : uint8_t { ContiguousCapacity1, ContiguousCapacity3, DoubleCapacity1, DoubleCapacity3, DoubleDefaultCapacity, Int32DefaultCapacity, ContiguousCapacity1OwnStorage, ContiguousCapacity3OwnStorage };
enum class ArraySliceRange : uint8_t { Bounded, Whole, BoundedWithPropertyRoom, WholeWithPropertyRoom };
// Number: an array of int32 that takes a number which may be none becomes an array of doubles.
enum class ArrayStoredValue : uint8_t { Value, Number };

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
#elif CPU(ARM64)
inline constexpr StubRegister arrayPushKindRegister = StubRegister::T11;
#endif

inline constexpr int64_t arraySliceToEnd = 1ll << 32;

constexpr StubKey arrayLiteral(unsigned count) { return StubKey(StubFamily::ArrayLiteral).withCount(count); }
// Behind the elements: the address of sizeOfFrameArray(count) bytes in the frame of the caller.
constexpr StubKey frameArrayLiteral(unsigned count) { return StubKey(StubFamily::FrameArrayLiteral).withCount(count); }
constexpr size_t sizeOfFrameArray(unsigned capacity) { return sizeof(JSArray) + sizeof(IndexingHeader) + capacity * sizeof(EncodedJSValue); }
static_assert(sizeOfArrayWithStorage(3) == sizeOfFrameArray(3) && propertyRoomOfArray == initialOutOfLineCapacity * sizeof(EncodedJSValue));
constexpr StubKey arrayLiteralWithDouble(unsigned count) { return StubKey(StubFamily::ArrayLiteralWithDouble).withCount(count); }
constexpr StubKey arrayLiteralWithDoubleSlow() { return StubKey(StubFamily::ArrayLiteralWithDoubleSlow); }
// A cell around the storage of an array of constants. The immediate: the request. The storage is the node in the image, but for SharedValues: there the operand is the word of the statics for it.
enum class ArrayOfData : uint8_t { Int32, Double, Values, SharedValues };
constexpr StubKey arrayOfData(ArrayOfData form) { return StubKey(StubFamily::ArrayOfData).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey newEmptyArray(EmptyArray array) { return StubKey(StubFamily::NewEmptyArray).withForm(static_cast<uint8_t>(array)); }
constexpr StubKey newEmptyArrayWithCapacity() { return StubKey(StubFamily::NewEmptyArrayWithCapacity); }
constexpr StubKey newArrayWithLength(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayWithLength).withForm(static_cast<uint8_t>(kind)); }
constexpr StubKey newEmptyArrayAtSite(ArrayElementKind seed) { return StubKey(StubFamily::NewEmptyArrayAtSite).withForm(static_cast<uint8_t>(seed)); }
constexpr StubKey arraySlice(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::BoundedWithPropertyRoom : ArraySliceRange::Bounded)); }
constexpr StubKey arrayCopy(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::WholeWithPropertyRoom : ArraySliceRange::Whole)); }
constexpr StubKey arrayPush() { return StubKey(StubFamily::ArrayPush); }
constexpr StubKey arrayPushOfKnownKind() { return StubKey(StubFamily::ArrayPushOfKnownKind); }
constexpr StubKey arrayPushDouble() { return StubKey(StubFamily::ArrayPushDouble); }
// The operands: an array, and the array whose elements go behind its own as the iterator of an array reads them: a hole as undefined.
constexpr StubKey appendElements() { return StubKey(StubFamily::AppendElements); }
// What stands behind the code in line of an access to an element of an array whose holes read as undefined.
constexpr StubKey arrayPutAtIndex(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtIndex).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey arrayPutAtNumber(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtNumber).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey arrayGetAtIndex() { return StubKey(StubFamily::ArrayGetAtIndex); }
constexpr StubKey arrayGetAtNumber() { return StubKey(StubFamily::ArrayGetAtNumber); }
// The answer of `in`: 1 or 0.
constexpr StubKey arrayHasAtIndex() { return StubKey(StubFamily::ArrayHasAtIndex); }
constexpr StubKey arrayHasAtNumber() { return StubKey(StubFamily::ArrayHasAtNumber); }

struct ArrayAllocation {
    GPRReg array;
    GPRReg storage;
    GPRReg allocator;
    GPRReg scratch;
    unsigned length;
    unsigned vectorLength;
    IndexingType indexingType;
    bool hasStorageOfItsOwn { false };
};

struct ArrayRefills {
    std::optional<AssemblyHelpers::FreeListRefill> storage;
    AssemblyHelpers::FreeListRefill array;
};

[[nodiscard]] ArrayRefills allocateArrayWithLateRefills(StubAssembler&, const ArrayAllocation&, JumpList& slowPath);
void finishArrayAllocation(StubAssembler&, const ArrayAllocation&);
void emitLateRefills(StubAssembler&, const ArrayRefills&, JumpList& slowPath);
Jump branchIfHavingABadTime(StubAssembler&);
// In the space of plain cells.
ptrdiff_t offsetOfArrayWithStorageAllocator(unsigned vectorLength, unsigned propertyRoom = 0);
void emitEmptyArrayOfHoles(StubAssembler&, EmptyArray);
void emitElementBarrierAndReturn(StubAssembler&, GPRReg array, GPRReg value);

static_assert(exists(arrayLiteral(0)) && exists(arrayLiteral(maxArrayElementsInRegisters)) && !exists(arrayLiteral(maxArrayElementsInRegisters).withCount(maxArrayElementsInRegisters + 1)));
static_assert(exists(arrayLiteralWithDouble(1)) && !exists(arrayLiteralWithDouble(1).withCount(0)) && exists(arrayLiteralWithDoubleSlow()));
static_assert(exists(newEmptyArray(EmptyArray::ContiguousCapacity3OwnStorage)) && !exists(newEmptyArray(EmptyArray::Int32DefaultCapacity).withForm(8)));
static_assert(exists(newArrayWithLength(ArrayElementKind::Contiguous)) && exists(newEmptyArrayAtSite(ArrayElementKind::Contiguous)) && exists(newEmptyArrayWithCapacity()));
static_assert(exists(arraySlice()) && exists(arrayCopy()) && exists(arrayPush()) && exists(arrayPushOfKnownKind()) && exists(arrayPushDouble()) && exists(appendElements()));
static_assert(bodyOf(arrayPushOfKnownKind()) == arrayPush());
static_assert(exists(arrayPutAtIndex(ArrayStoredValue::Number)) && exists(arrayPutAtNumber(ArrayStoredValue::Number)) && exists(arrayGetAtIndex()) && exists(arrayGetAtNumber()) && exists(arrayHasAtIndex()) && exists(arrayHasAtNumber()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
