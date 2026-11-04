/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineCallsOnWays.h"

#if ENABLE(AOT)

#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// Each block with a call costs a search for the block and a look at the reads behind the call. A value that so many arms pass by is taken to live across a call, as its flag says.
static constexpr unsigned mostBlocksWithCallsAskedAbout = 4;

static const Block* findBlockAroundCallOnWays(const Order& order, Position call)
{
    std::span<Block* const> blocks = order.blocksInOrder();
    auto next = std::ranges::upper_bound(blocks, call, { }, [&](const Block* block) {
        return order.headOf(block);
    });
    ASSERT(next != blocks.begin());
    return *(next - 1);
}

// `call`: the late position of the first call, outside rare regions, that the value may live across. noPosition if there is none.
// A value that is dead behind the first call of a block is dead behind the others too.
static bool callStandsOnWayToReadOnWays(Allocator& allocator, const Operation* value, Position call)
{
    const Liveness& liveness = allocator.m_liveness;
    Bank bank = bankOf(value->type());
    for (unsigned asked = 0; asked < mostBlocksWithCallsAskedAbout; ++asked) {
        if (call > liveness.of(value).lastUse)
            return false;
        const Block* block = findBlockAroundCallOnWays(allocator.m_order, call);
        if (!allocator.isDeadOnEveryPathFrom(value, block, call))
            return true;
        call = liveness.nextCallBehindEndOf(block, bank);
    }
    return true;
}

bool callStandsOnWayFromDefinitionToRead(Allocator& allocator, const Operation* value)
{
    const Liveness& liveness = allocator.m_liveness;
    // The calls of a rare region count for what is defined in it, and the scan lists none of them.
    if (allocator.isInRareRegion() || liveness.isSunk(value))
        return true;
    // Every way from the definition to a read leads through the rest of the block.
    Bank bank = bankOf(value->type());
    if (liveness.lastCallIn(allocator.m_block, bank) > liveness.of(value).definition)
        return true;
    return callStandsOnWayToReadOnWays(allocator, value, liveness.nextCallBehindEndOf(allocator.m_block, bank));
}

bool callStandsOnWayFromHeadToRead(Allocator& allocator, const Operation* value)
{
    ASSERT(!allocator.isInRareRegion());
    return callStandsOnWayToReadOnWays(allocator, value, allocator.m_liveness.nextCallBehindHeadOf(allocator.m_block, bankOf(value->type())));
}

bool callStandsOnWayFromPhiToRead(Allocator& allocator, const Operation* phi, const Block* block)
{
    const Liveness& liveness = allocator.m_liveness;
    Position call = liveness.nextCallBehindHeadOf(block, bankOf(phi->type()));
    // Every way from the head to a read behind the block leads through the whole of it.
    if (call < allocator.m_order.endOf(block))
        return liveness.of(phi).lastUse > call;
    return callStandsOnWayToReadOnWays(allocator, phi, call);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
