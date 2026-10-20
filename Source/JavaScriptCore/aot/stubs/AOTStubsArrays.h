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
#include "IndexingType.h"

namespace JSC { namespace AOT {

static_assert(maxArrayElementsInRegisters <= 4);

enum class ArrayElementKind : uint8_t { Int32, Double, Contiguous };
enum class EmptyArray : uint8_t { ContiguousCapacity1, ContiguousCapacity3, DoubleCapacity1, DoubleCapacity3, DoubleDefaultCapacity, Int32DefaultCapacity };
enum class ArraySliceRange : uint8_t { Bounded, Whole };

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
constexpr StubKey arrayLiteralWithDouble(unsigned count) { return StubKey(StubFamily::ArrayLiteralWithDouble).withCount(count); }
constexpr StubKey arrayLiteralWithDoubleSlow() { return StubKey(StubFamily::ArrayLiteralWithDoubleSlow); }
constexpr StubKey newEmptyArray(EmptyArray array) { return StubKey(StubFamily::NewEmptyArray).withForm(static_cast<uint8_t>(array)); }
constexpr StubKey newEmptyArrayWithCapacity() { return StubKey(StubFamily::NewEmptyArrayWithCapacity); }
constexpr StubKey newArrayWithLength(ArrayElementKind kind) { return StubKey(StubFamily::NewArrayWithLength).withForm(static_cast<uint8_t>(kind)); }
constexpr StubKey newEmptyArrayAtSite(ArrayElementKind seed) { return StubKey(StubFamily::NewEmptyArrayAtSite).withForm(static_cast<uint8_t>(seed)); }
constexpr StubKey arraySlice() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::Bounded)); }
constexpr StubKey arrayCopy() { return StubKey(StubFamily::ArraySlice).withForm(static_cast<uint8_t>(ArraySliceRange::Whole)); }
constexpr StubKey arrayPush() { return StubKey(StubFamily::ArrayPush); }
constexpr StubKey arrayPushOfKnownKind() { return StubKey(StubFamily::ArrayPushOfKnownKind); }
constexpr StubKey arrayPushDouble() { return StubKey(StubFamily::ArrayPushDouble); }

struct ArrayAllocation {
    GPRReg array;
    GPRReg storage;
    GPRReg allocator;
    GPRReg scratch;
    unsigned length;
    unsigned vectorLength;
    IndexingType indexingType;
};

struct ArrayRefills {
    AssemblyHelpers::FreeListRefill storage;
    AssemblyHelpers::FreeListRefill array;
};

[[nodiscard]] ArrayRefills allocateArrayWithLateRefills(StubAssembler&, const ArrayAllocation&, JumpList& slowPath);
void finishArrayAllocation(StubAssembler&, const ArrayAllocation&);
void emitLateRefills(StubAssembler&, const ArrayRefills&, JumpList& slowPath);
Jump branchIfHavingABadTime(StubAssembler&);
ptrdiff_t offsetOfArrayStorageAllocator(unsigned vectorLength);
void emitEmptyArrayOfHoles(StubAssembler&, EmptyArray);
void emitElementBarrierAndReturn(StubAssembler&, GPRReg array, GPRReg value);

static_assert(exists(arrayLiteral(0)) && exists(arrayLiteral(maxArrayElementsInRegisters)) && !exists(arrayLiteral(maxArrayElementsInRegisters).withCount(maxArrayElementsInRegisters + 1)));
static_assert(exists(arrayLiteralWithDouble(1)) && !exists(arrayLiteralWithDouble(1).withCount(0)) && exists(arrayLiteralWithDoubleSlow()));
static_assert(exists(newEmptyArray(EmptyArray::Int32DefaultCapacity)) && !exists(newEmptyArray(EmptyArray::Int32DefaultCapacity).withForm(6)));
static_assert(exists(newArrayWithLength(ArrayElementKind::Contiguous)) && exists(newEmptyArrayAtSite(ArrayElementKind::Contiguous)) && exists(newEmptyArrayWithCapacity()));
static_assert(exists(arraySlice()) && exists(arrayCopy()) && exists(arrayPush()) && exists(arrayPushOfKnownKind()) && exists(arrayPushDouble()));
static_assert(bodyOf(arrayPushOfKnownKind()) == arrayPush());

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
