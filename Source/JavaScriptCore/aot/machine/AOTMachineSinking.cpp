/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

// At every place that its definition dominates it gives the same result, and it does nothing else.
static bool canSinkIntoRareRegion(const Operation* operation)
{
    // The Extracts of a tuple stand right behind it.
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
        // A write may stand between a load from any other heap and the region.
        return operation->has(OperationFlag::ReadsImmutableHeap) && !operation->traps();

    case Patch: {
        const PatchOperation* patch = operation->as<PatchOperation>();
        return patch->effects.maySink() && !patch->isSuspension();
    }

    // An Opaque stays where the emitter put it. An ArgumentReg is in its register at the entrypoint alone.
    case Opaque:
    case ArgumentReg:
    case Phi:
    case Extract:
    case Nop:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Upsilon:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The Extracts of a Patch that ends a block stand at the head of its first successor, and nothing that takes a register may stand between the Patch and them.
static bool extractsStandAtHeadOfSinkTarget(const Order& order, const Block* entry)
{
    // On the way to a landing pad the results of the Patch do not exist. Any other entry of a rare region has one predecessor.
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

    // Every way into the region leads through its entry. So a definition outside that reaches a read inside stands on every way to the entry, and so do its children.
    const BlockPlace& placeOfEntry = m_order.of(entry);
    Position head = placeOfEntry.head;
    Position early = m_order.earlyPositionOf(operation);
    if (head <= early)
        return false;

    // In a loop that begins behind the definition it would be computed on every turn that takes the rare way, and its children would live through the loop.
    if (placeOfEntry.loop != noLoop && m_order.loops()[placeOfEntry.loop].begin > early)
        return false;

    // A child may be computed from an address in the frame, and no such value lives across a suspension.
    if (m_nextSuspension < head)
        return false;

    set(operation, ValueFlag::IsSunk);
    if (m_liveness.m_sunk.isEmpty())
        m_liveness.m_sunk.grow(m_proc.arena(), m_order.mostBlocks());
    m_liveness.m_sunk[entry->index()].append(m_proc.arena(), operation);

    // A sum takes in what it would where it stood, which gives the same wherever it is computed. Nothing else is fused into what sinks. A child that nothing else reads sinks in its turn.
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

        // What sank into a block runs in a row at one pair of positions. A leaf outlives that pair, so that no result or clobber of one of them takes the register that a later one reads it from.
        ValueFacts& read = facts(leaf);
        read.lastUse = std::max(read.lastUse, head + 2);
    });

    // It is a call of the region from here on. Its own result is defined behind this and counts as living across it, which claims more and never less.
    if (patch)
        noteCall(patch, head + 1, entry);
    return true;
}

void BackwardScan::noteRegionOfUse(Operation* value, const Block* user)
{
    uint32_t& regionOfUses = m_summariesOfReads[value->index()].regionOfUses;
    // No region has that number, so whatever the region of this user is, the answer stays.
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
