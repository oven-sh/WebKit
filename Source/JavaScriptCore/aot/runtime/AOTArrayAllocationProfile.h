/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "ArrayConventions.h"
#include "IndexingType.h"
#include <cstdint>
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

class JSArray;
class Structure;
class VM;

namespace AOT {

struct ArrayAllocationProfile {
    static constexpr unsigned allocationCountShift = 16;
    static constexpr unsigned shapeShift = 8;
    static constexpr uint32_t capacityMask = 0xff;
    static constexpr uint32_t shapeMask = 0xff << shapeShift;
    static constexpr uint32_t oneAllocation = 1u << allocationCountShift;
    static constexpr uint32_t countMask = 0xffffu << allocationCountShift;
    static constexpr uint32_t shortestInterval = 64;
    static constexpr uint8_t maxIntervalDoublings = 4;
    static constexpr uint8_t reviewsInWindow = 6;
    static constexpr unsigned lengthsKept = 3;
    static constexpr unsigned sizeOfLastArrayWord = 8;

    unsigned capacity() const { return word & capacityMask; }
    bool isDueForReview() const { return (word & countMask) == countMask; }
    uint32_t interval() const { return shape() ? shortestInterval << intervalDoublings : 1; }
    void startInterval() { word = (word & ~countMask) | (interval() - 1 - (interval() > reviewsInWindow ? reviews : 0)) << allocationCountShift; }
    IndexingType shape() const { return static_cast<IndexingType>((word & shapeMask) >> shapeShift); }
    void set(unsigned capacity, IndexingType shape)
    {
        ASSERT(capacity <= capacityMask);
        word = (word & ~(capacityMask | shapeMask)) | capacity | static_cast<uint32_t>(shape) << shapeShift;
    }

    uint32_t word;
    uint8_t reviews : 4;
    uint8_t intervalDoublings : 4;
    uint8_t largestLengths[lengthsKept];
};

static_assert(sizeof(ArrayAllocationProfile) == 8 && !OBJECT_OFFSETOF(ArrayAllocationProfile, word));
static_assert(ArrayAllocationProfile::reviewsInWindow < 16 && ArrayAllocationProfile::maxIntervalDoublings < 16 && ArrayAllocationProfile::shortestInterval << ArrayAllocationProfile::maxIntervalDoublings <= 1u << 16);
static_assert(std::is_trivially_default_constructible_v<ArrayAllocationProfile>);
static_assert(BASE_CONTIGUOUS_VECTOR_LEN_MAX <= ArrayAllocationProfile::capacityMask);
static_assert(Int32Shape < DoubleShape && DoubleShape < ContiguousShape);

void reviewArrayAllocationProfile(ArrayAllocationProfile&, JSArray* lastArrayOrNull);

inline constexpr unsigned propertyRoomOfArray = 4 * sizeof(uint64_t);
constexpr size_t sizeOfArrayWithStorage(unsigned capacity, unsigned propertyRoom = 0) { return (3 + capacity) * sizeof(uint64_t) + propertyRoom; }

enum class NewElements : bool { AreHoles, WrittenByCaller };
JSArray* tryCreateArrayWithStorage(VM&, Structure*, unsigned publicLength, unsigned vectorLength, unsigned propertyRoom = 0, NewElements = NewElements::AreHoles);
inline constexpr uint32_t maxValuesInArrayLiteral = (1u << 25) - 1;

inline constexpr unsigned holeArrayCapacityShift = 8;
inline constexpr unsigned maxHoleArrayCapacity = 127;
inline constexpr unsigned seedShiftBesideLengthAndCapacity = 16;
static_assert(maxHoleArrayCapacity < 1u << (seedShiftBesideLengthAndCapacity - holeArrayCapacityShift));
inline constexpr unsigned arrayLiteralWithDoubleIsNumberMaskShift = 8;
inline constexpr uint32_t valuesAreRawNumbers = 1u << 8;

inline constexpr unsigned maxLengthForElementwiseCopy = 64;

namespace NewArrayWithHolesFlag {
inline constexpr uint32_t mustHaveIndexingType = 2;
} // namespace NewArrayWithHolesFlag

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
