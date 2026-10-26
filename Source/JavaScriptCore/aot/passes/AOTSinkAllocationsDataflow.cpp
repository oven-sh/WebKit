/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

bool ObjectState::hasCell() const
{
    return variants.containsIf([](const ObjectVariant& variant) {
        return variant.shape == cellShape;
    });
}

bool ObjectState::hasOneShape() const
{
    return std::ranges::all_of(variants, [&](const ObjectVariant& variant) {
        return variant.shape == variants[0].shape;
    });
}

bool ObjectState::hasConflictingTags() const
{
    for (size_t index = 1; index < variants.size(); ++index) {
        const ObjectVariant& variant = variants[index];
        if (variant.tagBit && variant.tagBit == variants[index - 1].tagBit)
            return true;
    }
    return false;
}

uint32_t ObjectState::tagBits() const
{
    uint32_t bits = 0;
    for (const ObjectVariant& variant : variants)
        bits |= variant.tagMask();
    return bits;
}

VirtualObjectID ObjectState::objectHeldIn(uint32_t field) const
{
    const HeldObject* held = findEntry(heldObjects, &HeldObject::field, field);
    return held ? held->object : VirtualObjectID();
}

void ObjectState::assume(TypeID type)
{
    size_t position = std::ranges::lower_bound(assumedTypes, type) - assumedTypes.begin();
    if (position == assumedTypes.size() || assumedTypes[position] != type)
        assumedTypes.insert(position, type);
}

void ObjectState::keepAssumedTypesOf(const ObjectState& other)
{
    assumedTypes.removeAllMatching([&](TypeID type) {
        return !other.assumedTypes.contains(type);
    });
}

void ObjectState::addVariant(ObjectVariant variant)
{
    size_t position = std::ranges::lower_bound(variants, variant) - variants.begin();
    if (position < variants.size() && variants[position] == variant)
        return;
    variants.insert(position, variant);
}

void ObjectState::becomeCell()
{
    variants = { ObjectVariant { std::nullopt, cellShape } };
    heldObjects.clear();
    fieldsValidInCell.clearAll();
    cellJoinedAt = { };
    lacksFieldsOnSomeWay = false;
    isMissingOnSomeWay = false;
    isCellMissingOnSomeWay = false;
}

void ObjectState::normalize()
{
    if (!hasOneShape())
        return;

    VirtualShapeID shape = variants[0].shape;
    if (variants.size() > 1 || variants[0].tagBit)
        variants = { ObjectVariant { std::nullopt, shape } };
    if (shape == cellShape)
        cellJoinedAt = { };
}

const ObjectState* SinkingState::find(VirtualObjectID object) const
{
    const ObjectEntry* entry = findEntry(objects, &ObjectEntry::object, object);
    return entry ? &entry->state : nullptr;
}

std::optional<size_t> SinkingState::indexOf(VirtualObjectID object) const
{
    const ObjectEntry* entry = findEntry(objects, &ObjectEntry::object, object);
    if (!entry)
        return std::nullopt;
    return entry - objects.span().data();
}

ObjectState& SinkingState::add(VirtualObjectID object, ObjectState&& state)
{
    size_t position = positionOfEntry(objects, &ObjectEntry::object, object);
    RELEASE_ASSERT(position == objects.size() || objects[position].object != object);
    objects.insert(position, ObjectEntry { object, WTF::move(state) });
    return objects[position].state;
}

void SinkingState::set(std::span<ObjectEntry> movedEntries)
{
    Vector<ObjectEntry*, 8> addedEntries;
    for (ObjectEntry& entry : movedEntries) {
        if (ObjectState* state = find(entry.object))
            *state = WTF::move(entry.state);
        else
            addedEntries.append(&entry);
    }
    if (addedEntries.isEmpty())
        return;

    AOT::sort(addedEntries, { }, &ObjectEntry::object);
    size_t source = objects.size();
    size_t destination = source + addedEntries.size();
    objects.grow(destination);
    for (size_t added = addedEntries.size(); added;) {
        if (source && objects[source - 1].object > addedEntries[added - 1]->object) {
            --source;
            --destination;
            objects[destination] = WTF::move(objects[source]);
            continue;
        }
        --added;
        --destination;
        objects[destination] = ObjectEntry { addedEntries[added]->object, WTF::move(addedEntries[added]->state) };
    }
}

size_t SinkingState::weight() const
{
    size_t result = aliases.size() + tests.size();
    for (const ObjectEntry& entry : objects)
        result += entry.state.variants.size() + entry.state.heldObjects.size() + entry.state.assumedTypes.size();
    return result;
}

bool SinkingState::canReach(VirtualObjectID from, VirtualObjectID to) const
{
    // Objects hold only objects of the state, and an allocation asks before its object is one.
    if (from != to && !find(to))
        return false;

    BitVector visited;
    Vector<VirtualObjectID, 4> worklist { from };
    while (!worklist.isEmpty()) {
        VirtualObjectID object = worklist.takeLast();
        if (object == to)
            return true;
        if (!visited.add(object.index()))
            continue;
        const ObjectState* state = find(object);
        if (!state)
            continue;
        for (const HeldObject& held : state->heldObjects)
            worklist.append(held.object);
    }
    return false;
}

bool SinkingState::isDestructured(VirtualObjectID object) const
{
    return aliases.containsIf([&](const AliasEntry& alias) {
        return alias.kind == AliasKind::Results && alias.object == object;
    });
}

void SinkingDecisions::buildAtHead(BlockID block, VirtualObjectID object)
{
    if (m_objectsBuiltAtHeads.add(pairOf(block.index(), object.index())).isNewEntry) {
        m_blocksWithObjectsBuiltAtHeads.set(block.index());
        didDecide(object);
    }
}

void SinkingDecisions::holdCells(ValueID parameter, VirtualObjectID boundObject)
{
    if (m_parametersHoldingCells.add(parameter.index()))
        didDecide(boundObject);
}

void SinkingDecisions::holdCells(VirtualObjectID holder, uint32_t field)
{
    if (m_fieldsHoldingCells.add(pairOf(holder.index(), field)).isNewEntry)
        didDecide(holder);
}

void SinkingDecisions::didDecide(VirtualObjectID object)
{
    m_didChange = true;
    m_objectsOfNewDecisions.appendIfNotContains(object);
}

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

static bool mustBeBuiltBefore(AllocationSinking& sinking, BlockID block, VirtualObjectID object, const ObjectState& state)
{
    if (!state.hasCell() || state.isCell())
        return false;
    if (!sinking.inputs().blocksInCycles.get(block.index()))
        return false;

    const BlockCycles& cycles = sinking.analyses().blockCycles();
    BlockID origin = sinking.at(object).block;
    return !cycles.areInSameCycle(block, origin) || cycles.depthOf(block) > cycles.depthOf(origin);
}

static bool mergeAliases(AllocationSinking& sinking, SinkingState& into, const SinkingState& from)
{
    const Function& function = sinking.function();
    bool isHeadMerge = into.isAtHead;
    return retainEqualEntries(into.aliases, from.aliases, &AliasEntry::value, [&](const AliasEntry& entry) {
        if (!isHeadMerge)
            return;
        Opcode opcode = function.at(entry.value).opcode;
        if (opcode == Opcode::Parameter || opcode == Opcode::AllGuards || interpretedCasts.contains(opcode))
            sinking.decisions().holdCells(entry.value, entry.object);
        else
            sinking.abandon(entry.object);
    });
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

static void mergeHeldObjects(AllocationSinking& sinking, VirtualObjectID object, ObjectState& target, const ObjectState& source)
{
    if (target.heldObjects.isEmpty() && source.heldObjects.isEmpty())
        return;

    const VirtualFamily& family = sinking.familyOf(object);
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
    AOT::sort(fields);

    for (uint32_t field : fields) {
        VirtualObjectID objectInTarget = target.objectHeldIn(field);
        VirtualObjectID objectInSource = source.objectHeldIn(field);
        if (objectInTarget == objectInSource)
            continue;
        if (!objectInSource && !storesPlainly(source, field))
            continue;
        if (!objectInTarget && !storesPlainly(target, field)) {
            target.hold(field, objectInSource);
            continue;
        }
        holdCellsInFamilyOf(sinking, object, field);
        if (objectInTarget)
            target.release(field);
    }
}

static bool mergeObject(AllocationSinking& sinking, SinkingState& into, VirtualObjectID object, ObjectState& target, const ObjectState& source)
{
    bool becomesCell = into.isAtHead && willSolveAgainWithCell(sinking, into.block, object);
    if (!becomesCell && target.variants.size() == 1 && target == source)
        return false;

    ObjectState before = target;
    target.keepAssumedTypesOf(source);
    target.lacksFieldsOnSomeWay |= source.lacksFieldsOnSomeWay;
    target.isMissingOnSomeWay |= source.isMissingOnSomeWay;
    target.isCellMissingOnSomeWay |= source.isCellMissingOnSomeWay;
    if (becomesCell) {
        target.becomeCell();
        return target != before;
    }

    if (source.hasCell()) {
        if (target.hasCell())
            target.fieldsValidInCell.filter(source.fieldsValidInCell);
        else
            target.fieldsValidInCell = source.fieldsValidInCell;
    }
    mergeHeldObjects(sinking, object, target, source);
    for (const ObjectVariant& variant : source.variants)
        target.addVariant(variant);

    if (into.isAtHead) {
        sinking.tagVariants(into.block, object, target);
        if (target.hasCell() && !target.isCell() && !target.cellJoinedAt)
            target.cellJoinedAt = source.cellJoinedAt ? source.cellJoinedAt : into.block;
        if (mustBeBuiltBefore(sinking, into.block, object, target) || target.hasConflictingTags())
            sinking.decisions().buildAtHead(into.block, object);
        if (!target.isCell() && willSolveAgainWithCell(sinking, into.block, object))
            target.becomeCell();
    }
    return target != before;
}

static bool mergeObjects(AllocationSinking& sinking, SinkingState& into, const SinkingState& from)
{
    bool changed = false;
    size_t index = 0;
    auto markAsMissingOnSomeWay = [&](ObjectState& state) {
        changed |= !std::exchange(state.isCell() ? state.isCellMissingOnSomeWay : state.isMissingOnSomeWay, true);
    };
    for (const ObjectEntry& source : from.objects) {
        for (; index < into.objects.size() && into.objects[index].object < source.object; ++index)
            markAsMissingOnSomeWay(into.objects[index].state);
        if (index < into.objects.size() && into.objects[index].object == source.object) {
            changed |= mergeObject(sinking, into, source.object, into.objects[index].state, source.state);
            ++index;
            continue;
        }
        into.objects.insert(index, source);
        markAsMissingOnSomeWay(into.objects[index].state);
        ++index;
        changed = true;
    }
    for (; index < into.objects.size(); ++index)
        markAsMissingOnSomeWay(into.objects[index].state);
    return changed;
}

template<typename From>
static bool mergeStates(AllocationSinking& sinking, SinkingState& into, From&& from)
{
    if (!from.isReachable)
        return false;
    bool isHeadWithOneIncomingEdge = from.isAtHead && !sinking.inputs().mergeBlocks.get(into.block.index());
    if (!into.isReachable || isHeadWithOneIncomingEdge) {
        if (into == from)
            return false;
        into = std::forward<From>(from);
        return true;
    }

    into.isAtHead = into.isAtHead && from.isAtHead;
    bool changed = mergeObjects(sinking, into, from);
    changed |= mergeAliases(sinking, into, from);
    changed |= retainEqualEntries(into.tests, from.tests, &TestEntry::test, [](const TestEntry&) { });
    return changed;
}

static void enterEdge(AllocationSinking& sinking, SinkingState& state, Edge edge)
{
    const Function& function = sinking.function();
    BlockID target = targetOf(function, edge).block;
    if (!state.isReachable) {
        state = SinkingState::unreachableAt(target);
        return;
    }

    bool isPlain = sinking.isPlain(edge, target) && !sinking.checksShortcuts();
    if (isPlain && ObjectInterpreter(sinking, SinkingMode::Analyze, state).tryEnterPlainEdge(target))
        return;
    OutgoingEdge outgoing = isPlain ? OutgoingEdge { edge, target, { } } : outgoingEdgeOf(function, edge);
    const Terminator* terminator = edge.isHandlerEdge() ? nullptr : &function.at(edge.from).terminator;
    if (sinking.checksShortcuts() && sinking.isPlain(edge, target)) [[unlikely]] {
        SinkingState shortcut = state;
        bool didEnter = ObjectInterpreter(sinking, SinkingMode::Analyze, shortcut).tryEnterPlainEdge(target);
        ObjectInterpreter(sinking, SinkingMode::Analyze, state).enterEdge(outgoing, terminator, nullptr);
        RELEASE_ASSERT(!didEnter || shortcut == state);
        return;
    }
    ObjectInterpreter(sinking, SinkingMode::Analyze, state).enterEdge(outgoing, terminator, nullptr);
}

namespace {

class SinkingSolver {
    WTF_MAKE_NONCOPYABLE(SinkingSolver);
public:
    explicit SinkingSolver(AllocationSinking& sinking)
        : m_sinking(sinking)
        , m_function(sinking.function())
        , m_cfg(sinking.analyses().cfg())
        , m_inputs(sinking.inputs())
        , m_heads(m_function.blocks.size())
        , m_blocksEnteredWithNothing(m_function.blocks.size())
        , m_isPending(m_cfg.reversePostOrder().size())
    {
    }

    BitVector takeBlocksEnteredWithNothing() { return WTF::move(m_blocksEnteredWithNothing); }

    AllocationSinking::Heads run()
    {
        BlockID root = m_cfg.root();
        m_blocksEnteredWithNothing.quickSet(root.index());
        m_isPending.quickSet(m_cfg.reversePostOrderIndex(root));
        std::span<const BlockID> order = m_cfg.reversePostOrder();
        for (bool shouldRepeat = true; shouldRepeat;) {
            shouldRepeat = false;
            for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
                if (m_isPending.quickClear(blockOrder))
                    shouldRepeat |= visit(order[blockOrder], blockOrder);
            }
            shouldRepeat = shouldRepeat && !m_sinking.hasSpentItsWork();
        }
        return WTF::move(m_heads);
    }

private:
    struct HandlerEntry {
        SinkingState state;
        bool includesCurrentState { true };
    };

    void meetCurrentState(HandlerEntry& entry, const SinkingState& state)
    {
        if (entry.includesCurrentState)
            return;
        mergeStates(m_sinking, entry.state, state);
        entry.includesCurrentState = true;
    }

    bool didChangeHead(BlockID target, uint32_t fromOrder)
    {
        uint32_t targetOrder = m_cfg.reversePostOrderIndex(target);
        m_isPending.quickSet(targetOrder);
        return targetOrder <= fromOrder;
    }

    void makeHeadOfBlockEnteredWithNothing(BlockID block)
    {
        if (m_blocksEnteredWithNothing.quickClear(block.index()))
            m_heads[block.index()] = SinkingState::nothingAt(block);
    }

    bool meetAtHead(BlockID target, SinkingState&& state, uint32_t fromOrder)
    {
        makeHeadOfBlockEnteredWithNothing(target);
        std::optional<SinkingState>& head = m_heads[target.index()];
        if (head) {
            if (!mergeStates(m_sinking, *head, WTF::move(state)))
                return false;
        } else
            head = WTF::move(state);
        return didChangeHead(target, fromOrder);
    }

    bool propagate(Edge edge, SinkingState&& state, uint32_t fromOrder)
    {
        if (state.holdsNothing())
            return propagateNothing(edge, state.isReachable, fromOrder);
        m_sinking.didWork(1 + state.weight() + m_function.at(targetOf(m_function, edge).block).parameters.size());
        enterEdge(m_sinking, state, edge);
        return meetAtHead(targetOf(m_function, edge).block, WTF::move(state), fromOrder);
    }

    bool propagateNothing(Edge edge, bool isReachable, uint32_t fromOrder)
    {
        BlockID target = targetOf(m_function, edge).block;
        bool isEnteredWithNothing = m_blocksEnteredWithNothing.quickGet(target.index());
        const std::optional<SinkingState>& head = m_heads[target.index()];
        if (!isReachable) {
            if (head || isEnteredWithNothing)
                return false;
            return meetAtHead(target, SinkingState::unreachableAt(target), fromOrder);
        }

        bool isEntered = m_inputs.blocksWithJoins.get(target.index()) || m_sinking.isIgnored(edge);
        if (!isEntered) {
            if (isEnteredWithNothing || (head && head->isReachable && head->holdsNothing()))
                return false;
            if (!head) {
                m_blocksEnteredWithNothing.quickSet(target.index());
                return didChangeHead(target, fromOrder);
            }
        }

        SinkingState state = SinkingState::nothingAt(target);
        if (isEntered) {
            state.block = edge.from;
            state.isAtHead = false;
            enterEdge(m_sinking, state, edge);
        }
        return meetAtHead(target, WTF::move(state), fromOrder);
    }

    bool propagateNothingFrom(BlockID id, bool isReachable, uint32_t blockOrder)
    {
        bool shouldRepeat = false;
        forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target&) {
            shouldRepeat |= propagateNothing(edge, isReachable, blockOrder);
        });
        return shouldRepeat;
    }

    bool visit(BlockID id, uint32_t blockOrder)
    {
        if (m_blocksEnteredWithNothing.quickGet(id.index())) {
            if (!m_inputs.blocksWithCandidates.get(id.index()))
                return propagateNothingFrom(id, true, blockOrder);
            makeHeadOfBlockEnteredWithNothing(id);
        }
        const SinkingState& head = *m_heads[id.index()];
        if (head.holdsNothing() && (!head.isReachable || !m_inputs.blocksWithCandidates.get(id.index())))
            return propagateNothingFrom(id, head.isReachable, blockOrder);

        const Block& block = m_function.at(id);
        m_sinking.prepare(id);
        SinkingState state = head;
        m_sinking.didWork(1 + state.objects.size());
        for (const ObjectEntry& entry : state.objects)
            m_sinking.didCarry(entry.object);
        std::optional<HandlerEntry> handlerEntry;
        if (block.handler)
            handlerEntry = HandlerEntry { state };

        state.isAtHead = false;
        ObjectInterpreter interpreter(m_sinking, SinkingMode::Analyze, state);
        uint32_t numberOfValuesPassed = 0;
        for (uint32_t entry : m_sinking.relevantValuesOf(id)) {
            uint32_t index = entry & ~AllocationSinking::onlyWritesOrThrows;
            ValueID value = block.values[index];
            if (handlerEntry && index > numberOfValuesPassed)
                handlerEntry->includesCurrentState = false;
            numberOfValuesPassed = index + 1;
            bool mayThrowToHandler = handlerEntry && m_inputs.throwingValues.get(value.index());
            if (mayThrowToHandler)
                meetCurrentState(*handlerEntry, state);
            interpreter.executeRelevantValue(block.values, entry);
            if (!handlerEntry)
                continue;
            handlerEntry->includesCurrentState = false;
            if (mayThrowToHandler)
                meetCurrentState(*handlerEntry, state);
        }
        if (handlerEntry && block.values.size() > numberOfValuesPassed)
            handlerEntry->includesCurrentState = false;
        if (m_sinking.checksShortcuts()) [[unlikely]]
            interpreter.checkThatOtherValuesChangeNothing(id);

        bool mayThrowToHandler = handlerEntry && m_inputs.throwingTerminators.get(id.index());
        if (mayThrowToHandler)
            meetCurrentState(*handlerEntry, state);
        interpreter.executeTerminatorOf(id);
        if (handlerEntry) {
            handlerEntry->includesCurrentState = false;
            if (mayThrowToHandler)
                meetCurrentState(*handlerEntry, state);
        }

        bool shouldRepeat = false;
        uint32_t numberOfTargets = block.terminator.targets.size();
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            bool isLastTarget = targetIndex + 1 == numberOfTargets;
            shouldRepeat |= propagate(Edge { id, targetIndex }, isLastTarget ? WTF::move(state) : SinkingState(state), blockOrder);
        }
        if (handlerEntry)
            shouldRepeat |= propagate(Edge::toHandlerOf(id), WTF::move(handlerEntry->state), blockOrder);
        return shouldRepeat;
    }

    AllocationSinking& m_sinking;
    const Function& m_function;
    const CFG& m_cfg;
    const SinkingInputs& m_inputs;
    AllocationSinking::Heads m_heads;
    BitVector m_blocksEnteredWithNothing;
    BitVector m_isPending;
};

} // anonymous namespace

void AllocationSinking::solve()
{
    for (uint64_t round = 1;; ++round) {
        m_mustSolveAgain = false;
        findWhetherObjectsAreBuiltBeforeBranches();
        m_decisions.forgetNewDecisions();
        for (VirtualFamily& family : m_families)
            family.forgetTagBits();

        if (m_blocksCarrying.size() != numberOfCandidates())
            m_blocksCarrying.fill(0, numberOfCandidates());
        ++m_numberOfSweeps;
        SinkingSolver solver(*this);
        m_heads = solver.run();
        m_blocksEnteredWithNothing = solver.takeBlocksEnteredWithNothing();
        if (hasSpentItsWork()) {
            if (!startOverWithFewerCandidates()) {
                giveUpOnEveryCandidate();
                return;
            }
            round = 0;
            continue;
        }

        bool changed = m_decisions.takeDidChange();
        if (!changed && !m_mustSolveAgain)
            return;
        if (round >= limit(LimitID::MaxSinkingRounds)) {
            for (VirtualObjectID object : Vector<VirtualObjectID, 4>(m_decisions.objectsOfNewDecisions()))
                abandon(object, LimitID::MaxSinkingRounds);
        }
        setAsideJoinsThatCarryOnlyCells();
    }
}

void AllocationSinking::giveUpOnEveryCandidate()
{
    Vector<uint32_t> candidates;
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        if (!m_candidates[index].isAbandoned)
            candidates.append(index);
    }
    giveUpForLackOfWork(candidates.span());
}

bool AllocationSinking::startOverWithFewerCandidates()
{
    if (!m_setsAsideJoins)
        return false;
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    Vector<uint32_t> mostFrequentFirst;
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        if (!m_candidates[index].isJoin && !m_candidates[index].isAbandoned)
            mostFrequentFirst.append(index);
    }
    if (mostFrequentFirst.size() < 2)
        return false;
    AOT::stableSort(mostFrequentFirst, std::ranges::greater { }, [&](uint32_t index) {
        return frequencies.frequencyOf(m_candidates[index].block);
    });

    uint64_t blocksInAll = 0;
    for (uint32_t index : mostFrequentFirst)
        blocksInAll += m_blocksCarrying[index];
    uint64_t workOfOneSweep = std::max<uint64_t>(m_work / m_numberOfSweeps, 1);
    uint64_t blocksThatFit = blocksInAll * std::min(m_maxWork / expectedNumberOfSweeps, workOfOneSweep) / workOfOneSweep;
    size_t numberKept = 0;
    for (uint64_t blocks = 0; numberKept < mostFrequentFirst.size() / 2; ++numberKept) {
        blocks += m_blocksCarrying[mostFrequentFirst[numberKept]];
        if (blocks > blocksThatFit)
            break;
    }
    numberKept = std::max<size_t>(numberKept, 1);

    giveUpForLackOfWork(mostFrequentFirst.span().subspan(numberKept));
    m_hasStartedOverWithFewerCandidates = true;
    m_work = 0;
    m_numberOfSweeps = 0;
    m_blocksCarrying.fill(0);
    return true;
}

void AllocationSinking::giveUpForLackOfWork(std::span<const uint32_t> candidates)
{
    for (uint32_t index : candidates) {
        m_candidates[index].isAbandoned = true;
        m_candidates[index].abandonedBy = LimitID::MaxSinkingWork;
    }
    if (candidates.empty() || m_hasRecordedLackOfWork || !m_policy.recordsLosses)
        return;
    m_hasRecordedLackOfWork = true;
    context().build.gaveUp(LimitID::MaxSinkingWork, m_analyses.subject());
}

void AllocationSinking::setAsideJoinsThatCarryOnlyCells()
{
    if (!m_setsAsideJoins)
        return;
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        Candidate& candidate = m_candidates[index];
        if (!candidate.isJoin || candidate.isAbandoned)
            continue;
        const SinkingState* head = headOf(candidate.block);
        const ObjectState* state = head ? head->find(VirtualObjectID(index)) : nullptr;
        if (state && !state->isCell())
            continue;
        setAside(index);
    }
}

void AllocationSinking::setAside(uint32_t join)
{
    m_candidates[join].isAbandoned = true;
    m_decisions.holdCellsAsBefore(m_candidates[join].definition);
    m_joinsSetAside.append(join);
}

bool buildsOperand(const Program& program, const Value& user, uint32_t operandIndex)
{
    return interpretationOf(program, user) == Interpretation::None && familyOf(user.opcode) != OperationFamily::RuntimeValidation && !isHeldOperand(user, operandIndex);
}

bool AllocationSinking::isBuiltInItsBlock(const Candidate& allocation) const
{
    return std::ranges::any_of(m_analyses.useLists().usesOf(allocation.definition), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && use.block == allocation.block && buildsOperand(m_program, m_function.at(use.user), use.operandIndex);
    });
}

void AllocationSinking::setAsideJoinsOfAllocationsBuiltInTheirBlocks()
{
    BitVector mayLeaveItsBlockUnbuilt(numberOfCandidates());
    bool hasJoin = false;
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        const Candidate& candidate = m_candidates[index];
        hasJoin |= candidate.isJoin && !candidate.isAbandoned;
        if (!candidate.isJoin && !candidate.isAbandoned && !isBuiltInItsBlock(candidate))
            mayLeaveItsBlockUnbuilt.quickSet(index);
    }
    if (!hasJoin)
        return;

    const EscapeAnalysis& escape = m_analyses.escape();
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        const Candidate& candidate = m_candidates[index];
        if (!candidate.isJoin || candidate.isAbandoned)
            continue;
        PointsToSet pointsTo = escape.pointsTo(candidate.definition);
        bool mayCarryAnUnbuiltObject = false;
        for (ObjectID object : pointsTo.objects) {
            VirtualObjectID allocation = candidateDefinedBy(escape.at(object).definition);
            mayCarryAnUnbuiltObject |= allocation && mayLeaveItsBlockUnbuilt.quickGet(allocation.index());
        }
        if (!mayCarryAnUnbuiltObject)
            setAside(index);
    }
}

void AllocationSinking::takeBackJoinsSetAside()
{
    for (uint32_t index : m_joinsSetAside) {
        m_candidates[index].isAbandoned = false;
        m_decisions.holdObjectsAgain(m_candidates[index].definition);
    }
    m_joinsSetAside.shrink(0);
}

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
    AOT::sort(state.variants);
    removeRepeatedElements(state.variants);
}

static IncomingBinding bindingOf(const EdgeResult& result)
{
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return { IncomingBinding::Kind::NarrowedOperand, result.subject };
    case EdgeResultKind::Guard:
        return { IncomingBinding::Kind::Guard, result.subject };
    case EdgeResultKind::DeclaredResult:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Rewrite asks while the target has head parameters that this edge does not fill yet, so the bindings come from the edge alone.
OutgoingEdge outgoingEdgeOf(const Function& function, Edge edge)
{
    const Target& target = targetOf(function, edge);
    uint32_t numberOfResults = edge.isHandlerEdge() ? 0 : numberOfEdgeResults(function, edge.from, edge.targetIndex);

    OutgoingEdge outgoing { edge, target.block, { } };
    outgoing.bindings.reserveInitialCapacity(target.arguments.size() + numberOfResults);
    for (ValueID argument : target.arguments)
        outgoing.bindings.append(IncomingBinding { IncomingBinding::Kind::Argument, argument });
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex)
        outgoing.bindings.append(bindingOf(edgeResultAt(function, { edge.from, edge.targetIndex, resultIndex })));
    return outgoing;
}

static Vector<ValueID, 4> originalParametersOf(AllocationSinking& sinking, SinkingMode mode, BlockID block)
{
    Vector<ValueID, 4> parameters(sinking.function().at(block).parameters.span());
    if (mode != SinkingMode::Rewrite)
        return parameters;

    const RewriteTables& tables = sinking.tables();
    if (tables.blocksWithHeadParameters.get(block.index()))
        parameters.removeAt(tables.firstHeadParameter[block.index()], tables.headValues[block.index()].size());
    return parameters;
}

static const IncomingBinding& bindingOfJoin(AllocationSinking& sinking, SinkingMode mode, const OutgoingEdge& outgoing, VirtualObjectID join)
{
    size_t index = originalParametersOf(sinking, mode, outgoing.target).find(sinking.at(join).definition);
    RELEASE_ASSERT(index != notFound);
    return outgoing.bindings[index];
}

static bool isJoinReboundOnEdge(const ObjectInterpreter& interpreter, AllocationSinking& sinking, const OutgoingEdge& outgoing, VirtualObjectID object)
{
    const Candidate& candidate = sinking.at(object);
    if (!candidate.isJoin || candidate.block != outgoing.target)
        return false;
    return interpreter.objectOf(bindingOfJoin(sinking, interpreter.mode(), outgoing, object).value) != object;
}

static BitVector collectReferencedObjects(const SinkingState& state)
{
    BitVector referencedObjects;
    for (const AliasEntry& alias : state.aliases)
        referencedObjects.set(alias.object.index());
    for (const ObjectEntry& holder : state.objects) {
        for (const HeldObject& entry : holder.state.heldObjects)
            referencedObjects.set(entry.object.index());
    }
    return referencedObjects;
}

static bool isParameterOf(const UseLists& useLists, BlockID block, ValueID value)
{
    ValueLocation location = useLists.locationOf(value);
    return location.placement == ValuePlacement::Parameter && location.block == block;
}

static bool hasFieldHoldingCells(AllocationSinking& sinking, VirtualObjectID object)
{
    uint32_t numberOfFields = sinking.familyOf(object).numberOfFields();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        if (sinking.decisions().holdsCells(object, field))
            return true;
    }
    return false;
}

static void requireCellInCoveredBlock(AllocationSinking& sinking, SinkingState& state, BlockID covered, VirtualObjectID object)
{
    if (sinking.decisions().isBuiltAtHead(covered, object))
        sinking.abandon(object);
    else
        sinking.decisions().buildAtHead(covered, object);
    state.find(object)->becomeCell();
}

static void buildOnEdge(ObjectInterpreter& interpreter, AllocationSinking& sinking, const OutgoingEdge& outgoing, VirtualObjectID object)
{
    if (outgoing.edge.isHandlerEdge())
        requireCellInCoveredBlock(sinking, interpreter.state(), outgoing.edge.from, object);
    else
        interpreter.build(object, BuildCause { });
}

static VirtualObjectID objectGuardedByTest(const ObjectInterpreter& interpreter, const Terminator& terminator, ValueID subject)
{
    const SinkingState& state = interpreter.state();
    const TestEntry* test = terminator.kind == TerminatorKind::Branch ? state.findTest(terminator.conditionOperand()) : nullptr;
    if (!test || !test->object)
        return interpreter.objectOf(subject);
    return state.find(test->object) ? test->object : VirtualObjectID();
}

static AliasEntry aliasOfBinding(const ObjectInterpreter& interpreter, const Terminator* terminator, ValueID parameter, const IncomingBinding& binding)
{
    switch (binding.kind) {
    case IncomingBinding::Kind::Other:
        return { };
    case IncomingBinding::Kind::Guard:
        RELEASE_ASSERT(terminator);
        return { parameter, objectGuardedByTest(interpreter, *terminator, binding.value), AliasKind::Guard };
    case IncomingBinding::Kind::Argument:
    case IncomingBinding::Kind::NarrowedOperand:
        if (VirtualObjectID object = interpreter.objectOf(binding.value))
            return { parameter, object, AliasKind::Object };
        return { parameter, interpreter.guardedObjectOf(binding.value), AliasKind::Guard };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ObjectValues valuesEnteringJoin(ObjectInterpreter& interpreter, AllocationSinking& sinking, const EnteredEdge& entered, const ObjectEntry& join)
{
    ObjectValues values;
    for (const ConsumedSource& consumed : entered.consumedSources) {
        if (consumed.join != join.object)
            continue;
        if (const ObjectValues* carried = interpreter.values().find(consumed.source))
            values = *carried;
        if (join.state.isCell())
            values.cell = interpreter.cellOf(consumed.source);
        return values;
    }

    if (join.state.isCell())
        values.cell = interpreter.resolve(bindingOfJoin(sinking, interpreter.mode(), *entered.edge, join.object).value);
    return values;
}

static void removeDeadEntries(AllocationSinking& sinking, SinkingState& state, BlockID target)
{
    if (state.holdsNothing())
        return;

    const UseLists& useLists = sinking.analyses().useLists();
    auto isLive = [&](ValueID value) {
        return isParameterOf(useLists, target, value) || sinking.isLiveAtHead(value, target);
    };
    bool everyNameIsLive = std::ranges::all_of(state.objects, [&](const ObjectEntry& entry) {
        return isLive(sinking.at(entry.object).definition);
    }) && std::ranges::all_of(state.aliases, [&](const AliasEntry& alias) {
        return isLive(alias.value);
    }) && std::ranges::all_of(state.tests, [&](const TestEntry& test) {
        return isLive(test.test);
    });
    if (everyNameIsLive)
        return;

    state.aliases.removeAllMatching([&](const AliasEntry& alias) {
        return !isLive(alias.value);
    });
    state.tests.removeAllMatching([&](const TestEntry& test) {
        return !isLive(test.test);
    });

    size_t numberOfEntries = state.objects.size();
    Vector<bool, 16> isLiveEntry(FillWith { }, numberOfEntries, false);
    size_t numberOfLiveEntries = 0;
    Vector<uint32_t, 8> worklist;
    auto markEntryLive = [&](size_t index) {
        if (std::exchange(isLiveEntry[index], true))
            return;
        ++numberOfLiveEntries;
        if (!state.objects[index].state.heldObjects.isEmpty())
            worklist.append(static_cast<uint32_t>(index));
    };
    auto markLive = [&](VirtualObjectID object) {
        if (!object || numberOfLiveEntries == numberOfEntries)
            return;
        if (std::optional<size_t> index = state.indexOf(object))
            markEntryLive(*index);
    };
    for (size_t index = 0; index < numberOfEntries; ++index) {
        if (isLive(sinking.at(state.objects[index].object).definition))
            markEntryLive(index);
    }
    for (const AliasEntry& alias : state.aliases)
        markLive(alias.object);
    for (const TestEntry& test : state.tests)
        markLive(test.object);
    while (!worklist.isEmpty()) {
        for (const HeldObject& entry : state.objects[worklist.takeLast()].state.heldObjects)
            markLive(entry.object);
    }
    if (numberOfLiveEntries == numberOfEntries)
        return;
    size_t index = 0;
    state.objects.removeAllMatching([&](const ObjectEntry&) {
        return !isLiveEntry[index++];
    });
}

void ObjectInterpreter::enterEdge(const OutgoingEdge& outgoing, const Terminator* terminator, const SinkingState* headOfTarget)
{
    m_enteredEdge.edge = &outgoing;
    m_enteredEdge.terminator = terminator;
    m_enteredEdge.newAliases.shrink(0);
    m_enteredEdge.newJoinStates.shrink(0);
    m_enteredEdge.consumedSources.shrink(0);
    uint32_t targetIndex = outgoing.edge.targetIndex;
    bool isPlain = !isRewriting() && m_sinking.isPlain(outgoing.edge, outgoing.target);
    bool takesShortcuts = isPlain && !m_sinking.checksShortcuts();
    bool isTaken = m_state.isReachable && (takesShortcuts || (!m_sinking.isIgnored(outgoing.edge) && !(terminator && isDeadEdge(*terminator, targetIndex))));
    RELEASE_ASSERT(!isPlain || isTaken || !m_state.isReachable);
    if (!isTaken || (m_mode != SinkingMode::Analyze && !headOfTarget)) {
        m_state = SinkingState::unreachableAt(outgoing.target);
        return;
    }

    if (takesShortcuts) {
        forgetAnswersOfTests();
        if (m_sinking.decisions().buildsAtHeadOf(outgoing.target))
            applyDecisions(outgoing);
    } else {
        std::optional<SinkingState> stateBefore;
        if (isPlain) [[unlikely]] {
            stateBefore = m_state;
            ObjectInterpreter(m_sinking, m_mode, *stateBefore).forgetAnswersOfTests();
        }
        if (terminator)
            narrowOnEdge(*terminator, targetIndex);
        bindParameters(outgoing);
        RELEASE_ASSERT(!isPlain || (m_state == *stateBefore && m_enteredEdge.newAliases.isEmpty() && m_enteredEdge.newJoinStates.isEmpty() && m_enteredEdge.consumedSources.isEmpty()));
        RELEASE_ASSERT(!isPlain || !objectOf(function().at(outgoing.target).subject));
        applyDecisions(outgoing);
        RELEASE_ASSERT(!isPlain || m_sinking.decisions().buildsAtHeadOf(outgoing.target) || m_state == *stateBefore);
        if (isPlain) [[unlikely]]
            stateBefore = m_state;
        commitBindings();
        RELEASE_ASSERT(!isPlain || m_state == *stateBefore);
    }
    dropDeadEntries(outgoing.target, headOfTarget);
    if (!takesShortcuts)
        abandonJoinsOfLiveSources();
    makeFieldsOfCellsValid(outgoing, headOfTarget);

    bool meetsStatesOfCoveredBlock = m_mode == SinkingMode::Analyze && outgoing.edge.isHandlerEdge();
    for (ObjectEntry& entry : m_state.objects) {
        entry.state.normalize();
        if (!meetsStatesOfCoveredBlock)
            continue;
        m_sinking.tagVariants(outgoing.target, entry.object, entry.state);
        if (entry.state.hasConflictingTags())
            requireCellInCoveredBlock(m_sinking, m_state, outgoing.edge.from, entry.object);
    }
    m_state.block = outgoing.target;
    m_state.isAtHead = true;
}

bool ObjectInterpreter::tryEnterPlainEdge(BlockID target)
{
    ASSERT(m_mode == SinkingMode::Analyze && m_state.isReachable);
    if (m_sinking.decisions().buildsAtHeadOf(target) || !m_state.aliases.isEmpty() || !m_state.tests.isEmpty())
        return false;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!m_sinking.isDefinitionLiveIn(entry.object, target))
            return false;
    }

    bool canMeetOtherShapes = m_sinking.inputs().mergeBlocks.get(target.index());
    for (ObjectEntry& entry : m_state.objects) {
        ObjectState& state = entry.state;
        if (state.isCell()) {
            if (canMeetOtherShapes && !m_sinking.at(entry.object).isJoin && !state.isCellMissingOnSomeWay)
                state.fieldsValidInCell = m_sinking.loadableFieldsOf(entry.object);
            else
                state.fieldsValidInCell.clearAll();
        }
        state.normalize();
    }
    m_state.block = target;
    m_state.isAtHead = true;
    return true;
}

void ObjectInterpreter::applyDecisions(const OutgoingEdge& outgoing)
{
    VirtualObjectID subject = objectOf(function().at(outgoing.target).subject);
    Vector<VirtualObjectID, 4> objects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && (entry.object == subject || m_sinking.decisions().isBuiltAtHead(outgoing.target, entry.object)))
            objects.append(entry.object);
    }
    for (VirtualObjectID object : objects) {
        if (stateOf(object)->isCell())
            continue;
        willEmitOnEdge(outgoing);
        buildOnEdge(*this, m_sinking, outgoing, object);
    }
}

void ObjectInterpreter::bindParameters(const OutgoingEdge& outgoing)
{
    Vector<ValueID, 4> parameters = originalParametersOf(m_sinking, m_mode, outgoing.target);
    RELEASE_ASSERT(parameters.size() == outgoing.bindings.size());

    // Asked before anything is built on the edge: a build turns the subject of an interpreted SwitchOnShape into a cell.
    const Terminator* terminator = m_enteredEdge.terminator;
    bool isTestKept = terminator && !hasInterpretedTest(*terminator);
    for (size_t index = 0; index < parameters.size(); ++index) {
        IncomingBinding binding = outgoing.bindings[index];
        if (isTestKept && binding.kind != IncomingBinding::Kind::Argument)
            binding.kind = IncomingBinding::Kind::Other;
        bindParameter(parameters[index], binding);
    }
}

void ObjectInterpreter::bindParameter(ValueID parameter, const IncomingBinding& binding)
{
    if (VirtualObjectID join = m_sinking.candidateDefinedBy(parameter)) {
        enterJoin(join, binding);
        return;
    }

    const OutgoingEdge& outgoing = *m_enteredEdge.edge;
    AliasEntry alias = aliasOfBinding(*this, m_enteredEdge.terminator, parameter, binding);
    if (!alias.object)
        return;
    if (isJoinReboundOnEdge(*this, m_sinking, outgoing, alias.object))
        m_sinking.abandon(alias.object);
    TypeID parameterType = function().at(parameter).type;
    if (alias.kind == AliasKind::Object && !fitsType(alias.object, parameterType))
        m_sinking.abandon(alias.object);
    if (alias.kind == AliasKind::Object)
        assumeTypeOfAlias(alias.object, parameterType);
    bool isSubject = parameter == function().at(outgoing.target).subject;
    if (!isSubject && !m_sinking.decisions().holdsCells(parameter)) {
        m_enteredEdge.newAliases.append(alias);
        return;
    }

    if (alias.kind == AliasKind::Object && outgoing.edge.isHandlerEdge() && !fitsTypeWithoutCasts(alias.object, parameterType))
        m_sinking.abandon(alias.object);
    if (alias.kind == AliasKind::Guard && m_sinking.mayBeListOfNumbers(alias.object))
        m_sinking.abandon(alias.object);
    if (alias.kind == AliasKind::Guard && m_mode == SinkingMode::Count)
        m_sinking.at(alias.object).needsGuard = true;
    if (stateOf(alias.object)->isCell())
        return;
    willEmitOnEdge(outgoing);
    buildOnEdge(*this, m_sinking, outgoing, alias.object);
}

void ObjectInterpreter::enterJoin(VirtualObjectID join, const IncomingBinding& binding)
{
    const OutgoingEdge& outgoing = *m_enteredEdge.edge;
    VirtualObjectID source = objectOf(binding.value);
    if (source == join)
        return;
    if (m_mode == SinkingMode::Count && source)
        m_sinking.didEnterJoin(join, source);
    if (binding.kind == IncomingBinding::Kind::Other)
        m_sinking.abandon(join);
    if (source) {
        const ObjectState& sourceState = *stateOf(source);
        TypeID joinType = function().at(m_sinking.at(join).definition).type;
        bool canCastCell = !outgoing.edge.isHandlerEdge() && (sourceState.isCell() || !sourceState.hasCell());
        if (!(canCastCell ? fitsType(source, joinType) : fitsTypeWithoutCasts(source, joinType)))
            m_sinking.abandon(join);
    }

    bool isOfOtherFamily = source && m_sinking.at(source).family != m_sinking.at(join).family;
    bool entersAsCell = isOfOtherFamily || m_sinking.decisions().isBuiltAtHead(outgoing.target, join);
    if (source && !entersAsCell) {
        m_enteredEdge.consumedSources.append(ConsumedSource { source, join });
        return;
    }
    if (source && !stateOf(source)->isCell()) {
        willEmitOnEdge(outgoing);
        buildOnEdge(*this, m_sinking, outgoing, source);
    }

    VirtualShapeID shape = cellShape;
    if (!entersAsCell) {
        if (std::optional<VirtualConstant> constant = virtualConstantOf(program(), function(), binding.value))
            shape = m_sinking.familyOf(join).intern(VirtualShape::ofConstant(*constant));
    }
    m_enteredEdge.newJoinStates.append(ObjectEntry { join, ObjectState::withShape(shape) });
}

void ObjectInterpreter::commitBindings()
{
    // Building the holder of a source for another parameter builds the source too, so its state is read after every build of the edge.
    for (const ConsumedSource& consumed : m_enteredEdge.consumedSources)
        m_enteredEdge.newJoinStates.append(ObjectEntry { consumed.join, *stateOf(consumed.source) });

    Vector<std::pair<VirtualObjectID, ObjectValues>, 1> valuesOfJoins;
    if (isRewriting()) {
        for (const ObjectEntry& join : m_enteredEdge.newJoinStates)
            valuesOfJoins.append({ join.object, valuesEnteringJoin(*this, m_sinking, m_enteredEdge, join) });
    }

    BlockID target = m_enteredEdge.edge->target;
    if (isRewriting()) {
        const CompactList<ValueID>& parameters = function().at(target).parameters;
        m_state.aliases.removeAllMatching([&](const AliasEntry& alias) {
            return parameters.contains(alias.value);
        });
    } else if (!m_state.aliases.isEmpty()) {
        const UseLists& useLists = m_sinking.analyses().useLists();
        m_state.aliases.removeAllMatching([&](const AliasEntry& alias) {
            return isParameterOf(useLists, target, alias.value);
        });
    }
    for (const AliasEntry& alias : m_enteredEdge.newAliases)
        m_state.setAlias(alias.value, alias.object, alias.kind);
    m_state.set(m_enteredEdge.newJoinStates.mutableSpan());
    for (auto& [join, joinValues] : valuesOfJoins)
        values().ensure(join) = WTF::move(joinValues);
}

void ObjectInterpreter::abandonJoinsOfLiveSources()
{
    if (m_mode != SinkingMode::Analyze)
        return;

    for (const ConsumedSource& consumed : m_enteredEdge.consumedSources) {
        if (stateOf(consumed.join)->isCell())
            continue;
        bool hasSeveralJoins = std::ranges::count(m_enteredEdge.consumedSources, consumed.source, &ConsumedSource::source) > 1;
        if (hasSeveralJoins || stateOf(consumed.source))
            m_sinking.abandon(consumed.join);
    }
    if (m_enteredEdge.newJoinStates.isEmpty())
        return;
    BitVector referencedObjects = collectReferencedObjects(m_state);
    for (const ObjectEntry& join : m_enteredEdge.newJoinStates) {
        if (referencedObjects.get(join.object.index()))
            m_sinking.abandon(join.object);
    }
}

void ObjectInterpreter::makeFieldsOfCellsValid(const OutgoingEdge& outgoing, const SinkingState* headOfTarget)
{
    bool isHandlerEdge = outgoing.edge.isHandlerEdge();
    bool canMeetOtherShapes = m_sinking.inputs().mergeBlocks.get(outgoing.target.index());
    m_noCellHasValidFields = false;
    for (ObjectEntry& entry : m_state.objects) {
        ObjectState& state = entry.state;
        if (!state.hasCell())
            continue;
        bool isJoin = m_sinking.at(entry.object).isJoin;
        if (isHandlerEdge || (isJoin && state.isCell())) {
            state.fieldsValidInCell.clearAll();
            continue;
        }
        if (isJoin || !state.isCell())
            continue;
        if (m_mode == SinkingMode::Analyze) {
            if (canMeetOtherShapes && !state.isCellMissingOnSomeWay)
                state.fieldsValidInCell = m_sinking.loadableFieldsOf(entry.object);
            else
                state.fieldsValidInCell.clearAll();
            continue;
        }

        const ObjectState* head = headOfTarget->find(entry.object);
        if (!head || !head->hasCell() || head->isCell())
            continue;
        if (m_mode == SinkingMode::Count && hasFieldHoldingCells(m_sinking, entry.object))
            m_sinking.didBuildAfterMerge(entry.object, outgoing.target);
        state.fieldsValidInCell = head->fieldsValidInCell;
        if (state.fieldsValidInCell.isEmpty())
            continue;

        willEmitOnEdge(outgoing);
        const VirtualObjectDescription& description = m_sinking.descriptionOf(entry.object);
        if (m_mode == SinkingMode::Count) {
            if (description.kind == VirtualObjectKind::Object)
                m_sinking.at(entry.object).needsGuard = true;
            continue;
        }

        const VirtualFamily& family = m_sinking.familyOf(entry.object);
        ObjectValues& carried = values().ensure(entry.object);
        for (size_t bit : state.fieldsValidInCell) {
            uint32_t field = static_cast<uint32_t>(bit);
            Value load = makeFieldRead(program(), description, family.fieldAt(field), carried.cell, carried.guard);
            if (Value::opcodesWithReceivers.contains(load.opcode))
                setExactReceivers(load, description.layout, cursor().builder().types());
            carried.setField(field, cursor().emit(WTF::move(load)));
        }
    }
}

void ObjectInterpreter::dropDeadEntries(BlockID target, const SinkingState* headOfTarget)
{
    if (m_mode == SinkingMode::Analyze) {
        removeDeadEntries(m_sinking, m_state, target);
        return;
    }

    // fillTarget() resolves the arguments of the edge after this, so a cell stays although the head does not have it.
    Vector<VirtualObjectID, 4> deadObjects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && !headOfTarget->find(entry.object))
            deadObjects.append(entry.object);
    }
    for (VirtualObjectID object : deadObjects) {
        m_state.remove(object);
        if (isRewriting()) {
            values().remove(object);
            continue;
        }
        Candidate& candidate = m_sinking.at(object);
        candidate.diesVirtual = true;
        candidate.diesOutsideJoins |= !std::ranges::contains(m_enteredEdge.consumedSources, object, &ConsumedSource::source);
    }
    m_state.aliases.removeAllMatching([&](const AliasEntry& alias) {
        return !stateOf(alias.object);
    });
    m_state.tests.removeAllMatching([&](const TestEntry& test) {
        return !headOfTarget->findTest(test.test);
    });
}

void ObjectInterpreter::willEmitOnEdge(const OutgoingEdge& outgoing)
{
    if (m_mode != SinkingMode::Count || outgoing.edge.isHandlerEdge())
        return;
    if (function().at(outgoing.edge.from).terminator.targets.size() > 1)
        m_sinking.addEdgeNeedingCode(outgoing.edge);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
