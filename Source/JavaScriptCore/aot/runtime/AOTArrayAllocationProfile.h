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
class VM;

namespace AOT {

struct ArrayAllocationProfile {
    static constexpr unsigned allocationCountShift = 16;
    static constexpr unsigned shapeShift = 8;
    static constexpr uint32_t capacityMask = 0xff;
    static constexpr uint32_t shapeMask = 0xff << shapeShift;
    static constexpr uint32_t interval = 64;
    static constexpr uint32_t intervalMask = (interval - 1) << allocationCountShift;
    static constexpr uint32_t countAtReview = 1 << allocationCountShift;
    static constexpr uint8_t reviewsBeforeShrinking = 4;
    static constexpr unsigned sizeOfLastArrayWord = 8;

    unsigned capacity() const { return word & capacityMask; }
    bool isDueForReview() const { return (word & intervalMask) == countAtReview; }
    IndexingType shape() const { return static_cast<IndexingType>((word & shapeMask) >> shapeShift); }
    void set(unsigned capacity, IndexingType shape)
    {
        ASSERT(capacity <= capacityMask);
        word = (word & ~(capacityMask | shapeMask)) | capacity | static_cast<uint32_t>(shape) << shapeShift;
    }

    uint32_t word;
    uint8_t reviewsWithRoomToSpare;
    uint8_t largestLengthSinceChange;
};

static_assert(sizeof(ArrayAllocationProfile) == 8 && !OBJECT_OFFSETOF(ArrayAllocationProfile, word));
static_assert(std::is_trivially_default_constructible_v<ArrayAllocationProfile>);
static_assert(BASE_CONTIGUOUS_VECTOR_LEN_MAX <= ArrayAllocationProfile::capacityMask);
static_assert(Int32Shape < DoubleShape && DoubleShape < ContiguousShape);

void reviewArrayAllocationProfile(ArrayAllocationProfile&, JSArray* lastArrayOrNull);

// Makes room for `additional` more elements, unless the next step of the usual growth does. Changes nothing a program can see.
void reserveArrayCapacity(VM&, JSArray*, uint64_t additional);

inline constexpr unsigned holeArrayCapacityShift = 8;
inline constexpr unsigned maxHoleArrayCapacity = 127;
inline constexpr unsigned arrayLiteralWithDoubleIsNumberMaskShift = 8;
inline constexpr uint32_t valuesAreRawNumbers = 1u << 8;

inline constexpr unsigned maxLengthForElementwiseCopy = 64;
inline constexpr unsigned maxLengthForElementwiseMove = 32;

namespace NewArrayWithHolesFlag {
inline constexpr uint32_t lengthIsCapacity = 1;
inline constexpr uint32_t mustHaveIndexingType = 2;
} // namespace NewArrayWithHolesFlag

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
