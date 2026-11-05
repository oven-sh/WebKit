/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocator.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

bool callStandsOnWayFromDefinitionToRead(Allocator&, const Operation* value);
bool callStandsOnWayFromHeadToRead(Allocator&, const Operation* value);
bool callStandsOnWayFromPhiToRead(Allocator&, const Operation* phi, const Block*);

bool isDeadOnEveryPathFromBlockOutsideRareRegions(Allocator&, const Operation* value, const Block*, const BlockPlace&, Position from, uint32_t firstReadFrom);

ALWAYS_INLINE uint32_t findPlaceOfBlockAtPosition(const Order& order, Position position, uint32_t placeAhead)
{
    Block* const* const blocks = order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(order.blocksInOrder().size());
    const BlockPlace* const places = order.places();
    auto standsBehindPosition = [&](uint32_t place) ALWAYS_INLINE_LAMBDA {
        return places[blocks[place]->index()].head > position;
    };
    ASSERT(placeAhead < numberOfBlocks && !standsBehindPosition(placeAhead));

    uint32_t low = placeAhead;
    uint32_t high = numberOfBlocks;
    for (uint32_t step = 1; step < numberOfBlocks - low; step *= 2) {
        if (standsBehindPosition(low + step)) {
            high = low + step;
            break;
        }
        low += step;
    }
    while (high - low > 1) {
        uint32_t middle = low + (high - low) / 2;
        if (standsBehindPosition(middle))
            high = middle;
        else
            low = middle;
    }
    return low;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
