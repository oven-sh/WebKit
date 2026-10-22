/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include <algorithm>
#include <functional>

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

static bool holdsNothing(const SinkingState& state)
{
    return state.objects.isEmpty() && state.aliases.isEmpty() && state.tests.isEmpty();
}

namespace {

class SinkingSolver {
    WTF_MAKE_NONCOPYABLE(SinkingSolver);
public:
    SinkingSolver(AllocationSinking& sinking, SinkingProblem& problem)
        : m_sinking(sinking)
        , m_problem(problem)
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
        m_problem.merge(entry.state, state);
        entry.includesCurrentState = true;
    }

    bool didChangeHead(BlockID target, uint32_t fromOrder)
    {
        uint32_t targetOrder = m_cfg.reversePostOrderIndex(target);
        m_isPending.quickSet(targetOrder);
        return targetOrder <= fromOrder;
    }

    // Most blocks are reached and have nothing to do with any object. Their states are made when somebody wants to see one.
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
            if (!m_problem.merge(*head, WTF::move(state)))
                return false;
        } else
            head = WTF::move(state);
        return didChangeHead(target, fromOrder);
    }

    bool propagate(Edge edge, SinkingState&& state, uint32_t fromOrder)
    {
        if (holdsNothing(state))
            return propagateNothing(edge, state.isReachable, fromOrder);
        m_sinking.didWork(1 + state.weight() + m_function.at(targetOf(m_function, edge).block).parameters.size());
        m_problem.enterEdge(state, edge);
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
            if (isEnteredWithNothing || (head && head->isReachable && holdsNothing(*head)))
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
            m_problem.enterEdge(state, edge);
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
        if (holdsNothing(head) && (!head.isReachable || !m_inputs.blocksWithCandidates.get(id.index())))
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
            // A value that was passed over counts for the handler as one that was executed.
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
    SinkingProblem& m_problem;
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

        SinkingProblem problem(*this);
        if (m_blocksCarrying.size() != numberOfCandidates())
            m_blocksCarrying.fill(0, numberOfCandidates());
        ++m_numberOfSweeps;
        SinkingSolver solver(*this, problem);
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
        if (round >= limit(LimitID::MaxSinkingRounds))
            giveUpOnNewDecisions();
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

// The work grows with the number of objects that are followed and with the blocks that each is carried through.
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
    std::ranges::stable_sort(mostFrequentFirst, std::ranges::greater { }, [&](uint32_t index) {
        return frequencies.frequencyOf(m_candidates[index].block);
    });

    // An object costs what it is carried through. What may stay is the share of that which the limit pays for, and never more than half of the allocations.
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

bool buildsItsOperands(const Program& program, const Value& user)
{
    return interpretationOf(program, user) == Interpretation::None && familyOf(user.opcode) != OperationFamily::RuntimeValidation;
}

bool AllocationSinking::isBuiltInItsBlock(const Candidate& allocation) const
{
    return std::ranges::any_of(m_analyses.useLists().usesOf(allocation.definition), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && use.block == allocation.block && buildsItsOperands(m_program, m_function.at(use.user));
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

        // A head inside a loop is first reached with the one shape of the object and then, over the back edge, with that shape under a tag.
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
    return State::nothingAt(entryBlock);
}

void SinkingProblem::enterEdge(State& state, Edge edge)
{
    const Function& function = m_sinking.function();
    BlockID target = targetOf(function, edge).block;
    if (!state.isReachable) {
        state = State::unreachableAt(target);
        return;
    }

    bool isPlain = m_sinking.isPlain(edge, target) && !m_sinking.checksShortcuts();
    if (isPlain && ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).tryEnterPlainEdge(target))
        return;
    OutgoingEdge outgoing = isPlain ? OutgoingEdge { edge, target, { } } : outgoingEdgeOf(function, edge);
    const Terminator* terminator = edge.isHandlerEdge() ? nullptr : &function.at(edge.from).terminator;
    if (m_sinking.checksShortcuts() && m_sinking.isPlain(edge, target)) [[unlikely]] {
        State shortcut = state;
        bool didEnter = ObjectInterpreter(m_sinking, SinkingMode::Analyze, shortcut).tryEnterPlainEdge(target);
        ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).enterEdge(outgoing, terminator, nullptr);
        RELEASE_ASSERT(!didEnter || shortcut == state);
        return;
    }
    ObjectInterpreter(m_sinking, SinkingMode::Analyze, state).enterEdge(outgoing, terminator, nullptr);
}

bool SinkingProblem::replaces(const State& into, const State& from) const
{
    bool isHeadWithOneIncomingEdge = from.isAtHead && !m_sinking.inputs().mergeBlocks.get(into.block.index());
    return !into.isReachable || isHeadWithOneIncomingEdge;
}

bool SinkingProblem::merge(State& into, const State& from)
{
    if (!from.isReachable)
        return false;
    if (!replaces(into, from))
        return mergeParts(into, from);
    if (into == from)
        return false;
    into = from;
    return true;
}

bool SinkingProblem::merge(State& into, State&& from)
{
    if (!from.isReachable)
        return false;
    if (!replaces(into, from))
        return mergeParts(into, from);
    if (into == from)
        return false;
    into = WTF::move(from);
    return true;
}

bool SinkingProblem::mergeParts(State& into, const State& from)
{
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
    bool becomesCell = into.isAtHead && willSolveAgainWithCell(m_sinking, into.block, object);
    if (!becomesCell && target.variants.size() == 1 && target == source)
        return false;

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
