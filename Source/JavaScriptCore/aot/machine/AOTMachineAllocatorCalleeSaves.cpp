/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include <bit>

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned costOfCalleeSaveNotYetSaved = 2;

static bool isWorthCalleeSaveNotYetSaved(const Allocator& allocator, const Operation* value, const Block* block)
{
    if (allocator.m_order.of(block).loop != noLoop)
        return true;

    const Liveness& liveness = allocator.m_liveness;
    const ValueFacts& facts = liveness.of(value);
    unsigned worth = 1;
    Position lastRead = 0;
    for (uint32_t use = facts.firstUse; use; use = liveness.use(use).next) {
        lastRead = liveness.use(use).position;
        if (!liveness.isUseInRareRegion(use) && ++worth > costOfCalleeSaveNotYetSaved)
            return true;
    }
    return facts.lastUse != noPosition && facts.lastUse > lastRead;
}

RegisterMask Allocator::calleeSavesWorthTakingFor(const Operation* value, Bank bank, const Block* block) const
{
    RegisterMask calleeSaved = m_file.calleeSavedIn(bank);
    RegisterMask notYetSaved = calleeSaved & ~m_allocation.usedCalleeSaves();
    if (!notYetSaved || !m_proc.traits().chargesForCalleeSaves || m_proc.traits().savesRegistersOfCallers)
        return calleeSaved;
    if (isARM64() && bank == Bank::GP && (std::popcount(calleeSaved & ~notYetSaved) & 1))
        return calleeSaved;
    if (isWorthCalleeSaveNotYetSaved(*this, value, block))
        return calleeSaved;
    return calleeSaved & ~notYetSaved;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
