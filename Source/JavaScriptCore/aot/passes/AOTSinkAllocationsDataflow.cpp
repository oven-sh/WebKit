/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool willSolveAgainWithCell(AllocationSinking& sinking, BlockID head, VirtualObjectID object)
{
    if (sinking.at(object).isAbandoned)
        return true;
    const SinkingDecisions& decisions = sinking.decisions();
    return std::ranges::contains(decisions.objectsOfNewDecisions(), object) && decisions.isBuiltAtHead(head, object);
}

template<typename Entry, size_t inlineCapacity, typename Key>
static bool retainEqualEntries(Vector<Entry, inlineCapacity>& into, const Vector<Entry, inlineCapacity>& from, Key Entry::* key, NOESCAPE const Invocable<void(const Entry&)> auto& didDrop)
{
    size_t numberOfRetainedEntries = 0;
    size_t fromIndex = 0;
    for (size_t index = 0; index < into.size(); ++index) {
        Entry entry = into[index];
        while (fromIndex < from.size() && from[fromIndex].*key < entry.*key)
            didDrop(from[fromIndex++]);
        if (fromIndex < from.size() && from[fromIndex].*key == entry.*key) {
            const Entry& other = from[fromIndex++];
            if (other == entry) {
                into[numberOfRetainedEntries++] = entry;
                continue;
            }
            didDrop(other);
        }
        didDrop(entry);
    }
    while (fromIndex < from.size())
        didDrop(from[fromIndex++]);

    bool changed = numberOfRetainedEntries != into.size();
    into.shrink(numberOfRetainedEntries);
    return changed;
}

void AllocationSinking::solve()
{
    for (uint64_t round = 1;; ++round) {
        m_mustSolveAgain = false;
        m_decisions.forgetNewDecisions();
        for (VirtualFamily& family : m_families)
            family.forgetTagBits();

        SinkingProblem problem(*this);
        m_heads = m_analyses.solveForward(problem);

        bool changed = m_decisions.takeDidChange();
        if (!changed && !m_mustSolveAgain)
            return;
        if (round >= limit(LimitID::MaxSinkingRounds))
            giveUpOnNewDecisions();
    }
}

void AllocationSinking::giveUpOnNewDecisions()
{
    Vector<VirtualObjectID, 4> objects(m_decisions.objectsOfNewDecisions());
    for (VirtualObjectID object : objects)
        abandon(object, LimitID::MaxSinkingRounds);
}

// The object is built on the ways into this one block and stays virtual on every path that does not come here.
void AllocationSinking::buildAtHeadForLackOfTagBits(BlockID block, VirtualObjectID object)
{
    if (m_decisions.isBuiltAtHead(block, object)) {
        abandon(object, LimitID::MaxVirtualObjectVariants);
        return;
    }
    m_decisions.buildAtHead(block, object);
    if (m_policy.recordsLosses)
        context().build.gaveUp(LimitID::MaxVirtualObjectVariants, m_analyses.subject(at(object).definition));
}

void AllocationSinking::tagVariants(BlockID block, VirtualObjectID object, ObjectState& state)
{
    if (!state.needsTag())
        return;

    VirtualFamily& family = familyOf(object);
    uint64_t maxTagBits = limit(LimitID::MaxVirtualObjectVariants);
    for (ObjectVariant& variant : state.variants) {
        if (variant.tagBit)
            continue;

        // A head inside a loop is first reached with the one shape of the object and then, over the back edge, with that shape
        // under a tag. One bit stands for both, or every such head would take a bit of its own.
        size_t taggedVariant = state.variants.findIf([&](const ObjectVariant& other) {
            return other.tagBit && other.shape == variant.shape;
        });
        if (taggedVariant != notFound && !family.findTagBit(block, variant.shape))
            family.shareTagBit(block, variant.shape, *state.variants[taggedVariant].tagBit);

        variant.tagBit = family.tagBitFor(block, variant.shape, maxTagBits);
        if (!variant.tagBit) {
            buildAtHeadForLackOfTagBits(block, object);
            state.becomeCell();
            return;
        }
    }
    std::ranges::sort(state.variants);
    removeRepeatedElements(state.variants);
}

SinkingProblem::State SinkingProblem::stateAtEntry()
{
    State state;
    state.block = entryBlock;
    return state;
}

void SinkingProblem::execute(State& state, const DataflowStep& step)
{
    if (!state.isReachable)
        return;
    state.isAtHead = false;
    ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).execute(step.id);
}

void SinkingProblem::executeTerminator(State& state, const DataflowTerminatorStep& step)
{
    if (!state.isReachable)
        return;
    state.isAtHead = false;
    ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).executeTerminator(step.terminator);
}

void SinkingProblem::enterEdge(State& state, Edge edge)
{
    const Function& function = m_sinking.function();
    BlockID target = targetOf(function, edge).block;
    if (!state.isReachable) {
        state = State::unreachableAt(target);
        return;
    }

    OutgoingEdge outgoing = outgoingEdgeOf(function, edge);
    const Terminator* terminator = edge.isHandlerEdge() ? nullptr : &function.at(edge.from).terminator;
    ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).enterEdge(outgoing, terminator, nullptr);
}

bool SinkingProblem::merge(State& into, const State& from)
{
    if (!from.isReachable)
        return false;

    bool isHeadWithOneIncomingEdge = from.isAtHead && !m_sinking.inputs().mergeBlocks.get(into.block.index());
    if (!into.isReachable || isHeadWithOneIncomingEdge) {
        if (into == from)
            return false;
        into = from;
        return true;
    }

    into.isAtHead = into.isAtHead && from.isAtHead;
    bool changed = mergeObjects(into, from);
    changed |= mergeAliases(into, from);
    changed |= mergeTests(into, from);
    return changed;
}

bool SinkingProblem::mergeObjects(State& into, const State& from)
{
    bool changed = false;
    size_t index = 0;
    for (const ObjectEntry& source : from.objects) {
        while (index < into.objects.size() && into.objects[index].object < source.object)
            ++index;
        if (index < into.objects.size() && into.objects[index].object == source.object) {
            changed |= mergeObject(into, source.object, into.objects[index].state, source.state);
            continue;
        }
        into.objects.insert(index, source);
        changed = true;
    }
    return changed;
}

bool SinkingProblem::mergeObject(State& into, VirtualObjectID object, ObjectState& target, const ObjectState& source)
{
    ObjectState before = target;
    target.keepAssumedTypesOf(source);
    target.lacksFieldsOnSomeWay |= source.lacksFieldsOnSomeWay;
    if (into.isAtHead && willSolveAgainWithCell(m_sinking, into.block, object)) {
        target.becomeCell();
        return target != before;
    }

    if (source.hasCell()) {
        if (target.hasCell())
            target.fieldsValidInCell.filter(source.fieldsValidInCell);
        else
            target.fieldsValidInCell = source.fieldsValidInCell;
    }
    mergeHeldObjects(object, target, source);
    mergeVariants(into, object, target, source);

    if (into.isAtHead) {
        if (target.hasCell() && !target.isCell() && !target.cellJoinedAt)
            target.cellJoinedAt = source.cellJoinedAt ? source.cellJoinedAt : into.block;
        if (mustBeBuiltBefore(into.block, object, target) || target.hasConflictingTags())
            m_sinking.decisions().buildAtHead(into.block, object);
        if (!target.isCell() && willSolveAgainWithCell(m_sinking, into.block, object))
            target.becomeCell();
    }
    return target != before;
}

bool SinkingProblem::mergeVariants(State& into, VirtualObjectID object, ObjectState& target, const ObjectState& source)
{
    Vector<ObjectVariant, 1> before = target.variants;
    for (const ObjectVariant& variant : source.variants)
        target.addVariant(variant);
    if (into.isAtHead)
        m_sinking.tagVariants(into.block, object, target);
    return target.variants != before;
}

static void holdCellsInFamilyOf(AllocationSinking& sinking, VirtualObjectID object, uint32_t field)
{
    VirtualFamilyID family = sinking.at(object).family;
    for (uint32_t index = 0; index < sinking.numberOfCandidates(); ++index) {
        VirtualObjectID member(index);
        if (sinking.at(member).family == family)
            sinking.decisions().holdCells(member, field);
    }
}

bool SinkingProblem::mergeHeldObjects(VirtualObjectID object, ObjectState& target, const ObjectState& source)
{
    if (target.heldObjects.isEmpty() && source.heldObjects.isEmpty())
        return false;

    const VirtualFamily& family = m_sinking.familyOf(object);
    auto storesPlainly = [&](const ObjectState& side, uint32_t field) {
        if (side.objectHeldIn(field))
            return false;
        return side.hasCell() || side.variants.containsIf([&](const ObjectVariant& variant) {
            return hasStored(family.at(variant.shape), field);
        });
    };

    Vector<uint32_t, 4> fields;
    for (const HeldObject& entry : target.heldObjects)
        fields.append(entry.field);
    for (const HeldObject& entry : source.heldObjects) {
        if (!target.objectHeldIn(entry.field))
            fields.append(entry.field);
    }
    std::ranges::sort(fields);

    bool changed = false;
    for (uint32_t field : fields) {
        VirtualObjectID objectInTarget = target.objectHeldIn(field);
        VirtualObjectID objectInSource = source.objectHeldIn(field);
        if (objectInTarget == objectInSource)
            continue;
        if (!objectInSource && !storesPlainly(source, field))
            continue;
        if (!objectInTarget && !storesPlainly(target, field)) {
            target.hold(field, objectInSource);
            changed = true;
            continue;
        }
        holdCellsInFamilyOf(m_sinking, object, field);
        if (objectInTarget) {
            target.release(field);
            changed = true;
        }
    }
    return changed;
}

bool SinkingProblem::mergeAliases(State& into, const State& from)
{
    const Function& function = m_sinking.function();
    bool isHeadMerge = into.isAtHead;
    return retainEqualEntries(into.aliases, from.aliases, &AliasEntry::value, [&](const AliasEntry& entry) {
        if (!isHeadMerge)
            return;
        // An alias that reaches a head is used behind it, where the name stands for no object from here on: it has to be a value.
        // An early visit may leave a cast in place that a later one, with more variants, interprets, and the head keeps what the early visit said.
        Opcode opcode = function.at(entry.value).opcode;
        if (opcode == Opcode::Parameter || opcode == Opcode::AllGuards || interpretedCasts.contains(opcode))
            m_sinking.decisions().holdCells(entry.value, entry.object);
        else
            m_sinking.abandon(entry.object);
    });
}

bool SinkingProblem::mergeTests(State& into, const State& from)
{
    return retainEqualEntries(into.tests, from.tests, &TestEntry::test, [](const TestEntry&) { });
}

bool SinkingProblem::mustBeBuiltBefore(BlockID block, VirtualObjectID object, const ObjectState& state) const
{
    if (!state.hasCell() || state.isCell())
        return false;
    if (!m_sinking.inputs().blocksInCycles.get(block.index()))
        return false;

    const BlockCycles& cycles = m_sinking.analyses().blockCycles();
    BlockID origin = m_sinking.at(object).block;
    return !cycles.areInSameCycle(block, origin) || cycles.depthOf(block) > cycles.depthOf(origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
