/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineCallsOnWays.h"

#if ENABLE(AOT)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned mostBlocksWithCallsAskedAbout = 4;

static bool callStandsOnWayToReadOnWays(Allocator& allocator, const Operation* value, Position call, const Block* blockAheadOfCall)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    Block* const* const blocks = order.blocksInOrder().data();
    const ValueFacts& facts = liveness.of(value);
    Bank bank = bankOf(value->type());
    uint32_t place = order.of(blockAheadOfCall).orderIndex;
    for (unsigned asked = 0; asked < mostBlocksWithCallsAskedAbout; ++asked) {
        if (call > facts.lastUse)
            return false;
        place = findPlaceOfBlockAtPosition(order, call, place);
        const Block* block = blocks[place];
        const BlockPlace& placeOfBlock = order.of(block);
        if (facts.definition == noPosition || facts.definition > call || facts.has(ValueFlag::IsKeptForCollector) || placeOfBlock.isRare())
            return true;
        if (!isDeadOnEveryPathFromBlockOutsideRareRegions(allocator, value, block, placeOfBlock, call, allocator.findReadFrom(value, call)))
            return true;
        call = liveness.nextCallBehindEndOf(block, bank);
    }
    return true;
}

bool callStandsOnWayFromDefinitionToRead(Allocator& allocator, const Operation* value)
{
    const Liveness& liveness = allocator.m_liveness;
    if (allocator.isInRareRegion() || liveness.isSunk(value))
        return true;
    Bank bank = bankOf(value->type());
    if (liveness.lastCallIn(allocator.m_block, bank) > liveness.of(value).definition)
        return true;
    return callStandsOnWayToReadOnWays(allocator, value, liveness.nextCallBehindEndOf(allocator.m_block, bank), allocator.m_block);
}

bool callStandsOnWayFromHeadToRead(Allocator& allocator, const Operation* value)
{
    ASSERT(!allocator.isInRareRegion());
    return callStandsOnWayToReadOnWays(allocator, value, allocator.m_liveness.nextCallBehindHeadOf(allocator.m_block, bankOf(value->type())), allocator.m_block);
}

bool callStandsOnWayFromPhiToRead(Allocator& allocator, const Operation* phi, const Block* block)
{
    const Liveness& liveness = allocator.m_liveness;
    Position call = liveness.nextCallBehindHeadOf(block, bankOf(phi->type()));
    if (call < allocator.m_order.endOf(block))
        return liveness.of(phi).lastUse > call;
    return callStandsOnWayToReadOnWays(allocator, phi, call, block);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
