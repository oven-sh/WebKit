/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStartOrder.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

namespace {

struct RankedUnit {
    friend auto operator<=>(const RankedUnit&, const RankedUnit&) = default;

    uint32_t rank;
    uint32_t position;
};

} // anonymous namespace

Vector<uint32_t> rankFunctionsByListedUnits(std::span<const uint32_t> positionsOfListedUnits, const LinkedFunctions& functions, size_t numberOfFunctions)
{
    Vector<uint32_t> ranks(FillWith { }, numberOfFunctions, noStartRank);
    uint32_t nextRank = 0;
    for (uint32_t position : positionsOfListedUnits) {
        uint32_t& rank = ranks[functions[position].unit.function.index()];
        if (rank == noStartRank)
            rank = nextRank++;
    }
    return ranks;
}

Vector<uint32_t> positionsOfRankedUnits(std::span<const uint32_t> startRanks, const LinkedFunctions& functions)
{
    Vector<RankedUnit> units;
    for (uint32_t position = 0; position < functions.size(); ++position) {
        uint32_t rank = startRanks[functions[position].unit.function.index()];
        if (rank != noStartRank)
            units.append({ rank, position });
    }
    std::ranges::sort(units);
    return units.map([](const RankedUnit& unit) {
        return unit.position;
    });
}

std::optional<DataID> findDataNamedBy(const Mention& mention, std::span<const uint32_t> key)
{
    if (mention.space == NumberSpace::DataStorage || mention.space == NumberSpace::DataElements)
        return DataID(key[0]);
    if (mention.space == NumberSpace::Request) {
        auto kind = static_cast<RequestKind>(key[0]);
        if (kind == RequestKind::Data || kind == RequestKind::CollectionData)
            return DataID(key[1]);
    }
    return std::nullopt;
}

Vector<uint32_t> listedNumbersFirst(std::span<const uint32_t> listed, size_t count)
{
    Vector<uint32_t> order;
    order.reserveInitialCapacity(count);
    BitVector isListed(count);
    for (uint32_t number : listed) {
        if (number < count && !isListed.quickSet(number))
            order.append(number);
    }
    for (uint32_t number = 0; number < count; ++number) {
        if (!isListed.quickGet(number))
            order.append(number);
    }
    RELEASE_ASSERT(order.size() == count);
    return order;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
