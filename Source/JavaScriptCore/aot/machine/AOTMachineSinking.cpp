/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

static bool canSinkIntoRareRegion(const Operation* operation)
{
    if (!operation->type().isNumeric())
        return false;

    switch (operation->opcode()) {
    case Add:
    case Sub:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
        return true;

    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return operation->has(OperationFlag::ReadsImmutableHeap) && !operation->traps();

    case Patch: {
        const PatchOperation* patch = operation->as<PatchOperation>();
        return patch->effects.maySink() && !patch->isSuspension();
    }

    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool extractsStandAtHeadOfSinkTarget(const Order& order, const Block* entry)
{
    if (entry->isLandingPad())
        return false;
    std::span<Block* const> predecessors = order.of(entry).predecessors;
    ASSERT(predecessors.size() == 1);
    const Block* predecessor = predecessors.front();
    const Operation* end = predecessor->last();
    return end->opcode() == Patch && end->type().isTuple() && predecessor->successor(0).block() == entry;
}

bool BackwardScan::trySink(Operation* operation)
{
    ASSERT(facts(operation).lastUse != noPosition);
    ASSERT(!facts(operation).has(ValueFlag::IsFused) && !facts(operation).has(ValueFlag::TakesOverTrap));

    uint32_t region = m_summariesOfReads[operation->index()].regionOfUses;
    if (region >= severalRegions || region == m_order.of(operation->owner()).rareRegion)
        return false;
    if (!canSinkIntoRareRegion(operation))
        return false;
    ASSERT(!operation->mustStay());

    Block* entry = m_order.rareRegions()[region].onlyEntry;
    if (!entry || extractsStandAtHeadOfSinkTarget(m_order, entry))
        return false;

    const BlockPlace& placeOfEntry = m_order.of(entry);
    Position head = placeOfEntry.head;
    Position early = m_order.earlyPositionOf(operation);
    if (head <= early)
        return false;

    if (placeOfEntry.loop != noLoop && m_order.loops()[placeOfEntry.loop].begin > early)
        return false;

    if (m_nextSuspension < head)
        return false;

    set(operation, ValueFlag::IsSunk);
    if (m_liveness.m_sunk.isEmpty())
        m_liveness.m_sunk.grow(m_proc.arena(), m_order.mostBlocks());
    m_liveness.m_sunk[entry->index()].append(m_proc.arena(), operation);

    tryFuseScaledIndex(operation);
    const PatchOperation* patch = operation->as<PatchOperation>();
    forEachLeaf(m_liveness, operation, [&](const Operation*, unsigned index, Operation* leaf) {
        Reg hint;
        if (patch && patch->constraintOf(index).isReg())
            hint = patch->constraintOf(index).reg();
        addUse(leaf, head, hint);
        if (leaf->hasNoPosition())
            return;
        noteRegionOfUse(leaf, entry);

        ValueFacts& read = facts(leaf);
        read.lastUse = std::max(read.lastUse, head + 2);
    });

    if (patch)
        noteCall(patch, head + 1, entry);
    return true;
}

void BackwardScan::noteRegionOfUse(Operation* value, const Block* user)
{
    uint32_t& regionOfUses = m_summariesOfReads[value->index()].regionOfUses;
    if (regionOfUses == severalRegions)
        return;
    uint32_t region = m_order.of(user).rareRegion;
    if (region == noRareRegion || (regionOfUses != noRareRegion && regionOfUses != region))
        regionOfUses = severalRegions;
    else
        regionOfUses = region;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
