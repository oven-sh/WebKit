/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

struct CopyableUnit {
    size_t size;
    size_t alignment;
};

struct CopyableUse {
    uint32_t copyable;
    uint32_t firstSite;
    uint32_t lastSite;
    int64_t reach;
};

struct RangeInUnit {
    uint32_t begin;
    uint32_t end;
};

enum class CopyPolicy : uint8_t {
    BeforeUser,
    AfterUsers,
};

struct UnitPlacementInputs {
    std::span<const size_t> sizes;
    std::span<const size_t> alignedOffsets;
    // One for each unit, or one for all of them.
    std::span<const size_t> alignments;
    const BitVector& fallsIntoNext;
    std::span<const CopyableUnit> copyables;
    std::span<const CopyableUse> uses;
    std::span<const uint32_t> firstUseByUnit;
    std::span<const CopyableUse> usesOfCopyables;
    std::span<const uint32_t> firstUseByCopyable;
    size_t alignmentAfterCopies;
    size_t start;
    size_t shiftOfFirstUnit;
    CopyPolicy policy;
    // What should lie in one line of lineSize bytes, unit by unit, each no longer than a line. A unit is moved up by less than a line for it, unless another unit falls into it.
    std::span<const RangeInUnit> rangesKeptInLine { };
    std::span<const uint32_t> firstRangeByUnit { };
    size_t lineSize { 1 };
};

struct PlacedCopy {
    uint32_t copyable;
    size_t offset;
    Vector<size_t, 2> copyOffsetByUse;
};

struct UnitPlacement {
    Vector<size_t> unitStarts;
    Vector<size_t> copyOffsetByUse;
    Vector<PlacedCopy, 4> copies;
    size_t end { 0 };
};

UnitPlacement placeUnits(const UnitPlacementInputs&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
