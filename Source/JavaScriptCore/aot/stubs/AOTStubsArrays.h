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
enum class EmptyArray : uint8_t { ContiguousCapacity1, ContiguousCapacity3, DoubleCapacity1, DoubleCapacity3, DoubleDefaultCapacity, Int32DefaultCapacity, Int32Capacity1, Int32Capacity3, ContiguousCapacity5, ContiguousCapacity7, ContiguousCapacity9, ContiguousCapacity13, ContiguousCapacity17 };
enum class ArraySliceRange : uint8_t { Bounded, Whole, BoundedWithPropertyRoom, WholeWithPropertyRoom, BoundedAsDoubles, WholeAsDoubles };
enum class ArrayStoredValue : uint8_t { Value, Number, ValueInSloppyCode };
enum class ArrayRead : uint8_t { OfListAsMade, OfAnyList, OfPlainList };
enum class SharedElements : uint8_t { InImage, InStorageOfSite };
enum class SearchedElements : uint8_t { Values, ValuesFromStart };

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
constexpr StubKey arrayLiteralWithInt32(unsigned count) { return StubKey(StubFamily::ArrayLiteralWithInt32).withCount(count); }
constexpr StubKey arrayLiteralOfNumbers(unsigned count) { return StubKey(StubFamily::ArrayLiteralOfNumbers).withCount(count); }
constexpr StubKey arrayLiteralWithRoom(unsigned count, bool holdsInt32) { return StubKey(StubFamily::ArrayLiteralWithRoom).withForm(holdsInt32).withCount(count); }
constexpr StubKey arrayLiteralWithDouble(unsigned count) { return StubKey(StubFamily::ArrayLiteralWithDouble).withCount(count); }
constexpr StubKey arrayLiteralWithDoubleSlow() { return StubKey(StubFamily::ArrayLiteralWithDoubleSlow); }
constexpr StubKey newCopyOnWriteArray(ArrayElementKind kind, SharedElements elements) { return StubKey(StubFamily::NewCopyOnWriteArray).withForm(static_cast<uint8_t>(kind) + 3 * static_cast<uint8_t>(elements)); }
constexpr StubKey listOwnKeys() { return StubKey(StubFamily::ListOwnKeys); }
constexpr StubKey newEmptyArray(EmptyArray array) { return StubKey(StubFamily::NewEmptyArray).withForm(static_cast<uint8_t>(array)); }
constexpr StubKey newEmptyArrayWithCapacity() { return StubKey(StubFamily::NewEmptyArrayWithCapacity); }
constexpr StubKey newArrayFromBuffer() { return StubKey(StubFamily::NewArrayFromBuffer); }
constexpr StubKey newArrayOfList(bool isContiguous) { return StubKey(StubFamily::NewArrayOfList).withForm(isContiguous); }
constexpr StubKey newArrayWithLength(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayWithLength).withForm(static_cast<uint8_t>(kind)); }
constexpr StubKey newArrayOfHolesSlow() { return StubKey(StubFamily::NewArrayOfHolesSlow); }
constexpr StubKey newArrayOfLength(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayOfLength).withForm(static_cast<uint8_t>(kind)); }
constexpr StubKey newArrayOfGiven() { return StubKey(StubFamily::NewArrayOfGiven); }
constexpr StubKey newEmptyArrayAtSite(ArrayElementKind seed) { return StubKey(StubFamily::NewEmptyArrayAtSite).withForm(static_cast<uint8_t>(seed)); }
constexpr StubKey newArrayWithLengthAtSite(ArrayElementKind seed) { return StubKey(StubFamily::NewArrayWithLengthAtSite).withForm(static_cast<uint8_t>(seed)); }
constexpr StubKey arraySlice(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::BoundedWithPropertyRoom : ArraySliceRange::Bounded)); }
constexpr StubKey arrayCopy(bool withPropertyRoom = false) { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(withPropertyRoom ? ArraySliceRange::WholeWithPropertyRoom : ArraySliceRange::Whole)); }
constexpr StubKey arraySliceAsDoubles() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::BoundedAsDoubles)); }
constexpr StubKey arrayCopyAsDoubles() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::WholeAsDoubles)); }
constexpr StubKey createArguments() { return StubKey(StubFamily::CreateArguments); }
constexpr StubKey createMappedArguments() { return StubKey(StubFamily::CreateMappedArguments); }
constexpr StubKey arrayPush() { return StubKey(StubFamily::ArrayPush); }
constexpr StubKey arrayPushOfKnownKind() { return StubKey(StubFamily::ArrayPushOfKnownKind); }
constexpr StubKey appendElements() { return StubKey(StubFamily::AppendElements); }
constexpr StubKey appendMembers() { return StubKey(StubFamily::AppendMembers); }
constexpr StubKey appendValuesOfMap() { return StubKey(StubFamily::AppendValuesOfMap); }
constexpr StubKey appendIterable() { return StubKey(StubFamily::AppendIterable); }
constexpr StubKey destructureIterable() { return StubKey(StubFamily::DestructureIterable); }
constexpr StubKey arrayPutAtIndex(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtIndex).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey storeWideningIntegers() { return StubKey(StubFamily::StoreWideningIntegers); }
constexpr StubKey arrayPutAtNumber(ArrayStoredValue stored) { return StubKey(StubFamily::ArrayPutAtNumber).withForm(static_cast<uint8_t>(stored)); }
constexpr StubKey reserveArrayCapacity() { return StubKey(StubFamily::ReserveArrayCapacity); }
struct ListWhoseLengthIsStoredWithoutRunTime {
    static constexpr IndexingType firstKind = ArrayWithUndecided;
    static constexpr IndexingType lastKind = ArrayWithContiguous;
    static constexpr uint32_t bitsThatItsStructureMayNotHave = Structure::s_anyImmutablePropertiesBits;
    static constexpr uint32_t mostElementsOfVector = (MarkedSpace::largeCutoff - sizeof(IndexingHeader)) / sizeof(EncodedJSValue);
};
constexpr StubKey setArrayLength(bool isInSloppyCode) { return StubKey(StubFamily::SetArrayLength).withForm(isInSloppyCode); }
constexpr StubKey arrayGetAtIndex(ArrayRead read = ArrayRead::OfListAsMade) { return StubKey(StubFamily::ArrayGetAtIndex).withForm(static_cast<uint8_t>(read)); }
constexpr StubKey arrayGetAtNumber(ArrayRead read = ArrayRead::OfListAsMade) { return StubKey(StubFamily::ArrayGetAtNumber).withForm(static_cast<uint8_t>(read)); }
constexpr StubKey arrayIndexOfString(SearchedElements elements) { return StubKey(StubFamily::ArrayIndexOfString).withForm(static_cast<uint8_t>(elements)); }

Jump branchIfHavingABadTime(StubAssembler&);
ptrdiff_t offsetOfArrayWithStorageAllocator(unsigned vectorLength, unsigned propertyRoom = 0);
void emitEmptyArrayOfHoles(StubAssembler&, EmptyArray);
void emitElementBarrierAndReturn(StubAssembler&, GPRReg array, GPRReg value);

struct SparseElementSearch {
    GPRReg owner;
    GPRReg index;
    GPRReg entry;
    GPRReg probe;
    GPRReg table;
};
void emitFindSparseElement(StubAssembler&, const SparseElementSearch&, JumpList& isAbsent);

static_assert(exists(arrayLiteral(0)) && exists(arrayLiteral(maxArrayElementsInRegisters)) && !exists(arrayLiteral(maxArrayElementsInRegisters).withCount(maxArrayElementsInRegisters + 1)));
static_assert(exists(arrayLiteralWithRoom(1, true)) && exists(arrayLiteralWithRoom(maxArrayElementsInRegisters, false)) && !exists(arrayLiteralWithRoom(1, false).withCount(0)));
static_assert(exists(arrayLiteralWithDouble(1)) && !exists(arrayLiteralWithDouble(1).withCount(0)) && exists(arrayLiteralWithDoubleSlow()));
static_assert(exists(newCopyOnWriteArray(ArrayElementKind::Contiguous, SharedElements::InStorageOfSite)) && !exists(newCopyOnWriteArray(ArrayElementKind::Contiguous, SharedElements::InStorageOfSite).withForm(6)));
static_assert(exists(newEmptyArray(EmptyArray::ContiguousCapacity17)) && !exists(newEmptyArray(EmptyArray::ContiguousCapacity17).withForm(13)));
static_assert(exists(newArrayWithLengthAtSite(ArrayElementKind::Double)) && !exists(newArrayWithLengthAtSite(ArrayElementKind::Contiguous)));
static_assert(exists(newArrayOfLength(ArrayElementKind::Contiguous)) && exists(newArrayOfGiven()));
static_assert(exists(newArrayWithLength(ArrayElementKind::Contiguous)) && exists(newEmptyArrayAtSite(ArrayElementKind::Contiguous)) && exists(newEmptyArrayWithCapacity()) && exists(newArrayOfHolesSlow()));
static_assert(exists(arraySlice(true)) && exists(arrayCopy(true)) && exists(arraySliceAsDoubles()) && exists(arrayCopyAsDoubles()) && !exists(arrayCopyAsDoubles().withForm(6)));
static_assert(exists(arrayIndexOfString(SearchedElements::Values)) && exists(arrayIndexOfString(SearchedElements::ValuesFromStart)) && !exists(arrayIndexOfString(SearchedElements::Values).withForm(2)));
static_assert(exists(arraySlice()) && exists(arrayCopy()) && exists(createArguments()) && exists(arrayPush()) && exists(arrayPushOfKnownKind()) && exists(appendElements()));
static_assert(exists(appendMembers()));
static_assert(bodyOf(arrayPushOfKnownKind()) == arrayPush());
static_assert(exists(arrayPutAtIndex(ArrayStoredValue::ValueInSloppyCode)) && !exists(arrayPutAtNumber(ArrayStoredValue::ValueInSloppyCode)));
static_assert(exists(arrayPutAtIndex(ArrayStoredValue::Number)) && exists(arrayPutAtNumber(ArrayStoredValue::Number)) && exists(arrayGetAtIndex()) && exists(arrayGetAtIndex(ArrayRead::OfPlainList)) && !exists(arrayGetAtIndex().withForm(3)) && exists(arrayGetAtNumber(ArrayRead::OfPlainList)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
